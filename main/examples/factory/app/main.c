


/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <dirent.h>
#include "sdkconfig.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "bmu.h"
#include "button.h"
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
#include "esp32c5_sdio_slave.h"
#endif
#if CONFIG_T_PANEL_P4_HAS_LORA
#include "lora.h"
#include "lora_app.h"
#endif
#include "storage.h"
#include "lcd.h"
#include "system.h"
#include "start_page.h"
#include "audio.h"
#include "camera.h"
#if CONFIG_T_PANEL_P4_BOARD_RECT
#include "motion_sensor.h"
#include "haptic_motor.h"
#endif
#include "esp_io_expander.h"
#include "t_panel_p4_bsp.h"
#include "ui.h"
#include "lvgl_page_manager.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"

#include "board_config.h"

static const char *TAG = "factory_app";

#define VIDEO_PLAYER_TASK_STACK_SIZE 12288
#define VIDEO_PLAYER_MAX_DRAW_W 640
#define VIDEO_PLAYER_MAX_DRAW_H 360
#define VIDEO_PLAYER_DRAW_CHUNK_LINES 16
#define VIDEO_PLAYER_MAX_SAMPLE_BYTES (1024 * 1024)
#define VIDEO_PLAYER_DEFAULT_FRAME_US 33333

#define WIFI_SSID "your_ssid"
#define WIFI_PASSWORD "your_password"

static t_panel_p4_bsp_t s_bsp;
static i2c_master_bus_handle_t i2c_bus = NULL;
static esp_io_expander_handle_t io_expander = NULL;
init_error_t s_init_error = INIT_ERROR_NONE;

static void factory_init_task(void *arg)
{
    bool init_ok = (bmu_init(i2c_bus) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_BMU_ERROR;
    }

#if CONFIG_T_PANEL_P4_HAS_LORA
    init_ok = (lora_init(io_expander) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_LORA_ERROR;
    }
#endif

    init_ok = (audio_init(&s_bsp) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_AUDIO_ERROR;
    }

#if CONFIG_T_PANEL_P4_HAS_ESP32C5
    esp32c5_sdio_slave_config_t c5_config = ESP32C5_SDIO_SLAVE_CONFIG_DEFAULT();
    c5_config.enable_gpio = (gpio_num_t)ESP32C5_EN;
    c5_config.initial_ssid = "xinyuandianzi";
    c5_config.initial_password = "AA15994823428";
    c5_config.initial_authmode = WIFI_AUTH_WPA_WPA2_PSK;
    init_ok = (esp32c5_sdio_slave_init(&c5_config) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_SLAVE_ERROR;
    }
    else
    {
        esp_err_t time_ret = factory_time_sync_start();
        if (time_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Start network time sync failed: %s",
                     esp_err_to_name(time_ret));
        }
    }
#endif

    init_ok = (camera_init(&s_bsp, lcd_get_display()) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_CAMERA_ERROR;
    }

    init_ok = (storage_init(io_expander) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_SD_ERROR;
    }

#if CONFIG_T_PANEL_P4_BOARD_RECT
    init_ok = (motion_sensor_init(i2c_bus) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_SENSOR_ERROR;
    }

    init_ok = (haptic_motor_init(i2c_bus) == ESP_OK);
    if (!init_ok)
    {
        s_init_error |= INIT_MOTOR_ERROR;
    }
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&s_bsp));
    i2c_bus = t_panel_p4_bsp_get_i2c_bus(&s_bsp);
    io_expander = t_panel_p4_bsp_get_io_expander(&s_bsp);
    ESP_ERROR_CHECK(lcd_init(i2c_bus, io_expander));
    ESP_ERROR_CHECK(button_init());

    display_panel_t *lcd = lcd_get_display();
    
    create_page_ui();
    ESP_ERROR_CHECK(ui_init());
    xTaskCreate(factory_init_task, "factory_init", 8192, NULL, 5, NULL);

    // while (1)
    // {
    // ESP_LOGI(TAG, "SRAM free/min/largest: %u/%u/%u KB",
    //          heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
    //          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
    //          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    // ESP_LOGI(TAG, "PSRAM free/largest: %u/%u KB",
    //          heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
    //          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
    // vTaskDelay(5000 / portTICK_PERIOD_MS);
    // }
}
