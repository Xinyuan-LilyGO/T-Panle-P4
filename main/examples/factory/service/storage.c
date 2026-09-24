#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#define MOUNT_POINT "/sdcard"
#define SD_APP_MAX_FILES 5

static const char *TAG = "factory_storage";
static sdmmc_card_t *s_sd_card;
static tinyusb_msc_storage_handle_t s_msc_storage_handle;
static SemaphoreHandle_t s_storage_mutex;
static bool s_usb_msc_mode;
static volatile bool s_app_mounted;
static volatile bool s_msc_storage_event_failed;

static void storage_ldo_init(void)
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
        ESP_LOGE(TAG, "Failed to acquire SD LDO channel: %s", esp_err_to_name(ret));
    }
}

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
        s_app_mounted = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
        s_usb_msc_mode = !s_app_mounted;
        ESP_LOGI(TAG, "MSC storage switched to %s",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB host" : "application");
    }
    else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
             event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
             event->id == TINYUSB_MSC_EVENT_FORMAT_FAILED)
    {
        s_app_mounted = false;
        s_msc_storage_event_failed = true;
        ESP_LOGW(TAG, "MSC storage switch failed, event=%d", event->id);
    }
}

static bool storage_app_mount_is_ready(void)
{
    return s_app_mounted && !s_usb_msc_mode && s_sd_card != NULL;
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

esp_err_t storage_init(esp_io_expander_handle_t io_expander)
{
    storage_ldo_init();
#if CONFIG_T_PANEL_P4_HAS_XL9555
    uint32_t pin_mask = (1UL << XL9555_SD_VDD_EN);
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, pin_mask, IO_EXPANDER_OUTPUT),
                        TAG,
                        "Set SD power pin direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, pin_mask, 1),
                        TAG,
                        "Enable SD power failed");
#else
    (void)io_expander;
#endif
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
    s_app_mounted = true;

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

bool usb_otg_msc_set(bool usb_mode)
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
        audio_stop_music();
        s_app_mounted = false;
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
        s_app_mounted = !usb_mode;
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

bool usb_otg_msc_is_usb_mode(void)
{
    return s_usb_msc_mode;
}

bool storage_app_access_begin(void)
{
    if (!storage_app_mount_is_ready() || s_storage_mutex == NULL)
    {
        return false;
    }
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return false;
    }
    if (!storage_app_mount_is_ready())
    {
        xSemaphoreGive(s_storage_mutex);
        return false;
    }
    return true;
}

void storage_app_access_end(void)
{
    if (s_storage_mutex != NULL)
    {
        xSemaphoreGive(s_storage_mutex);
    }
}

bool storage_info_get(factory_storage_info_t *info)
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
