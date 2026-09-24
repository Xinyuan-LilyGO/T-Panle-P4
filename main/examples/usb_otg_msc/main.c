#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "board_config.h"

static const char *TAG = "usb_otg_msc";

static tinyusb_msc_storage_handle_t s_storage_handle = NULL;
static sdmmc_card_t *s_sd_card = NULL;

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

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
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
static const tusb_desc_device_qualifier_t s_device_qualifier = {
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

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},
    "LILYGO",
    "T-Panel-P4 SD MSC",
    "TPANELP4MSC",
    "SD Card",
};

static esp_err_t board_i2c_init(i2c_master_bus_handle_t *bus_handle)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_cfg, bus_handle);
}

static void board_ldo_init(void)
{
    esp_ldo_channel_handle_t ldo_handle = NULL;
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = 4,
        .voltage_mv = 3300,
        .flags.adjustable = true,
    };

    esp_err_t ret = esp_ldo_acquire_channel(&ldo_config, &ldo_handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "Acquire LDO channel failed: %s", esp_err_to_name(ret));
    }
}

#if CONFIG_T_PANEL_P4_HAS_XL9555
static esp_err_t board_sd_power_enable(i2c_master_bus_handle_t bus_handle)
{
    esp_io_expander_handle_t expander = NULL;
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_xl9555(bus_handle, XL9555_I2C_ADDR, &expander),
                        TAG, "Create XL9555 failed");

    uint32_t pin_mask = (1UL << XL9555_SD_VDD_EN);
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, pin_mask, IO_EXPANDER_OUTPUT),
                        TAG, "Set SD power pin dir failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, pin_mask, 1),
                        TAG, "Enable SD power failed");

    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "SD card power enabled");
    return ESP_OK;
}
#endif

static esp_err_t sdmmc_storage_init(sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(out_card, ESP_ERR_INVALID_ARG, TAG, "Invalid card pointer");

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
    ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, TAG, "No memory for sdmmc_card_t");

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

    switch (event->id)
    {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "MSC storage mounted to %s",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB host" : "application");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(TAG, "MSC storage mount failed or format required");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    static t_panel_p4_bsp_t bsp;
    i2c_master_bus_handle_t i2c_bus = NULL;

    ESP_LOGI(TAG, "USB MSC SD card example");
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
    i2c_bus = t_panel_p4_bsp_get_i2c_bus(&bsp);

    board_ldo_init();
#if CONFIG_T_PANEL_P4_HAS_XL9555
    ESP_ERROR_CHECK(board_sd_power_enable(i2c_bus));
#endif
    ESP_ERROR_CHECK(sdmmc_storage_init(&s_sd_card));

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .medium.card = s_sd_card,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage_handle));
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_callback(msc_mount_changed_cb, NULL));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_config_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.high_speed_config = s_msc_hs_config_desc;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "USB MSC ready. Connect USB to PC to access the SD card.");
    ESP_LOGW(TAG, "Do not access the same SD card from ESP app while it is mounted on PC.");
}
