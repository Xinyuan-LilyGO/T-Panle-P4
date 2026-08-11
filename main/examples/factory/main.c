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
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
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
#include "axp517.h"
#include "driver/i2c_master.h"
#include "audio.h"
#include "camera.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "ui.h"
#include "lcd_jd9365_driver.h"
#include "lcd_jd9365_touch.h"
#include "lvgl_page_manager.h"
#include "lora_app.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "eh_host_feat_wifi.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "T_Panle_P4_board_config.h"

static const char *TAG = "factory_example";

#define MOUNT_POINT "/sdcard"
#define SD_APP_MAX_FILES 5
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_BUFFER_LINES 72

#define BMU_STARTUP_CHARGE_CURRENT_MA 512
#define BMU_NORMAL_INPUT_CURRENT_MA 1500
#define WIFI_STARTUP_MAX_TX_POWER_QDBM 64

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SCAN_DONE_BIT BIT1

#define VIDEO_PLAYER_TASK_STACK_SIZE 12288
#define VIDEO_PLAYER_MAX_DRAW_W 640
#define VIDEO_PLAYER_MAX_DRAW_H 360
#define VIDEO_PLAYER_DRAW_CHUNK_LINES 16
#define VIDEO_PLAYER_MAX_SAMPLE_BYTES (1024 * 1024)
#define VIDEO_PLAYER_DEFAULT_FRAME_US 33333
static i2c_master_bus_handle_t i2c_bus = NULL;
static esp_io_expander_handle_t io_expander = NULL;
static lcd_driver_t s_lcd;
static touch_handle_t s_touch;
static axp517_handle_t axp;

static bool s_uart_to_c5 = false;
static bool s_time_sync_task_started = false;
static bool s_bmu_ready = false;
static bool s_bmu_charger_enabled = false;
static bool s_bmu_battery_detection_enabled = false;
static bool s_bmu_fuel_gauge_enabled = false;
static bool s_bmu_bc12_enabled = false;
static bool s_wifi_connected = false;
static volatile bool s_wifi_manual_scan = false;
static volatile bool s_wifi_manual_connect = false;
static bool s_bluetooth_enabled =
#if defined(CONFIG_BT_ENABLED) || defined(CONFIG_ESP_HOSTED_HOST_FEAT_BT)
    true;
#else
    false;
#endif
static bool s_bluetooth_connected = false;
static volatile uint32_t s_lcd_flush_count = 0;
static EventGroupHandle_t s_wifi_event_group;
static sdmmc_card_t *s_sd_card = NULL;
static tinyusb_msc_storage_handle_t s_msc_storage_handle = NULL;
static SemaphoreHandle_t s_storage_mutex = NULL;
static bool s_usb_msc_mode = false;
static volatile bool s_msc_storage_event_failed = false;

volatile bool irq_flag;
init_error_t s_init_error = INIT_ERROR_NONE;

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum
{
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum
{
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN = 0x81,
};

static const tusb_desc_device_t s_msc_device_desc = {
    .bLength = sizeof(s_msc_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_msc_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if TUD_OPT_HIGH_SPEED
static const tusb_desc_device_qualifier_t s_msc_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

static const uint8_t s_msc_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif

static const char *s_msc_string_desc[] = {
    (const char[]){0x09, 0x04},
    "LILYGO",
    "T-Panel-P4 SD MSC",
    "TPANELP4MSC",
    "SD Card",
};

static esp_err_t board_i2c_init(i2c_master_bus_handle_t *ret_i2c_bus)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, ret_i2c_bus), TAG, "I2C init failed");
    return ESP_OK;
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_data_t touch_data = {0};

    if (touch_get_multiple_points(&s_touch, &touch_data) == ESP_OK && touch_data.finger_count > 0)
    {
        data->point.x = touch_data.points[0].x;
        data->point.y = touch_data.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    s_lcd_flush_count++;

    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    esp_err_t ret = lcd_jd9365_draw_bitmap(&s_lcd, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(ret));
    }

    lv_display_flush_ready(disp);
}

static esp_err_t lvgl_port_init(void)
{
    lv_init();

    const size_t draw_buf_size = (size_t)LCD_H_RES * LVGL_BUFFER_LINES * (LCD_BIT_PER_PIXEL / 8);
    void *draw_buf_1 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *draw_buf_2 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *draw_buf_mem = "PSRAM";
    if (!draw_buf_1 || !draw_buf_2)
    {
        if (draw_buf_1)
        {
            heap_caps_free(draw_buf_1);
        }
        if (draw_buf_2)
        {
            heap_caps_free(draw_buf_2);
        }
        draw_buf_1 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        draw_buf_2 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        draw_buf_mem = "internal SRAM";
    }
    ESP_RETURN_ON_FALSE(draw_buf_1 && draw_buf_2, ESP_ERR_NO_MEM, TAG, "LVGL draw buffer alloc failed");
    ESP_LOGI(TAG, "LVGL draw buffers: %u bytes x2, %s", (unsigned)draw_buf_size, draw_buf_mem);

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_NO_MEM, TAG, "LVGL display create failed");
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
    lv_display_set_buffers(display, draw_buf_1, draw_buf_2, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display);

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_NO_MEM, TAG, "LVGL input device create failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    lv_indev_set_gesture_min_distance(indev, 160); // 默认 50，越大越不容易触发
    lv_indev_set_gesture_min_velocity(indev, 10);  // 默认 3，越大要求滑动越快

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create LVGL tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "start LVGL tick failed");

    return ESP_OK;
}

