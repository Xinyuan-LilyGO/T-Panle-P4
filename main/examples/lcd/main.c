/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#if CONFIG_T_PANEL_P4_HAS_XL9555
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#endif
#include "display_panel.h"
#include "touch_panel.h"

static const char *TAG = "lcd_example";

#ifdef CONFIG_T_PANEL_P4_BOARD_RECT
extern const uint8_t image_jpg_start[] asm("_binary_wallhaven_o5k319_resized_jpg_start");
extern const uint8_t image_jpg_end[] asm("_binary_wallhaven_o5k319_resized_jpg_end");
#elif CONFIG_T_PANEL_P4_BOARD_ROUND
extern const uint8_t image_jpg_start[] asm("_binary_wallhaven_8gr6l2_resized_circle_jpg_start");
extern const uint8_t image_jpg_end[] asm("_binary_wallhaven_8gr6l2_resized_circle_jpg_end");
#else 
extern const uint8_t image_jpg_start[] asm("_binary_wallhaven-8gr6l2_resized_jpg_start");
extern const uint8_t image_jpg_end[] asm("_binary_wallhaven-8gr6l2_resized_jpg_end");
#endif

static void scan_i2c_bus(i2c_master_bus_handle_t bus, const char *reason)
{
    uint8_t found_count = 0;
    ESP_LOGI(TAG, "I2C scan start (%s)", reason);

    for (uint8_t address = 0x08; address <= 0x77; ++address)
    {
        esp_err_t ret = i2c_master_probe(bus, address, 20);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", address);
            ++found_count;
        }
        else if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_TIMEOUT)
        {
            ESP_LOGD(TAG, "I2C probe 0x%02X failed: %s", address, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "I2C scan complete: %u device(s) found", found_count);
}

static esp_err_t show_rgb_test_pattern(display_panel_t *lcd)
{
    uint8_t *frame_buffer = display_panel_get_next_frame_buffer(lcd);
    ESP_RETURN_ON_FALSE(frame_buffer != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    const uint32_t stripe_height = DISPLAY_PANEL_V_RES / 3;
    uint8_t *pixel = frame_buffer;

    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++)
    {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;

        if (y < stripe_height)
        {
            red = 0xFF;
        }
        else if (y < stripe_height * 2)
        {
            green = 0xFF;
        }
        else
        {
            blue = 0xFF;
        }

        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++)
        {
            *pixel++ = red;
            *pixel++ = green;
            *pixel++ = blue;
        }
    }

    ESP_RETURN_ON_ERROR(display_panel_draw_bitmap(lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                                  DISPLAY_PANEL_V_RES, frame_buffer),
                        TAG, "draw RGB test pattern failed");
    ESP_LOGI(TAG, "R-G-B test pattern displayed for 2 seconds");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_OK;
}

static esp_err_t show_color_test_pattern(display_panel_t *lcd, uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t *frame_buffer = display_panel_get_next_frame_buffer(lcd);
    ESP_RETURN_ON_FALSE(frame_buffer != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    uint8_t *pixel = frame_buffer;

    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++)
    {
        // LVGL/ESP-IDF user B,G,R
        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++)
        {
            *pixel++ = blue; // blue
            *pixel++ = green; // Green
            *pixel++ = red; // red
        }
    }

    ESP_RETURN_ON_ERROR(display_panel_draw_bitmap(lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                                  DISPLAY_PANEL_V_RES, frame_buffer),
                        TAG, "draw RGB test pattern failed");
    ESP_LOGI(TAG, "R-G-B test pattern displayed for 2 seconds");
    return ESP_OK;
}

