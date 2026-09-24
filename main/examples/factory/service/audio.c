#include "audio.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_lyrics.h"
#include "t_panel_audio_codec.h"

static const char *TAG = "factory_audio";

#define AUDIO_MOUNT_POINT "/sdcard"
#define MUSIC_DIR AUDIO_MUSIC_DIR

#ifdef CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE
#define SAMPLE_RATE CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE
#else
#define SAMPLE_RATE (44100)
#endif
#define BITS_PER_SAMPLE (16)
#define CHANNELS (2)

#define RECORD_WAV_HEADER_SIZE 44U
#define RECORD_MAX_DATA_BYTES ((size_t)SAMPLE_RATE * CHANNELS * \
                               (BITS_PER_SAMPLE / 8) * \
                               (AUDIO_RECORD_MAX_DURATION_MS / 1000U))
#define RECORD_BUFFER_SIZE (RECORD_WAV_HEADER_SIZE + RECORD_MAX_DATA_BYTES)

#define AUDIO_PLAYER_TASK_PRIORITY (8)
#define AUDIO_CONTROL_TASK_PRIORITY (6)
#define AUDIO_CAPTURE_TASK_PRIORITY (4)

#define MIC_LEVEL_BUF_SAMPLES (2048)
#define MIC_LEVEL_DB_FLOOR (-60)
#define MIC_LEVEL_UPDATE_MS (50)
#define MIC_LEVEL_SMOOTH_NUM 1
#define MIC_LEVEL_SMOOTH_DEN 2

#define MUSIC_PLAYER_QUEUE_LEN 8
#define MUSIC_PATH_MAX_LEN AUDIO_MUSIC_PATH_MAX_LEN
#define MUSIC_URI_MAX_LEN (MUSIC_PATH_MAX_LEN + 8)
#define MUSIC_LRC_MAX_LINES 256
#define MUSIC_LRC_LINE_MAX_LEN 256
#define MUSIC_PROGRESS_UPDATE_MS 500
#define MUSIC_SPECTRUM_OUTPUT_UPDATE_MS 50
#define MUSIC_SPECTRUM_BAR_COUNT AUDIO_MUSIC_SPECTRUM_BAR_COUNT
#define MUSIC_SPECTRUM_SEGMENT_COUNT AUDIO_MUSIC_SPECTRUM_LEVEL_MAX

extern const uint8_t lilygo_mp3_start[] asm("_binary_lilygo_mp3_start");
extern const uint8_t lilygo_mp3_end[] asm("_binary_lilygo_mp3_end");

#define MUSIC_PLAYER_OUTPUT_SAMPLE_RATE SAMPLE_RATE

#ifdef CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST
#define MUSIC_PLAYER_OUTPUT_CHANNELS CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST
#else
#define MUSIC_PLAYER_OUTPUT_CHANNELS CHANNELS
#endif

#ifdef CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS
#define MUSIC_PLAYER_OUTPUT_BITS CONFIG_AUDIO_SIMPLE_PLAYER_BIT_CVT_DEST_BITS
#else
#define MUSIC_PLAYER_OUTPUT_BITS BITS_PER_SAMPLE
#endif

typedef struct
{
    int bitrate_bps;
    int sample_rate;
    int samples_per_frame;
    int frame_len;
    bool mpeg1;
    bool mono;
} music_mp3_frame_info_t;

typedef enum
{
    MUSIC_CMD_PLAY_CURRENT = 0,
    MUSIC_CMD_PLAY_INDEX,
    MUSIC_CMD_PAUSE,
    MUSIC_CMD_STOP,
    MUSIC_CMD_SET_VOLUME,
    MUSIC_CMD_SET_PLAY_MODE,
    MUSIC_CMD_PLAY_RECORDING,
    MUSIC_CMD_PLAY_RECORDING_FILE,
    MUSIC_CMD_PLAY_SELF_TEST,
} music_player_cmd_type_t;

typedef struct
{
    music_player_cmd_type_t type;
    int value;
} music_player_cmd_t;

static esp_codec_dev_handle_t s_rec_dev = NULL;
static esp_codec_dev_handle_t s_play_dev = NULL;
static t_panel_audio_codec_handle_t s_audio_codec = NULL;
static SemaphoreHandle_t s_audio_capture_mutex = NULL;
static SemaphoreHandle_t s_audio_playback_mutex = NULL;
static bool s_mic_level_task_started = false;
static volatile int s_mic_level_db[2] = {MIC_LEVEL_DB_FLOOR, MIC_LEVEL_DB_FLOOR};
static SemaphoreHandle_t s_record_mutex = NULL;
static uint8_t *s_record_buffer = NULL;
static volatile bool s_record_active = false;
static volatile bool s_record_paused = false;
static volatile bool s_record_file_playing = false;
static volatile bool s_record_playback_paused = false;
static char s_record_playback_path[AUDIO_MUSIC_PATH_MAX_LEN];
static volatile bool s_self_test_audio_playing = false;
static const uint8_t *s_embedded_audio_data = NULL;
static size_t s_embedded_audio_size = 0;
static size_t s_embedded_audio_offset = 0;
static uint32_t s_record_data_bytes = 0;
static int64_t s_record_started_us = 0;
static QueueHandle_t s_music_player_queue = NULL;
static TaskHandle_t s_music_player_task_handle = NULL;
static esp_asp_handle_t s_music_player_handle = NULL;
static volatile bool s_music_audio_active = false;
static volatile bool s_music_playback_done = false;
static volatile bool s_music_playback_error = false;
static bool s_music_output_open = false;
static int s_music_play_mode = 0;
static int s_music_selected_index = -1;
static int s_music_volume = AUDIO_DEFAULT_OUTPUT_VOLUME;
static int s_music_file_size_bytes = 0;
static int s_music_total_sec = 0;
static uint64_t s_music_output_bytes = 0;
static int64_t s_music_started_us = 0;
static int64_t s_music_pause_started_us = 0;
static int64_t s_music_paused_acc_us = 0;
static int64_t s_music_spectrum_last_us = 0;
static uint32_t s_music_spectrum_peak = 12000;
static char s_music_current_format[12] = "--";
static SemaphoreHandle_t s_music_state_mutex = NULL;
static char s_music_track_names[AUDIO_MUSIC_MAX_TRACKS][AUDIO_MUSIC_TRACK_NAME_MAX_LEN];
static int s_music_track_count = 0;
static audio_music_state_t s_music_state = {
    .revision = 1,
    .current_track = -1,
    .volume_percent = AUDIO_DEFAULT_OUTPUT_VOLUME,
    .format = "--",
    .sample_rate = "--",
    .bitrate = "--",
    .channels = "--",
};

static void music_state_revision_increment_locked(void)
{
    s_music_state.revision++;
    if (s_music_state.revision == 0)
    {
        s_music_state.revision = 1;
    }
}

static void music_state_text_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0)
    {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void music_state_set_playing(bool playing)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.playing = playing;
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_lyrics(const char *prev, const char *current, const char *next)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.lyrics_valid = prev != NULL || current != NULL || next != NULL;
    music_state_text_copy(s_music_state.lyrics_prev, sizeof(s_music_state.lyrics_prev), prev);
    music_state_text_copy(s_music_state.lyrics_current, sizeof(s_music_state.lyrics_current), current);
    music_state_text_copy(s_music_state.lyrics_next, sizeof(s_music_state.lyrics_next), next);
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_track_params(const char *format, const char *sample_rate,
                                         const char *bitrate, const char *channels)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    music_state_text_copy(s_music_state.format, sizeof(s_music_state.format), format);
    music_state_text_copy(s_music_state.sample_rate, sizeof(s_music_state.sample_rate), sample_rate);
    music_state_text_copy(s_music_state.bitrate, sizeof(s_music_state.bitrate), bitrate);
    music_state_text_copy(s_music_state.channels, sizeof(s_music_state.channels), channels);
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_progress(int current_sec, int total_sec)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.current_sec = current_sec;
    s_music_state.total_sec = total_sec;
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_spectrum(const uint8_t *levels, int count)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    memset(s_music_state.spectrum, 0, sizeof(s_music_state.spectrum));
    s_music_state.spectrum_valid = levels != NULL && count > 0;
    if (s_music_state.spectrum_valid)
    {
        if (count > AUDIO_MUSIC_SPECTRUM_BAR_COUNT)
        {
            count = AUDIO_MUSIC_SPECTRUM_BAR_COUNT;
        }
        memcpy(s_music_state.spectrum, levels, (size_t)count);
    }
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_current_track(int index, const char *cover_path)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.current_track = index;
    music_state_text_copy(s_music_state.cover_path, sizeof(s_music_state.cover_path), cover_path);
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_volume(int volume_percent)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.volume_percent = volume_percent;
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static void music_state_set_play_mode(int play_mode)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_music_state.play_mode = play_mode;
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
}

static int mic_rms_to_db(double sum_sq, size_t frame_count)
{
    if (frame_count == 0)
    {
        return MIC_LEVEL_DB_FLOOR;
    }

    double rms = sqrt(sum_sq / frame_count);
    if (rms < 1.0)
    {
        return MIC_LEVEL_DB_FLOOR;
    }

    float db = 20.0f * log10f((float)(rms / 32768.0));
    if (db < MIC_LEVEL_DB_FLOOR)
    {
        db = MIC_LEVEL_DB_FLOOR;
    }
    else if (db > 0.0f)
    {
        db = 0.0f;
    }

    return (int)db;
}

static void audio_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
}

static void audio_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static void audio_record_wav_header_write(uint8_t *buffer, uint32_t data_bytes)
{
    if (buffer == NULL)
    {
        return;
    }

    memset(buffer, 0, RECORD_WAV_HEADER_SIZE);
    memcpy(buffer, "RIFF", 4);
    audio_write_le32(buffer + 4, 36U + data_bytes);
    memcpy(buffer + 8, "WAVEfmt ", 8);
    audio_write_le32(buffer + 16, 16);
    audio_write_le16(buffer + 20, 1);
    audio_write_le16(buffer + 22, CHANNELS);
    audio_write_le32(buffer + 24, SAMPLE_RATE);
    audio_write_le32(buffer + 28, SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));
    audio_write_le16(buffer + 32, CHANNELS * (BITS_PER_SAMPLE / 8));
    audio_write_le16(buffer + 34, BITS_PER_SAMPLE);
    memcpy(buffer + 36, "data", 4);
    audio_write_le32(buffer + 40, data_bytes);
}

static void audio_record_write_chunk(const void *data, size_t size)
{
    if (!s_record_active || s_record_paused || data == NULL || size == 0 ||
        s_record_mutex == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return;
    }
    if (s_record_active && !s_record_paused && s_record_buffer)
    {
        size_t remaining = RECORD_MAX_DATA_BYTES - s_record_data_bytes;
        size_t copy_size = size < remaining ? size : remaining;
        if (copy_size > 0)
        {
            memcpy(s_record_buffer + RECORD_WAV_HEADER_SIZE + s_record_data_bytes,
                   data, copy_size);
            s_record_data_bytes += (uint32_t)copy_size;
        }
        if (s_record_data_bytes >= RECORD_MAX_DATA_BYTES)
        {
            audio_record_wav_header_write(s_record_buffer, s_record_data_bytes);
            s_record_active = false;
            ESP_LOGI(TAG, "Recording reached %u second limit: %" PRIu32 " bytes",
                     (unsigned)(AUDIO_RECORD_MAX_DURATION_MS / 1000U),
                     s_record_data_bytes);
        }
    }
    xSemaphoreGive(s_record_mutex);
}

