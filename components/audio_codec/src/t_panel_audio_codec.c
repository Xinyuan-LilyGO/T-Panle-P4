#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "t_panel_audio_codec.h"
#include "t_panel_audio_codec_internal.h"

#if !CONFIG_CODEC_ES8389_SUPPORT
#error "audio_codec requires CONFIG_CODEC_ES8389_SUPPORT"
#endif

#if !CONFIG_CODEC_ES7210_SUPPORT
#error "audio_codec requires CONFIG_CODEC_ES7210_SUPPORT"
#endif

static const char *TAG = "t_panel_audio";

#define T_PANEL_AUDIO_DBFS_FLOOR (-96.0f)

struct t_panel_audio_codec {
    t_panel_p4_bsp_t *bsp;
    t_panel_audio_codec_config_t config;
    t_panel_audio_input_t input;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_data_if_t *playback_data_if;
    const audio_codec_data_if_t *capture_data_if;
    t_panel_audio_chip_instance_t es8389;
    t_panel_audio_chip_instance_t es7210;
    esp_codec_dev_handle_t playback_dev;
    esp_codec_dev_handle_t capture_dev;
    bool started;
    bool speaker_requested;
    bool i2s_owned;
};

static unsigned mic_count(uint8_t mask)
{
    unsigned count = 0;
    while (mask) {
        count += mask & 1U;
        mask >>= 1;
    }
    return count;
}

float t_panel_audio_codec_calculate_dbfs_s16(const int16_t *samples,
                                             size_t sample_count,
                                             size_t stride)
{
    if (!samples || sample_count == 0 || stride == 0) {
        return T_PANEL_AUDIO_DBFS_FLOOR;
    }

    int64_t sum_sq = 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t sample = samples[i * stride];
        sum_sq += sample * sample;
    }
    const int32_t rms = (int32_t)sqrt((double)sum_sq / sample_count);
    if (rms == 0) {
        return T_PANEL_AUDIO_DBFS_FLOOR;
    }

    const float dbfs = 20.0f * log10f((float)rms / (float)INT16_MAX);
    return dbfs < T_PANEL_AUDIO_DBFS_FLOOR
               ? T_PANEL_AUDIO_DBFS_FLOOR
               : dbfs;
}

static t_panel_audio_input_t resolve_input(t_panel_audio_input_t input)
{
    if (input != T_PANEL_AUDIO_INPUT_AUTO) {
        return input;
    }
#if CONFIG_T_PANEL_P4_BOARD_RECT
    return T_PANEL_AUDIO_INPUT_ES7210;
#else
    return T_PANEL_AUDIO_INPUT_ES8389;
#endif
}

