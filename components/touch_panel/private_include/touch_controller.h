#pragma once

#include "touch_panel.h"

esp_err_t touch_controller_init(touch_panel_t *handle, i2c_master_bus_handle_t i2c_bus,
                                esp_io_expander_handle_t expander);
esp_err_t touch_controller_soft_reset(touch_panel_t *handle);
esp_err_t touch_controller_get_chip_id(touch_panel_t *handle, uint8_t *id);
esp_err_t touch_controller_get_fw_version(touch_panel_t *handle, uint8_t *version);
esp_err_t touch_controller_get_resolution(touch_panel_t *handle, uint16_t *x_res, uint16_t *y_res);
esp_err_t touch_controller_get_single_point(touch_panel_t *handle, touch_panel_data_t *data,
                                            uint8_t finger_num);
esp_err_t touch_controller_get_multiple_points(touch_panel_t *handle, touch_panel_data_t *data);
esp_err_t touch_controller_get_edge(touch_panel_t *handle, bool *edge_detected);
