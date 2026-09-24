#include "motion_sensor.h"

#include <string.h>

static t_panel_qmi8658c_t s_motion_sensor;

esp_err_t motion_sensor_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_motion_sensor.initialized)
    {
        return ESP_OK;
    }

    qmi8658c_config_t config = T_PANEL_QMI8658C_CONFIG_DEFAULT();
    return t_panel_qmi8658c_init(&s_motion_sensor,
                                 i2c_bus,
                                 T_PANEL_QMI8658C_ADDR_AUTO,
                                 &config);
}

bool motion_sensor_is_ready(void)
{
    return s_motion_sensor.initialized;
}

esp_err_t motion_sensor_read(qmi8658c_data_t *data)
{
    if (data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_motion_sensor.initialized)
    {
        memset(data, 0, sizeof(*data));
        return ESP_ERR_INVALID_STATE;
    }
    return t_panel_qmi8658c_read(&s_motion_sensor, data);
}

bool motion_sensor_device_info_get(uint8_t *address, uint8_t *revision)
{
    if (!s_motion_sensor.initialized)
    {
        return false;
    }
    if (address != NULL)
    {
        *address = s_motion_sensor.address;
    }
    if (revision != NULL)
    {
        *revision = s_motion_sensor.revision;
    }
    return true;
}
