#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct
{
    lv_image_dsc_t dsc;
    uint8_t *pixels;
} music_cover_image_t;

bool music_cover_image_load_mp3(const char *path, uint32_t target_width,
                                uint32_t target_height, music_cover_image_t *image);
void music_cover_image_release(music_cover_image_t *image);