static void init_ldo(void)
{
    esp_ldo_channel_handle_t ldo_handle = NULL;
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = 4,
        .voltage_mv = 3300,
        .flags.adjustable = true,
    };
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_config, &ldo_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to acquire LDO channel: %s", esp_err_to_name(ret));
    }
}

static esp_err_t sdmmc_card_storage_init(sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(out_card != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid SD card pointer");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD1_CLK;
    slot_config.cmd = SD1_CMD;
    slot_config.d0 = SD1_D0;
    slot_config.d1 = SD1_D1;
    slot_config.d2 = SD1_D2;
    slot_config.d3 = SD1_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_NO_MEM, TAG, "No memory for sdmmc_card_t");

    esp_err_t ret = (*host.init)();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        free(card);
        return ret;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        (*host.deinit)();
        free(card);
        return ret;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG)
        {
            host.deinit_p(host.slot);
        }
        else
        {
            (*host.deinit)();
        }
        free(card);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    *out_card = card;
    return ESP_OK;
}

static void msc_mount_changed_cb(tinyusb_msc_storage_handle_t handle,
                                 tinyusb_msc_event_t *event,
                                 void *arg)
{
    (void)handle;
    (void)arg;

    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE)
    {
        ESP_LOGI(TAG, "MSC storage switched to %s",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB host" : "application");
    }
    else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
             event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
             event->id == TINYUSB_MSC_EVENT_FORMAT_FAILED)
    {
        s_msc_storage_event_failed = true;
        ESP_LOGW(TAG, "MSC storage switch failed, event=%d", event->id);
    }
}

static bool storage_app_mount_is_ready(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL)
    {
        return false;
    }

    closedir(dir);
    return true;
}

static void storage_heap_log(const char *stage)
{
    ESP_LOGI(TAG,
             "Storage heap %s: internal free/largest=%u/%u KB, PSRAM free/largest=%u/%u KB",
             stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static esp_err_t sd_init(void)
{
    init_ldo();
    uint32_t pin_mask = (1UL << XL9555_SD_VDD_EN);
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, pin_mask, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, pin_mask, 1));
    vTaskDelay(pdMS_TO_TICKS(200)); // Wait for SD card power stabilize

    if (s_storage_mutex == NULL)
    {
        s_storage_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_storage_mutex != NULL, ESP_ERR_NO_MEM, TAG, "Create storage mutex failed");
    }

    ESP_LOGI(TAG, "Initializing SD card storage");
    ESP_RETURN_ON_ERROR(sdmmc_card_storage_init(&s_sd_card), TAG, "SD card storage init failed");

    tinyusb_msc_driver_config_t msc_driver_cfg = {
        .user_flags = {
            .auto_mount_off = true,
        },
        .callback = msc_mount_changed_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&msc_driver_cfg), TAG, "MSC driver install failed");

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.card = s_sd_card,
        .fat_fs = {
            .base_path = MOUNT_POINT,
            .config = {
                .format_if_mount_failed = false,
                .max_files = SD_APP_MAX_FILES,
                .allocation_unit_size = 16 * 1024,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc_storage_handle),
                        TAG,
                        "MSC SD storage init failed");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_msc_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_config_desc;
    tusb_cfg.descriptor.string = s_msc_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_msc_string_desc) / sizeof(s_msc_string_desc[0]);
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.high_speed_config = s_msc_hs_config_desc;
    tusb_cfg.descriptor.qualifier = &s_msc_device_qualifier;
#endif
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB driver install failed");

    s_usb_msc_mode = false;
    ESP_LOGI(TAG, "SD card mounted for APP at %s, USB MSC ready", MOUNT_POINT);
    return ESP_OK;
}

