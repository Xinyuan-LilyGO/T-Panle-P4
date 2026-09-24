#include "touch_panel.h"
#include "touch_controller.h"

esp_err_t touch_panel_init(touch_panel_t *handle, i2c_master_bus_handle_t i2c_bus,
                           esp_io_expander_handle_t expander)
{
    return touch_controller_init(handle, i2c_bus, expander);
}

esp_err_t touch_panel_soft_reset(touch_panel_t *handle)
{
    return touch_controller_soft_reset(handle);
}

esp_err_t touch_panel_get_chip_id(touch_panel_t *handle, uint8_t *id)
{
    return touch_controller_get_chip_id(handle, id);
}

esp_err_t touch_panel_get_fw_version(touch_panel_t *handle, uint8_t *version)
{
    return touch_controller_get_fw_version(handle, version);
}

esp_err_t touch_panel_get_resolution(touch_panel_t *handle, uint16_t *x_res, uint16_t *y_res)
{
    return touch_controller_get_resolution(handle, x_res, y_res);
}

esp_err_t touch_panel_get_single_point(touch_panel_t *handle, touch_panel_data_t *data,
                                       uint8_t finger_num)
{
    return touch_controller_get_single_point(handle, data, finger_num);
}

esp_err_t touch_panel_get_multiple_points(touch_panel_t *handle, touch_panel_data_t *data)
{
    return touch_controller_get_multiple_points(handle, data);
}

esp_err_t touch_panel_get_edge(touch_panel_t *handle, bool *edge_detected)
{
    return touch_controller_get_edge(handle, edge_detected);
}