static esp_err_t validate_config(const t_panel_audio_codec_config_t *config,
                                 t_panel_audio_input_t input)
{
    ESP_RETURN_ON_FALSE(config && (config->enable_playback || config->enable_capture),
                        ESP_ERR_INVALID_ARG, TAG, "no audio direction enabled");
    ESP_RETURN_ON_FALSE(config->sample_rate_hz >= 8000 &&
                            config->sample_rate_hz <= 96000,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sample rate");
    ESP_RETURN_ON_FALSE(config->bits_per_sample == 16 ||
                            config->bits_per_sample == 24 ||
                            config->bits_per_sample == 32,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sample width");
    ESP_RETURN_ON_FALSE(!config->enable_playback || config->playback_channels == 2,
                        ESP_ERR_INVALID_ARG, TAG,
                        "playback currently requires two channels");
    ESP_RETURN_ON_FALSE(!config->enable_capture ||
                            config->capture_channels == 2 ||
                            config->capture_channels == 4,
                        ESP_ERR_INVALID_ARG, TAG,
                        "capture channels must be two or four");
    ESP_RETURN_ON_FALSE(config->output_volume >= 0 && config->output_volume <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "volume must be 0..100");

    if (!config->enable_capture) {
        return ESP_OK;
    }
    if (input == T_PANEL_AUDIO_INPUT_ES8389) {
        ESP_RETURN_ON_FALSE(config->capture_channels == 2, ESP_ERR_INVALID_ARG,
                            TAG, "ES8389 capture requires two channels");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(input == T_PANEL_AUDIO_INPUT_ES7210,
                        ESP_ERR_INVALID_ARG, TAG, "invalid input codec");
#if !CONFIG_T_PANEL_P4_BOARD_RECT
    ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,
                        "ES7210 is only present on the Rect board");
#endif
    const uint8_t valid_mics = T_PANEL_AUDIO_ES7210_MIC1 |
                               T_PANEL_AUDIO_ES7210_MIC2 |
                               T_PANEL_AUDIO_ES7210_MIC3 |
                               T_PANEL_AUDIO_ES7210_MIC4;
    ESP_RETURN_ON_FALSE(config->es7210_mic_mask &&
                            !(config->es7210_mic_mask & ~valid_mics),
                        ESP_ERR_INVALID_ARG, TAG, "invalid ES7210 microphone mask");
    const bool tdm = mic_count(config->es7210_mic_mask) >= 3;
    ESP_RETURN_ON_FALSE(config->capture_channels == (tdm ? 4 : 2),
                        ESP_ERR_INVALID_ARG, TAG,
                        "ES7210 needs 4 capture channels when three or more microphones are selected");
    return ESP_OK;
}

static i2s_data_bit_width_t get_i2s_width(uint8_t bits)
{
    switch (bits) {
    case 24:
        return I2S_DATA_BIT_WIDTH_24BIT;
    case 32:
        return I2S_DATA_BIT_WIDTH_32BIT;
    default:
        return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

static bool uses_shared_es8389_device(t_panel_audio_codec_handle_t codec)
{
    return codec->config.enable_playback && codec->config.enable_capture &&
           codec->input == T_PANEL_AUDIO_INPUT_ES8389;
}

static esp_err_t create_data_interfaces(t_panel_audio_codec_handle_t codec)
{
    if (uses_shared_es8389_device(codec)) {
        audio_codec_i2s_cfg_t config = {
            .port = I2S_NUM_0,
            .tx_handle = codec->bsp->i2s_tx,
            .rx_handle = codec->bsp->i2s_rx,
        };
        codec->playback_data_if = audio_codec_new_i2s_data(&config);
        ESP_RETURN_ON_FALSE(codec->playback_data_if, ESP_ERR_NO_MEM, TAG,
                            "create duplex data interface failed");
        codec->capture_data_if = codec->playback_data_if;
        return ESP_OK;
    }

    if (codec->config.enable_playback) {
        audio_codec_i2s_cfg_t config = {
            .port = I2S_NUM_0,
            .tx_handle = codec->bsp->i2s_tx,
        };
        codec->playback_data_if = audio_codec_new_i2s_data(&config);
        ESP_RETURN_ON_FALSE(codec->playback_data_if, ESP_ERR_NO_MEM, TAG,
                            "create playback data interface failed");
    }
    if (codec->config.enable_capture) {
        audio_codec_i2s_cfg_t config = {
            .port = I2S_NUM_0,
            .rx_handle = codec->bsp->i2s_rx,
        };
        codec->capture_data_if = audio_codec_new_i2s_data(&config);
        ESP_RETURN_ON_FALSE(codec->capture_data_if, ESP_ERR_NO_MEM, TAG,
                            "create capture data interface failed");
    }
    return ESP_OK;
}

static esp_err_t create_codec_interfaces(t_panel_audio_codec_handle_t codec)
{
    const bool need_es8389 = codec->config.enable_playback ||
                             (codec->config.enable_capture &&
                              codec->input == T_PANEL_AUDIO_INPUT_ES8389);
    if (need_es8389) {
        codec->gpio_if = audio_codec_new_gpio();
        ESP_RETURN_ON_FALSE(codec->gpio_if, ESP_ERR_NO_MEM, TAG,
                            "create codec GPIO interface failed");

        esp_codec_dec_work_mode_t mode = ESP_CODEC_DEV_WORK_MODE_NONE;
        if (codec->config.enable_playback) {
            mode |= ESP_CODEC_DEV_WORK_MODE_DAC;
        }
        if (codec->config.enable_capture &&
            codec->input == T_PANEL_AUDIO_INPUT_ES8389) {
            mode |= ESP_CODEC_DEV_WORK_MODE_ADC;
        }
        ESP_RETURN_ON_ERROR(t_panel_es8389_create(codec->bsp->i2c_bus,
                                                  codec->gpio_if, mode,
                                                  codec->config.mclk_multiple,
                                                  &codec->es8389),
                            TAG, "initialize ES8389 failed");
    }

    if (codec->config.enable_capture &&
        codec->input == T_PANEL_AUDIO_INPUT_ES7210) {
        ESP_RETURN_ON_ERROR(t_panel_es7210_create(codec->bsp->i2c_bus,
                                                  codec->config.es7210_mic_mask,
                                                  codec->config.mclk_multiple,
                                                  &codec->es7210),
                            TAG, "initialize ES7210 failed");
    }
    return ESP_OK;
}

static esp_err_t create_devices(t_panel_audio_codec_handle_t codec)
{
    if (uses_shared_es8389_device(codec)) {
        esp_codec_dev_cfg_t config = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec->es8389.codec_if,
            .data_if = codec->playback_data_if,
        };
        codec->playback_dev = esp_codec_dev_new(&config);
        ESP_RETURN_ON_FALSE(codec->playback_dev, ESP_ERR_NO_MEM, TAG,
                            "create duplex device failed");
        codec->capture_dev = codec->playback_dev;
        return ESP_OK;
    }

    if (codec->config.enable_playback) {
        esp_codec_dev_cfg_t config = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = codec->es8389.codec_if,
            .data_if = codec->playback_data_if,
        };
        codec->playback_dev = esp_codec_dev_new(&config);
        ESP_RETURN_ON_FALSE(codec->playback_dev, ESP_ERR_NO_MEM, TAG,
                            "create playback device failed");
    }
    if (codec->config.enable_capture) {
        const audio_codec_if_t *input_if =
            codec->input == T_PANEL_AUDIO_INPUT_ES7210
                ? codec->es7210.codec_if
                : codec->es8389.codec_if;
        esp_codec_dev_cfg_t config = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = input_if,
            .data_if = codec->capture_data_if,
        };
        codec->capture_dev = esp_codec_dev_new(&config);
        ESP_RETURN_ON_FALSE(codec->capture_dev, ESP_ERR_NO_MEM, TAG,
                            "create capture device failed");
    }
    return ESP_OK;
}

static esp_codec_dev_sample_info_t playback_format(t_panel_audio_codec_handle_t codec)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = codec->config.bits_per_sample,
        .channel = codec->config.playback_channels,
        .sample_rate = codec->config.sample_rate_hz,
        .mclk_multiple = codec->config.mclk_multiple,
    };
}