bool factory_usb_otg_msc_set(bool usb_mode)
{
    if (s_storage_mutex == NULL || s_msc_storage_handle == NULL)
    {
        ESP_LOGW(TAG, "USB MSC storage is not ready");
        return false;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "USB MSC switch timeout");
        return false;
    }

    tinyusb_msc_mount_point_t mount_point = usb_mode ? TINYUSB_MSC_STORAGE_MOUNT_USB
                                                     : TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (usb_mode)
    {
        factory_audio_stop_music();
    }

    ESP_LOGI(TAG, "Switch USB MSC storage to %s", usb_mode ? "USB host" : "APP /sdcard");
    storage_heap_log("before switch");
    s_msc_storage_event_failed = false;
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(s_msc_storage_handle, mount_point);
    storage_heap_log("after switch");
    if (ret == ESP_OK && s_msc_storage_event_failed)
    {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK && !usb_mode)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!storage_app_mount_is_ready())
        {
            ESP_LOGW(TAG, "APP /sdcard is not mounted after USB MSC switch");
            ret = ESP_ERR_INVALID_STATE;
        }
    }

    if (ret == ESP_OK)
    {
        s_usb_msc_mode = usb_mode;
        camera_page_set_storage_ready(!usb_mode);
    }
    else
    {
        ESP_LOGW(TAG, "Switch USB MSC storage failed: %s", esp_err_to_name(ret));
        if (!usb_mode)
        {
            s_msc_storage_event_failed = false;
            (void)tinyusb_msc_set_storage_mount_point(s_msc_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
        }
    }

    xSemaphoreGive(s_storage_mutex);
    return ret == ESP_OK;
}

bool factory_usb_otg_msc_is_usb_mode(void)
{
    return s_usb_msc_mode;
}

bool factory_storage_info_get(factory_storage_info_t *info)
{
    if (info == NULL)
    {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->usb_mode = s_usb_msc_mode;
    snprintf(info->bus, sizeof(info->bus), "%s", s_usb_msc_mode ? "USB MSC" : "SDMMC 4-bit");

    if (s_sd_card != NULL)
    {
        uint64_t card_bytes = (uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size;
        const char *type = "SDSC";
        if (card_bytes >= (uint64_t)32 * 1024 * 1024 * 1024)
        {
            type = "SDXC";
        }
        else if (card_bytes >= (uint64_t)4 * 1024 * 1024 * 1024)
        {
            type = "SDHC";
        }
        snprintf(info->type, sizeof(info->type), "%s", type);
    }
    else
    {
        snprintf(info->type, sizeof(info->type), "NO CARD");
    }

    if (s_usb_msc_mode || !storage_app_mount_is_ready())
    {
        info->app_mounted = false;
        return true;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Read SD storage info failed: %s", esp_err_to_name(ret));
        info->app_mounted = false;
        return false;
    }

    info->app_mounted = true;
    info->total_bytes = total_bytes;
    info->free_bytes = free_bytes;
    return true;
}

static esp_err_t bmu_init(void)
{
    esp_err_t ret = axp517_init(&axp, i2c_bus, AXP517_I2C_ADDR);
    if (ret != ESP_OK)
    {
        ESP_LOGD(TAG, "AXP517 init failed\n");
        return ret;
    }
    ESP_LOGI(TAG, "AXP517 ready!\n");
    s_bmu_ready = true;

    // 使能所有 ADC 通道
    axp517_enable_adc_channels(&axp, AXP517_ADC_ALL_EN);
    s_bmu_fuel_gauge_enabled = (axp517_enable_fuel_gauge(&axp, true) == ESP_OK);
    axp517_set_low_battery_threshold(&axp, 10, 3);

    s_bmu_bc12_enabled = (axp517_enable_bc12_detect(&axp, true) == ESP_OK);
    axp517_enable_typec_detect(&axp, true);
    axp517_bc12_enable_auto_dpdm(&axp, true);

    // 设置充电参数
    axp517_set_input_current_limit(&axp, BMU_NORMAL_INPUT_CURRENT_MA);
    axp517_set_input_voltage_limit(&axp, 4600);
    axp517_set_vsys_min(&axp, 3500);
    axp517_set_charge_voltage(&axp, 4200);
    axp517_set_charge_current(&axp, BMU_STARTUP_CHARGE_CURRENT_MA);
    s_bmu_charger_enabled = (axp517_enable_charger(&axp, true) == ESP_OK);

    s_bmu_battery_detection_enabled = (axp517_enable_battery_detection(&axp, true) == ESP_OK);
    axp517_irq_clear_all(&axp);
    ESP_LOGI(TAG, "BMU charger current limited to %dmA for startup", BMU_STARTUP_CHARGE_CURRENT_MA);
    return ESP_OK;
}

static void set_lora_falg(lora_app_irq_event_t event, void *user_data)
{
    irq_flag = true;
}

static esp_err_t lora_init(void)
{
    const uint32_t pin_mask = (1UL << XL9555_LORA_PWR_EN);
    ESP_RETURN_ON_FALSE(io_expander != NULL, ESP_ERR_INVALID_STATE, TAG, "XL9555 not initialized");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, pin_mask, IO_EXPANDER_OUTPUT),
                        TAG,
                        "set LoRa power pin direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, pin_mask, 1),
                        TAG,
                        "enable LoRa power failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "LoRa power enabled via XL9555 pin %d", XL9555_LORA_PWR_EN);

    int state = lora_app_start();
    if (state != ESP_OK)
    {
        ESP_LOGE(TAG, "LORA init failed, code %d", state);
        return ESP_FAIL;
    }
    irq_flag = false;
    lora_app_set_irq_callback(set_lora_falg, NULL);

    lora_app_set_crc(false);
    lora_app_set_invert_iq(false);
    lora_app_set_current_limit(240.0f);

    char lora_chip[32];
    if (lora_app_get_chip() == LORA_APP_CHIP_SX1262)
    {
        snprintf(lora_chip, sizeof(lora_chip), "SX1262");
    }
    else if (lora_app_get_chip() == LORA_APP_CHIP_SX1276)
    {
        snprintf(lora_chip, sizeof(lora_chip), "SX1276");
    }
    else if (lora_app_get_chip() == LORA_APP_CHIP_LR1121)
    {
        snprintf(lora_chip, sizeof(lora_chip), "LR11121");
    }
    else if (lora_app_get_chip() == LORA_APP_CHIP_LR2021)
    {
        snprintf(lora_chip, sizeof(lora_chip), "LR2021");
    }
    ESP_LOGI(TAG, "LORA init %s suceess!\n", lora_chip);
    return ESP_OK;
}

static void time_sync_task(void *arg)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK)
    {
        ESP_LOGW(TAG, "SNTP sync timeout, retrying");
    }

    ESP_LOGI(TAG, "SNTP time synchronized");

    time_t now = 0;
    struct tm timeinfo = {0};
    char strftime_buf[64] = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC+8", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);

    vTaskDelete(NULL);
}