static void mic_level_task(void *arg)
{
    (void)arg;

    if (s_rec_dev == NULL)
    {
        ESP_LOGE(TAG, "Recorder device is not initialized");
        s_mic_level_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    int16_t *buf = heap_caps_malloc(MIC_LEVEL_BUF_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
    {
        buf = heap_caps_malloc(MIC_LEVEL_BUF_SAMPLES * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf)
    {
        ESP_LOGE(TAG, "Failed to allocate mic level buffer");
        s_mic_level_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    const size_t bytes_to_read = MIC_LEVEL_BUF_SAMPLES * sizeof(int16_t);
    const size_t frame_count = MIC_LEVEL_BUF_SAMPLES / CHANNELS;
    int smooth_l = MIC_LEVEL_DB_FLOOR;
    int smooth_r = MIC_LEVEL_DB_FLOOR;
    bool has_level = false;
    int64_t last_level_update_us = 0;

    while (1)
    {
        if (s_music_audio_active)
        {
            s_mic_level_db[0] = MIC_LEVEL_DB_FLOOR;
            s_mic_level_db[1] = MIC_LEVEL_DB_FLOOR;
            vTaskDelay(pdMS_TO_TICKS(MIC_LEVEL_UPDATE_MS));
            continue;
        }

        if (s_audio_capture_mutex)
        {
            xSemaphoreTake(s_audio_capture_mutex, portMAX_DELAY);
        }
        esp_err_t ret = t_panel_audio_codec_read(s_audio_codec, buf,
                                                 bytes_to_read);
        if (s_audio_capture_mutex)
        {
            xSemaphoreGive(s_audio_capture_mutex);
        }
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Read mic level failed: %s", esp_err_to_name(ret));
            s_mic_level_db[0] = MIC_LEVEL_DB_FLOOR;
            s_mic_level_db[1] = MIC_LEVEL_DB_FLOOR;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (last_level_update_us == 0 ||
            now_us - last_level_update_us >= (int64_t)MIC_LEVEL_UPDATE_MS * 1000)
        {
            double sum_sq_l = 0.0;
            double sum_sq_r = 0.0;
            for (size_t i = 0; i < frame_count; i++)
            {
                double l = (double)buf[i * CHANNELS];
                double r = (double)buf[i * CHANNELS + 1];
                sum_sq_l += l * l;
                sum_sq_r += r * r;
            }

            int db_l = mic_rms_to_db(sum_sq_l, frame_count);
            int db_r = mic_rms_to_db(sum_sq_r, frame_count);
            if (has_level)
            {
                smooth_l = (smooth_l * MIC_LEVEL_SMOOTH_NUM + db_l) / MIC_LEVEL_SMOOTH_DEN;
                smooth_r = (smooth_r * MIC_LEVEL_SMOOTH_NUM + db_r) / MIC_LEVEL_SMOOTH_DEN;
            }
            else
            {
                smooth_l = db_l;
                smooth_r = db_r;
                has_level = true;
            }

            s_mic_level_db[0] = smooth_l;
            s_mic_level_db[1] = smooth_r;
            last_level_update_us = now_us;
        }

        audio_record_write_chunk(buf, bytes_to_read);
    }
}

bool audio_music_playlist_set(const char *const *tracks, int track_count, int current_index)
{
    if (track_count < 0 || track_count > AUDIO_MUSIC_MAX_TRACKS ||
        (track_count > 0 && tracks == NULL) || s_music_state_mutex == NULL)
    {
        return false;
    }

    if (current_index < 0 || current_index >= track_count)
    {
        current_index = track_count > 0 ? 0 : -1;
    }

    if (xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    memset(s_music_track_names, 0, sizeof(s_music_track_names));
    for (int i = 0; i < track_count; i++)
    {
        music_state_text_copy(s_music_track_names[i], sizeof(s_music_track_names[i]), tracks[i]);
    }
    s_music_track_count = track_count;
    s_music_selected_index = current_index;
    s_music_state.current_track = current_index;
    music_state_revision_increment_locked();
    xSemaphoreGive(s_music_state_mutex);
    return true;
}

int audio_music_track_count_get(void)
{
    if (s_music_state_mutex == NULL || xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return 0;
    }
    int count = s_music_track_count;
    xSemaphoreGive(s_music_state_mutex);
    return count;
}

bool audio_music_track_name_get(int index, char *name, size_t name_size)
{
    if (name == NULL || name_size == 0 || s_music_state_mutex == NULL ||
        xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    bool valid = index >= 0 && index < s_music_track_count && s_music_track_names[index][0] != '\0';
    if (valid)
    {
        music_state_text_copy(name, name_size, s_music_track_names[index]);
    }
    else
    {
        name[0] = '\0';
    }
    xSemaphoreGive(s_music_state_mutex);
    return valid;
}

bool audio_music_state_get(audio_music_state_t *state)
{
    if (state == NULL || s_music_state_mutex == NULL ||
        xSemaphoreTake(s_music_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    *state = s_music_state;
    xSemaphoreGive(s_music_state_mutex);
    return true;
}

static bool music_build_track_path(int index, char *path, size_t path_size)
{
    char name[AUDIO_MUSIC_TRACK_NAME_MAX_LEN] = {0};
    if (!audio_music_track_name_get(index, name, sizeof(name)))
    {
        return false;
    }

    size_t dir_len = strlen(MUSIC_DIR);
    size_t name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > path_size)
    {
        return false;
    }

    memcpy(path, MUSIC_DIR, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1, name, name_len);
    path[dir_len + 1 + name_len] = '\0';
    return true;
}

static bool music_build_lrc_path(const char *track_path, char *lrc_path, size_t path_size, bool upper_ext)
{
    size_t path_len = strlen(track_path);
    if (path_len + 1 > path_size)
    {
        return false;
    }

    memcpy(lrc_path, track_path, path_len + 1);
    char *slash = strrchr(lrc_path, '/');
    char *dot = strrchr(lrc_path, '.');
    if (dot == NULL || (slash != NULL && dot < slash))
    {
        return false;
    }

    const char *ext = upper_ext ? ".LRC" : ".lrc";
    size_t base_len = (size_t)(dot - lrc_path);
    if (base_len + strlen(ext) + 1 > path_size)
    {
        return false;
    }
    memcpy(lrc_path + base_len, ext, strlen(ext) + 1);
    return true;
}

static char *music_trim_text(char *text)
{
    while (*text && isspace((unsigned char)*text))
    {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
    {
        *--end = '\0';
    }
    return text;
}

static bool music_lrc_parse_timestamp(const char **cursor, uint32_t *out_ms)
{
    const char *p = *cursor;
    if (*p != '[')
    {
        return false;
    }
    p++;

    if (!isdigit((unsigned char)*p))
    {
        return false;
    }

    uint32_t min = 0;
    while (isdigit((unsigned char)*p))
    {
        min = min * 10 + (uint32_t)(*p - '0');
        p++;
    }

    if (*p != ':')
    {
        return false;
    }
    p++;

    if (!isdigit((unsigned char)*p))
    {
        return false;
    }

    uint32_t sec = 0;
    while (isdigit((unsigned char)*p))
    {
        sec = sec * 10 + (uint32_t)(*p - '0');
        p++;
    }

    uint32_t frac_ms = 0;
    if (*p == '.')
    {
        p++;
        int digits = 0;
        while (isdigit((unsigned char)*p))
        {
            if (digits < 3)
            {
                frac_ms = frac_ms * 10 + (uint32_t)(*p - '0');
                digits++;
            }
            p++;
        }
        if (digits == 1)
        {
            frac_ms *= 100;
        }
        else if (digits == 2)
        {
            frac_ms *= 10;
        }
    }

    if (*p != ']')
    {
        return false;
    }
    p++;

    *out_ms = (min * 60 + sec) * 1000 + frac_ms;
    *cursor = p;
    return true;
}

static void music_lrc_copy_text(char *dst, size_t dst_size, const char *src)
{
    bool utf8_ok = true;
    const unsigned char *p = (const unsigned char *)src;
    while (*p)
    {
        if (*p < 0x80)
        {
            p++;
            continue;
        }

        int trail = 0;
        if ((*p & 0xe0) == 0xc0)
        {
            trail = 1;
            if (*p < 0xc2)
            {
                utf8_ok = false;
                break;
            }
        }
        else if ((*p & 0xf0) == 0xe0)
        {
            trail = 2;
        }
        else if ((*p & 0xf8) == 0xf0)
        {
            trail = 3;
            if (*p > 0xf4)
            {
                utf8_ok = false;
                break;
            }
        }
        else
        {
            utf8_ok = false;
            break;
        }

        p++;
        for (int i = 0; i < trail; i++)
        {
            if ((p[i] & 0xc0) != 0x80)
            {
                utf8_ok = false;
                break;
            }
        }
        if (!utf8_ok)
        {
            break;
        }
        p += trail;
    }

    if (!utf8_ok)
    {
        snprintf(dst, dst_size, "%s", "[LRC is not UTF-8]");
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_size)
    {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int music_lrc_compare(const void *a, const void *b)
{
    const music_lyrics_line_t *la = (const music_lyrics_line_t *)a;
    const music_lyrics_line_t *lb = (const music_lyrics_line_t *)b;
    return (la->time_ms > lb->time_ms) - (la->time_ms < lb->time_ms);
}

static int music_lrc_load(const char *track_path, music_lyrics_line_t *lines, int max_lines)
{
    char lrc_path[MUSIC_PATH_MAX_LEN] = {0};
    if (!music_build_lrc_path(track_path, lrc_path, sizeof(lrc_path), false))
    {
        return 0;
    }

    FILE *file = fopen(lrc_path, "r");
    if (file == NULL && music_build_lrc_path(track_path, lrc_path, sizeof(lrc_path), true))
    {
        file = fopen(lrc_path, "r");
    }
    if (file == NULL)
    {
        return 0;
    }

    int count = 0;
    char line[MUSIC_LRC_LINE_MAX_LEN] = {0};
    while (fgets(line, sizeof(line), file) != NULL && count < max_lines)
    {
        const char *p = line;
        uint32_t stamps[4] = {0};
        int stamp_count = 0;
        while (*p == '[' && stamp_count < (int)(sizeof(stamps) / sizeof(stamps[0])))
        {
            uint32_t stamp = 0;
            const char *next = p;
            if (!music_lrc_parse_timestamp(&next, &stamp))
            {
                stamp_count = 0;
                break;
            }
            stamps[stamp_count++] = stamp;
            p = next;
        }

        char *text = music_trim_text((char *)p);
        if (stamp_count == 0 || text[0] == '\0')
        {
            continue;
        }

        for (int i = 0; i < stamp_count && count < max_lines; i++)
        {
            lines[count].time_ms = stamps[i];
            music_lrc_copy_text(lines[count].text, sizeof(lines[count].text), text);
            count++;
        }
    }
    fclose(file);

    if (count > 1)
    {
        qsort(lines, count, sizeof(lines[0]), music_lrc_compare);
    }

    ESP_LOGI(TAG, "Loaded %d external LRC line(s) for %s", count, track_path);
    return count;
}

static void music_lrc_update(const music_lyrics_line_t *lines, int line_count, uint32_t elapsed_ms, int *last_index)
{
    if (line_count <= 0)
    {
        if (*last_index != -2)
        {
            music_state_set_lyrics(NULL, NULL, NULL);
            *last_index = -2;
        }
        return;
    }

    int index = -1;
    for (int i = 0; i < line_count; i++)
    {
        if (elapsed_ms < lines[i].time_ms)
        {
            break;
        }
        index = i;
    }

    if (index < 0)
    {
        index = 0;
    }

    if (index == *last_index)
    {
        return;
    }

    const char *prev = index > 0 ? lines[index - 1].text : "";
    const char *current = lines[index].text;
    const char *next = (index + 1 < line_count) ? lines[index + 1].text : "";
    music_state_set_lyrics(prev, current, next);
    *last_index = index;
}

static const char *music_format_name_from_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
    {
        return "Unsupported";
    }

    const char *ext = dot + 1;
    if (strcasecmp(ext, "mp3") == 0)
    {
        return "MP3";
    }
    if (strcasecmp(ext, "aac") == 0)
    {
        return "AAC";
    }
    if (strcasecmp(ext, "wav") == 0)
    {
        return "WAV";
    }
    if (strcasecmp(ext, "flac") == 0)
    {
        return "FLAC";
    }
    if (strcasecmp(ext, "m4a") == 0)
    {
        return "M4A";
    }
    if (strcasecmp(ext, "ts") == 0)
    {
        return "TS";
    }
    if (strcasecmp(ext, "ogg") == 0)
    {
        return "OGG";
    }
    if (strcasecmp(ext, "amrnb") == 0 || strcasecmp(ext, "amr") == 0)
    {
        return "AMR-NB";
    }
    if (strcasecmp(ext, "amrwb") == 0 || strcasecmp(ext, "awb") == 0)
    {
        return "AMR-WB";
    }
    return "Unsupported";
}

static bool music_format_supported_by_simple_player(const char *path)
{
    return strcmp(music_format_name_from_path(path), "Unsupported") != 0;
}

static bool music_build_track_uri(const char *path, char *uri, size_t uri_size)
{
    if (path == NULL || uri == NULL || uri_size == 0 || path[0] != '/')
    {
        return false;
    }
    int written = snprintf(uri, uri_size, "file://%s", path + 1);
    return written > 0 && (size_t)written < uri_size;
}

static int music_get_file_size_bytes(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    int size = 0;
    if (fseek(file, 0, SEEK_END) == 0)
    {
        long pos = ftell(file);
        if (pos > 0 && pos <= INT32_MAX)
        {
            size = (int)pos;
        }
    }
    fclose(file);
    return size;
}

static uint32_t music_id3_synchsafe_to_u32(const uint8_t size[4])
{
    return ((uint32_t)(size[0] & 0x7f) << 21) |
           ((uint32_t)(size[1] & 0x7f) << 14) |
           ((uint32_t)(size[2] & 0x7f) << 7) |
           (uint32_t)(size[3] & 0x7f);
}

static uint32_t music_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t music_read_le32_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t music_read_be64(const uint8_t *p)
{
    return ((uint64_t)music_read_be32(p) << 32) | (uint64_t)music_read_be32(p + 4);
}

static uint64_t music_read_le64(const uint8_t *p)
{
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static bool music_mp3_frame_info_get(const uint8_t h[4], music_mp3_frame_info_t *info)
{
    if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0)
    {
        return false;
    }

    uint8_t version = (h[1] >> 3) & 0x03;
    uint8_t layer = (h[1] >> 1) & 0x03;
    uint8_t bitrate_index = (h[2] >> 4) & 0x0f;
    uint8_t sample_index = (h[2] >> 2) & 0x03;
    if (version == 0x01 ||
        layer == 0x00 ||
        bitrate_index == 0x00 ||
        bitrate_index == 0x0f ||
        sample_index == 0x03)
    {
        return false;
    }

    static const uint16_t bitrate_mpeg1[3][16] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    };
    static const uint16_t bitrate_lsf[3][16] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    };

    int layer_index = 3 - layer;
    uint32_t kbps = version == 0x03 ? bitrate_mpeg1[layer_index][bitrate_index] : bitrate_lsf[layer_index][bitrate_index];
    if (kbps == 0)
    {
        return false;
    }

    static const uint16_t sample_rate_table[4][4] = {
        {11025, 12000, 8000, 0},
        {0, 0, 0, 0},
        {22050, 24000, 16000, 0},
        {44100, 48000, 32000, 0},
    };

    int sample_rate = sample_rate_table[version][sample_index];
    if (sample_rate <= 0)
    {
        return false;
    }

    int bitrate_bps = (int)kbps * 1000;
    int padding = (h[2] >> 1) & 0x01;
    int samples_per_frame = 1152;
    int frame_len = 0;
    if (layer == 0x03)
    {
        samples_per_frame = 384;
        frame_len = ((12 * bitrate_bps / sample_rate) + padding) * 4;
    }
    else if (layer == 0x01 && version != 0x03)
    {
        samples_per_frame = 576;
        frame_len = (72 * bitrate_bps / sample_rate) + padding;
    }
    else
    {
        samples_per_frame = 1152;
        frame_len = (144 * bitrate_bps / sample_rate) + padding;
    }
    if (frame_len <= 4)
    {
        return false;
    }

    if (info)
    {
        *info = (music_mp3_frame_info_t){
            .bitrate_bps = bitrate_bps,
            .sample_rate = sample_rate,
            .samples_per_frame = samples_per_frame,
            .frame_len = frame_len,
            .mpeg1 = version == 0x03,
            .mono = ((h[3] >> 6) & 0x03) == 0x03,
        };
    }
    return true;
}

static int music_estimate_mp3_total_sec(const char *path, int file_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    long start_pos = 0;
    uint8_t header[10] = {0};
    if (fread(header, 1, sizeof(header), file) == sizeof(header) &&
        memcmp(header, "ID3", 3) == 0)
    {
        start_pos = 10 + (long)music_id3_synchsafe_to_u32(&header[6]) + ((header[5] & 0x10) ? 10 : 0);
    }

    music_mp3_frame_info_t frame_info = {0};
    uint8_t h[4] = {0};
    if (fseek(file, start_pos, SEEK_SET) == 0)
    {
        for (int i = 0; i < 256 * 1024; i++)
        {
            long pos = ftell(file);
            if (fread(h, 1, sizeof(h), file) != sizeof(h))
            {
                break;
            }
            if (music_mp3_frame_info_get(h, &frame_info))
            {
                start_pos = pos;
                break;
            }
            fseek(file, pos + 1, SEEK_SET);
        }
    }

    int total_sec = 0;
    if (frame_info.sample_rate > 0)
    {
        uint8_t probe[64] = {0};
        int side_info_size = frame_info.mpeg1 ? (frame_info.mono ? 17 : 32) : (frame_info.mono ? 9 : 17);
        long xing_pos = start_pos + 4 + side_info_size;
        if (fseek(file, xing_pos, SEEK_SET) == 0 &&
            fread(probe, 1, sizeof(probe), file) >= 16 &&
            (memcmp(probe, "Xing", 4) == 0 || memcmp(probe, "Info", 4) == 0))
        {
            uint32_t flags = music_read_be32(&probe[4]);
            if (flags & 0x01)
            {
                uint32_t frames = music_read_be32(&probe[8]);
                if (frames > 0)
                {
                    total_sec = (int)(((uint64_t)frames * (uint64_t)frame_info.samples_per_frame) /
                                      (uint64_t)frame_info.sample_rate);
                }
            }
        }

        long vbri_pos = start_pos + 4 + 32;
        if (total_sec <= 0 &&
            fseek(file, vbri_pos, SEEK_SET) == 0 &&
            fread(probe, 1, 32, file) >= 18 &&
            memcmp(probe, "VBRI", 4) == 0)
        {
            uint32_t frames = music_read_be32(&probe[14]);
            if (frames > 0)
            {
                total_sec = (int)(((uint64_t)frames * (uint64_t)frame_info.samples_per_frame) /
                                  (uint64_t)frame_info.sample_rate);
            }
        }
    }

    if (total_sec > 0)
    {
        fclose(file);
        return total_sec;
    }
    if (frame_info.sample_rate > 0 && fseek(file, start_pos, SEEK_SET) == 0)
    {
        uint64_t total_samples = 0;
        int valid_frames = 0;
        int lost_sync = 0;
        while (!feof(file))
        {
            long pos = ftell(file);
            if (pos < 0)
            {
                break;
            }

            if (file_size > 0 && pos + 4 > file_size)
            {
                break;
            }

            uint8_t h[4] = {0};
            if (fread(h, 1, sizeof(h), file) != sizeof(h))
            {
                break;
            }

            music_mp3_frame_info_t info = {0};
            if (music_mp3_frame_info_get(h, &info))
            {
                total_samples += (uint64_t)info.samples_per_frame;
                valid_frames++;
                lost_sync = 0;
                if (fseek(file, pos + info.frame_len, SEEK_SET) != 0)
                {
                    break;
                }
            }
            else
            {
                lost_sync++;
                if (valid_frames > 0 && lost_sync > 4096)
                {
                    break;
                }
                if (fseek(file, pos + 1, SEEK_SET) != 0)
                {
                    break;
                }
            }
        }
        if (valid_frames > 0 && total_samples > 0)
        {
            fclose(file);
            return (int)((total_samples + (uint64_t)frame_info.sample_rate / 2ULL) /
                         (uint64_t)frame_info.sample_rate);
        }
    }
    fclose(file);

    if (frame_info.bitrate_bps <= 0 || file_size <= start_pos)
    {
        return 0;
    }
    int audio_size = file_size - (int)start_pos;
    if (audio_size > 128)
    {
        uint8_t id3v1[3] = {0};
        FILE *tail = fopen(path, "rb");
        if (tail)
        {
            if (fseek(tail, -128, SEEK_END) == 0 &&
                fread(id3v1, 1, sizeof(id3v1), tail) == sizeof(id3v1) &&
                memcmp(id3v1, "TAG", 3) == 0)
            {
                audio_size -= 128;
            }
            fclose(tail);
        }
    }
    return (int)(((uint64_t)audio_size * 8ULL) / (uint64_t)frame_info.bitrate_bps);
}

static int music_read_le32(const uint8_t *p)
{
    return (int)((uint32_t)p[0] |
                 ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[3] << 24));
}

static int music_estimate_wav_total_sec(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    uint8_t h[12] = {0};
    if (fread(h, 1, sizeof(h), file) != sizeof(h) ||
        memcmp(h, "RIFF", 4) != 0 ||
        memcmp(h + 8, "WAVE", 4) != 0)
    {
        fclose(file);
        return 0;
    }

    int byte_rate = 0;
    int data_size = 0;
    while (!feof(file))
    {
        uint8_t chunk[8] = {0};
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk))
        {
            break;
        }
        int chunk_size = music_read_le32(&chunk[4]);
        long next_pos = ftell(file) + chunk_size + (chunk_size & 1);
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16)
        {
            uint8_t fmt[16] = {0};
            if (fread(fmt, 1, sizeof(fmt), file) == sizeof(fmt))
            {
                byte_rate = music_read_le32(&fmt[8]);
            }
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            data_size = chunk_size;
        }
        if (byte_rate > 0 && data_size > 0)
        {
            break;
        }
        fseek(file, next_pos, SEEK_SET);
    }
    fclose(file);

    return byte_rate > 0 && data_size > 0 ? data_size / byte_rate : 0;
}

static int music_estimate_aac_adts_total_sec(const char *path, int file_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    long start_pos = 0;
    uint8_t id3[10] = {0};
    if (fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
        memcmp(id3, "ID3", 3) == 0)
    {
        start_pos = 10 + (long)music_id3_synchsafe_to_u32(&id3[6]) + ((id3[5] & 0x10) ? 10 : 0);
    }

    static const uint32_t sample_rate_table[16] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0,
    };

    uint64_t total_samples = 0;
    int sample_rate = 0;
    long scan_start = start_pos;
    bool found_frame = false;
    if (fseek(file, start_pos, SEEK_SET) == 0)
    {
        while (!feof(file))
        {
            long pos = ftell(file);
            if (pos < 0 || (file_size > 0 && pos + 7 > file_size))
            {
                break;
            }

            uint8_t h[7] = {0};
            if (fread(h, 1, sizeof(h), file) != sizeof(h))
            {
                break;
            }
            if (h[0] != 0xff || (h[1] & 0xf0) != 0xf0)
            {
                if (found_frame || pos - scan_start > 4096)
                {
                    break;
                }
                fseek(file, pos + 1, SEEK_SET);
                continue;
            }

            int sr_index = (h[2] >> 2) & 0x0f;
            int frame_len = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] & 0xe0) >> 5);
            if (sample_rate_table[sr_index] == 0 || frame_len < 7)
            {
                fseek(file, pos + 1, SEEK_SET);
                continue;
            }
            if (sample_rate == 0)
            {
                sample_rate = sample_rate_table[sr_index];
            }
            total_samples += (uint64_t)(h[6] & 0x03) + 1ULL;
            found_frame = true;
            fseek(file, pos + frame_len, SEEK_SET);
        }
    }
    fclose(file);

    if (sample_rate <= 0 || total_samples == 0)
    {
        return 0;
    }

    return (int)(((total_samples * 1024ULL) + (uint64_t)sample_rate / 2ULL) / (uint64_t)sample_rate);
}

