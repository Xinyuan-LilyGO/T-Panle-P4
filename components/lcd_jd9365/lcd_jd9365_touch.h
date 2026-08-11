/*
 * JD9365TX Touch Driver for ESP32-P4 T-Panel-P4
 * Based on Jadard TP debugging guide for RTOS
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define TOUCH_I2C_ADDR 0x68
#define MAX_TOUCH_POINTS 5

#define CHIP_ID_ADDR 0x40008076
#define TOUCH_DATA_ADDR 0x20011120
#define SYSTEM_SOFT_RESET_ADDR 0x40008004
#define POR_INIT_ADDR 0x40008081
#define FW_VERSION_ADDR 0x20000F1C
#define RESOLUTION_ADDR 0x20000904

    typedef struct
    {
        uint16_t x;
        uint16_t y;
        uint8_t pressure;
    } touch_info_t;

    typedef struct
    {
        uint8_t finger_count;
        bool edge_touch;
        touch_info_t points[MAX_TOUCH_POINTS];
    } touch_data_t;

    typedef struct
    {
        i2c_master_dev_handle_t i2c_dev;
        uint32_t touch_info_addr;
    } touch_handle_t;

    esp_err_t touch_init(touch_handle_t *handle,
                         i2c_master_bus_handle_t i2c_bus,
                         esp_io_expander_handle_t expander);

    esp_err_t touch_soft_reset(touch_handle_t *handle);

    esp_err_t touch_get_chip_id(touch_handle_t *handle, uint8_t *id);

    esp_err_t touch_get_fw_version(touch_handle_t *handle, uint8_t *version);

    esp_err_t touch_get_resolution(touch_handle_t *handle, uint16_t *x_res, uint16_t *y_res);

    esp_err_t touch_get_single_point(touch_handle_t *handle,
                                     touch_data_t *data,
                                     uint8_t finger_num);

    esp_err_t touch_get_multiple_points(touch_handle_t *handle,
                                        touch_data_t *data);

    esp_err_t touch_get_edge(touch_handle_t *handle, bool *edge_detected);

#ifdef __cplusplus
}
#endif
