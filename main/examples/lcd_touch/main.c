/*
 * Minimal ILI9882 LCD and Ilitek touch example for T-Panel-P4 Rect.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "display_panel.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_panel.h"

#define TOUCH_POLL_PERIOD_MS 10

static const char *TAG = "ili9882_example";

static display_panel_t s_display;
static touch_panel_t s_touch;

static size_t framebuffer_size(void)
{
    return (size_t)DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES *
           (DISPLAY_PANEL_BITS_PER_PIXEL / 8);
}

static void fill_test_pattern(uint8_t *framebuffer)
{
    const uint32_t band_width = DISPLAY_PANEL_H_RES / 4;

    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; ++y) {
        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; ++x) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;

            if (x < band_width) {
                red = 0xFF;
            } else if (x < band_width * 2) {
                green = 0xFF;
            } else if (x < band_width * 3) {
                blue = 0xFF;
            } else {
                red = 0xFF;
                green = 0xFF;
                blue = 0xFF;
            }

            const size_t offset = ((size_t)y * DISPLAY_PANEL_H_RES + x) * 3;
            framebuffer[offset] = red;
            framebuffer[offset + 1] = green;
            framebuffer[offset + 2] = blue;
        }
    }
}

static esp_err_t show_test_pattern(void)
{
    uint8_t *framebuffer = display_panel_get_next_frame_buffer(&s_display);
    ESP_RETURN_ON_FALSE(framebuffer, ESP_ERR_INVALID_STATE, TAG,
                        "LCD framebuffer is unavailable");

    fill_test_pattern(framebuffer);
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(framebuffer, framebuffer_size(),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED),
        TAG, "synchronize framebuffer failed");
    ESP_RETURN_ON_ERROR(
        display_panel_draw_bitmap(&s_display, 0, 0,
                                  DISPLAY_PANEL_H_RES,
                                  DISPLAY_PANEL_V_RES, framebuffer),
        TAG, "draw test pattern failed");
    return ESP_OK;
}

static void log_touch_information(void)
{
    uint8_t chip_id[2] = {0};
    if (touch_panel_get_chip_id(&s_touch, chip_id) == ESP_OK) {
        ESP_LOGI(TAG, "Touch chip ID: %02X%02X", chip_id[0], chip_id[1]);
    }

    uint8_t fw_version[8] = {0};
    if (touch_panel_get_fw_version(&s_touch, fw_version) == ESP_OK) {
        ESP_LOGI(TAG, "Touch FW AP: %u.%u.%u.%u, MP: %u.%u.%u.%u",
                 fw_version[0], fw_version[1], fw_version[2], fw_version[3],
                 fw_version[4], fw_version[5], fw_version[6], fw_version[7]);
    }

    uint16_t x_resolution = 0;
    uint16_t y_resolution = 0;
    if (touch_panel_get_resolution(&s_touch, &x_resolution, &y_resolution) == ESP_OK) {
        ESP_LOGI(TAG, "Touch resolution: %ux%u", x_resolution, y_resolution);
    }
}

void app_main(void)
{
    static t_panel_p4_bsp_t bsp;
    i2c_master_bus_handle_t i2c_bus = NULL;
    bool touch_ready = false;
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
    i2c_bus = t_panel_p4_bsp_get_i2c_bus(&bsp);

    ESP_ERROR_CHECK(display_panel_init(&s_display, NULL));
    ESP_ERROR_CHECK(display_panel_backlight_init());
    ESP_ERROR_CHECK(display_panel_set_brightness(50));
    ESP_ERROR_CHECK(show_test_pattern());

    const esp_err_t touch_ret = touch_panel_init(&s_touch, i2c_bus, NULL);
    if (touch_ret == ESP_OK) {
        touch_ready = true;
        log_touch_information();
    } else {
        ESP_LOGE(TAG, "Touch initialization failed: %s", esp_err_to_name(touch_ret));
    }

    ESP_LOGI(TAG, "LCD + touch example ready");
    while (true) {
        if (!touch_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        touch_panel_data_t touch_data = {0};
        const esp_err_t ret = touch_panel_get_multiple_points(&s_touch, &touch_data);

        if (ret == ESP_OK) {
            for (uint8_t i = 0; i < touch_data.finger_count; ++i) {
                ESP_LOGI(TAG, "point[%u]: x=%u y=%u strength=%u", i,
                         touch_data.points[i].x,
                         touch_data.points[i].y,
                         touch_data.points[i].strength);
            }
        } else if (ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "read touch report failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_PERIOD_MS));
    }
}
