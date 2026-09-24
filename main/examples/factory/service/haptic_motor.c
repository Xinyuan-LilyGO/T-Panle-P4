#include "haptic_motor.h"

#include "drv2605_driver.h"
#include "t_panel_p4_bsp.h"

#define HAPTIC_MOTOR_TEST_EFFECT 47

static drv2605_handle_t s_haptic_motor;
static bool s_haptic_motor_ready;

esp_err_t haptic_motor_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_haptic_motor_ready)
    {
        return ESP_OK;
    }

    drv2605_config_t config = DRV2605_CONFIG_DEFAULT(MOTOR_EN_PIN);
    config.rated_voltage_reg = DRV2605_ERM_RATED_VOLTAGE_MV(3000);
    config.overdrive_clamp_reg = DRV2605_ERM_OVERDRIVE_CLAMP_MV(3300);
    config.auto_calibrate = false;
    esp_err_t ret = drv2605_init(&s_haptic_motor, i2c_bus, &config);
    if (ret == ESP_OK)
    {
        s_haptic_motor_ready = true;
    }
    return ret;
}

bool haptic_motor_is_ready(void)
{
    return s_haptic_motor_ready;
}

esp_err_t haptic_motor_play_test(void)
{
    return s_haptic_motor_ready
               ? drv2605_play_effect(&s_haptic_motor, HAPTIC_MOTOR_TEST_EFFECT)
               : ESP_ERR_INVALID_STATE;
}

esp_err_t haptic_motor_stop(void)
{
    return s_haptic_motor_ready
               ? drv2605_stop(&s_haptic_motor)
               : ESP_ERR_INVALID_STATE;
}

esp_err_t haptic_motor_is_playing(bool *playing)
{
    if (playing == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_haptic_motor_ready
               ? drv2605_is_playing(&s_haptic_motor, playing)
               : ESP_ERR_INVALID_STATE;
}
