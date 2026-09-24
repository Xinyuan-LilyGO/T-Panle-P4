#include "esp_check.h"
#include "esp_io_expander_xl9555.h"
#include "bsp_private.h"

static const char *TAG = "t_panel_p4_bsp";

esp_err_t t_panel_p4_bsp_board_init(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp && bsp->i2c_bus, ESP_ERR_INVALID_ARG, TAG,
                        "I2C bus is not initialized");
    return esp_io_expander_new_i2c_xl9555(bsp->i2c_bus, XL9555_I2C_ADDR,
                                          &bsp->io_expander);
}

esp_err_t t_panel_p4_bsp_board_deinit(t_panel_p4_bsp_t *bsp)
{
    if (!bsp || !bsp->io_expander) {
        return ESP_OK;
    }
    const esp_err_t ret = esp_io_expander_del(bsp->io_expander);
    bsp->io_expander = NULL;
    return ret;
}

esp_err_t t_panel_p4_bsp_board_speaker_set(t_panel_p4_bsp_t *bsp, bool enabled)
{
    ESP_RETURN_ON_FALSE(bsp && bsp->io_expander, ESP_ERR_INVALID_ARG, TAG,
                        "IO expander is not initialized");
    const uint32_t mask = 1UL << XL9555_SPK_CRTL;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(bsp->io_expander, mask,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "set speaker pin direction failed");
    return esp_io_expander_set_level(bsp->io_expander, mask, enabled ? 1 : 0);
}

esp_err_t t_panel_p4_bsp_camera_flash_pwm_init(t_panel_p4_bsp_t *bsp)
{
    (void)bsp;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t t_panel_p4_bsp_camera_flash_set_brightness(
    t_panel_p4_bsp_t *bsp, t_panel_p4_camera_flash_t flash, uint8_t percent)
{
    (void)bsp;
    (void)flash;
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t t_panel_p4_bsp_camera_flash_set_all_brightness(
    t_panel_p4_bsp_t *bsp, uint8_t percent)
{
    (void)bsp;
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t t_panel_p4_bsp_camera_flash_pwm_deinit(t_panel_p4_bsp_t *bsp)
{
    (void)bsp;
    return ESP_ERR_NOT_SUPPORTED;
}
