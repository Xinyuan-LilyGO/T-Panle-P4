#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Data contracts shared by factory services and whichever UI is enabled. */
typedef struct
{
    bool wifi_connected;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool battery_charging;
    int battery_percent;
} status_info_t;

typedef struct
{
    int fps;
    int sram_percent;
    int psram_percent;
    uint32_t sram_free_kb;
    uint32_t psram_free_kb;
    int temp_c_x10;
} home_info_t;

typedef struct
{
    bool usb_mode;
    bool app_mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char type[24];
    char bus[24];
} factory_storage_info_t;