static esp_codec_dev_sample_info_t capture_format(t_panel_audio_codec_handle_t codec)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = codec->config.bits_per_sample,
        .channel = codec->config.capture_channels,
        .sample_rate = codec->config.sample_rate_hz,
        .mclk_multiple = codec->config.mclk_multiple,
    };
}

esp_err_t t_panel_audio_codec_init(t_panel_p4_bsp_t *bsp,
                                   const t_panel_audio_codec_config_t *config,
                                   t_panel_audio_codec_handle_t *ret_codec)
{
    ESP_RETURN_ON_FALSE(bsp && bsp->i2c_bus && config && ret_codec,
                        ESP_ERR_INVALID_ARG, TAG, "invalid initialization arguments");
    ESP_RETURN_ON_FALSE(!bsp->i2s_tx && !bsp->i2s_rx, ESP_ERR_INVALID_STATE,
                        TAG, "BSP I2S is already in use");
    *ret_codec = NULL;

    const t_panel_audio_input_t input = resolve_input(config->input);
    ESP_RETURN_ON_ERROR(validate_config(config, input), TAG,
                        "invalid audio configuration");

    t_panel_audio_codec_handle_t codec = calloc(1, sizeof(*codec));
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_NO_MEM, TAG, "allocate audio codec failed");
    codec->bsp = bsp;
    codec->config = *config;
    codec->input = input;
    codec->speaker_requested = config->enable_speaker && config->enable_playback;

    t_panel_p4_bsp_i2s_config_t i2s_config = T_PANEL_P4_BSP_I2S_CONFIG_DEFAULT();
    i2s_config.sample_rate_hz = config->sample_rate_hz;
    i2s_config.data_bit_width = get_i2s_width(config->bits_per_sample);
    i2s_config.mclk_multiple = config->mclk_multiple;
    i2s_config.enable_tx = config->enable_playback;
    i2s_config.enable_rx = config->enable_capture;

    esp_err_t ret = t_panel_p4_bsp_i2s_init(bsp, &i2s_config);
    if (ret == ESP_OK) {
        codec->i2s_owned = true;
        ret = create_data_interfaces(codec);
    }
    if (ret == ESP_OK) {
        ret = create_codec_interfaces(codec);
    }
    if (ret == ESP_OK) {
        ret = create_devices(codec);
    }
    if (ret == ESP_OK && codec->playback_dev) {
        ret = esp_codec_dev_set_out_vol(codec->playback_dev,
                                        config->output_volume);
    }
    if (ret == ESP_OK && codec->capture_dev) {
        ret = esp_codec_dev_set_in_gain(codec->capture_dev,
                                        config->input_gain_db);
    }
    if (ret == ESP_OK) {
        ret = t_panel_audio_codec_start(codec);
    }
    if (ret != ESP_OK) {
        t_panel_audio_codec_deinit(codec);
        return ret;
    }

    *ret_codec = codec;
    ESP_LOGI(TAG, "%s audio ready: input=%s, %lu Hz, %u-bit",
             t_panel_p4_bsp_get_board_name(),
             input == T_PANEL_AUDIO_INPUT_ES7210 ? "ES7210" : "ES8389",
             (unsigned long)config->sample_rate_hz, config->bits_per_sample);
    return ESP_OK;
}