static const char *wifi_auth_type_name(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "SEC";
    }
}

static void wifi_remote_set_error(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0)
    {
        snprintf(err, err_len, "%s", msg ? msg : "");
    }
}

static void wifi_prepare_scan(void)
{
#if CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G || CONFIG_SOC_WIFI_SUPPORT_5G
    esp_err_t ret;
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
        .wifi_5g_channel_mask = WIFI_CHANNEL_36 |
                                 WIFI_CHANNEL_40 |
                                 WIFI_CHANNEL_44 |
                                 WIFI_CHANNEL_48 |
                                 WIFI_CHANNEL_149 |
                                 WIFI_CHANNEL_153 |
                                 WIFI_CHANNEL_157 |
                                 WIFI_CHANNEL_161 |
                                 WIFI_CHANNEL_165,
    };
    ret = esp_wifi_set_country(&country);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set Wi-Fi country failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set Wi-Fi 2.4G+5G band mode failed: %s", esp_err_to_name(ret));
    }
#endif
}

static void wifi_scan_record_store(wifi_scan_ap_info_t *aps, int max_count, int *count, const wifi_ap_record_t *record)
{
    if (aps == NULL || count == NULL || record == NULL || record->ssid[0] == '\0')
    {
        return;
    }

    for (int i = 0; i < *count; i++)
    {
        if (strncmp(aps[i].ssid, (const char *)record->ssid, sizeof(aps[i].ssid)) == 0)
        {
            if (record->rssi > aps[i].rssi)
            {
                aps[i].rssi = record->rssi;
                snprintf(aps[i].type,
                         sizeof(aps[i].type),
                         "%s/%s",
                         record->primary > 14 ? "5G" : "2.4G",
                         wifi_auth_type_name(record->authmode));
                aps[i].authmode = record->authmode;
            }
            return;
        }
    }

    int dst = *count;
    if (dst >= max_count)
    {
        int weakest = 0;
        for (int i = 1; i < max_count; i++)
        {
            if (aps[i].rssi < aps[weakest].rssi)
            {
                weakest = i;
            }
        }
        if (record->rssi <= aps[weakest].rssi)
        {
            return;
        }
        dst = weakest;
    }
    else
    {
        (*count)++;
    }

    snprintf(aps[dst].ssid,
             sizeof(aps[dst].ssid),
             "%.32s",
             (const char *)record->ssid);
    aps[dst].rssi = record->rssi;
    snprintf(aps[dst].type,
             sizeof(aps[dst].type),
             "%s/%s",
             record->primary > 14 ? "5G" : "2.4G",
             wifi_auth_type_name(record->authmode));
    aps[dst].authmode = record->authmode;
}

