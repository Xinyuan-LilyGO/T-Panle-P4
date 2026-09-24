#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "esp_err.h"
#include "t_panel_p4_bsp.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_MUSIC_MAX_TRACKS 64
#define AUDIO_MUSIC_TRACK_NAME_MAX_LEN 256
#define AUDIO_MUSIC_TEXT_MAX_LEN 256
#define AUDIO_MUSIC_PATH_MAX_LEN 384
#define AUDIO_MUSIC_SPECTRUM_BAR_COUNT 32
#define AUDIO_MUSIC_SPECTRUM_LEVEL_MAX 12
#define AUDIO_MUSIC_DIR "/sdcard/music"
#define AUDIO_DEFAULT_OUTPUT_VOLUME 100
#define AUDIO_RECORD_MAX_DURATION_MS 10000U
#define AUDIO_RECORD_MEMORY_PATH "/memory/record.wav"
#define AUDIO_RECORD_SAVE_DIR "/sdcard/record"

typedef enum
{
    MUSIC_CONTROL_PLAY_MODE = 0,
    MUSIC_CONTROL_PREV,
    MUSIC_CONTROL_PLAY_PAUSE,
    MUSIC_CONTROL_NEXT,
    MUSIC_CONTROL_VOLUME,
    MUSIC_CONTROL_LIST,
    MUSIC_CONTROL_TRACK_SELECT,
} audio_music_control_action_t;

typedef struct
{
    uint32_t revision;
    bool playing;
    bool lyrics_valid;
    bool spectrum_valid;
    int current_track;
    int current_sec;
    int total_sec;
    int volume_percent;
    int play_mode;
    char format[16];
    char sample_rate[16];
    char bitrate[16];
    char channels[16];
    char lyrics_prev[AUDIO_MUSIC_TEXT_MAX_LEN];
    char lyrics_current[AUDIO_MUSIC_TEXT_MAX_LEN];
    char lyrics_next[AUDIO_MUSIC_TEXT_MAX_LEN];
    char cover_path[AUDIO_MUSIC_PATH_MAX_LEN];
    uint8_t spectrum[AUDIO_MUSIC_SPECTRUM_BAR_COUNT];
} audio_music_state_t;

esp_err_t audio_init(t_panel_p4_bsp_t *bsp);
bool audio_music_playlist_set(const char *const *tracks, int track_count, int current_index);
int audio_music_track_count_get(void);
bool audio_music_track_name_get(int index, char *name, size_t name_size);
bool audio_music_state_get(audio_music_state_t *state);
bool audio_music_control(audio_music_control_action_t action, int value);
void audio_stop_music(void);
void audio_get_mic_levels(int *mic0_db, int *mic1_db);
esp_err_t audio_self_test_play_lilygo(void);
void audio_self_test_stop(void);
bool audio_self_test_playback_is_active(void);
esp_err_t audio_record_start(char *path, size_t path_size);
esp_err_t audio_record_stop(void);
bool audio_record_is_active(void);
bool audio_record_is_paused(void);
esp_err_t audio_record_pause(void);
esp_err_t audio_record_resume(void);
uint32_t audio_record_elapsed_ms(void);
bool audio_record_available(void);
uint32_t audio_record_size_bytes(void);
uint32_t audio_record_duration_ms(void);
esp_err_t audio_record_clear(void);
esp_err_t audio_record_save_to_sd(char *path, size_t path_size);
esp_err_t audio_record_play_file(const char *path);
void audio_record_stop_playback(void);
bool audio_record_playback_is_active(void);
bool audio_record_playback_is_paused(void);
uint32_t audio_record_playback_elapsed_ms(void);

#endif // __AUDIO_H__
