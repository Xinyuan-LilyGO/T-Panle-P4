/*
 * ESP32-P4 record-then-play example.
 * Rect captures with ES7210; Standard/Round capture with ES8389.
 */

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "t_panel_audio_codec.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "audio_record";

#define SAMPLE_RATE            16000
#define BITS_PER_SAMPLE        16
#define CHANNELS               2
#define RECORD_CHUNK_SAMPLES   1024
#define RECORD_SECONDS         5
#define INPUT_GAIN_DB          30.0f
#define OUTPUT_VOLUME          70
#define LEVEL_LOG_INTERVAL_MS  500

#define RECORD_BYTES_PER_SECOND \
    ((size_t)SAMPLE_RATE * CHANNELS * sizeof(int16_t))
#define MAX_RECORD_BYTES \
    (RECORD_BYTES_PER_SECOND * RECORD_SECONDS)

static t_panel_p4_bsp_t s_bsp;
static t_panel_audio_codec_handle_t s_audio;

static void prepare_playback_pcm(int16_t *samples, size_t sample_count)
{
#if CONFIG_T_PANEL_P4_BOARD_RECT
    /* Rect drives a differential mono speaker from the two DAC channels. */
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        const int16_t mono = (int16_t)(((int32_t)samples[i] + samples[i + 1]) / 2);
        samples[i] = mono;
        samples[i + 1] = mono;
    }
#else
    (void)samples;
    (void)sample_count;
#endif
}

static size_t capture_audio(uint8_t *recording, int16_t *chunk)
{
    const size_t chunk_bytes = RECORD_CHUNK_SAMPLES * sizeof(*chunk);
    TickType_t last_level_log = xTaskGetTickCount();
    size_t recorded_bytes = 0;
    unsigned raw_log_count = 0;

    ESP_LOGI(TAG, "Recording started: %d seconds", RECORD_SECONDS);

    while (recorded_bytes < MAX_RECORD_BYTES) {
        const size_t remaining = MAX_RECORD_BYTES - recorded_bytes;
        const size_t read_size = remaining < chunk_bytes ? remaining : chunk_bytes;
        const esp_err_t ret = t_panel_audio_codec_read(s_audio, chunk, read_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        memcpy(recording + recorded_bytes, chunk, read_size);
        recorded_bytes += read_size;

        if (raw_log_count < 3) {
            ESP_LOGI(TAG, "Raw samples[0..7]: %d %d %d %d %d %d %d %d",
                     chunk[0], chunk[1], chunk[2], chunk[3],
                     chunk[4], chunk[5], chunk[6], chunk[7]);
            raw_log_count++;
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_level_log >= pdMS_TO_TICKS(LEVEL_LOG_INTERVAL_MS)) {
            const size_t samples_per_channel =
                read_size / sizeof(*chunk) / CHANNELS;
            const float mic1_dbfs = t_panel_audio_codec_calculate_dbfs_s16(
                chunk, samples_per_channel, CHANNELS);
            const float mic2_dbfs = t_panel_audio_codec_calculate_dbfs_s16(
                chunk + 1, samples_per_channel, CHANNELS);
            ESP_LOGI(TAG, "MIC1: %.1f dBFS | MIC2: %.1f dBFS | recorded %.1f s",
                     mic1_dbfs, mic2_dbfs,
                     (double)recorded_bytes / RECORD_BYTES_PER_SECOND);
            last_level_log = now;
        }

    }

    return recorded_bytes;
}

static esp_err_t playback_audio(const uint8_t *recording, size_t recorded_bytes,
                                int16_t *chunk)
{
    const size_t chunk_bytes = RECORD_CHUNK_SAMPLES * sizeof(*chunk);
    ESP_RETURN_ON_ERROR(t_panel_audio_codec_set_input_mute(s_audio, true),
                        TAG, "Mute microphone failed");
    ESP_RETURN_ON_ERROR(t_panel_audio_codec_set_speaker_enabled(s_audio, true),
                        TAG, "Enable speaker failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Playback started: %.1f seconds",
             (double)recorded_bytes / RECORD_BYTES_PER_SECOND);
    size_t offset = 0;
    esp_err_t ret = ESP_OK;
    while (offset < recorded_bytes) {
        const size_t remaining = recorded_bytes - offset;
        const size_t write_size = remaining < chunk_bytes ? remaining : chunk_bytes;
        memcpy(chunk, recording + offset, write_size);
        prepare_playback_pcm(chunk, write_size / sizeof(*chunk));

        ret = t_panel_audio_codec_write(s_audio, chunk, write_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(ret));
            break;
        }
        offset += write_size;
    }

    const esp_err_t speaker_ret =
        t_panel_audio_codec_set_speaker_enabled(s_audio, false);
    const esp_err_t mute_ret = t_panel_audio_codec_set_input_mute(s_audio, false);
    if (ret == ESP_OK) {
        ret = speaker_ret != ESP_OK ? speaker_ret : mute_ret;
    }
    ESP_LOGI(TAG, "Playback finished");
    return ret;
}

static void record_task(void *args)
{
    (void)args;
    uint8_t *recording = heap_caps_malloc(MAX_RECORD_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *chunk = heap_caps_malloc(RECORD_CHUNK_SAMPLES * sizeof(*chunk),
                                      MALLOC_CAP_DEFAULT);
    if (!recording || !chunk) {
        ESP_LOGE(TAG, "Allocate recording buffers failed (%u bytes PSRAM)",
                 (unsigned)MAX_RECORD_BYTES);
        free(recording);
        free(chunk);
        vTaskDelete(NULL);
        return;
    }

    unsigned cycle = 0;
    while (true) {
        ESP_LOGI(TAG, "Cycle %u", ++cycle);
        const size_t recorded_bytes = capture_audio(recording, chunk);
        const esp_err_t ret = playback_audio(recording, recorded_bytes, chunk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Record then Play ===");
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&s_bsp));

    t_panel_audio_codec_config_t config = T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT();
    config.sample_rate_hz = SAMPLE_RATE;
    config.bits_per_sample = BITS_PER_SAMPLE;
    config.playback_channels = CHANNELS;
    config.capture_channels = CHANNELS;
    config.enable_playback = true;
    config.enable_capture = true;
    config.enable_speaker = false;
    config.input = T_PANEL_AUDIO_INPUT_AUTO;
    config.input_gain_db = INPUT_GAIN_DB;
    config.output_volume = OUTPUT_VOLUME;
    ESP_ERROR_CHECK(t_panel_audio_codec_init(&s_bsp, &config, &s_audio));

    const t_panel_audio_input_t input = t_panel_audio_codec_get_input(s_audio);
    ESP_LOGI(TAG, "Capture codec: %s, record/play interval: %d seconds",
             input == T_PANEL_AUDIO_INPUT_ES7210 ? "ES7210" : "ES8389",
             RECORD_SECONDS);

    const BaseType_t created = xTaskCreate(record_task, "record_play", 4096,
                                            NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Create record task failed");
        ESP_ERROR_CHECK(t_panel_audio_codec_deinit(s_audio));
        ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&s_bsp));
    }
}
