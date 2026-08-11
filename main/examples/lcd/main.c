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

#include "T_Panle_P4_board_config.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/jpeg_decode.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "lcd_jd9365_driver.h"
#include "lcd_jd9365_touch.h"

static const char *TAG = "lcd_example";

extern const uint8_t image_jpg_start[] asm("_binary_wallhaven_8gr6l2_resized_jpg_start");
extern const uint8_t image_jpg_end[] asm("_binary_wallhaven_8gr6l2_resized_jpg_end");

static esp_err_t draw_embedded_jpeg(lcd_driver_t *lcd)
{
    const uint8_t *jpg_data = image_jpg_start;
    const size_t jpg_size = image_jpg_end - image_jpg_start;
    ESP_RETURN_ON_FALSE(jpg_size > 0, ESP_ERR_INVALID_SIZE, TAG, "empty embedded JPG");

    jpeg_decode_picture_info_t pic_info = {};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(jpg_data, jpg_size, &pic_info), TAG, "get JPG info failed");
    ESP_LOGI(TAG, "Embedded JPG: %lux%lu, %u bytes", pic_info.width, pic_info.height, (unsigned)jpg_size);

    ESP_RETURN_ON_FALSE(pic_info.width == LCD_H_RES && pic_info.height == LCD_V_RES,
                        ESP_ERR_INVALID_SIZE, TAG, "JPG must be %dx%d for full-screen display", LCD_H_RES, LCD_V_RES);

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    size_t in_buf_size = 0;
    uint8_t *in_buf = jpeg_alloc_decoder_mem(jpg_size, &in_mem_cfg, &in_buf_size);
    ESP_RETURN_ON_FALSE(in_buf != NULL, ESP_ERR_NO_MEM, TAG, "alloc JPG input buffer failed");
    memcpy(in_buf, jpg_data, jpg_size);

    const size_t out_size_expected = LCD_H_RES * LCD_V_RES * 3;
    size_t out_buf_size = 0;
    uint8_t *out_buf = jpeg_alloc_decoder_mem(out_size_expected, &out_mem_cfg, &out_buf_size);
    if (out_buf == NULL) {
        free(in_buf);
        ESP_LOGE(TAG, "alloc JPG output buffer failed");
        return ESP_ERR_NO_MEM;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t decode_engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    esp_err_t ret = jpeg_new_decoder_engine(&decode_engine_cfg, &decoder);
    if (ret != ESP_OK) {
        free(out_buf);
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
    ret = jpeg_decoder_process(decoder, &decode_cfg, in_buf, jpg_size, out_buf, out_buf_size, &decoded_size);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "JPG decoded: %lu bytes", decoded_size);
        ret = lcd_jd9365_draw_bitmap(lcd, 0, 0, LCD_H_RES, LCD_V_RES, out_buf);
    }

    esp_err_t del_ret = jpeg_del_decoder_engine(decoder);
    if (ret == ESP_OK) {
        ret = del_ret;
    }
    free(out_buf);
    free(in_buf);
    return ret;
}

static volatile bool touch_irq_flag = false;

static void IRAM_ATTR touch_isr(void *arg)
{
    touch_irq_flag = true;
}

void app_main(void)
{
    // 1. Init I2C bus
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    // 2. Init XL9555 IO expander
    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));

    // 3. Init LCD
    lcd_driver_t lcd = {};
    ESP_ERROR_CHECK(lcd_jd9365_init(&lcd, expander));

    // 4. Turn on backlight (50% brightness)
    ESP_ERROR_CHECK(lcd_backlight_init());
    ESP_ERROR_CHECK(lcd_backlight_set_brightness(50));

    // 5. Init touch
    touch_handle_t touch = {};
    ESP_ERROR_CHECK(touch_init(&touch, i2c_bus, expander));

    // 6. Decode the embedded JPG and draw it full-screen.
    ESP_ERROR_CHECK(draw_embedded_jpeg(&lcd));
    ESP_LOGI(TAG, "JPG image displayed");

    // 7. Configure TOUCH_INT (GPIO6) as input with pull-up to monitor interrupt line
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT, touch_isr, NULL));

    // 8. Touch loop: read touch data when interrupt fires
    touch_data_t td;
    while (1)
    {
        if (touch_irq_flag)
        {
            touch_irq_flag = false;
            // if (touch_get_single_point(&touch, &td, 1) == ESP_OK)
            // {
            //     ESP_LOGI(TAG, "Touch[0]: x=%d, y=%d,pressure=%d", , td.points[0].x, td.points[0].y,td.points[0].pressure);
            // }

            if (touch_get_multiple_points(&touch, &td) == ESP_OK)
            {
                for (int i = 0; i < td.finger_count; i++)
                {
                    ESP_LOGI(TAG, "MultiTouch[%d]: x=%d, y=%d,pressure=%d", i, td.points[i].x, td.points[i].y, td.points[i].pressure);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
