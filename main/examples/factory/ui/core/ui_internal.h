#pragma once

#include "sdkconfig.h"
#include "ui.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "camera.h"
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
#include "esp_netif.h"
#endif
#include "esp_timer.h"
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display_panel.h"
#include "system.h"
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
#include "esp32c5_sdio_slave.h"
#endif
#if CONFIG_T_PANEL_P4_HAS_LORA
#include "lora.h"
#include "lora_app.h"
#endif
#include "storage.h"
#include "lvgl_page_manager.h"
#include "display_dim.h"
#include "misc/cache/instance/lv_image_cache.h"
#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif

#include "home_page.h"
#include "music_page.h"
#include "record_page.h"
#include "camera_page.h"
#if CONFIG_T_PANEL_P4_HAS_LORA
#include "lora_page.h"
#endif
#include "file_page.h"
#if CONFIG_T_PANEL_P4_BOARD_RECT
#include "motion_sensor.h"
#include "haptic_motor.h"
#include "sensor_page.h"
#endif
#if CONFIG_T_PANEL_P4_BOARD_RECT || CONFIG_T_PANEL_P4_BOARD_STANDARD || CONFIG_T_PANEL_P4_BOARD_ROUND
#include "self_test_page.h"
#endif
#include "settings_page.h"
#include "start_page.h"
#include "status_bar.h"
#include "ui_widgets.h"