bool wifi_remote_scan(wifi_scan_ap_info_t *aps, int max_count, int *out_count, char *err, size_t err_len)
{
    if (out_count)
    {
        *out_count = 0;
    }
    if (aps == NULL || max_count <= 0 || out_count == NULL)
    {
        wifi_remote_set_error(err, err_len, "invalid argument");
        return false;
    }

    s_wifi_manual_scan = true;
    (void)esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_wifi_event_group)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    }

    wifi_prepare_scan();
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 20,
        .scan_time.active.max = 80,
    };

    int ap_count = 0;
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret == ESP_ERR_WIFI_STATE)
    {
        ESP_LOGW(TAG, "Wi-Fi scan busy, retry once");
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_wifi_scan_start(&scan_config, false);
    }

    if (ret == ESP_OK)
    {
        if (s_wifi_event_group == NULL)
        {
            ret = ESP_ERR_INVALID_STATE;
        }
        else
        {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_SCAN_DONE_BIT,
                                                   pdTRUE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(30000));
            if ((bits & WIFI_SCAN_DONE_BIT) == 0)
            {
                ESP_LOGW(TAG, "Wi-Fi async scan timeout");
                (void)esp_wifi_scan_stop();
                ret = ESP_ERR_TIMEOUT;
            }
        }
    }

    if (ret == ESP_OK)
    {
        uint16_t ap_num = max_count > 32 ? 32 : (uint16_t)max_count;
        wifi_ap_record_t *records = heap_caps_calloc(ap_num, sizeof(wifi_ap_record_t), MALLOC_CAP_DEFAULT);
        if (records == NULL)
        {
            ret = ESP_ERR_NO_MEM;
        }
        else
        {
            ret = esp_wifi_scan_get_ap_records(&ap_num, records);
            if (ret == ESP_OK)
            {
                for (int i = 0; i < (int)ap_num; i++)
                {
                    wifi_scan_record_store(aps, max_count, &ap_count, &records[i]);
                }
            }
            heap_caps_free(records);
        }
    }
    s_wifi_manual_scan = false;

    if (ret != ESP_OK && ap_count > 0)
    {
        ESP_LOGW(TAG, "Wi-Fi scan partial result: %d AP, last error: %s", ap_count, esp_err_to_name(ret));
        ret = ESP_OK;
    }

    if (ret != ESP_OK)
    {
        wifi_remote_set_error(err, err_len, esp_err_to_name(ret));
        return false;
    }

    *out_count = ap_count;
    wifi_remote_set_error(err, err_len, "");
    return true;
}

bool wifi_remote_connect(const char *ssid, const char *password, int authmode, char *err, size_t err_len)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        wifi_remote_set_error(err, err_len, "SSID is empty");
        return false;
    }

    s_wifi_manual_connect = true;
    s_wifi_connected = false;
    if (s_wifi_event_group)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    wifi_prepare_scan();
    (void)esp_wifi_scan_stop();
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_config_t wifi_config = {0};
    size_t ssid_len = strnlen(ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    if (password != NULL)
    {
        size_t pass_len = strnlen(password, sizeof(wifi_config.sta.password) - 1U);
        memcpy(wifi_config.sta.password, password, pass_len);
    }
    wifi_config.sta.threshold.authmode = authmode == WIFI_AUTH_OPEN ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK)
    {
        ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN)
        {
            ret = ESP_OK;
        }
    }

    if (ret != ESP_OK)
    {
        s_wifi_manual_connect = false;
        wifi_remote_set_error(err, err_len, esp_err_to_name(ret));
        return false;
    }

    wifi_remote_set_error(err, err_len, "");
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "Wi-Fi started, connecting");
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "Wi-Fi start connect failed: %s", esp_err_to_name(ret));
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "Wi-Fi connected, waiting for IP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        ESP_LOGI(TAG, "Wi-Fi scan done");
        if (s_wifi_event_group)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_wifi_manual_scan || s_wifi_manual_connect)
        {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, reconnect paused", event->reason);
            return;
        }

        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, retrying", event->reason);
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(ret));
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_manual_connect = false;    
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_time_sync_task_started)
        {
            s_time_sync_task_started = true;
            xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 5, NULL);
        }
    }
}

static esp_err_t esp32c5_slave_init(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, 1 << XL9555_UART_SEL, IO_EXPANDER_OUTPUT),
                        TAG, "set XL9555_UART_SEL output failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, 1 << XL9555_UART_SEL, s_uart_to_c5),
                        TAG, "switch UART mux to P4 failed");

    esp_err_t ret = esp_hosted_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init ESP Hosted");
        return ret;
    }

    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to connect to ESP32-C5 slave");
        esp_hosted_deinit();
        return ret;
    }
#if !CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_AUTO_INIT
    if (eh_host_feat_wifi_init() != 0)
    {
        ESP_LOGE(TAG, "Failed to init ESP Hosted Wi-Fi feature");
        esp_hosted_deinit();
        return ESP_FAIL;
    }
