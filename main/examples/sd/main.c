#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "storage_sd.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "sd_card";

static esp_err_t print_entry(const storage_sd_entry_t *entry, void *user_ctx)
{
    (void)user_ctx;
    const char *type = entry->type == STORAGE_SD_ENTRY_DIRECTORY ? "DIR" :
                       entry->type == STORAGE_SD_ENTRY_FILE ? "FILE" : "OTHER";
    ESP_LOGI(TAG, "%-5s %8llu bytes  %s", type,
             (unsigned long long)entry->size, entry->path);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "SD Card Component Example");

    t_panel_p4_bsp_t bsp;
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));

    storage_sd_config_t config = STORAGE_SD_CONFIG_DEFAULT();
    config.io_expander = t_panel_p4_bsp_get_io_expander(&bsp);

    storage_sd_handle_t storage = NULL;
    esp_err_t ret = storage_sd_mount(&config, &storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s. Check card insertion, power, and SDMMC wiring.",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&bsp));
        return;
    }

    static const char message[] = "Hello from the storage_sd component!\n";
    ESP_ERROR_CHECK(storage_sd_write_file(storage, "hello.txt", message, strlen(message)));

    uint8_t *data = NULL;
    size_t data_size = 0;
    ESP_ERROR_CHECK(storage_sd_read_file_alloc(storage, "hello.txt", &data, &data_size));
    ESP_LOGI(TAG, "Read hello.txt (%u bytes): %s", (unsigned)data_size, (char *)data);
    free(data);

    bool is_file = false;
    bool root_is_directory = false;
    ESP_ERROR_CHECK(storage_sd_is_file(storage, "hello.txt", &is_file));
    ESP_ERROR_CHECK(storage_sd_is_directory(storage, "/", &root_is_directory));
    ESP_LOGI(TAG, "hello.txt is file=%s, root is directory=%s",
             is_file ? "Yes" : "No", root_is_directory ? "Yes" : "No");

    storage_sd_scan_stats_t stats;
    ESP_ERROR_CHECK(storage_sd_scan(storage, "/", true, print_entry, NULL, &stats));
    ESP_LOGI(TAG, "Scan complete: files=%u, directories=%u, total file bytes=%llu",
             (unsigned)stats.file_count, (unsigned)stats.directory_count,
             (unsigned long long)stats.total_file_size);

    ESP_ERROR_CHECK(storage_sd_unmount(storage));
    ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&bsp));
}
