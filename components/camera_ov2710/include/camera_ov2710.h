#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "t_panel_p4_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_OV2710_MAX_BUFFER_COUNT 4

typedef enum {
    CAMERA_OV2710_PIXEL_FORMAT_RGB565,
    CAMERA_OV2710_PIXEL_FORMAT_RGB888,
} camera_ov2710_pixel_format_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t buffer_count;
    camera_ov2710_pixel_format_t pixel_format;
    bool log_supported_formats;
} camera_ov2710_config_t;

#define CAMERA_OV2710_CONFIG_DEFAULT()                     \
    {                                                       \
        .width = 1280,                                      \
        .height = 720,                                      \
        .fps = 25,                                          \
        .buffer_count = 3,                                  \
        .pixel_format = CAMERA_OV2710_PIXEL_FORMAT_RGB565,  \
        .log_supported_formats = true,                      \
    }

#define CAMERA_OV2710_1080P_CONFIG()                     \
    {                                                       \
        .width = 1920,                                      \
        .height = 1080,                                      \
        .fps = 25,                                          \
        .buffer_count = 3,                                  \
        .pixel_format = CAMERA_OV2710_PIXEL_FORMAT_RGB565,  \
        .log_supported_formats = true,                      \
    }

typedef struct camera_ov2710 *camera_ov2710_handle_t;

typedef struct {
    const void *data;
    size_t bytes_used;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t index;
    camera_ov2710_pixel_format_t pixel_format;
    bool error;
} camera_ov2710_frame_t;

esp_err_t camera_ov2710_init(t_panel_p4_bsp_t *bsp,
                             const camera_ov2710_config_t *config,
                             camera_ov2710_handle_t *ret_camera);
esp_err_t camera_ov2710_get_frame(camera_ov2710_handle_t camera,
                                  camera_ov2710_frame_t *frame);
esp_err_t camera_ov2710_return_frame(camera_ov2710_handle_t camera,
                                     const camera_ov2710_frame_t *frame);
esp_err_t camera_ov2710_deinit(camera_ov2710_handle_t camera);

#ifdef __cplusplus
}
#endif
