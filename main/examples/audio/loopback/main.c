/*
 * ESP32-P4 Audio Loopback Test
 * Codec: board-selected input + ES8389 output
 * Real-time microphone input to speaker output
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "t_panel_audio_codec.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "audio_loopback";

#define SAMPLE_RATE             44100
#define BITS_PER_SAMPLE         16
#define CHANNELS                2
#define LOOPBACK_BUF_BYTES      2048
#define DB_LOG_INTERVAL_MS      500

#if CONFIG_T_PANEL_P4_BOARD_RECT
#define OUTPUT_VOLUME           100
#define INPUT_GAIN_DB           12.0f
#define NOISE_GATE_OPEN_RMS      500
#define NOISE_GATE_CLOSE_RMS     250
#define NOISE_GATE_HOLD_BUFFERS  8
#else
#define OUTPUT_VOLUME           100
#define INPUT_GAIN_DB           12.0f
#define NOISE_GATE_OPEN_RMS      0
#define NOISE_GATE_CLOSE_RMS     0
#define NOISE_GATE_HOLD_BUFFERS  0
#endif

static t_panel_p4_bsp_t s_bsp;
static t_panel_audio_codec_handle_t s_audio;

static void loopback_task(void *args)
{
    (void)args;
    int16_t *buf = heap_caps_malloc(LOOPBACK_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate loopback buffer");
        vTaskDelete(NULL);
        return;
    }

    const size_t sample_count = LOOPBACK_BUF_BYTES / sizeof(int16_t);
    bool gate_open = NOISE_GATE_OPEN_RMS == 0;
    unsigned gate_hold = 0;
    TickType_t last_db_log = xTaskGetTickCount();
    ESP_LOGI(TAG, "Loopback running: volume=%d, input gain=%.1f dB, gate=%d/%d RMS",
             OUTPUT_VOLUME, INPUT_GAIN_DB, NOISE_GATE_OPEN_RMS,
             NOISE_GATE_CLOSE_RMS);

    while (1) {
        esp_err_t ret = t_panel_audio_codec_read(s_audio, buf,
                                                 LOOPBACK_BUF_BYTES);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int64_t channel_sum_sq[CHANNELS] = {0};
        for (size_t i = 0; i < sample_count; i++) {
            channel_sum_sq[i % CHANNELS] += (int32_t)buf[i] * buf[i];
        }
        const int64_t sum_sq = channel_sum_sq[0] + channel_sum_sq[1];
        const int32_t rms = (int32_t)sqrt((double)sum_sq / sample_count);

        if (NOISE_GATE_OPEN_RMS > 0) {
            if (!gate_open && rms >= NOISE_GATE_OPEN_RMS) {
                gate_open = true;
                gate_hold = NOISE_GATE_HOLD_BUFFERS;
            } else if (gate_open && rms >= NOISE_GATE_CLOSE_RMS) {
                gate_hold = NOISE_GATE_HOLD_BUFFERS;
            } else if (gate_open && gate_hold > 0) {
                gate_hold--;
            } else {
                gate_open = false;
            }
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - last_db_log >= pdMS_TO_TICKS(DB_LOG_INTERVAL_MS)) {
            const size_t channel_samples = sample_count / CHANNELS;
            ESP_LOGI(TAG,
                     "Mic level: MIC1=%.1f dBFS, MIC2=%.1f dBFS, combined=%.1f dBFS, gate=%s",
                     t_panel_audio_codec_calculate_dbfs_s16(
                         buf, channel_samples, CHANNELS),
                     t_panel_audio_codec_calculate_dbfs_s16(
                         buf + 1, channel_samples, CHANNELS),
                     t_panel_audio_codec_calculate_dbfs_s16(
                         buf, sample_count, 1),
                     gate_open ? "open" : "closed");
            last_db_log = now;
        }

        if (!gate_open) {
            memset(buf, 0, LOOPBACK_BUF_BYTES);
        }

        ret = t_panel_audio_codec_write(s_audio, buf, LOOPBACK_BUF_BYTES);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Audio Loopback Test (MIC -> Speaker) ===");
    esp_log_level_set("ES8389", ESP_LOG_DEBUG);
    esp_log_level_set("ES7210", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&s_bsp));

    t_panel_audio_codec_config_t config = T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT();
    config.sample_rate_hz = SAMPLE_RATE;
    config.bits_per_sample = BITS_PER_SAMPLE;
    config.playback_channels = CHANNELS;
    config.capture_channels = CHANNELS;
    config.output_volume = OUTPUT_VOLUME;
    config.input_gain_db = INPUT_GAIN_DB;
    ESP_ERROR_CHECK(t_panel_audio_codec_init(&s_bsp, &config, &s_audio));

    xTaskCreate(loopback_task, "loopback", 4096, NULL, 5, NULL);
}
