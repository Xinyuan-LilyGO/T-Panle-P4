#pragma once

#include "display_panel.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "touch_panel.h"

esp_err_t lcd_init(i2c_master_bus_handle_t i2c_bus,
                           esp_io_expander_handle_t io_expander);
display_panel_t *lcd_get_display(void);
bool lcd_touch_data_get(touch_panel_data_t *data);
