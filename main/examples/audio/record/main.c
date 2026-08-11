/*
 * ESP32-P4 Audio Recording Example
 * Codec: ES8389 (ADC mode, 2 microphones)
 * Continuously reads audio data and outputs dB level
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
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

#include "T_Panle_P4_board_config.h"

static const char *TAG = "audio_record";

#define I2C_NUM (0)
#define I2S_NUM (0)
#define SAMPLE_RATE (16000)
#define BITS_PER_SAMPLE (16)
#define CHANNELS (2)
#define RECORD_BUF_SIZE (1024)

#define ES8389_I2C_ADDR ES8389_CODEC_DEFAULT_ADDR

static esp_codec_dev_handle_t rec_dev = NULL;

static float calculate_db(int16_t *samples, size_t sample_count)
{
    if (sample_count == 0)
        return -96.0f;

    double sum_sq = 0.0;
    for (size_t i = 0; i < sample_count; i++)
    {
        double s = (double)samples[i];
        sum_sq += s * s;
    }
    double rms = sqrt(sum_sq / sample_count);
    if (rms < 1.0)
        return -96.0f;
    return 20.0f * log10f((float)(rms / 32768.0));
}

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

static esp_err_t init_codec(i2c_master_bus_handle_t bus_handle)
{
    // I2C control interface
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8389_I2C_ADDR,
        .bus_handle = bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    // I2S data interface (RX only for recording)
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx_handle), TAG, "I2S new channel failed");

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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "I2S init std mode failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = rx_handle,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // ES8389 codec in ADC mode (2 mics)
    es8389_codec_cfg_t codec_cfg = {
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
    const audio_codec_if_t *codec_if = es8389_codec_new(&codec_cfg);
    if (!codec_if)
    {
        ESP_LOGE(TAG, "ES8389 codec new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    rec_dev = esp_codec_dev_new(&dev_cfg);
    if (!rec_dev)
    {
        ESP_LOGE(TAG, "Codec dev new failed");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(rec_dev, 30.0), TAG, "Set mic gain failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNELS,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(rec_dev, &fs), TAG, "Codec dev open failed");

    ESP_LOGI(TAG, "ES8389 codec initialized (ADC mode, 2 mics, gain=30dB)");
    return ESP_OK;
}

static void record_task(void *args)
{
    int16_t *buf = heap_caps_malloc(RECORD_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!buf)
    {
        ESP_LOGE(TAG, "Failed to allocate record buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t samples_per_read = RECORD_BUF_SIZE;
    size_t bytes_to_read = samples_per_read * sizeof(int16_t);
    size_t frame_count = samples_per_read / CHANNELS;

    int read_count = 0;
    while (1)
    {
        esp_err_t ret = esp_codec_dev_read(rec_dev, buf, bytes_to_read);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (read_count < 3)
        {
            ESP_LOGI(TAG, "Raw samples[0..7]: %d %d %d %d %d %d %d %d",
                     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            read_count++;
        }

        double sum_sq_l = 0.0, sum_sq_r = 0.0;
        for (size_t i = 0; i < frame_count; i++)
        {
            double l = (double)buf[i * 2];
            double r = (double)buf[i * 2 + 1];
            sum_sq_l += l * l;
            sum_sq_r += r * r;
        }

        double rms_l = sqrt(sum_sq_l / frame_count);
        double rms_r = sqrt(sum_sq_r / frame_count);
        float db_l = (rms_l < 1.0) ? -96.0f : 20.0f * log10f((float)(rms_l / 32768.0));
        float db_r = (rms_r < 1.0) ? -96.0f : 20.0f * log10f((float)(rms_r / 32768.0));

        ESP_LOGI(TAG, "MIC-L: %.1f dBFS | MIC-R: %.1f dBFS", db_l, db_r);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Audio Recording - dB Meter ===");
    esp_log_level_set("ES8389", ESP_LOG_DEBUG);

    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(init_i2c(&bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized");

    ESP_ERROR_CHECK(init_codec(bus_handle));

    xTaskCreate(record_task, "record_task", 8192, NULL, 5, NULL);
}