static bool music_aac_adts_header_present(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return false;
    }

    long start_pos = 0;
    uint8_t id3[10] = {0};
    if (fread(id3, 1, sizeof(id3), file) == sizeof(id3) &&
        memcmp(id3, "ID3", 3) == 0)
    {
        start_pos = 10 + (long)music_id3_synchsafe_to_u32(&id3[6]) + ((id3[5] & 0x10) ? 10 : 0);
    }

    bool found = false;
    if (fseek(file, start_pos, SEEK_SET) == 0)
    {
        for (int i = 0; i < 4096; i++)
        {
            long pos = ftell(file);
            uint8_t h[7] = {0};
            if (pos < 0 || fread(h, 1, sizeof(h), file) != sizeof(h))
            {
                break;
            }

            int sr_index = (h[2] >> 2) & 0x0f;
            int frame_len = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] & 0xe0) >> 5);
            if (h[0] == 0xff &&
                (h[1] & 0xf0) == 0xf0 &&
                sr_index < 13 &&
                frame_len >= 7)
            {
                found = true;
                break;
            }
            fseek(file, pos + 1, SEEK_SET);
        }
    }
    fclose(file);
    return found;
}

static int music_mp4_scan_duration_sec(FILE *file, long start, long end, int depth)
{
    if (depth > 4 || start < 0 || end <= start)
    {
        return 0;
    }

    long pos = start;
    while (pos + 8 <= end)
    {
        uint8_t box[16] = {0};
        if (fseek(file, pos, SEEK_SET) != 0 || fread(box, 1, 8, file) != 8)
        {
            break;
        }

        uint64_t box_size = music_read_be32(box);
        int header_size = 8;
        if (box_size == 1)
        {
            if (fread(box + 8, 1, 8, file) != 8)
            {
                break;
            }
            box_size = music_read_be64(box + 8);
            header_size = 16;
        }
        else if (box_size == 0)
        {
            box_size = (uint64_t)(end - pos);
        }

        if (box_size < (uint64_t)header_size || box_size > (uint64_t)(end - pos))
        {
            break;
        }

        if (memcmp(box + 4, "mvhd", 4) == 0 || memcmp(box + 4, "mdhd", 4) == 0)
        {
            uint8_t version = 0;
            if (fseek(file, pos + header_size, SEEK_SET) != 0 ||
                fread(&version, 1, 1, file) != 1)
            {
                return 0;
            }

            uint8_t data[32] = {0};
            if (version == 1)
            {
                if (fseek(file, pos + header_size + 4, SEEK_SET) == 0 &&
                    fread(data, 1, 28, file) == 28)
                {
                    uint32_t timescale = music_read_be32(data + 16);
                    uint64_t duration = music_read_be64(data + 20);
                    return timescale > 0 ? (int)(duration / timescale) : 0;
                }
            }
            else
            {
                if (fseek(file, pos + header_size + 4, SEEK_SET) == 0 &&
                    fread(data, 1, 16, file) == 16)
                {
                    uint32_t timescale = music_read_be32(data + 8);
                    uint32_t duration = music_read_be32(data + 12);
                    return timescale > 0 ? (int)(duration / timescale) : 0;
                }
            }
            return 0;
        }

        if (memcmp(box + 4, "moov", 4) == 0 ||
            memcmp(box + 4, "trak", 4) == 0 ||
            memcmp(box + 4, "mdia", 4) == 0 ||
            memcmp(box + 4, "minf", 4) == 0 ||
            memcmp(box + 4, "stbl", 4) == 0)
        {
            int sec = music_mp4_scan_duration_sec(file,
                                                  pos + header_size,
                                                  pos + (long)box_size,
                                                  depth + 1);
            if (sec > 0)
            {
                return sec;
            }
        }

        pos += (long)box_size;
    }

    return 0;
}

