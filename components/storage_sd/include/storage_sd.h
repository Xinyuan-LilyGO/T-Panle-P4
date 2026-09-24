#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_io_expander.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SD_DEFAULT_MOUNT_POINT "/sdcard"
#define STORAGE_SD_PATH_MAX 512

typedef struct storage_sd_context *storage_sd_handle_t;

typedef struct {
    const char *mount_point;
    esp_io_expander_handle_t io_expander;
    size_t max_files;
    size_t allocation_unit_size;
    bool format_if_mount_failed;
} storage_sd_config_t;

#define STORAGE_SD_CONFIG_DEFAULT()                    \
    {                                                  \
        .mount_point = STORAGE_SD_DEFAULT_MOUNT_POINT, \
        .io_expander = NULL,                           \
        .max_files = 5,                                \
        .allocation_unit_size = 16 * 1024,             \
        .format_if_mount_failed = false,               \
    }

typedef enum {
    STORAGE_SD_ENTRY_UNKNOWN = 0,
    STORAGE_SD_ENTRY_FILE,
    STORAGE_SD_ENTRY_DIRECTORY,
} storage_sd_entry_type_t;

typedef struct {
    storage_sd_entry_type_t type;
    uint64_t size;
} storage_sd_file_info_t;

typedef struct {
    const char *name;
    const char *path;
    storage_sd_entry_type_t type;
    uint64_t size;
} storage_sd_entry_t;

typedef struct {
    size_t file_count;
    size_t directory_count;
    uint64_t total_file_size;
} storage_sd_scan_stats_t;

/* Entry strings remain valid only until the callback returns. */
typedef esp_err_t (*storage_sd_scan_cb_t)(const storage_sd_entry_t *entry, void *user_ctx);

esp_err_t storage_sd_mount(const storage_sd_config_t *config, storage_sd_handle_t *out_handle);
esp_err_t storage_sd_unmount(storage_sd_handle_t handle);
bool storage_sd_is_mounted(storage_sd_handle_t handle);
const char *storage_sd_get_mount_point(storage_sd_handle_t handle);
sdmmc_card_t *storage_sd_get_card(storage_sd_handle_t handle);

esp_err_t storage_sd_resolve_path(storage_sd_handle_t handle, const char *path,
                                  char *resolved_path, size_t resolved_path_size);
esp_err_t storage_sd_get_info(storage_sd_handle_t handle, const char *path,
                              storage_sd_file_info_t *info);
esp_err_t storage_sd_exists(storage_sd_handle_t handle, const char *path, bool *exists);
esp_err_t storage_sd_is_file(storage_sd_handle_t handle, const char *path, bool *is_file);
esp_err_t storage_sd_is_directory(storage_sd_handle_t handle, const char *path, bool *is_directory);

esp_err_t storage_sd_write_file(storage_sd_handle_t handle, const char *path,
                                const void *data, size_t size);
esp_err_t storage_sd_append_file(storage_sd_handle_t handle, const char *path,
                                 const void *data, size_t size);
esp_err_t storage_sd_read_file(storage_sd_handle_t handle, const char *path,
                               void *buffer, size_t buffer_size, size_t *bytes_read);
esp_err_t storage_sd_read_file_alloc(storage_sd_handle_t handle, const char *path,
                                     uint8_t **data, size_t *size);
esp_err_t storage_sd_make_directories(storage_sd_handle_t handle, const char *path);
esp_err_t storage_sd_remove(storage_sd_handle_t handle, const char *path);
esp_err_t storage_sd_rename(storage_sd_handle_t handle, const char *old_path,
                            const char *new_path);

esp_err_t storage_sd_scan(storage_sd_handle_t handle, const char *directory,
                          bool recursive, storage_sd_scan_cb_t callback,
                          void *user_ctx, storage_sd_scan_stats_t *stats);
esp_err_t storage_sd_count(storage_sd_handle_t handle, const char *directory,
                           bool recursive, storage_sd_scan_stats_t *stats);
esp_err_t storage_sd_get_file_count(storage_sd_handle_t handle, const char *directory,
                                    bool recursive, size_t *file_count);

#ifdef __cplusplus
}
#endif
