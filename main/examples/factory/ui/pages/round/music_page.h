#pragma once

#include "ui.h"

void music_page_register(void);
int music_page_get_current_track(void);
int music_page_get_volume(void);
void music_page_set_lyrics(const char *prev, const char *current, const char *next);
void music_page_set_play_state(bool playing);
void music_page_set_volume(int volume_percent);
void music_page_set_progress(int current_sec, int total_sec);
void music_page_set_playlist(const char *const *tracks, int track_count, int current_index);
void music_page_set_current_track(int index);
void music_page_set_artist_album(const char *artist, const char *album);
void music_page_set_track_params(const char *format, const char *sample_rate, const char *bitrate, const char *channels);
void music_page_set_spectrum_levels(const uint8_t *levels, int count);
void music_page_set_cover_file(const char *path);
int music_page_scan_sd_music(void);
int music_page_get_track_count(void);
const char *music_page_get_track_name(int index);