esp_err_t t_panel_audio_codec_deinit(t_panel_audio_codec_handle_t codec)
{
    if (!codec) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = t_panel_audio_codec_stop(codec);

    if (codec->playback_dev) {
        esp_codec_dev_delete(codec->playback_dev);
    }
    if (codec->capture_dev && codec->capture_dev != codec->playback_dev) {
        esp_codec_dev_delete(codec->capture_dev);
    }
    if (codec->playback_data_if) {
        audio_codec_delete_data_if(codec->playback_data_if);
    }
    if (codec->capture_data_if &&
        codec->capture_data_if != codec->playback_data_if) {
        audio_codec_delete_data_if(codec->capture_data_if);
    }
    t_panel_es7210_destroy(&codec->es7210);
    t_panel_es8389_destroy(&codec->es8389);
    if (codec->gpio_if) {
        audio_codec_delete_gpio_if(codec->gpio_if);
    }
    if (codec->i2s_owned) {
        const esp_err_t i2s_ret = t_panel_p4_bsp_i2s_deinit(codec->bsp);
        if (ret == ESP_OK) {
            ret = i2s_ret;
        }
    }
    free(codec);
    return ret;
}

esp_err_t t_panel_audio_codec_start(t_panel_audio_codec_handle_t codec)
{
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_INVALID_ARG, TAG, "codec is NULL");
    if (codec->started) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (codec->playback_dev) {
        esp_codec_dev_sample_info_t format = playback_format(codec);
        ret = esp_codec_dev_open(codec->playback_dev, &format);
    }
    if (ret == ESP_OK && codec->capture_dev &&
        codec->capture_dev != codec->playback_dev) {
        esp_codec_dev_sample_info_t format = capture_format(codec);
        ret = esp_codec_dev_open(codec->capture_dev, &format);
    }
    if (ret != ESP_OK) {
        if (codec->playback_dev) {
            esp_codec_dev_close(codec->playback_dev);
        }
        if (codec->capture_dev && codec->capture_dev != codec->playback_dev) {
            esp_codec_dev_close(codec->capture_dev);
        }
        return ret;
    }