static int music_estimate_mp4_total_sec(const char *path, int file_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }
    int sec = music_mp4_scan_duration_sec(file, 0, file_size, 0);
    fclose(file);
    return sec;
}

static int music_flac_total_sec(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    uint8_t marker[4] = {0};
    if (fread(marker, 1, sizeof(marker), file) != sizeof(marker) ||
        memcmp(marker, "fLaC", 4) != 0)
    {
        fclose(file);
        return 0;
    }

    bool last_block = false;
    while (!last_block)
    {
        uint8_t header[4] = {0};
        if (fread(header, 1, sizeof(header), file) != sizeof(header))
        {
            break;
        }

        last_block = (header[0] & 0x80) != 0;
        uint8_t block_type = header[0] & 0x7f;
        uint32_t block_len = ((uint32_t)header[1] << 16) |
                             ((uint32_t)header[2] << 8) |
                             (uint32_t)header[3];

        if (block_type == 0 && block_len >= 34)
        {
            uint8_t stream_info[34] = {0};
            if (fread(stream_info, 1, sizeof(stream_info), file) != sizeof(stream_info))
            {
                break;
            }

            uint32_t sample_rate = ((uint32_t)stream_info[10] << 12) |
                                   ((uint32_t)stream_info[11] << 4) |
                                   ((uint32_t)stream_info[12] >> 4);
            uint64_t total_samples = ((uint64_t)(stream_info[13] & 0x0f) << 32) |
                                     ((uint64_t)stream_info[14] << 24) |
                                     ((uint64_t)stream_info[15] << 16) |
                                     ((uint64_t)stream_info[16] << 8) |
                                     (uint64_t)stream_info[17];
            fclose(file);
            return sample_rate > 0 && total_samples > 0 ?
                       (int)((total_samples + sample_rate / 2ULL) / sample_rate) :
                       0;
        }

        if (fseek(file, block_len, SEEK_CUR) != 0)
        {
            break;
        }
    }

    fclose(file);
    return 0;
}

static int music_ogg_sample_rate_get(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    uint8_t buf[512] = {0};
    size_t scanned = 0;
    int sample_rate = 0;
    while (scanned < 64 * 1024)
    {
        size_t got = fread(buf, 1, sizeof(buf), file);
        if (got == 0)
        {
            break;
        }
        for (size_t i = 0; i + 16 <= got; i++)
        {
            if (memcmp(buf + i, "\x01vorbis", 7) == 0 && i + 16 <= got)
            {
                sample_rate = (int)music_read_le32_u32(buf + i + 12);
                break;
            }
            if (memcmp(buf + i, "OpusHead", 8) == 0)
            {
                sample_rate = 48000;
                break;
            }
        }
        if (sample_rate > 0)
        {
            break;
        }
        scanned += got;
    }
    fclose(file);
    return sample_rate;
}

static int music_ogg_total_sec(const char *path, int file_size)
{
    int sample_rate = music_ogg_sample_rate_get(path);
    if (sample_rate <= 0 || file_size <= 27)
    {
        return 0;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    const long search_len = file_size > 64 * 1024 ? 64 * 1024 : file_size;
    long start = file_size - search_len;
    if (fseek(file, start, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }

    uint8_t *buf = heap_caps_malloc(search_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        buf = heap_caps_malloc(search_len, MALLOC_CAP_DEFAULT);
    }
    if (buf == NULL)
    {
        fclose(file);
        return 0;
    }

    size_t got = fread(buf, 1, search_len, file);
    fclose(file);

    int sec = 0;
    for (long i = (long)got - 27; i >= 0; i--)
    {
        if (memcmp(buf + i, "OggS", 4) == 0)
        {
            uint64_t granule = music_read_le64(buf + i + 6);
            if (granule != UINT64_MAX && granule > 0)
            {
                sec = (int)((granule + (uint64_t)sample_rate / 2ULL) / (uint64_t)sample_rate);
            }
            break;
        }
    }
    heap_caps_free(buf);
    return sec;
}

static int music_get_duration_sec(const char *path, const char *format, int file_size)
{
    if (file_size <= 0)
    {
        return 0;
    }
    if (strcmp(format, "MP3") == 0)
    {
        return music_estimate_mp3_total_sec(path, file_size);
    }
    if (strcmp(format, "WAV") == 0)
    {
        return music_estimate_wav_total_sec(path);
    }
    if (strcmp(format, "AAC") == 0)
    {
        return music_estimate_aac_adts_total_sec(path, file_size);
    }
    if (strcmp(format, "M4A") == 0)
    {
        return music_estimate_mp4_total_sec(path, file_size);
    }
    if (strcmp(format, "FLAC") == 0)
    {
        return music_flac_total_sec(path);
    }
    if (strcmp(format, "OGG") == 0)
    {
        return music_ogg_total_sec(path, file_size);
    }
    return 0;
}

static void music_player_apply_volume(int volume)
{
    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 100)
    {
        volume = 100;
    }

    s_music_volume = volume;
    music_state_set_volume(volume);
    if (s_audio_codec)
    {
        if (s_audio_playback_mutex)
        {
            xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
        }
        t_panel_audio_codec_set_volume(s_audio_codec, volume);
        if (s_audio_playback_mutex)
        {
            xSemaphoreGive(s_audio_playback_mutex);
        }
    }
}

static uint16_t music_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

typedef struct
{
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    long data_offset;
    uint32_t data_size;
} music_wav_info_t;

static bool music_wav_info_read(FILE *file, music_wav_info_t *info)
{
    uint8_t riff[12] = {0};
    bool has_fmt = false;
    bool has_data = false;

    if (file == NULL || info == NULL ||
        fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
    {
        return false;
    }

    memset(info, 0, sizeof(*info));
    while (!feof(file))
    {
        uint8_t chunk[8] = {0};
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk))
        {
            break;
        }

        uint32_t chunk_size = (uint32_t)music_read_le32(chunk + 4);
        long payload_offset = ftell(file);
        if (payload_offset < 0)
        {
            break;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16)
        {
            uint8_t fmt[16] = {0};
            if (fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt))
            {
                break;
            }
            if (music_read_le16(fmt) != 1)
            {
                return false;
            }
            info->channels = music_read_le16(fmt + 2);
            info->sample_rate = (uint32_t)music_read_le32(fmt + 4);
            info->byte_rate = (uint32_t)music_read_le32(fmt + 8);
            info->bits_per_sample = music_read_le16(fmt + 14);
            has_fmt = true;
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            info->data_offset = payload_offset;
            info->data_size = chunk_size;
            has_data = true;
        }

        long next_offset = payload_offset + (long)chunk_size + (chunk_size & 1U);
        if (fseek(file, next_offset, SEEK_SET) != 0)
        {
            break;
        }
        if (has_fmt && has_data)
        {
            return info->sample_rate > 0 && info->byte_rate > 0 &&
                   (info->channels == 1 || info->channels == 2) &&
                   info->bits_per_sample == 16;
        }
    }
    return false;
}

static esp_codec_dev_sample_info_t music_default_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    return fs;
}

