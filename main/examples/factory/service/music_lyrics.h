#pragma once

#include <stdint.h>

#define MUSIC_LYRICS_TEXT_MAX_LEN 256

typedef struct
{
    uint32_t time_ms;
    char text[MUSIC_LYRICS_TEXT_MAX_LEN];
} music_lyrics_line_t;

int music_lyrics_load_embedded(const char *path, music_lyrics_line_t *lines, int max_lines);