#endif
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(esp_netif_init());                // Initialize ESP-Netif
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Create default event loop
    esp_netif_create_default_wifi_sta();              // Create default Wi-Fi station interface
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "xinyuandianzi",
            .password = "AA15994823428",
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t tx_power_ret = esp_wifi_set_max_tx_power(WIFI_STARTUP_MAX_TX_POWER_QDBM);
    if (tx_power_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set Wi-Fi max TX power failed: %s", esp_err_to_name(tx_power_ret));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static int percent_used(size_t free_size, size_t total_size)
{
    if (total_size == 0 || free_size > total_size)
    {
        return -1;
    }

    return (int)(((total_size - free_size) * 100) / total_size);
}

bool status_bar_info_get(status_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    uint8_t battery_percent = 0;
    axp517_status_t power_status = {0};
    bool battery_percent_valid = s_bmu_ready &&
                                 axp517_read_battery_percent(&axp, &battery_percent) == ESP_OK &&
                                 battery_percent <= 100;
    bool battery_charging = false;
    if (s_bmu_ready && axp517_get_status(&axp, &power_status) == ESP_OK)
    {
        bool charge_state =
            power_status.charge_status == AXP517_CHG_TRICKLE ||
            power_status.charge_status == AXP517_CHG_PRE ||
            power_status.charge_status == AXP517_CHG_CC ||
            power_status.charge_status == AXP517_CHG_CV;
        battery_charging = power_status.vbus_good &&
                           power_status.bat_current_dir == AXP517_BAT_CURRENT_CHARGE &&
                           charge_state;
    }

    *status = (status_info_t){
        .wifi_connected = s_wifi_connected,
        .bluetooth_enabled = s_bluetooth_enabled,
        .bluetooth_connected = s_bluetooth_connected,
        .battery_charging = battery_charging,
        .battery_percent = battery_percent_valid ? (int)battery_percent : -1,
    };
    return true;
}

bool bmu_charger_enable_set(bool enable)
{
    if (!s_bmu_ready)
    {
        return false;
    }

    uint8_t before = 0;
    uint8_t after = 0;
    axp517_read_byte(&axp, AXP517_REG_MOD_EN1, &before);

    esp_err_t ret = axp517_enable_charger(&axp, enable);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set BMU charger %s failed: %s", enable ? "ON" : "OFF", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    bool read_ok = axp517_read_byte(&axp, AXP517_REG_MOD_EN1, &after) == ESP_OK;
    bool hw_enabled = read_ok && ((after & AXP517_CHG_EN) != 0);
    s_bmu_charger_enabled = enable;

    ESP_LOGI(TAG,
             "BMU charger target=%s REG19 before=0x%02X after=%s0x%02X",
             enable ? "ON" : "OFF",
             before,
             read_ok ? "" : "read-failed:",
             after);

    if (read_ok && hw_enabled != enable)
    {
        ESP_LOGW(TAG,
                 "BMU charger readback mismatch: target=%s hw=%s",
                 enable ? "ON" : "OFF",
                 hw_enabled ? "ON" : "OFF");
    }

    return read_ok;
}

bool bmu_charge_current_set(int ma)
{
    if (!s_bmu_ready)
    {
        return false;
    }

    esp_err_t ret = axp517_set_charge_current(&axp, (uint16_t)ma);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set BMU charge current %dmA failed: %s", ma, esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool bmu_input_current_limit_set(int ma)
{
    if (!s_bmu_ready)
    {
        return false;
    }

    esp_err_t ret = axp517_set_input_current_limit(&axp, (uint16_t)ma);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set BMU input current limit %dmA failed: %s", ma, esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool bmu_low_battery_warn_set(int warn_percent)
{
    if (!s_bmu_ready)
    {
        return false;
    }

    esp_err_t ret = axp517_set_low_battery_threshold(&axp, (uint8_t)warn_percent, 3);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set BMU low battery warning %d%% failed: %s", warn_percent, esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool factory_power_enter_ship_mode(void)
{
    if (!s_bmu_ready)
    {
        ESP_LOGW(TAG, "Cannot enter ship mode: BMU is not ready");
        return false;
    }

    ESP_LOGW(TAG, "Entering AXP517 ship mode");
    lcd_backlight_set_brightness(SET_BRIGHTNESS_MIN);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = axp517_enter_ship_mode(&axp);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Enter ship mode failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

void factory_power_restart(void)
{
    ESP_LOGW(TAG, "Restart requested from settings page");
    lcd_backlight_set_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

bool factory_power_enter_low_power(void)
{
    bool ok = true;

    ESP_LOGI(TAG, "Entering low power profile");

    if (lcd_backlight_set_brightness(SET_BRIGHTNESS_MIN) != ESP_OK)
    {
        ok = false;
    }

    esp_err_t wifi_ret = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set Wi-Fi power save failed: %s", esp_err_to_name(wifi_ret));
    }

    wifi_ret = esp_wifi_set_max_tx_power(32);
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Limit Wi-Fi TX power failed: %s", esp_err_to_name(wifi_ret));
    }

    if (lora_app_is_started() && lora_app_sleep() != ESP_OK)
    {
        ESP_LOGW(TAG, "LoRa sleep request failed");
    }

    if (s_bmu_ready)
    {
        if (axp517_set_input_current_limit(&axp, BMU_STARTUP_CHARGE_CURRENT_MA) != ESP_OK)
        {
            ok = false;
        }
        if (axp517_set_charge_current(&axp, BMU_STARTUP_CHARGE_CURRENT_MA) != ESP_OK)
        {
            ok = false;
        }
        axp517_enable_chgled(&axp, false);
        s_bmu_fuel_gauge_enabled = (axp517_set_fuel_gauge_low_freq(&axp, true) == ESP_OK);
    }

    return ok;
}

bool factory_power_exit_low_power(void)
{
    bool ok = true;

    ESP_LOGI(TAG, "Leaving low power profile");

    esp_err_t wifi_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Disable Wi-Fi power save failed: %s", esp_err_to_name(wifi_ret));
    }

    wifi_ret = esp_wifi_set_max_tx_power(WIFI_STARTUP_MAX_TX_POWER_QDBM);
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Restore Wi-Fi TX power failed: %s", esp_err_to_name(wifi_ret));
    }

    if (lora_app_is_started() && lora_app_standby() != ESP_OK)
    {
        ESP_LOGW(TAG, "LoRa standby request failed");
    }

    if (s_bmu_ready)
    {
        if (axp517_set_fuel_gauge_low_freq(&axp, false) != ESP_OK)
        {
            ok = false;
        }
        else
        {
            s_bmu_fuel_gauge_enabled = true;
        }
        axp517_enable_chgled(&axp, true);
        if (axp517_set_input_current_limit(&axp, BMU_NORMAL_INPUT_CURRENT_MA) != ESP_OK)
        {
            ok = false;
        }
    }

    return ok;
}

bool bmu_status_info_get(bmu_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    *status = (bmu_info_t){
        .ready = s_bmu_ready,
        .charger_enabled = s_bmu_charger_enabled,
        .charger_hw_enabled = s_bmu_charger_enabled,
        .battery_detection_enabled = s_bmu_battery_detection_enabled,
        .fuel_gauge_enabled = s_bmu_fuel_gauge_enabled,
        .bc12_enabled = s_bmu_bc12_enabled,
        .bat_current_dir = -1,
        .battery_percent = -1,
        .battery_soh = -1,
        .charge_status = -1,
        .bc12_type = -1,
        .vbat_mv = -1,
        .vbus_mv = -1,
        .vsys_mv = -1,
        .ibus_ma = -1,
        .ichg_ma = -1,
        .idis_ma = -1,
        .ts_mv = -1,
        .die_temp_c_x10 = -1,
    };
    snprintf(status->fault_text, sizeof(status->fault_text), "%s", "Clear");

    if (!s_bmu_ready)
    {
        return false;
    }

    uint8_t mod1 = 0;
    if (axp517_read_byte(&axp, AXP517_REG_MOD_EN1, &mod1) == ESP_OK)
    {
        status->charger_hw_enabled = (mod1 & AXP517_CHG_EN) != 0;
    }

    axp517_status_t power_status = {0};
    if (axp517_get_status(&axp, &power_status) == ESP_OK)
    {
        status->bat_present = power_status.bat_present;
        status->bat_current_dir = (int)power_status.bat_current_dir;
        status->vindpm = power_status.vindpm;
        status->thermal_regulation = power_status.thermal_regulation;
        status->current_limit = power_status.current_limit;
        status->charge_status = (int)power_status.charge_status;
    }

    axp517_fault_t fault = {0};
    if (axp517_get_fault(&axp, &fault) == ESP_OK)
    {
        status->fault0 = fault.fault0;
        status->fault1 = fault.fault1;
    }
    axp517_get_fault_char(&axp, status->fault_text, sizeof(status->fault_text));

    axp517_bc_detect_t bc12 = AXP517_BC_UNKNOWN;
    if (axp517_get_bc_detect(&axp, &bc12) == ESP_OK)
    {
        status->bc12_type = (int)bc12;
    }

    uint8_t percent = 0;
    if (axp517_read_battery_percent(&axp, &percent) == ESP_OK && percent <= 100)
    {
        status->battery_percent = (int)percent;
    }

    uint8_t soh = 0;
    if (axp517_read_battery_soh(&axp, &soh) == ESP_OK && soh <= 100)
    {
        status->battery_soh = (int)soh;
    }

    uint16_t mv = 0;
    if (axp517_read_vbat(&axp, &mv) == ESP_OK)
    {
        status->vbat_mv = (int)mv;
    }
    if (axp517_read_vbus(&axp, &mv) == ESP_OK)
    {
        status->vbus_mv = (int)mv;
    }
    if (axp517_read_vsys(&axp, &mv) == ESP_OK)
    {
        status->vsys_mv = (int)mv;
    }
    if (axp517_read_ts_voltage(&axp, &mv) == ESP_OK)
    {
        status->ts_mv = (int)mv;
    }

    uint16_t ma = 0;
    if (axp517_read_ibus(&axp, &ma) == ESP_OK)
    {
        status->ibus_ma = (int)ma;
    }
    if (axp517_read_ibat_charge(&axp, &ma) == ESP_OK)
    {
        status->ichg_ma = (int)ma;
    }
    if (axp517_read_ibat_discharge(&axp, &ma) == ESP_OK)
    {
        status->idis_ma = (int)ma;
    }

    float die_temp = 0.0f;
    if (axp517_read_die_temperature(&axp, &die_temp) == ESP_OK)
    {
        status->die_temp_c_x10 = (int)(die_temp * 10.0f);
    }

    return true;
}

bool home_status_info_get(home_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    static bool s_status_inited = false;
    static uint32_t s_last_flush_count = 0;
    static int64_t s_last_sample_us = 0;

    int64_t now_us = esp_timer_get_time();
    if (!s_status_inited)
    {
        s_last_flush_count = s_lcd_flush_count;
        s_last_sample_us = now_us;
        s_status_inited = true;
    }

    uint32_t flush_count = s_lcd_flush_count;
    uint32_t flush_delta = flush_count - s_last_flush_count;
    int64_t elapsed_us = now_us - s_last_sample_us;
    int fps = -1;
    if (elapsed_us > 0)
    {
        fps = (int)((flush_delta * 1000000LL + elapsed_us / 2) / elapsed_us);
    }

    size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    int temp_c_x10 = -1;
    if (s_bmu_ready)
    {
        float die_temp = 0.0f;
        if (axp517_read_die_temperature(&axp, &die_temp) == ESP_OK)
        {
            temp_c_x10 = (int)(die_temp * 10.0f);
        }
    }

    *status = (home_info_t){
        .fps = fps,
        .sram_percent = percent_used(sram_free, sram_total),
        .psram_percent = percent_used(psram_free, psram_total),
        .sram_free_kb = (uint32_t)(sram_free / 1024),
        .psram_free_kb = (uint32_t)(psram_free / 1024),
        .temp_c_x10 = temp_c_x10,
    };
    s_last_flush_count = flush_count;
    s_last_sample_us = now_us;

    return true;
}

static void factory_init_task(void *arg)
{
    start_page_set_status("Init SD card...", 15);
    bool init_ok = (sd_init() == 0);
    start_page_set_device_status(START_DEVICE_SD, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_SD_ERROR;
    }

    start_page_set_status("Init BMU...", 30);
    init_ok = (bmu_init() == 0);
    start_page_set_device_status(START_DEVICE_BMU, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_BMU_ERROR;
    }

    start_page_set_status("Init LORA...", 45);
    init_ok = (lora_init() == 0);
    start_page_set_device_status(START_DEVICE_LORA, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_LORA_ERROR;
    }

    start_page_set_status("AUDIO...", 60);
    init_ok = (factory_audio_init(i2c_bus, io_expander) == ESP_OK);
    start_page_set_device_status(START_DEVICE_AUDIO, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_AUDIO_ERROR;
    }

    start_page_set_status("Init ESP32C5 SLAVE...", 75);
    init_ok = (esp32c5_slave_init() == 0);
    start_page_set_device_status(START_DEVICE_ESP32C5, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_SLAVE_ERROR;
    }

    start_page_set_status("Init CAMERA...", 90);
    init_ok = (camera_init(i2c_bus, &s_lcd) == ESP_OK);
    start_page_set_device_status(START_DEVICE_CAMERA, init_ok);
    if (!init_ok)
    {
        s_init_error |= INIT_CAMERA_ERROR;
    }

    start_page_set_status(s_init_error ? "Init failed" : "Ready", 100);

    vTaskDelay(pdMS_TO_TICKS(1000));
    ui_page_switch_async(PAGE_HOME);
    vTaskDelay(pdMS_TO_TICKS(200));
    ui_page_destroy(PAGE_START);
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

    ESP_ERROR_CHECK(board_i2c_init(&i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &io_expander));

    ESP_ERROR_CHECK(lcd_jd9365_init(&s_lcd, io_expander));
    ESP_ERROR_CHECK(lcd_backlight_init());
    ESP_ERROR_CHECK(lcd_backlight_set_brightness(40));
    ESP_ERROR_CHECK(touch_init(&s_touch, i2c_bus, io_expander));
    ESP_ERROR_CHECK(lvgl_port_init());

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
