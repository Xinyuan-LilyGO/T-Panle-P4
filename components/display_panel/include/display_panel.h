#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_PANEL_H_RES          720
#if CONFIG_T_PANEL_P4_BOARD_RECT
#define DISPLAY_PANEL_V_RES          1440
#else
#define DISPLAY_PANEL_V_RES          720
#endif
#if CONFIG_DISPLAY_PANEL_RGB565
#define DISPLAY_PANEL_BITS_PER_PIXEL 16
#elif CONFIG_DISPLAY_PANEL_RGB888
#define DISPLAY_PANEL_BITS_PER_PIXEL 24
#else
#error "Select an LCD pixel format"
#endif

typedef bool (*display_panel_color_trans_done_cb_t)(void *user_ctx);

typedef struct {
    esp_io_expander_handle_t expander;
    esp_ldo_channel_handle_t ldo_mipi_phy;
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_panel_io_handle_t mipi_dbi_io;
    esp_lcd_panel_handle_t panel_handle;
    SemaphoreHandle_t color_trans_done;
    SemaphoreHandle_t refresh_done;
    void *frame_buffers[2];
    uint8_t frame_buffer_count;
    uint8_t next_fb;
    display_panel_color_trans_done_cb_t color_trans_done_cb;
    void *color_trans_done_user_ctx;
} display_panel_t;

/* The reset expander is required by Standard/Round and ignored on Rect. */
esp_err_t display_panel_init(display_panel_t *display, esp_io_expander_handle_t reset_expander);
esp_err_t display_panel_deinit(display_panel_t *display);
esp_err_t display_panel_draw_bitmap_async(display_panel_t *display,
                                          int x_start, int y_start,
                                          int x_end, int y_end,
                                          const void *color_data);
esp_err_t display_panel_draw_bitmap(display_panel_t *display, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data);
void *display_panel_get_next_frame_buffer(display_panel_t *display);
void display_panel_set_color_trans_done_cb(display_panel_t *display,
                                           display_panel_color_trans_done_cb_t callback,
                                           void *user_ctx);
esp_err_t display_panel_wait_refresh_done(display_panel_t *display, uint32_t timeout_ms);
esp_err_t display_panel_backlight_init(void);
esp_err_t display_panel_backlight_set(bool on);
esp_err_t display_panel_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
