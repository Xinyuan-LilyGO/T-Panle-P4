#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lcd_jd9365_driver.h"

esp_err_t camera_init(i2c_master_bus_handle_t i2c_bus, lcd_driver_t *lcd);
bool camera_preview_start(uint32_t width, uint32_t height);
void camera_preview_stop(void);
bool camera_preview_stop_wait(uint32_t timeout_ms);
void camera_preview_clear_area(void);
void camera_preview_set_area(int x, int y, int w, int h);
bool camera_storage_is_ready(void);
esp_err_t camera_capture_photo_async(void);
