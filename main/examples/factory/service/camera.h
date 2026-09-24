#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "display_panel.h"
#include "t_panel_p4_bsp.h"

#ifndef CAMERA_UI_PREVIEW_X
#define CAMERA_UI_PREVIEW_X 0
#endif
#ifndef CAMERA_UI_PREVIEW_Y
#define CAMERA_UI_PREVIEW_Y 0
#endif
#ifndef CAMERA_UI_PREVIEW_WIDTH
#define CAMERA_UI_PREVIEW_WIDTH 720
#endif
#ifndef CAMERA_UI_PREVIEW_HEIGHT
#define CAMERA_UI_PREVIEW_HEIGHT 480
#endif
#define CAMERA_UI_LAST_PHOTO_WIDTH 128
#define CAMERA_UI_LAST_PHOTO_HEIGHT 72

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t fps_x10;
    uint32_t photo_count;
    uint32_t photo_generation;
    esp_err_t last_photo_result;
    bool running;
    bool photo_saving;
    bool storage_ready;
} camera_state_t;

typedef void (*camera_preview_frame_cb_t)(const uint8_t *pixels,
                                          uint32_t width,
                                          uint32_t height);

esp_err_t camera_init(t_panel_p4_bsp_t *bsp, display_panel_t *display);
bool camera_preview_start(uint32_t width, uint32_t height);
void camera_preview_stop(void);
bool camera_preview_stop_wait(uint32_t timeout_ms);
void camera_preview_clear_area(void);
void camera_preview_set_area(int x, int y, int w, int h);
void camera_preview_set_direct_crop(bool enabled);
void camera_preview_set_frame_callback(camera_preview_frame_cb_t callback);
bool camera_storage_is_ready(void);
esp_err_t camera_capture_photo_async(void);
esp_err_t camera_state_get(camera_state_t *state);
esp_err_t camera_last_photo_copy(uint8_t *dst,
                                 size_t dst_size,
                                 uint32_t *width,
                                 uint32_t *height,
                                 uint32_t *generation);
