/*
 * ESP32-P4 WAV playback example.
 * Board resources and the ES8389 playback path are initialized by the BSP
 * and audio_codec components.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "t_panel_audio_codec.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "audio_play_wav";

#define AUDIO_OUTPUT_VOLUME 100
#define PLAYBACK_CHUNK_SIZE 1024
#define SOFTWARE_GAIN       1.0f

typedef struct __attribute__((packed)) {
    char riff_tag[4];
    uint32_t file_size;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

extern const uint8_t wav_start[] asm("_binary_sample_3s_wav_start");
extern const uint8_t wav_end[] asm("_binary_sample_3s_wav_end");

static t_panel_p4_bsp_t s_bsp;
static t_panel_audio_codec_handle_t s_audio;
static const uint8_t *s_pcm_data;
static size_t s_pcm_size;

static size_t find_data_chunk(const uint8_t *wav, size_t wav_size)
{
    size_t offset = 12;
    while (offset + 8 <= wav_size) {
        uint32_t chunk_size = 0;
        memcpy(&chunk_size, wav + offset + 4, sizeof(chunk_size));
        if (memcmp(wav + offset, "data", 4) == 0) {
            return offset + 8;
        }
        if (chunk_size > wav_size - offset - 8) {
            break;
        }
        offset += 8 + chunk_size + (chunk_size & 1U);
    }
    return 0;
}

static int16_t apply_gain(int32_t sample, float gain)
{
    int32_t value = (int32_t)(sample * gain);
    if (value > INT16_MAX) {
        value = INT16_MAX;
    } else if (value < INT16_MIN) {
        value = INT16_MIN;
    }
    return (int16_t)value;
}

static void prepare_pcm(const int16_t *src, int16_t *dst,
                        size_t sample_count, float gain)
{
#if CONFIG_T_PANEL_P4_BOARD_RECT
    /* Rect uses the two DAC outputs as a differential mono speaker signal. */
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        const int32_t mono = ((int32_t)src[i] + src[i + 1]) / 2;
        dst[i] = apply_gain(mono, gain);
        dst[i + 1] = apply_gain(mono, gain);
    }
#else
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = apply_gain(src[i], gain);
    }
#endif
}

static void playback_task(void *args)
{
    (void)args;
    int16_t *buffer = heap_caps_malloc(PLAYBACK_CHUNK_SIZE,
                                       MALLOC_CAP_DEFAULT);
    if (!buffer) {
        ESP_LOGE(TAG, "Allocate playback buffer failed");
        vTaskDelete(NULL);
        return;
    }

    size_t offset = 0;
    unsigned loop_count = 0;
    ESP_LOGI(TAG, "Playback started: %u PCM bytes",
             (unsigned)s_pcm_size);

    while (true) {
        const size_t remaining = s_pcm_size - offset;
        const size_t write_size = remaining < PLAYBACK_CHUNK_SIZE
                                      ? remaining
                                      : PLAYBACK_CHUNK_SIZE;
        prepare_pcm((const int16_t *)(s_pcm_data + offset), buffer,
                    write_size / sizeof(int16_t), SOFTWARE_GAIN);

        const esp_err_t ret = t_panel_audio_codec_write(s_audio, buffer,
                                                         write_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(ret));
            break;
        }

        offset += write_size;
        if (offset >= s_pcm_size) {
            offset = 0;
            ESP_LOGI(TAG, "Playback loop %u complete", ++loop_count);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    t_panel_audio_codec_set_speaker_enabled(s_audio, false);
    free(buffer);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "WAV playback using BSP audio initialization");

    const size_t wav_size = wav_end - wav_start;
    if (wav_size < sizeof(wav_header_t)) {
        ESP_LOGE(TAG, "WAV file is too small");
        return;
    }

    const wav_header_t *header = (const wav_header_t *)wav_start;
    if (memcmp(header->riff_tag, "RIFF", 4) != 0 ||
        memcmp(header->wave_tag, "WAVE", 4) != 0 ||
        header->audio_format != 1 || header->num_channels != 2) {
        ESP_LOGE(TAG, "Only stereo PCM WAV files are supported");
        return;
    }

    const size_t data_offset = find_data_chunk(wav_start, wav_size);
    if (!data_offset) {
        ESP_LOGE(TAG, "WAV data chunk not found");
        return;
    }
    s_pcm_data = wav_start + data_offset;
    s_pcm_size = wav_size - data_offset;

    ESP_LOGI(TAG, "WAV: %lu Hz, %u-bit, %u channels, PCM=%u bytes",
             (unsigned long)header->sample_rate, header->bits_per_sample,
             header->num_channels, (unsigned)s_pcm_size);

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&s_bsp));

    t_panel_audio_codec_config_t config = T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT();
    config.sample_rate_hz = header->sample_rate;
    config.bits_per_sample = header->bits_per_sample;
    config.playback_channels = header->num_channels;
    config.enable_playback = true;
    config.enable_capture = false;
    config.enable_speaker = true;
    config.output_volume = AUDIO_OUTPUT_VOLUME;
    ESP_ERROR_CHECK(t_panel_audio_codec_init(&s_bsp, &config, &s_audio));

    const BaseType_t created = xTaskCreate(playback_task, "playback", 4096,
                                            NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Create playback task failed");
        ESP_ERROR_CHECK(t_panel_audio_codec_deinit(s_audio));
        ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&s_bsp));
    }
}
