/*
 * ESP32-P4 Audio Playback Example
 * Codec: ES8389 (DAC mode)
 * Speaker amplifier: controlled by XL9555_SPK_CRTL
 * Plays embedded WAV file (sample-3s.wav)
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8389_codec.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"

#include "esp_heap_caps.h"
#include "T_Panle_P4_board_config.h"

static const char *TAG = "audio_example";

#define I2C_NUM (0)
#define I2S_NUM (0)
#define AUDIO_SAMPLE_RATE (44100)
#define AUDIO_BITS_PER_SAMPLE (16)
#define AUDIO_CHANNELS (2)

#define ES8389_I2C_ADDR ES8389_CODEC_DEFAULT_ADDR

// WAV file header structure
typedef struct __attribute__((packed))
{
    char riff_tag[4]; // "RIFF"
    uint32_t file_size;
    char wave_tag[4]; // "WAVE"
    char fmt_tag[4];  // "fmt "
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

// Embed the WAV file into flash
extern const uint8_t wav_start[] asm("_binary_sample_3s_wav_start");
extern const uint8_t wav_end[] asm("_binary_sample_3s_wav_end");
const uint8_t *pcm_data;
size_t pcm_size;
i2s_chan_handle_t tx_handle = NULL;
static esp_codec_dev_handle_t play_dev = NULL;

static void i2s_music(void *args);

static esp_err_t init_i2c(i2c_master_bus_handle_t *bus_handle)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, bus_handle);
}

static esp_err_t init_xl9555_spk(i2c_master_bus_handle_t bus_handle)
{
    esp_io_expander_handle_t expander = NULL;
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_xl9555(bus_handle, XL9555_I2C_ADDR, &expander),
                        TAG, "XL9555 init failed");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, 1 << XL9555_SPK_CRTL, IO_EXPANDER_OUTPUT),
                        TAG, "Set SPK pin direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, 1 << XL9555_SPK_CRTL, 1),
                        TAG, "Set SPK pin level failed");

    ESP_LOGI(TAG, "Speaker amplifier enabled");
    return ESP_OK;
}

static esp_err_t init_i2s(i2s_chan_handle_t *tx_handle)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx_handle, NULL), TAG, "I2S new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_PIN,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DO_PIN,
            .din = I2S_DI_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*tx_handle, &std_cfg), TAG, "I2S init std mode failed");

    ESP_LOGI(TAG, "I2S initialized: %d Hz, %d-bit, %d ch",
             AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_CHANNELS);
    return ESP_OK;
}

static esp_err_t init_codec(i2c_master_bus_handle_t bus_handle, i2s_chan_handle_t tx_handle)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8389_I2C_ADDR,
        .bus_handle = bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .tx_handle = tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8389_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 0,
            .codec_dac_voltage = 0,
        },
        .mclk_div = I2S_MCLK_MULTIPLE_384,
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&codec_cfg);
    if (!codec_if)
    {
        ESP_LOGE(TAG, "ES8389 codec new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    play_dev = esp_codec_dev_new(&dev_cfg);
    if (!play_dev)
    {
        ESP_LOGE(TAG, "Codec dev new failed");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev, 80),
                        TAG, "Set volume failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel = AUDIO_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(play_dev, &fs),
                        TAG, "Codec dev open failed");

    ESP_LOGI(TAG, "ES8389 codec initialized (DAC mode, vol=80)");
    return ESP_OK;
}

static size_t find_data_chunk(const uint8_t *wav, size_t wav_size)
{
    size_t offset = 12; // skip RIFF header
    while (offset + 8 < wav_size)
    {
        uint32_t chunk_size = *(uint32_t *)(wav + offset + 4);
        if (memcmp(wav + offset, "data", 4) == 0)
        {
            return offset + 8;
        }
        offset += 8 + chunk_size;
    }
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Audio Playback Test (WAV file) ===");
    esp_log_level_set("ES8389", ESP_LOG_DEBUG);
    esp_log_level_set("I2S_IF", ESP_LOG_DEBUG);
    esp_log_level_set("Adev_Codec", ESP_LOG_DEBUG);

    size_t wav_size = wav_end - wav_start;
    ESP_LOGI(TAG, "WAV file size: %u bytes", (unsigned)wav_size);

    // Parse WAV header
    if (wav_size < sizeof(wav_header_t))
    {
        ESP_LOGE(TAG, "WAV file too small");
        return;
    }
    wav_header_t *hdr = (wav_header_t *)wav_start;
    ESP_LOGI(TAG, "WAV: %d Hz, %d-bit, %d ch, format=%d",
             hdr->sample_rate, hdr->bits_per_sample, hdr->num_channels, hdr->audio_format);

    // Find PCM data offset
    size_t data_offset = find_data_chunk(wav_start, wav_size);
    if (data_offset == 0)
    {
        ESP_LOGE(TAG, "WAV data chunk not found");
        return;
    }
    pcm_data = wav_start + data_offset;
    pcm_size = wav_size - data_offset;
    ESP_LOGI(TAG, "PCM data offset: %u, size: %u bytes", (unsigned)data_offset, (unsigned)pcm_size);

    // Init peripherals
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(init_i2c(&bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized");

    ESP_ERROR_CHECK(init_xl9555_spk(bus_handle));

    ESP_ERROR_CHECK(init_i2s(&tx_handle));

    ESP_ERROR_CHECK(init_codec(bus_handle, tx_handle));

    xTaskCreate(i2s_music, "i2s_music", 4096, NULL, 5, NULL);
}

// Software gain multiplier (adjust this value: 1.0=no change, 4.0=~12dB boost)
#define SOFTWARE_GAIN 1.0f

static void amplify_pcm(const int16_t *src, int16_t *dst, size_t sample_count, float gain)
{
    for (size_t i = 0; i < sample_count; i++) {
        int32_t val = (int32_t)(src[i] * gain);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        dst[i] = (int16_t)val;
    }
}

static void i2s_music(void *args)
{
    const size_t chunk_size = 1024;
    size_t offset = 0;
    int loop_count = 0;

    int16_t *amp_buf = heap_caps_malloc(chunk_size, MALLOC_CAP_DEFAULT);
    if (!amp_buf) {
        ESP_LOGE(TAG, "[music] Failed to allocate amplify buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[music] Task started, pcm_size=%u, gain=%.1f", (unsigned)pcm_size, SOFTWARE_GAIN);

    while (1)
    {
        size_t remaining = pcm_size - offset;
        size_t to_write = remaining < chunk_size ? remaining : chunk_size;

        size_t sample_count = to_write / sizeof(int16_t);
        amplify_pcm((const int16_t *)(pcm_data + offset), amp_buf, sample_count, SOFTWARE_GAIN);

        esp_err_t ret = esp_codec_dev_write(play_dev, amp_buf, to_write);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[music] codec dev write failed: %s", esp_err_to_name(ret));
            break;
        }

        offset += to_write;
        if (offset >= pcm_size)
        {
            loop_count++;
            ESP_LOGI(TAG, "[music] Loop %d complete", loop_count);
            offset = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    free(amp_buf);
    vTaskDelete(NULL);
}
