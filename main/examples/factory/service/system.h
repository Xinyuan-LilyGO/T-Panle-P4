#pragma once

#include "esp_err.h"
#include "factory_service_types.h"

void system_lcd_flush_record(void);
esp_err_t factory_time_sync_start(void);
bool status_bar_info_get(status_info_t *status);
bool home_status_info_get(home_info_t *status);
bool factory_power_enter_ship_mode(void);
void factory_power_restart(void);
bool factory_power_enter_low_power(void);
bool factory_power_exit_low_power(void);