static esp_err_t draw_embedded_jpeg(display_panel_t *lcd)
{
    const uint8_t *jpg_data = image_jpg_start;
    const size_t jpg_size = image_jpg_end - image_jpg_start;
    ESP_RETURN_ON_FALSE(jpg_size > 0, ESP_ERR_INVALID_SIZE, TAG, "empty embedded JPG");

    jpeg_decode_picture_info_t pic_info = {};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(jpg_data, jpg_size, &pic_info), TAG, "get JPG info failed");
    ESP_LOGI(TAG, "Embedded JPG: %lux%lu, %u bytes", pic_info.width, pic_info.height, (unsigned)jpg_size);

    ESP_RETURN_ON_FALSE(pic_info.width == DISPLAY_PANEL_H_RES &&
                            pic_info.height == DISPLAY_PANEL_V_RES,
                        ESP_ERR_INVALID_SIZE, TAG, "JPG must be %dx%d for full-screen display",
                        DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_buf_size = 0;
    uint8_t *in_buf = jpeg_alloc_decoder_mem(jpg_size, &in_mem_cfg, &in_buf_size);
    ESP_RETURN_ON_FALSE(in_buf != NULL, ESP_ERR_NO_MEM, TAG, "alloc JPG input buffer failed");
    memcpy(in_buf, jpg_data, jpg_size);

    const size_t out_size_expected = DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * 3;
    uint8_t *out_buf = display_panel_get_next_frame_buffer(lcd);
    if (out_buf == NULL)
    {
        free(in_buf);
        ESP_LOGE(TAG, "LCD framebuffer is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t decode_engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    esp_err_t ret = jpeg_new_decoder_engine(&decode_engine_cfg, &decoder);
    if (ret != ESP_OK)
    {
        free(in_buf);
        ESP_LOGE(TAG, "create JPG decoder failed");
        return ret;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(decoder, &decode_cfg, in_buf, jpg_size,
                               out_buf, out_size_expected, &decoded_size);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "JPG decoded: %lu bytes", decoded_size);
        if (decoded_size < out_size_expected)
        {
            ESP_LOGE(TAG, "incomplete JPG output: %lu/%u bytes", decoded_size,
                     (unsigned)out_size_expected);
            ret = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            ret = display_panel_draw_bitmap(lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                            DISPLAY_PANEL_V_RES, out_buf);
        }
    }

    esp_err_t del_ret = jpeg_del_decoder_engine(decoder);
    if (ret == ESP_OK)
    {
        ret = del_ret;
    }
    free(in_buf);
    return ret;
}

void app_main(void)
{
    static t_panel_p4_bsp_t bsp;
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_io_expander_handle_t expander = NULL;

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
    i2c_bus = t_panel_p4_bsp_get_i2c_bus(&bsp);

#if CONFIG_T_PANEL_P4_HAS_XL9555
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));
#endif

    display_panel_t lcd = {};
    ESP_ERROR_CHECK(display_panel_init(&lcd, expander));

    touch_panel_t touch = {};
    bool touch_ready = false;
    esp_err_t touch_ret = touch_panel_init(&touch, i2c_bus, expander);
    if (touch_ret == ESP_OK)
    {
        touch_ready = true;
        ESP_LOGI(TAG, "Touch panel initialized");
    }
    else if (touch_ret == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "Touch controller is not configured for this board");
    }
    else
    {
        ESP_LOGE(TAG, "Touch panel initialization failed: %s", esp_err_to_name(touch_ret));
        scan_i2c_bus(i2c_bus, "after touch init failure");
    }

    // 5. Turn on backlight (50% brightness)
    ESP_ERROR_CHECK(display_panel_backlight_init());
    ESP_ERROR_CHECK(display_panel_set_brightness(100));

    // 6. Display the RGB test pattern for 2 seconds.
    // ESP_ERROR_CHECK(show_rgb_test_pattern(&lcd));
    show_color_test_pattern(&lcd, 0xFF, 0x00, 0x00); // Red
    vTaskDelay(pdMS_TO_TICKS(2000));
    show_color_test_pattern(&lcd, 0x00, 0xFF, 0x00); // Green
    vTaskDelay(pdMS_TO_TICKS(2000));
    show_color_test_pattern(&lcd, 0x00, 0x00, 0xFF); // Blue
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 7. Decode the embedded JPG and draw it full-screen.
    ESP_ERROR_CHECK(draw_embedded_jpeg(&lcd));
    ESP_LOGI(TAG, "JPG image displayed");

    while (1)
    {
        if (touch_ready)
        {
            touch_panel_data_t touch_data = {};
            esp_err_t ret = touch_panel_get_multiple_points(&touch, &touch_data);
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "Touch points: %u", touch_data.finger_count);
                for (uint8_t i = 0; i < touch_data.finger_count; ++i)
                {
                    ESP_LOGI(TAG, "  [%u] x=%u, y=%u, strength=%u", i,
                             touch_data.points[i].x,
                             touch_data.points[i].y,
                             touch_data.points[i].strength);
                }
            }
            else if (ret != ESP_ERR_NOT_FOUND)
            {
                ESP_LOGW(TAG, "Read touch failed: %s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
