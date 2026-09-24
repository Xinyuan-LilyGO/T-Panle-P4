#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "qmi8658c_driver.h"

esp_err_t motion_sensor_init(i2c_master_bus_handle_t i2c_bus);
bool motion_sensor_is_ready(void);
esp_err_t motion_sensor_read(qmi8658c_data_t *data);
bool motion_sensor_device_info_get(uint8_t *address, uint8_t *revision);