static esp_err_t music_player_open_output(uint32_t sample_rate)
{
    if (s_play_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (sample_rate == SAMPLE_RATE && s_music_output_open)
    {
        music_player_apply_volume(s_music_volume);
        return ESP_OK;
    }

    s_music_audio_active = true;
    if (s_audio_capture_mutex)
    {
        xSemaphoreTake(s_audio_capture_mutex, portMAX_DELAY);
    }
    if (s_audio_playback_mutex)
    {
        xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
    }
    if (s_rec_dev && s_rec_dev != s_play_dev)
    {
        esp_codec_dev_close(s_rec_dev);
    }
    if (s_play_dev && s_music_output_open)
    {
        esp_codec_dev_close(s_play_dev);
        s_music_output_open = false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = MUSIC_PLAYER_OUTPUT_BITS,
        .channel = MUSIC_PLAYER_OUTPUT_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        (MUSIC_PLAYER_OUTPUT_CHANNELS > 1 ? ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) : 0),
        .sample_rate = sample_rate,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    esp_err_t ret = esp_codec_dev_open(s_play_dev, &fs);
    if (ret == ESP_OK)
    {
        s_music_output_open = true;
        esp_codec_dev_set_out_vol(s_play_dev, s_music_volume);
    }
    else
    {
        esp_codec_dev_sample_info_t default_fs = music_default_sample_info();
        if (s_play_dev && esp_codec_dev_open(s_play_dev, &default_fs) == ESP_OK)
        {
            s_music_output_open = true;
        }
        if (s_rec_dev && s_rec_dev != s_play_dev)
        {
            esp_codec_dev_open(s_rec_dev, &default_fs);
        }
        s_music_audio_active = false;
    }

    if (s_audio_playback_mutex)
    {
        xSemaphoreGive(s_audio_playback_mutex);
    }
    if (s_audio_capture_mutex)
    {
        xSemaphoreGive(s_audio_capture_mutex);
    }
    return ret;
}

static void music_player_restore_audio_devices(void)
{
    if (!s_music_audio_active)
    {
        return;
    }

    if (s_audio_capture_mutex)
    {
        xSemaphoreTake(s_audio_capture_mutex, portMAX_DELAY);
    }
    if (s_audio_playback_mutex)
    {
        xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
    }

    if (s_play_dev && s_music_output_open)
    {
        esp_codec_dev_close(s_play_dev);
        s_music_output_open = false;
    }

    esp_codec_dev_sample_info_t fs = music_default_sample_info();
    if (s_play_dev && esp_codec_dev_open(s_play_dev, &fs) == ESP_OK)
    {
        s_music_output_open = true;
        esp_codec_dev_set_out_vol(s_play_dev, s_music_volume);
    }
    if (s_rec_dev && s_rec_dev != s_play_dev)
    {
        esp_err_t rec_ret = esp_codec_dev_open(s_rec_dev, &fs);
        if (rec_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Restore recorder failed: %s", esp_err_to_name(rec_ret));
        }
    }

    s_music_audio_active = false;
    if (s_audio_playback_mutex)
    {
        xSemaphoreGive(s_audio_playback_mutex);
    }
    if (s_audio_capture_mutex)
    {
        xSemaphoreGive(s_audio_capture_mutex);
    }
}

static bool music_player_send_cmd(music_player_cmd_type_t type, int value)
{
    if (s_music_player_queue == NULL)
    {
        ESP_LOGW(TAG, "Music command queue is not ready: type=%d value=%d", type, value);
        return false;
    }

    music_player_cmd_t cmd = {
        .type = type,
        .value = value,
    };
    bool ok = xQueueSend(s_music_player_queue, &cmd, 0) == pdTRUE;
    ESP_LOGI(TAG, "Music command %s: type=%d value=%d", ok ? "sent" : "failed", type, value);
    return ok;
}

static void music_player_set_track_state(int index, const esp_asp_music_info_t *info, int total_sec)
{
    char name[AUDIO_MUSIC_TRACK_NAME_MAX_LEN] = {0};
    if (audio_music_track_name_get(index, name, sizeof(name)))
    {
        ESP_LOGI(TAG, "Playing track: %s", name);
    }

    char sample[16] = {0};
    char bitrate[16] = {0};
    const char *channels = info->channels == 1 ? "Mono" : (info->channels == 2 ? "Stereo" : "--");
    if (info->sample_rate > 0)
    {
        snprintf(sample, sizeof(sample), "%dHz", info->sample_rate);
    }
    else
    {
        snprintf(sample, sizeof(sample), "--");
    }
    if (info->bitrate > 0)
    {
        snprintf(bitrate, sizeof(bitrate), "%dkbps", info->bitrate / 1000);
    }
    else
    {
        snprintf(bitrate, sizeof(bitrate), "--");
    }
    music_state_set_track_params(s_music_current_format, sample, bitrate, channels);
    music_state_set_progress(0, total_sec);
    music_state_set_playing(true);
}

static int32_t music_pcm_sample_read(const uint8_t *data, int sample_index)
{
#if MUSIC_PLAYER_OUTPUT_BITS == 16
    const uint8_t *p = data + (sample_index * 2);
    return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
#elif MUSIC_PLAYER_OUTPUT_BITS == 24
    const uint8_t *p = data + (sample_index * 3);
    int32_t sample = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (sample & 0x00800000)
    {
        sample |= 0xff000000;
    }
    return sample >> 8;
#elif MUSIC_PLAYER_OUTPUT_BITS == 32
    const uint8_t *p = data + (sample_index * 4);
    int32_t sample = ((int32_t)p[0]) |
                     ((int32_t)p[1] << 8) |
                     ((int32_t)p[2] << 16) |
                     ((int32_t)p[3] << 24);
    return sample >> 16;
#else
    (void)data;
    (void)sample_index;
    return 0;
#endif
}

static void music_player_update_spectrum_from_pcm(const uint8_t *data, int data_size)
{
    int64_t now_us = esp_timer_get_time();
    if (s_music_spectrum_last_us > 0 &&
        now_us - s_music_spectrum_last_us < (int64_t)MUSIC_SPECTRUM_OUTPUT_UPDATE_MS * 1000)
    {
        return;
    }
    s_music_spectrum_last_us = now_us;

    const int bytes_per_sample = MUSIC_PLAYER_OUTPUT_BITS / 8;
    const int channels = MUSIC_PLAYER_OUTPUT_CHANNELS > 0 ? MUSIC_PLAYER_OUTPUT_CHANNELS : 1;
    if (bytes_per_sample <= 0 || data_size < bytes_per_sample * channels)
    {
        return;
    }

    int frame_count = data_size / (bytes_per_sample * channels);
    if (frame_count <= 0)
    {
        return;
    }

    uint64_t sum[MUSIC_SPECTRUM_BAR_COUNT] = {0};
    uint16_t count[MUSIC_SPECTRUM_BAR_COUNT] = {0};
    for (int frame = 0; frame < frame_count; frame++)
    {
        int64_t mixed = 0;
        for (int ch = 0; ch < channels; ch++)
        {
            int32_t sample = music_pcm_sample_read(data, frame * channels + ch);
            mixed += sample < 0 ? -(int64_t)sample : sample;
        }

        uint32_t amp = (uint32_t)(mixed / channels);
        int bar = (frame * MUSIC_SPECTRUM_BAR_COUNT) / frame_count;
        if (bar >= MUSIC_SPECTRUM_BAR_COUNT)
        {
            bar = MUSIC_SPECTRUM_BAR_COUNT - 1;
        }
        sum[bar] += amp;
        count[bar]++;
    }

    uint32_t peak = s_music_spectrum_peak;
    if (peak < 2000)
    {
        peak = 2000;
    }

    uint8_t levels[MUSIC_SPECTRUM_BAR_COUNT] = {0};
    uint32_t frame_peak = 0;
    for (int bar = 0; bar < MUSIC_SPECTRUM_BAR_COUNT; bar++)
    {
        uint32_t avg = count[bar] ? (uint32_t)(sum[bar] / count[bar]) : 0;
        if (avg > frame_peak)
        {
            frame_peak = avg;
        }

        uint32_t scaled = (avg * MUSIC_SPECTRUM_SEGMENT_COUNT) / peak;
        if (scaled < 1)
        {
            scaled = 1;
        }
        else if (scaled > MUSIC_SPECTRUM_SEGMENT_COUNT)
        {
            scaled = MUSIC_SPECTRUM_SEGMENT_COUNT;
        }
        levels[bar] = (uint8_t)scaled;
    }

    if (frame_peak > peak)
    {
        peak = frame_peak;
    }
    else
    {
        peak = ((peak * 31) + frame_peak) / 32;
    }
    s_music_spectrum_peak = peak < 2000 ? 2000 : peak;

    music_state_set_spectrum(levels, MUSIC_SPECTRUM_BAR_COUNT);
}

static int music_player_input_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (data == NULL || data_size <= 0 || s_embedded_audio_data == NULL ||
        s_embedded_audio_offset >= s_embedded_audio_size)
    {
        return 0;
    }

    size_t remaining = s_embedded_audio_size - s_embedded_audio_offset;
    size_t copy_size = remaining < (size_t)data_size ? remaining : (size_t)data_size;
    memcpy(data, s_embedded_audio_data + s_embedded_audio_offset, copy_size);
    s_embedded_audio_offset += copy_size;
    return (int)copy_size;
}

static int music_player_output_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (data == NULL || data_size <= 0)
    {
        return 0;
    }
    if (!s_record_file_playing && !s_self_test_audio_playing)
    {
        music_player_update_spectrum_from_pcm(data, data_size);
    }
    if (s_audio_playback_mutex)
    {
        xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
    }
    int ret = ESP_FAIL;
    if (s_audio_codec && s_play_dev && s_music_output_open)
    {
        ret = t_panel_audio_codec_write(s_audio_codec, data,
                                        (size_t)data_size);
        if (ret == ESP_OK)
        {
            s_music_output_bytes += data_size;
        }
    }
    if (s_audio_playback_mutex)
    {
        xSemaphoreGive(s_audio_playback_mutex);
    }
    if (ret != ESP_OK)
    {
        s_music_playback_error = true;
    }
    return ret == ESP_OK ? 0 : ESP_FAIL;
}

static int music_player_event_cb(esp_asp_event_pkt_t *event, void *ctx)
{
    (void)ctx;
    if (event == NULL)
    {
        return 0;
    }

    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && event->payload != NULL)
    {
        esp_asp_music_info_t info = {0};
        memcpy(&info, event->payload, event->payload_size > (int)sizeof(info) ? sizeof(info) : event->payload_size);
        if (s_music_total_sec <= 0 && s_music_file_size_bytes > 0 && info.bitrate > 0)
        {
            s_music_total_sec = (int)(((uint64_t)s_music_file_size_bytes * 8ULL) / (uint64_t)info.bitrate);
        }
        if (!s_record_file_playing && !s_self_test_audio_playing)
        {
            music_player_set_track_state(s_music_selected_index, &info, s_music_total_sec);
        }
        ESP_LOGI(TAG, "Music info: rate=%d channels=%d bits=%d bitrate=%d",
                 info.sample_rate,
                 info.channels,
                 info.bits,
                 info.bitrate);
    }
    else if (event->type == ESP_ASP_EVENT_TYPE_STATE && event->payload != NULL)
    {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        memcpy(&state, event->payload, event->payload_size > (int)sizeof(state) ? sizeof(state) : event->payload_size);
        ESP_LOGI(TAG, "Music player state: %s", esp_audio_simple_player_state_to_str(state));
        if (state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_STOPPED)
        {
            s_music_playback_done = true;
        }
        else if (state == ESP_ASP_STATE_ERROR)
        {
            s_music_playback_error = true;
            s_music_playback_done = true;
        }
    }
    return 0;
}

static uint32_t music_player_elapsed_ms_get(void)
{
    int64_t started_us = s_music_started_us;
    if (started_us > 0)
    {
        int64_t now_us = esp_timer_get_time();
        int64_t paused_us = s_music_paused_acc_us;
        if (s_music_pause_started_us > 0)
        {
            paused_us += now_us - s_music_pause_started_us;
        }
        int64_t elapsed_us = now_us - started_us - paused_us;
        return elapsed_us > 0 ? (uint32_t)(elapsed_us / 1000) : 0;
    }

    uint32_t bytes_per_sec = MUSIC_PLAYER_OUTPUT_SAMPLE_RATE *
                             MUSIC_PLAYER_OUTPUT_CHANNELS *
                             (MUSIC_PLAYER_OUTPUT_BITS / 8);
    if (bytes_per_sec == 0)
    {
        return 0;
    }
    uint64_t bytes = 0;
    if (s_audio_playback_mutex)
    {
        xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
    }
    bytes = s_music_output_bytes;
    if (s_audio_playback_mutex)
    {
        xSemaphoreGive(s_audio_playback_mutex);
    }
    return (uint32_t)((bytes * 1000ULL) / bytes_per_sec);
}

static void music_player_clock_start(void)
{
    s_music_started_us = esp_timer_get_time();
    s_music_pause_started_us = 0;
    s_music_paused_acc_us = 0;
}