#if CONFIG_T_PANEL_P4_BOARD_RECT
    if (codec->playback_dev) {
        ret = t_panel_es8389_configure_rect_output(codec->playback_dev);
    }
#endif
    if (ret == ESP_OK && codec->speaker_requested) {
        ret = t_panel_p4_bsp_speaker_set_enabled(codec->bsp, true);
    }
    if (ret != ESP_OK) {
        if (codec->playback_dev) {
            esp_codec_dev_close(codec->playback_dev);
        }
        if (codec->capture_dev && codec->capture_dev != codec->playback_dev) {
            esp_codec_dev_close(codec->capture_dev);
        }
        return ret;
    }
    codec->started = true;
    return ESP_OK;
}

esp_err_t t_panel_audio_codec_stop(t_panel_audio_codec_handle_t codec)
{
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_INVALID_ARG, TAG, "codec is NULL");
    esp_err_t ret = t_panel_p4_bsp_speaker_set_enabled(codec->bsp, false);
    if (codec->playback_dev) {
        const esp_err_t close_ret = esp_codec_dev_close(codec->playback_dev);
        if (ret == ESP_OK) {
            ret = close_ret;
        }
    }
    if (codec->capture_dev && codec->capture_dev != codec->playback_dev) {
        const esp_err_t close_ret = esp_codec_dev_close(codec->capture_dev);
        if (ret == ESP_OK) {
            ret = close_ret;
        }
    }
    codec->started = false;
    return ret;
}

esp_err_t t_panel_audio_codec_write(t_panel_audio_codec_handle_t codec,
                                    void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev && codec->started &&
                            data && size && size <= INT_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid playback arguments");
    return esp_codec_dev_write(codec->playback_dev, data, (int)size);
}

esp_err_t t_panel_audio_codec_read(t_panel_audio_codec_handle_t codec,
                                   void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev && codec->started &&
                            data && size && size <= INT_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid capture arguments");
    return esp_codec_dev_read(codec->capture_dev, data, (int)size);
}

esp_err_t t_panel_audio_codec_set_volume(t_panel_audio_codec_handle_t codec,
                                         int volume)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev && volume >= 0 && volume <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "invalid volume");
    return esp_codec_dev_set_out_vol(codec->playback_dev, volume);
}

esp_err_t t_panel_audio_codec_get_volume(t_panel_audio_codec_handle_t codec,
                                         int *volume)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev && volume,
                        ESP_ERR_INVALID_ARG, TAG, "invalid volume output");
    return esp_codec_dev_get_out_vol(codec->playback_dev, volume);
}

esp_err_t t_panel_audio_codec_set_output_mute(t_panel_audio_codec_handle_t codec,
                                              bool mute)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev, ESP_ERR_INVALID_ARG,
                        TAG, "playback is not enabled");
    return esp_codec_dev_set_out_mute(codec->playback_dev, mute);
}

esp_err_t t_panel_audio_codec_get_output_mute(t_panel_audio_codec_handle_t codec,
                                              bool *muted)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev && muted,
                        ESP_ERR_INVALID_ARG, TAG, "invalid mute output");
    return esp_codec_dev_get_out_mute(codec->playback_dev, muted);
}

esp_err_t t_panel_audio_codec_set_input_gain(t_panel_audio_codec_handle_t codec,
                                             float gain_db)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev, ESP_ERR_INVALID_ARG,
                        TAG, "capture is not enabled");
    return esp_codec_dev_set_in_gain(codec->capture_dev, gain_db);
}

esp_err_t t_panel_audio_codec_set_input_channel_gain(t_panel_audio_codec_handle_t codec,
                                                     uint16_t channel_mask,
                                                     float gain_db)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev && channel_mask,
                        ESP_ERR_INVALID_ARG, TAG, "invalid input channel gain");
    return esp_codec_dev_set_in_channel_gain(codec->capture_dev,
                                             channel_mask, gain_db);
}

