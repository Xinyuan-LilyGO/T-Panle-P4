#include "storage_sd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#define STORAGE_SD_LDO_CHANNEL 4
#define STORAGE_SD_LDO_VOLTAGE_MV 3300
#define STORAGE_SD_POWER_STABILIZE_MS 200
#define STORAGE_SD_SCAN_MAX_DEPTH 32

static const char *TAG = "storage_sd";

struct storage_sd_context {
    char mount_point[STORAGE_SD_PATH_MAX];
    sdmmc_card_t *card;
    sd_pwr_ctrl_handle_t power_ctrl;
    esp_io_expander_handle_t io_expander;
    SemaphoreHandle_t mutex;
    bool mounted;
    bool expander_power_enabled;
};

static esp_err_t storage_sd_errno_to_err(int error)
{
    switch (error) {
    case ENOENT:
        return ESP_ERR_NOT_FOUND;
    case ENOMEM:
    case ENOSPC:
        return ESP_ERR_NO_MEM;
    case EINVAL:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

static esp_err_t storage_sd_ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    if (errno != EEXIST) {
        return storage_sd_errno_to_err(errno);
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool storage_sd_path_has_parent_segment(const char *path)
{
    const char *segment = path;
    while (*segment) {
        while (*segment == '/') {
            segment++;
        }
        const char *end = strchr(segment, '/');
        const size_t length = end ? (size_t)(end - segment) : strlen(segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            return true;
        }
        if (!end) {
            break;
        }
        segment = end + 1;
    }
    return false;
}

static esp_err_t storage_sd_lock(storage_sd_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle && handle->mounted && handle->mutex,
                        ESP_ERR_INVALID_STATE, TAG, "SD card is not mounted");
    return xSemaphoreTakeRecursive(handle->mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void storage_sd_unlock(storage_sd_handle_t handle)
{
    xSemaphoreGiveRecursive(handle->mutex);
}

static void storage_sd_power_disable(storage_sd_handle_t handle)
{
#if CONFIG_T_PANEL_P4_HAS_XL9555
    if (handle->expander_power_enabled && handle->io_expander) {
        const uint32_t pin_mask = 1UL << XL9555_SD_VDD_EN;
        const esp_err_t ret = esp_io_expander_set_level(handle->io_expander, pin_mask, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Disable SD power through XL9555 failed: %s", esp_err_to_name(ret));
        }
        handle->expander_power_enabled = false;
    }
#endif
    if (handle->power_ctrl) {
        const esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(handle->power_ctrl);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Release SD LDO4 failed: %s", esp_err_to_name(ret));
        }
        handle->power_ctrl = NULL;
    }
}

static esp_err_t storage_sd_power_enable(storage_sd_handle_t handle)
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
        return ret;
    }

#if CONFIG_T_PANEL_P4_HAS_XL9555
    if (!handle->io_expander) {
        ESP_LOGE(TAG, "Standard/Round requires an initialized XL9555 handle");
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t pin_mask = 1UL << XL9555_SD_VDD_EN;
    ret = esp_io_expander_set_dir(handle->io_expander, pin_mask, IO_EXPANDER_OUTPUT);
    if (ret == ESP_OK) {
        ret = esp_io_expander_set_level(handle->io_expander, pin_mask, 1);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable SD power through XL9555 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    handle->expander_power_enabled = true;
    ESP_LOGI(TAG, "SD power enabled through XL9555 pin %d", XL9555_SD_VDD_EN);
#endif

    vTaskDelay(pdMS_TO_TICKS(STORAGE_SD_POWER_STABILIZE_MS));
    ESP_LOGI(TAG, "SD LDO4 supply ready at 3.3 V");
    return ESP_OK;
}

esp_err_t storage_sd_mount(const storage_sd_config_t *config, storage_sd_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && out_handle && config->mount_point &&
                            config->mount_point[0] == '/' && config->max_files > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid mount configuration");
    ESP_RETURN_ON_FALSE(strlen(config->mount_point) < STORAGE_SD_PATH_MAX,
                        ESP_ERR_INVALID_SIZE, TAG, "Mount point is too long");
    *out_handle = NULL;

    storage_sd_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "Allocate storage handle failed");
    snprintf(handle->mount_point, sizeof(handle->mount_point), "%s", config->mount_point);
    size_t mount_length = strlen(handle->mount_point);
    while (mount_length > 1 && handle->mount_point[mount_length - 1] == '/') {
        handle->mount_point[--mount_length] = '\0';
    }
    handle->io_expander = config->io_expander;
    handle->mutex = xSemaphoreCreateRecursiveMutex();
    if (!handle->mutex) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = storage_sd_power_enable(handle);
    if (ret != ESP_OK) {
        storage_sd_power_disable(handle);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files = (int)config->max_files,
        .allocation_unit_size = config->allocation_unit_size,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // GPIO39-GPIO44 are ESP32-P4 slot 0's native SDMMC IOMUX pins.
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.pwr_ctrl_handle = handle->power_ctrl;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD1_CLK;
    slot_config.cmd = SD1_CMD;
    slot_config.d0 = SD1_D0;
    slot_config.d1 = SD1_D1;
    slot_config.d2 = SD1_D2;
    slot_config.d3 = SD1_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SDMMC slot %d at %s: CLK=%d CMD=%d D0-D3=%d,%d,%d,%d",
             host.slot, handle->mount_point, SD1_CLK, SD1_CMD,
             SD1_D0, SD1_D1, SD1_D2, SD1_D3);
    ret = esp_vfs_fat_sdmmc_mount(handle->mount_point, &host, &slot_config,
                                  &mount_config, &handle->card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD card failed: %s", esp_err_to_name(ret));
        storage_sd_power_disable(handle);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }

    handle->mounted = true;
    *out_handle = handle;
    ESP_LOGI(TAG, "SD card mounted at %s", handle->mount_point);
    sdmmc_card_print_info(stdout, handle->card);
    return ESP_OK;
}

esp_err_t storage_sd_unmount(storage_sd_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Storage handle is NULL");
    if (handle->mounted) {
        const esp_err_t ret = esp_vfs_fat_sdcard_unmount(handle->mount_point, handle->card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Unmount SD card failed: %s", esp_err_to_name(ret));
            return ret;
        }
        handle->mounted = false;
        handle->card = NULL;
    }
    storage_sd_power_disable(handle);
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return ESP_OK;
}

bool storage_sd_is_mounted(storage_sd_handle_t handle)
{
    return handle && handle->mounted;
}

const char *storage_sd_get_mount_point(storage_sd_handle_t handle)
{
    return handle && handle->mounted ? handle->mount_point : NULL;
}

sdmmc_card_t *storage_sd_get_card(storage_sd_handle_t handle)
{
    return handle && handle->mounted ? handle->card : NULL;
}

esp_err_t storage_sd_resolve_path(storage_sd_handle_t handle, const char *path,
                                  char *resolved_path, size_t resolved_path_size)
{
    ESP_RETURN_ON_FALSE(handle && handle->mounted && path && resolved_path && resolved_path_size,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid path arguments");
    ESP_RETURN_ON_FALSE(!storage_sd_path_has_parent_segment(path), ESP_ERR_INVALID_ARG, TAG,
                        "Parent path segments are not allowed: %s", path);

    int written;
    const size_t mount_length = strlen(handle->mount_point);
    if (strncmp(path, handle->mount_point, mount_length) == 0 &&
        (path[mount_length] == '\0' || path[mount_length] == '/')) {
        written = snprintf(resolved_path, resolved_path_size, "%s", path);
    } else if (path[0] == '/') {
        written = snprintf(resolved_path, resolved_path_size, "%s%s", handle->mount_point, path);
    } else if (path[0] == '\0' || strcmp(path, ".") == 0) {
        written = snprintf(resolved_path, resolved_path_size, "%s", handle->mount_point);
    } else {
        written = snprintf(resolved_path, resolved_path_size, "%s/%s", handle->mount_point, path);
    }
    return written >= 0 && (size_t)written < resolved_path_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_sd_get_info(storage_sd_handle_t handle, const char *path,
                              storage_sd_file_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "File info is NULL");
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    struct stat st;
    if (ret == ESP_OK && stat(full_path, &st) != 0) {
        ret = storage_sd_errno_to_err(errno);
    }
    if (ret == ESP_OK) {
        info->type = S_ISDIR(st.st_mode) ? STORAGE_SD_ENTRY_DIRECTORY :
                     S_ISREG(st.st_mode) ? STORAGE_SD_ENTRY_FILE : STORAGE_SD_ENTRY_UNKNOWN;
        info->size = (uint64_t)st.st_size;
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_exists(storage_sd_handle_t handle, const char *path, bool *exists)
{
    ESP_RETURN_ON_FALSE(exists, ESP_ERR_INVALID_ARG, TAG, "Exists result is NULL");
    storage_sd_file_info_t info;
    const esp_err_t ret = storage_sd_get_info(handle, path, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        *exists = false;
        return ESP_OK;
    }
    *exists = ret == ESP_OK;
    return ret;
}

esp_err_t storage_sd_is_file(storage_sd_handle_t handle, const char *path, bool *is_file)
{
    ESP_RETURN_ON_FALSE(is_file, ESP_ERR_INVALID_ARG, TAG, "File result is NULL");
    storage_sd_file_info_t info;
    ESP_RETURN_ON_ERROR(storage_sd_get_info(handle, path, &info), TAG, "Get file info failed");
    *is_file = info.type == STORAGE_SD_ENTRY_FILE;
    return ESP_OK;
}

esp_err_t storage_sd_is_directory(storage_sd_handle_t handle, const char *path, bool *is_directory)
{
    ESP_RETURN_ON_FALSE(is_directory, ESP_ERR_INVALID_ARG, TAG, "Directory result is NULL");
    storage_sd_file_info_t info;
    ESP_RETURN_ON_ERROR(storage_sd_get_info(handle, path, &info), TAG, "Get directory info failed");
    *is_directory = info.type == STORAGE_SD_ENTRY_DIRECTORY;
    return ESP_OK;
}

static esp_err_t storage_sd_write_mode(storage_sd_handle_t handle, const char *path,
                                       const void *data, size_t size, const char *mode)
{
    ESP_RETURN_ON_FALSE(data || size == 0, ESP_ERR_INVALID_ARG, TAG, "Write data is NULL");
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    FILE *file = NULL;
    if (ret == ESP_OK) {
        file = fopen(full_path, mode);
        if (!file) {
            ret = storage_sd_errno_to_err(errno);
        }
    }
    if (ret == ESP_OK && size > 0 && fwrite(data, 1, size, file) != size) {
        ret = storage_sd_errno_to_err(errno);
    }
    if (file && fclose(file) != 0 && ret == ESP_OK) {
        ret = storage_sd_errno_to_err(errno);
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_write_file(storage_sd_handle_t handle, const char *path,
                                const void *data, size_t size)
{
    return storage_sd_write_mode(handle, path, data, size, "wb");
}

esp_err_t storage_sd_append_file(storage_sd_handle_t handle, const char *path,
                                 const void *data, size_t size)
{
    return storage_sd_write_mode(handle, path, data, size, "ab");
}

esp_err_t storage_sd_read_file(storage_sd_handle_t handle, const char *path,
                               void *buffer, size_t buffer_size, size_t *bytes_read)
{
    ESP_RETURN_ON_FALSE(buffer && buffer_size && bytes_read, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid read arguments");
    *bytes_read = 0;
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    FILE *file = NULL;
    if (ret == ESP_OK) {
        file = fopen(full_path, "rb");
        if (!file) {
            ret = storage_sd_errno_to_err(errno);
        }
    }
    if (ret == ESP_OK) {
        *bytes_read = fread(buffer, 1, buffer_size, file);
        if (ferror(file)) {
            ret = ESP_FAIL;
        }
    }
    if (file) {
        fclose(file);
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_read_file_alloc(storage_sd_handle_t handle, const char *path,
                                     uint8_t **data, size_t *size)
{
    ESP_RETURN_ON_FALSE(data && size, ESP_ERR_INVALID_ARG, TAG, "Invalid allocated read arguments");
    *data = NULL;
    *size = 0;
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");

    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    FILE *file = NULL;
    long length = 0;
    if (ret == ESP_OK) {
        file = fopen(full_path, "rb");
        if (!file) {
            ret = storage_sd_errno_to_err(errno);
        }
    }
    if (ret == ESP_OK && (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
                          fseek(file, 0, SEEK_SET) != 0)) {
        ret = ESP_FAIL;
    }
    uint8_t *buffer = NULL;
    if (ret == ESP_OK) {
        buffer = malloc((size_t)length + 1);
        if (!buffer) {
            ret = ESP_ERR_NO_MEM;
        }
    }
    if (ret == ESP_OK && length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK) {
        buffer[length] = '\0';
        *data = buffer;
        *size = (size_t)length;
    } else {
        free(buffer);
    }
    if (file) {
        fclose(file);
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_make_directories(storage_sd_handle_t handle, const char *path)
{
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    const size_t start = strlen(handle->mount_point) + 1;
    if (ret == ESP_OK) {
        for (size_t i = start; full_path[i]; i++) {
            if (full_path[i] == '/') {
                full_path[i] = '\0';
                ret = storage_sd_ensure_directory(full_path);
                full_path[i] = '/';
                if (ret != ESP_OK) {
                    break;
                }
            }
        }
    }
    if (ret == ESP_OK && strcmp(full_path, handle->mount_point) != 0) {
        ret = storage_sd_ensure_directory(full_path);
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_remove(storage_sd_handle_t handle, const char *path)
{
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, path, full_path, sizeof(full_path));
    if (ret == ESP_OK && strcmp(full_path, handle->mount_point) == 0) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK && remove(full_path) != 0) {
        ret = storage_sd_errno_to_err(errno);
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_rename(storage_sd_handle_t handle, const char *old_path,
                            const char *new_path)
{
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    char old_full_path[STORAGE_SD_PATH_MAX];
    char new_full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, old_path, old_full_path, sizeof(old_full_path));
    if (ret == ESP_OK) {
        ret = storage_sd_resolve_path(handle, new_path, new_full_path, sizeof(new_full_path));
    }
    if (ret == ESP_OK && rename(old_full_path, new_full_path) != 0) {
        ret = storage_sd_errno_to_err(errno);
    }
    storage_sd_unlock(handle);
    return ret;
}

static esp_err_t storage_sd_scan_internal(const char *directory, bool recursive, size_t depth,
                                          storage_sd_scan_cb_t callback, void *user_ctx,
                                          storage_sd_scan_stats_t *stats)
{
    if (depth > STORAGE_SD_SCAN_MAX_DEPTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        return storage_sd_errno_to_err(errno);
    }

    esp_err_t ret = ESP_OK;
    struct dirent *item;
    while (ret == ESP_OK && (item = readdir(dir)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        char path[STORAGE_SD_PATH_MAX];
        const int written = snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            ret = storage_sd_errno_to_err(errno);
            break;
        }
        storage_sd_entry_t entry = {
            .name = item->d_name,
            .path = path,
            .type = S_ISDIR(st.st_mode) ? STORAGE_SD_ENTRY_DIRECTORY :
                    S_ISREG(st.st_mode) ? STORAGE_SD_ENTRY_FILE : STORAGE_SD_ENTRY_UNKNOWN,
            .size = (uint64_t)st.st_size,
        };
        if (entry.type == STORAGE_SD_ENTRY_FILE) {
            stats->file_count++;
            stats->total_file_size += entry.size;
        } else if (entry.type == STORAGE_SD_ENTRY_DIRECTORY) {
            stats->directory_count++;
        }
        if (callback) {
            ret = callback(&entry, user_ctx);
        }
        if (ret == ESP_OK && recursive && entry.type == STORAGE_SD_ENTRY_DIRECTORY) {
            ret = storage_sd_scan_internal(path, true, depth + 1, callback, user_ctx, stats);
        }
    }
    closedir(dir);
    return ret;
}

esp_err_t storage_sd_scan(storage_sd_handle_t handle, const char *directory,
                          bool recursive, storage_sd_scan_cb_t callback,
                          void *user_ctx, storage_sd_scan_stats_t *stats)
{
    ESP_RETURN_ON_ERROR(storage_sd_lock(handle), TAG, "Lock storage failed");
    storage_sd_scan_stats_t local_stats = {0};
    char full_path[STORAGE_SD_PATH_MAX];
    esp_err_t ret = storage_sd_resolve_path(handle, directory, full_path, sizeof(full_path));
    if (ret == ESP_OK) {
        struct stat st;
        if (stat(full_path, &st) != 0) {
            ret = storage_sd_errno_to_err(errno);
        } else if (!S_ISDIR(st.st_mode)) {
            ret = ESP_ERR_INVALID_ARG;
        }
    }
    if (ret == ESP_OK) {
        ret = storage_sd_scan_internal(full_path, recursive, 0, callback, user_ctx, &local_stats);
    }
    if (stats) {
        *stats = local_stats;
    }
    storage_sd_unlock(handle);
    return ret;
}

esp_err_t storage_sd_count(storage_sd_handle_t handle, const char *directory,
                           bool recursive, storage_sd_scan_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats, ESP_ERR_INVALID_ARG, TAG, "Scan stats is NULL");
    return storage_sd_scan(handle, directory, recursive, NULL, NULL, stats);
}

esp_err_t storage_sd_get_file_count(storage_sd_handle_t handle, const char *directory,
                                    bool recursive, size_t *file_count)
{
    ESP_RETURN_ON_FALSE(file_count, ESP_ERR_INVALID_ARG, TAG, "File count is NULL");
    storage_sd_scan_stats_t stats;
    ESP_RETURN_ON_ERROR(storage_sd_count(handle, directory, recursive, &stats), TAG,
                        "Count files failed");
    *file_count = stats.file_count;
    return ESP_OK;
}
