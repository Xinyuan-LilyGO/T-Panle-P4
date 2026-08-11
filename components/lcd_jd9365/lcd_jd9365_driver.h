#pragma once

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

/* LCD parameters */
#define LCD_H_RES                   (720)
#define LCD_V_RES                   (720)
#if CONFIG_EXAMPLE_CAMERA_ESP_VIDEO
#define LCD_BIT_PER_PIXEL           (24)
#define LCD_DPI_CLOCK_FREQ_MHZ      (24)
#else
#define LCD_BIT_PER_PIXEL           (24)
#define LCD_DPI_CLOCK_FREQ_MHZ      (24)
#endif
#define LCD_MIPI_DSI_LANE_NUM       (2)
#define LCD_MIPI_DSI_LANE_BITRATE   (1500)  // Mbps

/* MIPI DSI PHY LDO */
#define MIPI_DSI_PHY_PWR_LDO_CHAN       (3)
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)

/* Pixel format */
#if LCD_BIT_PER_PIXEL == 24
#define LCD_MIPI_DPI_PX_FORMAT      LCD_COLOR_PIXEL_FORMAT_RGB888
#elif LCD_BIT_PER_PIXEL == 18
#define LCD_MIPI_DPI_PX_FORMAT      LCD_COLOR_PIXEL_FORMAT_RGB666
#elif LCD_BIT_PER_PIXEL == 16
#define LCD_MIPI_DPI_PX_FORMAT      LCD_COLOR_PIXEL_FORMAT_RGB565
#endif

typedef struct {
    esp_io_expander_handle_t expander;
    esp_ldo_channel_handle_t ldo_mipi_phy;
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_panel_io_handle_t mipi_dbi_io;
    esp_lcd_panel_handle_t panel_handle;
    SemaphoreHandle_t color_trans_done;
    SemaphoreHandle_t refresh_done;
    void *frame_buffers[2];
    uint8_t next_fb;
} lcd_driver_t;

esp_err_t lcd_jd9365_init(lcd_driver_t *drv, esp_io_expander_handle_t expander);
esp_err_t lcd_jd9365_deinit(lcd_driver_t *drv);
esp_err_t lcd_jd9365_draw_bitmap(lcd_driver_t *drv, int x_start, int y_start,
                                  int x_end, int y_end, const void *color_data);
void *lcd_jd9365_get_next_frame_buffer(lcd_driver_t *drv);
esp_err_t lcd_jd9365_wait_refresh_done(lcd_driver_t *drv, uint32_t timeout_ms);
esp_err_t lcd_backlight_init(void);
esp_err_t lcd_backlight_set(bool on);
esp_err_t lcd_backlight_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