static void music_player_clock_pause(void)
{
    if (s_music_pause_started_us == 0)
    {
        s_music_pause_started_us = esp_timer_get_time();
    }
}

static void music_player_clock_resume(void)
{
    if (s_music_pause_started_us > 0)
    {
        s_music_paused_acc_us += esp_timer_get_time() - s_music_pause_started_us;
        s_music_pause_started_us = 0;
    }
}

static void music_player_wait_done(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    while (!s_music_playback_done && waited_ms < timeout_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
}

static int music_player_random_index(int current_index)
{
    int count = audio_music_track_count_get();
    if (count <= 0)
    {
        return -1;
    }
    if (count == 1)
    {
        return 0;
    }

    int next = current_index;
    for (int i = 0; i < 4 && next == current_index; i++)
    {
        next = (int)(esp_random() % (uint32_t)count);
    }
    if (next == current_index)
    {
        next = (current_index + 1) % count;
    }
    return next;
}

static int music_player_adjacent_index(int current_index, int step)
{
    int count = audio_music_track_count_get();
    if (count <= 0)
    {
        return -1;
    }
    if (current_index < 0 || current_index >= count)
    {
        current_index = 0;
    }
    return (current_index + step + count) % count;
}

static int music_player_sequential_next_index(int current_index)
{
    int count = audio_music_track_count_get();
    if (count <= 0)
    {
        return -1;
    }
    if (current_index < 0)
    {
        return 0;
    }
    if (current_index + 1 >= count)
    {
        return -1;
    }
    return current_index + 1;
}

static int music_player_next_index_after_finish(int current_index)
{
    switch (s_music_play_mode)
    {
    case 1:
        return music_player_random_index(current_index);
    case 2:
        return current_index;
    case 0:
    default:
        return music_player_sequential_next_index(current_index);
    }
}

static bool music_player_handle_running_cmd(const music_player_cmd_t *cmd,
                                            bool *stop,
                                            int *next_index,
                                            bool *paused)
{
    switch (cmd->type)
    {
    case MUSIC_CMD_PLAY_CURRENT:
        if (*paused)
        {
            if (music_player_open_output(MUSIC_PLAYER_OUTPUT_SAMPLE_RATE) != ESP_OK)
            {
                s_music_playback_error = true;
                music_state_set_playing(false);
                return true;
            }
            if (esp_audio_simple_player_resume(s_music_player_handle) == ESP_GMF_ERR_OK)
            {
                music_player_clock_resume();
                *paused = false;
                music_state_set_playing(true);
            }
            else
            {
                music_player_restore_audio_devices();
                music_state_set_playing(false);
            }
        }
        return true;
    case MUSIC_CMD_PLAY_INDEX:
        *next_index = cmd->value;
        *stop = true;
        esp_audio_simple_player_stop(s_music_player_handle);
        return false;
    case MUSIC_CMD_PAUSE:
        if (!*paused)
        {
            if (esp_audio_simple_player_pause(s_music_player_handle) == ESP_GMF_ERR_OK)
            {
                music_player_clock_pause();
                *paused = true;
                music_state_set_playing(false);
                music_player_restore_audio_devices();
            }
        }
        return true;
    case MUSIC_CMD_STOP:
        *stop = true;
        esp_audio_simple_player_stop(s_music_player_handle);
        return false;
    case MUSIC_CMD_SET_VOLUME:
        music_player_apply_volume(cmd->value);
        return true;
    case MUSIC_CMD_SET_PLAY_MODE:
        s_music_play_mode = cmd->value;
        if (s_music_play_mode < 0 || s_music_play_mode > 2)
        {
            s_music_play_mode = 0;
        }
        music_state_set_play_mode(s_music_play_mode);
        return true;
    default:
        return true;
    }
}

static int music_player_play_index(int index)
{
    char path[MUSIC_PATH_MAX_LEN] = {0};
    char uri[MUSIC_URI_MAX_LEN] = {0};
    if (!music_build_track_path(index, path, sizeof(path)) ||
        !music_build_track_uri(path, uri, sizeof(uri)))
    {
        music_state_set_lyrics("Open failed", "Invalid track path", "");
        music_state_set_playing(false);
        return -1;
    }

    const char *format = music_format_name_from_path(path);
    if (!music_format_supported_by_simple_player(path))
    {
        music_state_set_track_params("Unsupported", "--", "--", "--");
        music_state_set_progress(0, 0);
        music_state_set_lyrics("Unsupported file", "No decoder for this format", "Use mp3/aac/flac/wav/m4a/ts/ogg/amr");
        music_state_set_playing(false);
        ESP_LOGW(TAG, "Unsupported music type: %s", path);
        return -1;
    }
    if (strcmp(format, "AAC") == 0 && !music_aac_adts_header_present(path))
    {
        music_state_set_track_params("AAC", "No ADTS", "--", "--");
        music_state_set_progress(0, 0);
        music_state_set_lyrics("Unsupported AAC file", "AAC needs ADTS headers", "Use .m4a for MP4 AAC or convert to ADTS AAC");
        music_state_set_playing(false);
        ESP_LOGW(TAG, "Unsupported AAC without ADTS header: %s", path);
        return -1;
    }

    music_lyrics_line_t *lyrics = heap_caps_calloc(MUSIC_LRC_MAX_LINES,
                                                   sizeof(music_lyrics_line_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lyrics == NULL)
    {
        lyrics = heap_caps_calloc(MUSIC_LRC_MAX_LINES, sizeof(music_lyrics_line_t), MALLOC_CAP_DEFAULT);
    }
    int lyric_count = (lyrics != NULL && strcmp(format, "MP3") == 0)
                          ? music_lyrics_load_embedded(path, lyrics, MUSIC_LRC_MAX_LINES)
                          : 0;
    if (lyrics != NULL && lyric_count == 0)
    {
        lyric_count = music_lrc_load(path, lyrics, MUSIC_LRC_MAX_LINES);
    }
    int lyric_index = -3;

    snprintf(s_music_current_format, sizeof(s_music_current_format), "%s", format);
    s_music_selected_index = index;
    music_state_set_current_track(index, strcmp(format, "MP3") == 0 ? path : NULL);
    s_music_file_size_bytes = music_get_file_size_bytes(path);
    s_music_total_sec = music_get_duration_sec(path, format, s_music_file_size_bytes);
    ESP_LOGI(TAG, "Music duration: %s %d sec", format, s_music_total_sec);
    s_music_output_bytes = 0;
    s_music_started_us = 0;
    s_music_pause_started_us = 0;
    s_music_paused_acc_us = 0;
    s_music_spectrum_last_us = 0;
    s_music_spectrum_peak = 12000;
    s_music_playback_done = false;
    s_music_playback_error = false;
    music_state_set_spectrum(NULL, 0);

    music_player_apply_volume(s_music_volume);
    music_state_set_track_params(s_music_current_format, "Parsing", "--", "--");
    music_state_set_progress(0, s_music_total_sec);
    music_state_set_playing(true);
    music_lrc_update(lyrics, lyric_count, 0, &lyric_index);

    if (music_player_open_output(MUSIC_PLAYER_OUTPUT_SAMPLE_RATE) != ESP_OK)
    {
        heap_caps_free(lyrics);
        music_state_set_track_params(s_music_current_format, "I2S open failed", "--", "--");
        music_state_set_lyrics("Playback failed", "Can not open output sample rate", "");
        music_state_set_playing(false);
        return -1;
    }

    esp_gmf_err_t run_ret = esp_audio_simple_player_run(s_music_player_handle, uri, NULL);
    if (run_ret != ESP_GMF_ERR_OK)
    {
        ESP_LOGE(TAG, "Audio simple player run failed: %d", run_ret);
        heap_caps_free(lyrics);
        music_player_restore_audio_devices();
        music_state_set_track_params(s_music_current_format, "Run failed", "--", "--");
        music_state_set_lyrics("Playback failed", "Audio simple player can not run", "");
        music_state_set_playing(false);
        return -1;
    }
    music_player_clock_start();

    bool stop = false;
    bool paused = false;
    int next_index = -1;
    uint32_t last_progress_ms = 0;

    while (!stop && next_index < 0 && !s_music_playback_done && !s_music_playback_error)
    {
        music_player_cmd_t cmd = {0};
        if (xQueueReceive(s_music_player_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            music_player_handle_running_cmd(&cmd, &stop, &next_index, &paused);
        }

        uint32_t elapsed_ms = music_player_elapsed_ms_get();
        if (!paused && elapsed_ms - last_progress_ms >= MUSIC_PROGRESS_UPDATE_MS)
        {
            music_state_set_progress((int)(elapsed_ms / 1000), s_music_total_sec);
            music_lrc_update(lyrics, lyric_count, elapsed_ms, &lyric_index);
            last_progress_ms = elapsed_ms;
        }
    }

    if (!s_music_playback_done)
    {
        esp_audio_simple_player_stop(s_music_player_handle);
        music_player_wait_done(2000);
    }

    heap_caps_free(lyrics);
    music_player_restore_audio_devices();

    if (next_index >= 0)
    {
        return next_index;
    }
    if (s_music_playback_error)
    {
        music_state_set_track_params(s_music_current_format, "Decode failed", "--", "--");
        music_state_set_lyrics("Playback failed", "Audio simple player decode error", "Try another file or re-encode it");
        music_state_set_playing(false);
        return -1;
    }
    if (!stop)
    {
        int next_index = music_player_next_index_after_finish(index);
        if (next_index < 0)
        {
            music_state_set_playing(false);
        }
        return next_index;
    }
    music_state_set_playing(false);
    return -1;
}

static void music_player_play_recording(void)
{
    if (s_record_buffer == NULL || s_record_data_bytes == 0)
    {
        ESP_LOGW(TAG, "No recording is available in PSRAM");
        s_record_file_playing = false;
        return;
    }

    s_music_file_size_bytes = (int)(RECORD_WAV_HEADER_SIZE + s_record_data_bytes);
    s_music_total_sec = (int)(s_record_data_bytes /
                              (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8)));
    s_music_output_bytes = 0;
    s_music_started_us = 0;
    s_music_pause_started_us = 0;
    s_music_paused_acc_us = 0;
    s_music_playback_done = false;
    s_music_playback_error = false;
    s_record_file_playing = true;
    s_record_playback_paused = false;

    if (music_player_open_output(MUSIC_PLAYER_OUTPUT_SAMPLE_RATE) != ESP_OK)
    {
        s_record_file_playing = false;
        s_record_playback_paused = false;
        return;
    }
    music_player_clock_start();

    ESP_LOGI(TAG, "Playing PSRAM recording PCM: %u bytes",
             (unsigned)s_record_data_bytes);

    bool stop = false;
    bool paused = false;
    uint32_t offset = 0;
    while (!stop && offset < s_record_data_bytes && !s_music_playback_error)
    {
        music_player_cmd_t cmd = {0};
        if (xQueueReceive(s_music_player_queue, &cmd, 0) == pdTRUE)
        {
            if (cmd.type == MUSIC_CMD_STOP)
            {
                stop = true;
            }
            else if (cmd.type == MUSIC_CMD_PAUSE)
            {
                if (!paused)
                {
                    paused = true;
                    s_record_playback_paused = true;
                    music_player_clock_pause();
                }
            }
            else if (cmd.type == MUSIC_CMD_PLAY_CURRENT)
            {
                if (paused)
                {
                    paused = false;
                    s_record_playback_paused = false;
                    music_player_clock_resume();
                }
            }
            else if (cmd.type == MUSIC_CMD_SET_VOLUME)
            {
                music_player_apply_volume(cmd.value);
            }
        }
        if (stop)
        {
            break;
        }
        if (paused)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t remaining = s_record_data_bytes - offset;
        uint32_t chunk_size = remaining < 8192U ? remaining : 8192U;
        if (music_player_output_cb(s_record_buffer + RECORD_WAV_HEADER_SIZE + offset,
                                   (int)chunk_size, NULL) != 0)
        {
            break;
        }
        offset += chunk_size;
    }

    music_player_restore_audio_devices();
    s_music_started_us = 0;
    s_record_file_playing = false;
    s_record_playback_paused = false;
}

static void music_player_play_saved_recording(void)
{
    if (s_record_playback_path[0] == '\0')
    {
        s_record_file_playing = false;
        return;
    }

    /* The simple player keeps its pipeline asynchronous; drain the previous
     * instance before registering a new file decoder. */
    esp_audio_simple_player_stop(s_music_player_handle);
    if (s_music_audio_active)
    {
        music_player_wait_done(1000);
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    FILE *file = fopen(s_record_playback_path, "rb");
    music_wav_info_t wav = {0};
    if (file == NULL || !music_wav_info_read(file, &wav) ||
        fseek(file, wav.data_offset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Invalid saved WAV file: %s", s_record_playback_path);
        if (file != NULL)
        {
            fclose(file);
        }
        s_record_file_playing = false;
        s_record_playback_path[0] = '\0';
        return;
    }

    if (wav.channels != MUSIC_PLAYER_OUTPUT_CHANNELS ||
        wav.bits_per_sample != MUSIC_PLAYER_OUTPUT_BITS)
    {
        ESP_LOGE(TAG, "Unsupported saved WAV format: %u channels, %u bits",
                 (unsigned)wav.channels, (unsigned)wav.bits_per_sample);
        fclose(file);
        s_record_file_playing = false;
        s_record_playback_path[0] = '\0';
        return;
    }

    uint8_t *buffer = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        buffer = heap_caps_malloc(8192, MALLOC_CAP_DEFAULT);
    }
    if (buffer == NULL)
    {
        fclose(file);
        s_record_file_playing = false;
        s_record_playback_path[0] = '\0';
        return;
    }

    s_music_file_size_bytes = (int)wav.data_size;
    s_music_total_sec = wav.byte_rate > 0 ? (int)(wav.data_size / wav.byte_rate) : 0;
    s_music_output_bytes = 0;
    s_music_started_us = 0;
    s_music_pause_started_us = 0;
    s_music_paused_acc_us = 0;
    s_music_playback_done = false;
    s_music_playback_error = false;
    s_record_file_playing = true;
    s_record_playback_paused = false;

    if (music_player_open_output(wav.sample_rate) != ESP_OK)
    {
        free(buffer);
        fclose(file);
        s_record_file_playing = false;
        s_record_playback_path[0] = '\0';
        return;
    }
    music_player_clock_start();

    ESP_LOGI(TAG, "Playing saved WAV PCM: %s (%u Hz, %u bytes)",
             s_record_playback_path, (unsigned)wav.sample_rate,
             (unsigned)wav.data_size);

    bool stop = false;
    bool paused = false;
    uint32_t remaining = wav.data_size;
    while (!stop && remaining > 0 && !s_music_playback_error)
    {
        music_player_cmd_t cmd = {0};
        if (xQueueReceive(s_music_player_queue, &cmd, 0) == pdTRUE)
        {
            if (cmd.type == MUSIC_CMD_STOP)
            {
                stop = true;
            }
            else if (cmd.type == MUSIC_CMD_PAUSE)
            {
                if (!paused)
                {
                    paused = true;
                    s_record_playback_paused = true;
                    music_player_clock_pause();
                }
            }
            else if (cmd.type == MUSIC_CMD_PLAY_CURRENT)
            {
                if (paused)
                {
                    paused = false;
                    s_record_playback_paused = false;
                    music_player_clock_resume();
                }
            }
            else if (cmd.type == MUSIC_CMD_SET_VOLUME)
            {
                music_player_apply_volume(cmd.value);
            }
        }
        if (stop)
        {
            break;
        }
        if (paused)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t to_read = remaining < 8192U ? remaining : 8192U;
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0)
        {
            break;
        }
        if (music_player_output_cb(buffer, (int)bytes_read, NULL) != 0)
        {
            break;
        }
        remaining -= (uint32_t)bytes_read;
    }

    free(buffer);
    fclose(file);
    music_player_restore_audio_devices();
    s_music_started_us = 0;
    s_record_file_playing = false;
    s_record_playback_paused = false;
    s_record_playback_path[0] = '\0';
}

static void music_player_play_self_test_audio(void)
{
    const size_t data_size = (size_t)(lilygo_mp3_end - lilygo_mp3_start);
    if (data_size == 0)
    {
        ESP_LOGE(TAG, "Embedded lilygo.mp3 is empty");
        return;
    }

    s_embedded_audio_data = lilygo_mp3_start;
    s_embedded_audio_size = data_size;
    s_embedded_audio_offset = 0;
    s_music_file_size_bytes = (int)data_size;
    s_music_total_sec = 0;
    s_music_output_bytes = 0;
    s_music_playback_done = false;
    s_music_playback_error = false;
    s_self_test_audio_playing = true;

    if (music_player_open_output(MUSIC_PLAYER_OUTPUT_SAMPLE_RATE) != ESP_OK)
    {
        s_self_test_audio_playing = false;
        s_embedded_audio_data = NULL;
        s_embedded_audio_size = 0;
        return;
    }

    ESP_LOGI(TAG, "Playing embedded lilygo.mp3: %u bytes", (unsigned)data_size);
    esp_gmf_err_t run_ret = esp_audio_simple_player_run(
        s_music_player_handle, "raw://flash/lilygo.mp3", NULL);
    if (run_ret != ESP_GMF_ERR_OK)
    {
        ESP_LOGE(TAG, "Embedded MP3 player run failed: %d", run_ret);
        music_player_restore_audio_devices();
        s_self_test_audio_playing = false;
        s_embedded_audio_data = NULL;
        s_embedded_audio_size = 0;
        return;
    }

    bool stop = false;
    while (!stop && !s_music_playback_done && !s_music_playback_error)
    {
        music_player_cmd_t cmd = {0};
        if (xQueueReceive(s_music_player_queue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE)
        {
            continue;
        }
        if (cmd.type == MUSIC_CMD_STOP || cmd.type == MUSIC_CMD_PAUSE)
        {
            stop = true;
        }
        else if (cmd.type == MUSIC_CMD_SET_VOLUME)
        {
            music_player_apply_volume(cmd.value);
        }
    }

    if (!s_music_playback_done)
    {
        esp_audio_simple_player_stop(s_music_player_handle);
        music_player_wait_done(2000);
    }
    music_player_restore_audio_devices();
    s_self_test_audio_playing = false;
    s_embedded_audio_data = NULL;
    s_embedded_audio_size = 0;
    s_embedded_audio_offset = 0;
}

static void music_player_task(void *arg)
{
    (void)arg;
    int current_index = -1;
    music_player_cmd_t cmd = {0};

    while (1)
    {
        if (xQueueReceive(s_music_player_queue, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (cmd.type)
        {
        case MUSIC_CMD_PLAY_SELF_TEST:
            current_index = -1;
            music_player_play_self_test_audio();
            continue;
        case MUSIC_CMD_PLAY_RECORDING:
            current_index = -1;
            music_player_play_recording();
            continue;
        case MUSIC_CMD_PLAY_RECORDING_FILE:
            current_index = -1;
            music_player_play_saved_recording();
            continue;
        case MUSIC_CMD_PLAY_CURRENT:
            if (s_music_selected_index >= 0)
            {
                current_index = s_music_selected_index;
            }
            else if (audio_music_track_count_get() > 0)
            {
                current_index = 0;
            }
            break;
        case MUSIC_CMD_PLAY_INDEX:
            current_index = cmd.value;
            s_music_selected_index = cmd.value;
            break;
        case MUSIC_CMD_SET_VOLUME:
            music_player_apply_volume(cmd.value);
            continue;
        case MUSIC_CMD_SET_PLAY_MODE:
            s_music_play_mode = cmd.value;
            if (s_music_play_mode < 0 || s_music_play_mode > 2)
            {
                s_music_play_mode = 0;
            }
            continue;
        case MUSIC_CMD_PAUSE:
        case MUSIC_CMD_STOP:
        default:
            music_state_set_playing(false);
            continue;
        }

        while (current_index >= 0)
        {
            current_index = music_player_play_index(current_index);
        }
    }
}

static esp_err_t music_player_start(void)
{
    if (s_music_player_queue == NULL)
    {
        s_music_player_queue = xQueueCreate(MUSIC_PLAYER_QUEUE_LEN, sizeof(music_player_cmd_t));
        ESP_RETURN_ON_FALSE(s_music_player_queue != NULL, ESP_ERR_NO_MEM, TAG, "Create music queue failed");
    }

    if (s_music_player_handle == NULL)
    {
        esp_asp_cfg_t cfg = {
            .in.cb = music_player_input_cb,
            .in.user_ctx = NULL,
            .out.cb = music_player_output_cb,
            .out.user_ctx = NULL,
            .task_prio = AUDIO_PLAYER_TASK_PRIORITY,
            .task_stack = 16 * 1024,
            .task_core = 1,
            .task_stack_in_ext = false,
        };
        esp_gmf_err_t ret = esp_audio_simple_player_new(&cfg, &s_music_player_handle);
        ESP_RETURN_ON_FALSE(ret == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Create audio simple player failed: %d", ret);
        ret = esp_audio_simple_player_set_event(s_music_player_handle, music_player_event_cb, NULL);
        ESP_RETURN_ON_FALSE(ret == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Set audio simple player event failed: %d", ret);
    }

    if (s_music_player_task_handle == NULL)
    {
        BaseType_t ok = xTaskCreate(music_player_task,
                                    "music_player",
                                    8192,
                                    NULL,
                                    AUDIO_CONTROL_TASK_PRIORITY,
                                    &s_music_player_task_handle);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Create music player task failed");
    }

    return ESP_OK;
}

esp_err_t audio_init(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp != NULL && bsp->i2c_bus != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "BSP is not initialized");

    t_panel_audio_codec_config_t codec_config =
        T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT();
    codec_config.sample_rate_hz = SAMPLE_RATE;
    codec_config.bits_per_sample = BITS_PER_SAMPLE;
    codec_config.playback_channels = CHANNELS;
    codec_config.capture_channels = CHANNELS;
    codec_config.enable_playback = true;
    codec_config.enable_capture = true;
    codec_config.enable_speaker = true;
    codec_config.input = T_PANEL_AUDIO_INPUT_AUTO;
    codec_config.es7210_mic_mask = T_PANEL_AUDIO_ES7210_MIC1 |
                                   T_PANEL_AUDIO_ES7210_MIC2;
    codec_config.output_volume = AUDIO_DEFAULT_OUTPUT_VOLUME;
    codec_config.input_gain_db = 30.0f;

    ESP_RETURN_ON_ERROR(t_panel_audio_codec_init(bsp, &codec_config,
                                                 &s_audio_codec),
                        TAG, "Initialize BSP audio codec failed");
    s_play_dev = t_panel_audio_codec_get_playback_dev(s_audio_codec);
    s_rec_dev = t_panel_audio_codec_get_capture_dev(s_audio_codec);
    ESP_RETURN_ON_FALSE(s_play_dev != NULL && s_rec_dev != NULL,
                        ESP_FAIL, TAG, "Get BSP codec devices failed");
    s_music_output_open = true;

    if (s_audio_capture_mutex == NULL)
    {
        s_audio_capture_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_capture_mutex != NULL, ESP_ERR_NO_MEM,
                            TAG, "Create audio capture mutex failed");
    }
    if (s_audio_playback_mutex == NULL)
    {
        s_audio_playback_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_playback_mutex != NULL, ESP_ERR_NO_MEM,
                            TAG, "Create audio playback mutex failed");
    }
    if (s_record_mutex == NULL)
    {
        s_record_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_record_mutex != NULL, ESP_ERR_NO_MEM, TAG, "Create record mutex failed");
    }
    if (s_music_state_mutex == NULL)
    {
        s_music_state_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_music_state_mutex != NULL, ESP_ERR_NO_MEM, TAG, "Create music state mutex failed");
    }

    ESP_RETURN_ON_ERROR(music_player_start(), TAG, "Start music player failed");

    if (!s_mic_level_task_started)
    {
        s_mic_level_task_started = true;
        if (xTaskCreate(mic_level_task, "mic_level", 4096, NULL,
                        AUDIO_CAPTURE_TASK_PRIORITY, NULL) != pdPASS)
        {
            s_mic_level_task_started = false;
            return ESP_ERR_NO_MEM;
        }
    }

    t_panel_audio_input_t input = t_panel_audio_codec_get_input(s_audio_codec);
    ESP_LOGI(TAG, "BSP audio initialized: input=%s, %d Hz, mic_gain=30dB, vol=%d",
             input == T_PANEL_AUDIO_INPUT_ES7210 ? "ES7210" : "ES8389",
             SAMPLE_RATE, AUDIO_DEFAULT_OUTPUT_VOLUME);
    return ESP_OK;
}

bool audio_music_control(audio_music_control_action_t action, int value)
{
    switch (action)
    {
    case MUSIC_CONTROL_PLAY_PAUSE:
        if (value)
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_CURRENT, 0);
            music_state_set_playing(ok);
            return ok;
        }
        music_state_set_playing(false);
        return music_player_send_cmd(MUSIC_CMD_PAUSE, 0);
    case MUSIC_CONTROL_TRACK_SELECT:
        s_music_selected_index = value;
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_INDEX, value);
            music_state_set_playing(ok);
            return ok;
        }
    case MUSIC_CONTROL_VOLUME:
        return music_player_send_cmd(MUSIC_CMD_SET_VOLUME, value);
    case MUSIC_CONTROL_PLAY_MODE:
        return music_player_send_cmd(MUSIC_CMD_SET_PLAY_MODE, value);
    case MUSIC_CONTROL_PREV:
    {
        int index = music_player_adjacent_index(s_music_selected_index, -1);
        if (index < 0)
        {
            return false;
        }
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_INDEX, index);
            music_state_set_playing(ok);
            return ok;
        }
    }
    case MUSIC_CONTROL_NEXT:
    {
        int index = music_player_adjacent_index(s_music_selected_index, 1);
        if (index < 0)
        {
            return false;
        }
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_INDEX, index);
            music_state_set_playing(ok);
            return ok;
        }
    }
    default:
        return true;
    }
}

