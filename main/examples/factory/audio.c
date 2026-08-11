#include "audio.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "es8389_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "T_Panle_P4_board_config.h"
#include "ui.h"

static const char *TAG = "factory_audio";

#define AUDIO_MOUNT_POINT "/sdcard"
#define MUSIC_DIR AUDIO_MOUNT_POINT "/music"

#define I2S_NUM (0)
#define SAMPLE_RATE (44100)
#define BITS_PER_SAMPLE (16)
#define CHANNELS (2)
#define VOL (100)

#define MIC_LEVEL_BUF_SAMPLES (1024)
#define MIC_LEVEL_DB_FLOOR (-60)
#define MIC_LEVEL_UPDATE_MS (200)
#define MIC_LEVEL_SMOOTH_NUM 3
#define MIC_LEVEL_SMOOTH_DEN 4

#define MUSIC_PLAYER_QUEUE_LEN 8
#define MUSIC_PATH_MAX_LEN 384
#define MUSIC_URI_MAX_LEN (MUSIC_PATH_MAX_LEN + 8)
#define MUSIC_LRC_MAX_LINES 96
#define MUSIC_LRC_TEXT_MAX_LEN 128
#define MUSIC_LRC_LINE_MAX_LEN 256
#define MUSIC_PROGRESS_UPDATE_MS 500
#define MUSIC_SPECTRUM_OUTPUT_UPDATE_MS 50

#ifdef CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE
#define MUSIC_PLAYER_OUTPUT_SAMPLE_RATE CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE
#else
#define MUSIC_PLAYER_OUTPUT_SAMPLE_RATE SAMPLE_RATE
#endif

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
} music_player_cmd_type_t;

typedef struct
{
    music_player_cmd_type_t type;
    int value;
} music_player_cmd_t;

typedef struct
{
    uint32_t time_ms;
    char text[MUSIC_LRC_TEXT_MAX_LEN];
} music_lrc_line_t;

