/*
 * ESP32-P4 SD Card Example (SDMMC 4-line mode)
 * Pins: CMD=44, CLK=43, D0=39, D1=40, D2=41, D3=42
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_ldo_regulator.h" // 包含 LDO 驱动头文件

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "T_Panle_P4_board_config.h"

#define MOUNT_POINT "/sdcard"

static const char *TAG = "sd_card";

static esp_err_t sd_write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "%s", data);
    fclose(f);
    ESP_LOGI(TAG, "File written: %s", path);
    return ESP_OK;
}

static esp_err_t sd_read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_FAIL;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != NULL)
    {
        printf("%s", buf);
    }
    fclose(f);
    ESP_LOGI(TAG, "File read: %s", path);
    return ESP_OK;
}

static void sd_list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Failed to open dir: %s", path);
        return;
    }
    struct dirent *entry;
    ESP_LOGI(TAG, "Contents of %s:", path);
    while ((entry = readdir(dir)) != NULL)
    {
        ESP_LOGI(TAG, "  %s (%s)", entry->d_name,
                 (entry->d_type == DT_DIR) ? "DIR" : "FILE");
    }
    closedir(dir);
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

void app_main(void)
{
    ESP_LOGI(TAG, "SD Card Example");

    init_ldo();

    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    // Create the XL9555 expander instance on the specified I2C address.
    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(bus_handle, XL9555_I2C_ADDR, &expander));
    uint32_t pin_mask = (1UL << XL9555_SD_VDD_EN);
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, pin_mask, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, pin_mask, 1));
    vTaskDelay(pdMS_TO_TICKS(200)); // Wait for SD card power stabilize

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    ESP_LOGI(TAG, "Mounting SD card at %s", MOUNT_POINT);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD1_CLK;
    slot_config.cmd = SD1_CMD;
    slot_config.d0 = SD1_D0;
    slot_config.d1 = SD1_D1;
    slot_config.d2 = SD1_D2;
    slot_config.d3 = SD1_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ESP_LOGI(TAG, "Mounting filesystem");

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. Make sure SD card is formatted with FAT.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "SD card mounted successfully");

    sdmmc_card_print_info(stdout, card);

    // // List root directory
    sd_list_dir(MOUNT_POINT);

    // Write test file
    const char *test_file = MOUNT_POINT "/hello.txt";
    sd_write_file(test_file, "Hello from ESP32-P4 SD card test!\n");

    // Read test file back
    sd_read_file(test_file);

    // Get card info
    struct stat st;
    if (stat(test_file, &st) == 0) {
        ESP_LOGI(TAG, "File size: %ld bytes", st.st_size);
    }

    // Unmount
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(TAG, "SD card unmounted");
}
