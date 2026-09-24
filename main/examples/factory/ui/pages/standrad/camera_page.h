#pragma once

#include "ui.h"

void camera_page_register(void);
const char *camera_page_get_home_mode(void);
int camera_page_get_home_fps_x10(void);
void camera_page_set_status(const char *text, bool ok);
void camera_page_set_shot_count(uint32_t count);
void camera_page_set_preview_info(uint32_t width, uint32_t height, uint32_t fps_x10);
void camera_page_set_last_photo(const uint8_t *rgb888, uint32_t width, uint32_t height);
void camera_page_set_storage_ready(bool ready);
void camera_page_set_preview_frame(const uint8_t *rgb888, uint32_t width, uint32_t height);