void audio_stop_music(void)
{
    if (s_music_player_handle == NULL)
    {
        return;
    }

    (void)music_player_send_cmd(MUSIC_CMD_STOP, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

esp_err_t audio_self_test_play_lilygo(void)
{
    if (s_music_player_handle == NULL || s_music_player_queue == NULL ||
        s_record_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_self_test_audio_playing)
    {
        return ESP_OK;
    }

    if (!music_player_send_cmd(MUSIC_CMD_STOP, 0))
    {
        return ESP_FAIL;
    }
    return music_player_send_cmd(MUSIC_CMD_PLAY_SELF_TEST, 0)
               ? ESP_OK
               : ESP_FAIL;
}

void audio_self_test_stop(void)
{
    if (s_music_player_queue != NULL)
    {
        (void)music_player_send_cmd(MUSIC_CMD_STOP, 0);
    }
}

bool audio_self_test_playback_is_active(void)
{
    return s_self_test_audio_playing;
}

void audio_get_mic_levels(int *mic0_db, int *mic1_db)
{
    if (mic0_db)
    {
        *mic0_db = s_mic_level_db[0];
    }
    if (mic1_db)
    {
        *mic1_db = s_mic_level_db[1];
    }
}

esp_err_t audio_record_start(char *path, size_t path_size)
{
    if (s_rec_dev == NULL || s_record_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_record_active || s_record_file_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_stop_music();
    if (s_record_buffer == NULL)
    {
        s_record_buffer = heap_caps_malloc(RECORD_BUFFER_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_record_buffer != NULL, ESP_ERR_NO_MEM, TAG,
                            "Allocate %u-byte recording buffer failed",
                            (unsigned)RECORD_BUFFER_SIZE);
    }

    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_record_data_bytes = 0;
    audio_record_wav_header_write(s_record_buffer, 0);
    s_record_started_us = esp_timer_get_time();
    s_record_paused = false;
    s_record_active = true;
    xSemaphoreGive(s_record_mutex);

    if (path && path_size > 0)
    {
        snprintf(path, path_size, "%s", AUDIO_RECORD_MEMORY_PATH);
    }
    ESP_LOGI(TAG, "PSRAM recording started: max=%u ms, capacity=%u bytes",
             (unsigned)AUDIO_RECORD_MAX_DURATION_MS,
             (unsigned)RECORD_BUFFER_SIZE);
    return ESP_OK;
}

esp_err_t audio_record_stop(void)
{
    if (s_record_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_record_buffer == NULL || (!s_record_active && s_record_data_bytes == 0))
    {
        s_record_active = false;
        xSemaphoreGive(s_record_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_record_active = false;
    s_record_paused = false;
    int64_t elapsed_us = s_record_started_us > 0 ? esp_timer_get_time() - s_record_started_us : 0;
    audio_record_wav_header_write(s_record_buffer, s_record_data_bytes);
    s_record_started_us = 0;
    uint32_t data_bytes = s_record_data_bytes;
    xSemaphoreGive(s_record_mutex);

    uint32_t measured_rate = elapsed_us > 0
                                 ? (uint32_t)(((uint64_t)data_bytes * 1000000ULL) /
                                              ((uint64_t)elapsed_us * CHANNELS * (BITS_PER_SAMPLE / 8)))
                                 : 0;
    ESP_LOGI(TAG, "Recording stopped: %" PRIu32 " bytes, elapsed=%" PRIi64
                  " ms, measured_rate=%" PRIu32 " Hz",
             data_bytes, elapsed_us / 1000, measured_rate);
    return ESP_OK;
}

bool audio_record_is_active(void)
{
    return s_record_active;
}

bool audio_record_is_paused(void)
{
    return s_record_active && s_record_paused;
}

esp_err_t audio_record_pause(void)
{
    if (s_record_file_playing)
    {
        return music_player_send_cmd(MUSIC_CMD_PAUSE, 0) ? ESP_OK : ESP_FAIL;
    }
    if (s_record_mutex == NULL || !s_record_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_record_paused = true;
    xSemaphoreGive(s_record_mutex);
    return ESP_OK;
}

esp_err_t audio_record_resume(void)
{
    if (s_record_file_playing)
    {
        return music_player_send_cmd(MUSIC_CMD_PLAY_CURRENT, 0) ? ESP_OK : ESP_FAIL;
    }
    if (s_record_mutex == NULL || !s_record_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_record_paused = false;
    xSemaphoreGive(s_record_mutex);
    return ESP_OK;
}

uint32_t audio_record_elapsed_ms(void)
{
    return (uint32_t)(((uint64_t)s_record_data_bytes * 1000ULL) /
                      (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8)));
}

bool audio_record_available(void)
{
    return s_record_buffer != NULL && s_record_data_bytes > 0 && !s_record_active;
}

uint32_t audio_record_size_bytes(void)
{
    return audio_record_available()
               ? RECORD_WAV_HEADER_SIZE + s_record_data_bytes
               : 0;
}

uint32_t audio_record_duration_ms(void)
{
    return audio_record_elapsed_ms();
}

esp_err_t audio_record_clear(void)
{
    if (s_record_mutex == NULL || s_record_active || s_record_file_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_record_buffer != NULL && s_record_data_bytes > 0)
    {
        memset(s_record_buffer, 0, RECORD_WAV_HEADER_SIZE + s_record_data_bytes);
    }
    s_record_data_bytes = 0;
    s_record_started_us = 0;
    s_record_paused = false;
    xSemaphoreGive(s_record_mutex);
    return ESP_OK;
}

esp_err_t audio_record_save_to_sd(char *path, size_t path_size)
{
    if (path == NULL || path_size == 0 || s_record_mutex == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_record_active || s_record_file_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_record_available())
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (mkdir(AUDIO_RECORD_SAVE_DIR, 0775) != 0 && errno != EEXIST)
    {
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm time_info = {0};
    if (now > 1609459200 && localtime_r(&now, &time_info) != NULL)
    {
        int written = snprintf(path, path_size,
                               AUDIO_RECORD_SAVE_DIR "/record_%04d%02d%02d_%02d%02d%02d.wav",
                               time_info.tm_year + 1900, time_info.tm_mon + 1,
                               time_info.tm_mday, time_info.tm_hour,
                               time_info.tm_min, time_info.tm_sec);
        if (written < 0 || (size_t)written >= path_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    else
    {
        int written = snprintf(path, path_size,
                               AUDIO_RECORD_SAVE_DIR "/record_%010" PRIu64 ".wav",
                               (uint64_t)(esp_timer_get_time() / 1000));
        if (written < 0 || (size_t)written >= path_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (xSemaphoreTake(s_record_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL)
    {
        xSemaphoreGive(s_record_mutex);
        return ESP_FAIL;
    }

    size_t total_size = RECORD_WAV_HEADER_SIZE + s_record_data_bytes;
    size_t written_size = fwrite(s_record_buffer, 1, total_size, file);
    int close_ret = fclose(file);
    xSemaphoreGive(s_record_mutex);

    if (written_size != total_size || close_ret != 0)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved recording to %s (%u bytes)", path,
             (unsigned)total_size);
    return ESP_OK;
}

esp_err_t audio_record_play_file(const char *path)
{
    if (path == NULL || s_music_player_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_record_active || s_record_file_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool memory_recording = strcmp(path, AUDIO_RECORD_MEMORY_PATH) == 0;
    if (memory_recording && !audio_record_available())
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (!memory_recording)
    {
        const size_t dir_len = strlen(AUDIO_RECORD_SAVE_DIR);
        const size_t path_len = strlen(path);
        const char *ext = strrchr(path, '.');
        struct stat st = {0};
        if (path_len <= dir_len || path_len >= sizeof(s_record_playback_path) ||
            strncmp(path, AUDIO_RECORD_SAVE_DIR, dir_len) != 0 ||
            path[dir_len] != '/' || ext == NULL || strcasecmp(ext, ".wav") != 0 ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            return ESP_ERR_NOT_FOUND;
        }
    }

    audio_stop_music();
    s_record_file_playing = true;
    music_player_cmd_type_t command = MUSIC_CMD_PLAY_RECORDING;
    if (!memory_recording)
    {
        snprintf(s_record_playback_path, sizeof(s_record_playback_path), "%s", path);
        command = MUSIC_CMD_PLAY_RECORDING_FILE;
    }
    if (!music_player_send_cmd(command, 0))
    {
        s_record_file_playing = false;
        s_record_playback_path[0] = '\0';
        s_record_playback_paused = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_record_stop_playback(void)
{
    if (s_record_file_playing)
    {
        (void)music_player_send_cmd(MUSIC_CMD_STOP, 0);
    }
}

bool audio_record_playback_is_active(void)
{
    return s_record_file_playing;
}

bool audio_record_playback_is_paused(void)
{
    return s_record_file_playing && s_record_playback_paused;
}

uint32_t audio_record_playback_elapsed_ms(void)
{
    if (s_music_started_us > 0)
    {
        return music_player_elapsed_ms_get();
    }

    const uint32_t bytes_per_sec = MUSIC_PLAYER_OUTPUT_SAMPLE_RATE *
                                   MUSIC_PLAYER_OUTPUT_CHANNELS *
                                   (MUSIC_PLAYER_OUTPUT_BITS / 8);
    if (bytes_per_sec == 0)
    {
        return 0;
    }
    uint64_t output_bytes = 0;
    if (s_audio_playback_mutex)
    {
        xSemaphoreTake(s_audio_playback_mutex, portMAX_DELAY);
    }
    output_bytes = s_music_output_bytes;
    if (s_audio_playback_mutex)
    {
        xSemaphoreGive(s_audio_playback_mutex);
    }
    return (uint32_t)((output_bytes * 1000ULL) / bytes_per_sec);
}
