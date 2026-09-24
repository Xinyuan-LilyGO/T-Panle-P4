#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_PANEL_MAX_POINTS 5

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
} touch_panel_point_t;

typedef struct {
    uint8_t finger_count;
    bool edge_touch;
    touch_panel_point_t points[TOUCH_PANEL_MAX_POINTS];
} touch_panel_data_t;

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    uint32_t touch_info_addr;
    void *driver_handle;
    void *io_handle;
} touch_panel_t;

esp_err_t touch_panel_init(touch_panel_t *handle, i2c_master_bus_handle_t i2c_bus,
                           esp_io_expander_handle_t expander);
esp_err_t touch_panel_soft_reset(touch_panel_t *handle);
esp_err_t touch_panel_get_chip_id(touch_panel_t *handle, uint8_t *id);
esp_err_t touch_panel_get_fw_version(touch_panel_t *handle, uint8_t *version);
esp_err_t touch_panel_get_resolution(touch_panel_t *handle, uint16_t *x_res, uint16_t *y_res);
esp_err_t touch_panel_get_single_point(touch_panel_t *handle, touch_panel_data_t *data,
                                       uint8_t finger_num);
esp_err_t touch_panel_get_multiple_points(touch_panel_t *handle, touch_panel_data_t *data);
esp_err_t touch_panel_get_edge(touch_panel_t *handle, bool *edge_detected);

#ifdef __cplusplus
}
#endif
