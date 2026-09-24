# storage_sd

Board-aware SDMMC/FATFS storage for T-Panel-P4. The component enables ESP LDO
channel 4 at 3.3 V for every board. Standard and Round also require the BSP's
XL9555 handle to enable `XL9555_SD_VDD_EN`; Rect needs no external power switch.

```c
t_panel_p4_bsp_t bsp;
ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));

storage_sd_config_t config = STORAGE_SD_CONFIG_DEFAULT();
config.io_expander = t_panel_p4_bsp_get_io_expander(&bsp);

storage_sd_handle_t storage;
ESP_ERROR_CHECK(storage_sd_mount(&config, &storage));
ESP_ERROR_CHECK(storage_sd_make_directories(storage, "data"));
ESP_ERROR_CHECK(storage_sd_write_file(storage, "data/test.bin", data, data_size));

storage_sd_scan_stats_t stats;
ESP_ERROR_CHECK(storage_sd_count(storage, "/", true, &stats));

ESP_ERROR_CHECK(storage_sd_unmount(storage));
ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&bsp));
```

Paths may be relative to the mount point, such as `data/test.bin`, or absolute
under the configured mount point, such as `/sdcard/data/test.bin`. A leading
slash such as `/data/test.bin` is also treated as mount-relative. Parent (`..`)
segments are rejected.

`storage_sd_read_file()` reads at most the supplied buffer size and reports the
number of bytes read. Use `storage_sd_read_file_alloc()` when the whole file is
needed; it allocates one extra zero byte for convenient text use, and the caller
must release the returned buffer with `free()`.
