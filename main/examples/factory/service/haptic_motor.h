#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t haptic_motor_init(i2c_master_bus_handle_t i2c_bus);
bool haptic_motor_is_ready(void);
esp_err_t haptic_motor_play_test(void);
esp_err_t haptic_motor_stop(void);
esp_err_t haptic_motor_is_playing(bool *playing);
