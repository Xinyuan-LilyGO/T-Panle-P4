/*
 * ESP32-P4 Audio Loopback Test
 * Codec: ES8389 (ADC+DAC mode)
 * Real-time microphone input → speaker output
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
#include <math.h>
#include "esp_heap_caps.h"

#include "T_Panle_P4_board_config.h"

static const char *TAG = "audio_loopback";

#define I2C_NUM (0)
#define I2S_NUM (0)
#define SAMPLE_RATE (44100)
#define BITS_PER_SAMPLE (16)
#define CHANNELS (2)
#define LOOPBACK_BUF_BYTES (2048)
#define VOL (100)
#define NOISE_GATE_THRESHOLD (0)

#define ES8389_I2C_ADDR ES8389_CODEC_DEFAULT_ADDR

static esp_codec_dev_handle_t rec_dev = NULL;
static esp_codec_dev_handle_t play_dev = NULL;

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

static esp_err_t init_codec(i2c_master_bus_handle_t bus_handle)
{
    // I2C control interface
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8389_I2C_ADDR,
        .bus_handle = bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    // I2S channel (TX + RX)
    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "I2S new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "I2S RX init failed");

    // Data interface for playback (TX)
    audio_codec_i2s_cfg_t i2s_tx_cfg = {
        .port = I2S_NUM,
        .tx_handle = tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if_tx = audio_codec_new_i2s_data(&i2s_tx_cfg);

    // Data interface for recording (RX)
    audio_codec_i2s_cfg_t i2s_rx_cfg = {
        .port = I2S_NUM,
        .tx_handle = NULL,
        .rx_handle = rx_handle,
    };
    const audio_codec_data_if_t *data_if_rx = audio_codec_new_i2s_data(&i2s_rx_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // ES8389 codec - DAC for playback
    es8389_codec_cfg_t dac_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {0},
        .mclk_div = I2S_MCLK_MULTIPLE_384,
    };
    const audio_codec_if_t *dac_if = es8389_codec_new(&dac_cfg);

    // ES8389 codec - ADC for recording
    es8389_codec_cfg_t adc_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {0},
        .mclk_div = I2S_MCLK_MULTIPLE_384,
    };
    const audio_codec_if_t *adc_if = es8389_codec_new(&adc_cfg);

    if (!dac_if || !adc_if)
    {
        ESP_LOGE(TAG, "ES8389 codec new failed");
        return ESP_FAIL;
    }

    // Playback device
    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = dac_if,
        .data_if = data_if_tx,
    };
    play_dev = esp_codec_dev_new(&play_cfg);

    // Recording device
    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = adc_if,
        .data_if = data_if_rx,
    };
    rec_dev = esp_codec_dev_new(&rec_cfg);

    if (!play_dev || !rec_dev)
    {
        ESP_LOGE(TAG, "Codec dev new failed");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev, VOL), TAG, "Set vol failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(rec_dev, 30.0), TAG, "Set mic gain failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(play_dev, &fs), TAG, "Open play dev failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(rec_dev, &fs), TAG, "Open rec dev failed");

    ESP_LOGI(TAG, "Codec initialized (ADC+DAC, %d Hz, mic_gain=24dB, vol=%d)", SAMPLE_RATE, VOL);
    return ESP_OK;
}

static void loopback_task(void *args)
{
    int16_t *buf = heap_caps_malloc(LOOPBACK_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!buf)
    {
        ESP_LOGE(TAG, "Failed to allocate loopback buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t sample_count = LOOPBACK_BUF_BYTES / sizeof(int16_t);

    ESP_LOGI(TAG, "Loopback running...");

    while (1)
    {
        esp_err_t ret = esp_codec_dev_read(rec_dev, buf, LOOPBACK_BUF_BYTES);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Noise gate: calculate RMS, mute if below threshold
        int64_t sum_sq = 0;
        for (size_t i = 0; i < sample_count; i++)
        {
            sum_sq += (int32_t)buf[i] * buf[i];
        }
        int32_t rms = (int32_t)sqrt((double)sum_sq / sample_count);

        if (rms < NOISE_GATE_THRESHOLD)
        {
            memset(buf, 0, LOOPBACK_BUF_BYTES);
        }

        ret = esp_codec_dev_write(play_dev, buf, LOOPBACK_BUF_BYTES);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Audio Loopback Test (MIC -> Speaker) ===");
    esp_log_level_set("ES8389", ESP_LOG_DEBUG);

    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(init_i2c(&bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized");

    ESP_ERROR_CHECK(init_xl9555_spk(bus_handle));
    ESP_ERROR_CHECK(init_codec(bus_handle));

    xTaskCreate(loopback_task, "loopback", 4096, NULL, 5, NULL);
}