esp_err_t t_panel_audio_codec_get_input_gain(t_panel_audio_codec_handle_t codec,
                                             float *gain_db)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev && gain_db,
                        ESP_ERR_INVALID_ARG, TAG, "invalid gain output");
    return esp_codec_dev_get_in_gain(codec->capture_dev, gain_db);
}

esp_err_t t_panel_audio_codec_set_input_mute(t_panel_audio_codec_handle_t codec,
                                             bool mute)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev, ESP_ERR_INVALID_ARG,
                        TAG, "capture is not enabled");
    return esp_codec_dev_set_in_mute(codec->capture_dev, mute);
}

esp_err_t t_panel_audio_codec_get_input_mute(t_panel_audio_codec_handle_t codec,
                                             bool *muted)
{
    ESP_RETURN_ON_FALSE(codec && codec->capture_dev && muted,
                        ESP_ERR_INVALID_ARG, TAG, "invalid mute output");
    return esp_codec_dev_get_in_mute(codec->capture_dev, muted);
}

esp_err_t t_panel_audio_codec_set_speaker_enabled(t_panel_audio_codec_handle_t codec,
                                                  bool enabled)
{
    ESP_RETURN_ON_FALSE(codec && codec->playback_dev, ESP_ERR_INVALID_ARG,
                        TAG, "playback is not enabled");
    codec->speaker_requested = enabled;
    if (!codec->started) {
        return ESP_OK;
    }
    return t_panel_p4_bsp_speaker_set_enabled(codec->bsp, enabled);
}

bool t_panel_audio_codec_is_speaker_enabled(t_panel_audio_codec_handle_t codec)
{
    return codec && t_panel_p4_bsp_speaker_is_enabled(codec->bsp);
}

t_panel_audio_input_t t_panel_audio_codec_get_input(t_panel_audio_codec_handle_t codec)
{
    return codec ? codec->input : T_PANEL_AUDIO_INPUT_AUTO;
}

static esp_codec_dev_handle_t chip_dev(t_panel_audio_codec_handle_t codec,
                                       t_panel_audio_chip_t chip)
{
    if (!codec) {
        return NULL;
    }
    if (chip == T_PANEL_AUDIO_CHIP_ES7210) {
        return codec->es7210.codec_if ? codec->capture_dev : NULL;
    }
    if (chip == T_PANEL_AUDIO_CHIP_ES8389) {
        if (!codec->es8389.codec_if) {
            return NULL;
        }
        return codec->playback_dev ? codec->playback_dev : codec->capture_dev;
    }
    return NULL;
}

esp_err_t t_panel_audio_codec_read_reg(t_panel_audio_codec_handle_t codec,
                                       t_panel_audio_chip_t chip,
                                       int reg, int *value)
{
    esp_codec_dev_handle_t dev = chip_dev(codec, chip);
    ESP_RETURN_ON_FALSE(dev && value, ESP_ERR_INVALID_ARG, TAG,
                        "codec chip is not available");
    return esp_codec_dev_read_reg(dev, reg, value);
}

esp_err_t t_panel_audio_codec_write_reg(t_panel_audio_codec_handle_t codec,
                                        t_panel_audio_chip_t chip,
                                        int reg, int value)
{
    esp_codec_dev_handle_t dev = chip_dev(codec, chip);
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG,
                        "codec chip is not available");
    return esp_codec_dev_write_reg(dev, reg, value);
}

esp_err_t t_panel_audio_codec_dump_regs(t_panel_audio_codec_handle_t codec,
                                        t_panel_audio_chip_t chip)
{
    esp_codec_dev_handle_t dev = chip_dev(codec, chip);
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG,
                        "codec chip is not available");
    return esp_codec_dev_dump_reg(dev);
}

esp_codec_dev_handle_t t_panel_audio_codec_get_playback_dev(t_panel_audio_codec_handle_t codec)
{
    return codec ? codec->playback_dev : NULL;
}

esp_codec_dev_handle_t t_panel_audio_codec_get_capture_dev(t_panel_audio_codec_handle_t codec)
{
    return codec ? codec->capture_dev : NULL;
}