static esp_codec_dev_handle_t s_rec_dev = NULL;
static esp_codec_dev_handle_t s_play_dev = NULL;
static SemaphoreHandle_t s_audio_codec_mutex = NULL;
static bool s_mic_level_task_started = false;
static QueueHandle_t s_music_player_queue = NULL;
static TaskHandle_t s_music_player_task_handle = NULL;
static esp_asp_handle_t s_music_player_handle = NULL;
static volatile bool s_music_audio_active = false;
static volatile bool s_music_playback_done = false;
static volatile bool s_music_playback_error = false;
static bool s_music_output_open = false;
static int s_music_play_mode = 0;
static int s_music_selected_index = -1;
static int s_music_volume = 64;
static int s_music_file_size_bytes = 0;
static int s_music_total_sec = 0;
static uint64_t s_music_output_bytes = 0;
static int64_t s_music_started_us = 0;
static int64_t s_music_pause_started_us = 0;
static int64_t s_music_paused_acc_us = 0;
static int64_t s_music_spectrum_last_us = 0;
static uint32_t s_music_spectrum_peak = 12000;
static char s_music_current_format[12] = "--";

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

    int16_t *buf = heap_caps_malloc(MIC_LEVEL_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
    {
        buf = heap_caps_malloc(MIC_LEVEL_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
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

    while (1)
    {
        if (s_music_audio_active)
        {
            home_page_set_mic_levels(MIC_LEVEL_DB_FLOOR, MIC_LEVEL_DB_FLOOR);
            vTaskDelay(pdMS_TO_TICKS(MIC_LEVEL_UPDATE_MS));
            continue;
        }

        if (s_audio_codec_mutex)
        {
            xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
        }
        esp_err_t ret = esp_codec_dev_read(s_rec_dev, buf, bytes_to_read);
        if (s_audio_codec_mutex)
        {
            xSemaphoreGive(s_audio_codec_mutex);
        }
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Read mic level failed: %s", esp_err_to_name(ret));
            home_page_set_mic_levels(MIC_LEVEL_DB_FLOOR, MIC_LEVEL_DB_FLOOR);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

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

        home_page_set_mic_levels(smooth_l, smooth_r);
        vTaskDelay(pdMS_TO_TICKS(MIC_LEVEL_UPDATE_MS));
    }
}

static bool music_build_track_path(int index, char *path, size_t path_size)
{
    const char *name = music_page_get_track_name(index);
    if (name == NULL || name[0] == '\0')
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
    const music_lrc_line_t *la = (const music_lrc_line_t *)a;
    const music_lrc_line_t *lb = (const music_lrc_line_t *)b;
    return (la->time_ms > lb->time_ms) - (la->time_ms < lb->time_ms);
}

static int music_lrc_load(const char *track_path, music_lrc_line_t *lines, int max_lines)
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

    ESP_LOGI(TAG, "Loaded %d lyric line(s) for %s", count, track_path);
    return count;
}

static void music_lrc_update(const music_lrc_line_t *lines, int line_count, uint32_t elapsed_ms, int *last_index)
{
    if (line_count <= 0)
    {
        if (*last_index != -2)
        {
            music_page_set_lyrics(NULL, NULL, NULL);
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
    music_page_set_lyrics(prev, current, next);
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

static bool music_buffer_contains(const uint8_t *buf, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || len < needle_len)
    {
        return false;
    }
    for (size_t i = 0; i + needle_len <= len; i++)
    {
        if (memcmp(buf + i, needle, needle_len) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool music_file_has_embedded_cover(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return false;
    }

    uint8_t buf[512] = {0};
    size_t carry = 0;
    size_t scanned = 0;
    bool found = false;
    while (scanned < 1024 * 1024)
    {
        size_t got = fread(buf + carry, 1, sizeof(buf) - carry, file);
        if (got == 0)
        {
            break;
        }
        size_t len = carry + got;
        if (music_buffer_contains(buf, len, "APIC") ||
            music_buffer_contains(buf, len, "covr") ||
            music_buffer_contains(buf, len, "PICTURE"))
        {
            found = true;
            break;
        }
        carry = len > 16 ? 16 : len;
        memmove(buf, buf + len - carry, carry);
        scanned += got;
    }
    fclose(file);
    return found;
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
    if (s_play_dev)
    {
        if (s_audio_codec_mutex)
        {
            xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
        }
        esp_codec_dev_set_out_vol(s_play_dev, volume);
        if (s_audio_codec_mutex)
        {
            xSemaphoreGive(s_audio_codec_mutex);
        }
    }
    music_page_set_volume(volume);
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

    if (s_audio_codec_mutex)
    {
        xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
    }

    s_music_audio_active = true;
    if (s_rec_dev)
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
        if (s_rec_dev)
        {
            esp_codec_dev_open(s_rec_dev, &default_fs);
        }
        s_music_audio_active = false;
    }

    if (s_audio_codec_mutex)
    {
        xSemaphoreGive(s_audio_codec_mutex);
    }
    return ret;
}

static void music_player_restore_audio_devices(void)
{
    if (s_audio_codec_mutex)
    {
        xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
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
    if (s_rec_dev)
    {
        esp_err_t rec_ret = esp_codec_dev_open(s_rec_dev, &fs);
        if (rec_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Restore recorder failed: %s", esp_err_to_name(rec_ret));
        }
    }

    s_music_audio_active = false;
    if (s_audio_codec_mutex)
    {
        xSemaphoreGive(s_audio_codec_mutex);
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

static void music_player_set_track_ui(int index, const esp_asp_music_info_t *info, int total_sec)
{
    const char *name = music_page_get_track_name(index);
    if (name)
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
    music_page_set_track_params(s_music_current_format, sample, bitrate, channels);
    music_page_set_progress(0, total_sec);
    music_page_set_play_state(true);
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

    music_page_set_spectrum_levels(levels, MUSIC_SPECTRUM_BAR_COUNT);
}

static int music_player_output_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (data == NULL || data_size <= 0)
    {
        return 0;
    }
    music_player_update_spectrum_from_pcm(data, data_size);
    if (s_audio_codec_mutex)
    {
        xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
    }
    int ret = ESP_FAIL;
    if (s_play_dev && s_music_output_open)
    {
        ret = esp_codec_dev_write(s_play_dev, data, data_size);
        if (ret == ESP_OK)
        {
            s_music_output_bytes += data_size;
        }
    }
    if (s_audio_codec_mutex)
    {
        xSemaphoreGive(s_audio_codec_mutex);
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
        music_player_set_track_ui(s_music_selected_index, &info, s_music_total_sec);
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
    if (s_audio_codec_mutex)
    {
        xSemaphoreTake(s_audio_codec_mutex, portMAX_DELAY);
    }
    bytes = s_music_output_bytes;
    if (s_audio_codec_mutex)
    {
        xSemaphoreGive(s_audio_codec_mutex);
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
    int count = music_page_get_track_count();
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
    int count = music_page_get_track_count();
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
    int count = music_page_get_track_count();
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
                music_page_set_play_state(false);
                return true;
            }
            if (esp_audio_simple_player_resume(s_music_player_handle) == ESP_GMF_ERR_OK)
            {
                music_player_clock_resume();
                *paused = false;
                music_page_set_play_state(true);
            }
            else
            {
                music_player_restore_audio_devices();
                music_page_set_play_state(false);
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
                music_page_set_play_state(false);
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
        music_page_set_lyrics("Open failed", "Invalid track path", "");
        music_page_set_play_state(false);
        return -1;
    }

    const char *format = music_format_name_from_path(path);
    if (!music_format_supported_by_simple_player(path))
    {
        music_page_set_track_params("Unsupported", "--", "--", "--");
        music_page_set_progress(0, 0);
        music_page_set_lyrics("Unsupported file", "No decoder for this format", "Use mp3/aac/flac/wav/m4a/ts/ogg/amr");
        music_page_set_play_state(false);
        ESP_LOGW(TAG, "Unsupported music type: %s", path);
        return -1;
    }
    if (strcmp(format, "AAC") == 0 && !music_aac_adts_header_present(path))
    {
        music_page_set_track_params("AAC", "No ADTS", "--", "--");
        music_page_set_progress(0, 0);
        music_page_set_lyrics("Unsupported AAC file", "AAC needs ADTS headers", "Use .m4a for MP4 AAC or convert to ADTS AAC");
        music_page_set_play_state(false);
        ESP_LOGW(TAG, "Unsupported AAC without ADTS header: %s", path);
        return -1;
    }

    music_lrc_line_t *lyrics = heap_caps_calloc(MUSIC_LRC_MAX_LINES,
                                                 sizeof(music_lrc_line_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lyrics == NULL)
    {
        lyrics = heap_caps_calloc(MUSIC_LRC_MAX_LINES, sizeof(music_lrc_line_t), MALLOC_CAP_DEFAULT);
    }
    int lyric_count = lyrics ? music_lrc_load(path, lyrics, MUSIC_LRC_MAX_LINES) : 0;
    int lyric_index = -3;

    snprintf(s_music_current_format, sizeof(s_music_current_format), "%s", format);
    s_music_selected_index = index;
    music_page_set_current_track(index);
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
    music_page_set_spectrum_levels(NULL, 0);

    music_player_apply_volume(s_music_volume);
    music_page_set_track_params(s_music_current_format, "Parsing", "--", "--");
    music_page_set_progress(0, s_music_total_sec);
    music_page_set_play_state(true);
    music_lrc_update(lyrics, lyric_count, 0, &lyric_index);

    if (music_player_open_output(MUSIC_PLAYER_OUTPUT_SAMPLE_RATE) != ESP_OK)
    {
        heap_caps_free(lyrics);
        music_page_set_track_params(s_music_current_format, "I2S open failed", "--", "--");
        music_page_set_lyrics("Playback failed", "Can not open output sample rate", "");
        music_page_set_play_state(false);
        return -1;
    }

    const char *track_name = music_page_get_track_name(index);
    ESP_LOGI(TAG, "Playing track: %s uri=%s", track_name ? track_name : "(unknown)", uri);
    esp_gmf_err_t run_ret = esp_audio_simple_player_run(s_music_player_handle, uri, NULL);
    if (run_ret != ESP_GMF_ERR_OK)
    {
        ESP_LOGE(TAG, "Audio simple player run failed: %d", run_ret);
        heap_caps_free(lyrics);
        music_player_restore_audio_devices();
        music_page_set_track_params(s_music_current_format, "Run failed", "--", "--");
        music_page_set_lyrics("Playback failed", "Audio simple player can not run", "");
        music_page_set_play_state(false);
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
            music_page_set_progress((int)(elapsed_ms / 1000), s_music_total_sec);
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
        music_page_set_track_params(s_music_current_format, "Decode failed", "--", "--");
        music_page_set_lyrics("Playback failed", "Audio simple player decode error", "Try another file or re-encode it");
        music_page_set_play_state(false);
        return -1;
    }
    if (!stop)
    {
        int next_index = music_player_next_index_after_finish(index);
        if (next_index < 0)
        {
            music_page_set_play_state(false);
        }
        return next_index;
    }
    music_page_set_play_state(false);
    return -1;
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
        case MUSIC_CMD_PLAY_CURRENT:
            if (s_music_selected_index >= 0)
            {
                current_index = s_music_selected_index;
            }
            else if (music_page_get_track_count() > 0)
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
            music_page_set_play_state(false);
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
            .out.cb = music_player_output_cb,
            .out.user_ctx = NULL,
            .task_prio = 5,
            .task_stack = 16 * 1024,
            .task_core = 0,
            .task_stack_in_ext = true,
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
                                    4,
                                    &s_music_player_task_handle);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Create music player task failed");
    }

    return ESP_OK;
}

esp_err_t factory_audio_init(i2c_master_bus_handle_t i2c_bus,
                             esp_io_expander_handle_t io_expander)
{
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_ARG, TAG, "I2C bus is NULL");
    ESP_RETURN_ON_FALSE(io_expander != NULL, ESP_ERR_INVALID_ARG, TAG, "IO expander is NULL");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, 1 << XL9555_SPK_CRTL, IO_EXPANDER_OUTPUT),
                        TAG, "Set SPK pin direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, 1 << XL9555_SPK_CRTL, 1),
                        TAG, "Set SPK pin level failed");
    ESP_LOGI(TAG, "Speaker amplifier enabled");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

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

    audio_codec_i2s_cfg_t i2s_tx_cfg = {
        .port = I2S_NUM,
        .tx_handle = tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if_tx = audio_codec_new_i2s_data(&i2s_tx_cfg);

    audio_codec_i2s_cfg_t i2s_rx_cfg = {
        .port = I2S_NUM,
        .tx_handle = NULL,
        .rx_handle = rx_handle,
    };
    const audio_codec_data_if_t *data_if_rx = audio_codec_new_i2s_data(&i2s_rx_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8389_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {0},
        .no_dac_ref = false,
        .mclk_div = 384,
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&codec_cfg);

    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "ES8389 codec new failed");

    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if_tx,
    };
    s_play_dev = esp_codec_dev_new(&play_cfg);

    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if_rx,
    };
    s_rec_dev = esp_codec_dev_new(&rec_cfg);

    ESP_RETURN_ON_FALSE(s_play_dev != NULL && s_rec_dev != NULL, ESP_FAIL, TAG, "Codec dev new failed");

    if (s_audio_codec_mutex == NULL)
    {
        s_audio_codec_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_codec_mutex != NULL, ESP_ERR_NO_MEM, TAG, "Create audio codec mutex failed");
    }

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_play_dev, VOL), TAG, "Set vol failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_rec_dev, 30.0), TAG, "Set mic gain failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_play_dev, &fs), TAG, "Open play dev failed");
    s_music_output_open = true;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_rec_dev, &fs), TAG, "Open rec dev failed");
    ESP_RETURN_ON_ERROR(music_player_start(), TAG, "Start music player failed");

    if (!s_mic_level_task_started)
    {
        s_mic_level_task_started = true;
        if (xTaskCreate(mic_level_task, "mic_level", 4096, NULL, 4, NULL) != pdPASS)
        {
            s_mic_level_task_started = false;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Codec initialized (ADC+DAC, %d Hz, mic_gain=30dB, vol=%d)", SAMPLE_RATE, VOL);
    return ESP_OK;
}

bool music_page_on_control(music_control_action_t action, int value)
{
    switch (action)
    {
    case MUSIC_CONTROL_PLAY_PAUSE:
        if (value)
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_CURRENT, 0);
            music_page_set_play_state(ok);
            return ok;
        }
        music_page_set_play_state(false);
        return music_player_send_cmd(MUSIC_CMD_PAUSE, 0);
    case MUSIC_CONTROL_TRACK_SELECT:
        s_music_selected_index = value;
        {
            bool ok = music_player_send_cmd(MUSIC_CMD_PLAY_INDEX, value);
            music_page_set_play_state(ok);
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
            music_page_set_play_state(ok);
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
            music_page_set_play_state(ok);
            return ok;
        }
    }
    default:
        return true;
    }
}

void factory_audio_stop_music(void)
{
    if (s_music_player_handle == NULL)
    {
        return;
    }

    (void)music_player_send_cmd(MUSIC_CMD_STOP, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}
