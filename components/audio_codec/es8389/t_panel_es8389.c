#include <string.h>

#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8389_codec.h"
#include "t_panel_audio_codec_internal.h"

static const char *TAG = "t_panel_es8389";

#define ES8389_DAC_MISC_CONTROL_2_REG 0x45
#define ES8389_DAC2_INVERT_MASK       (1U << 5)

esp_err_t t_panel_es8389_create(i2c_master_bus_handle_t i2c_bus,
                                const audio_codec_gpio_if_t *gpio_if,
                                esp_codec_dec_work_mode_t mode,
                                uint16_t mclk_multiple,
                                t_panel_audio_chip_instance_t *instance)
{
    ESP_RETURN_ON_FALSE(i2c_bus && gpio_if && instance, ESP_ERR_INVALID_ARG,
                        TAG, "invalid ES8389 arguments");
    memset(instance, 0, sizeof(*instance));

    audio_codec_i2c_cfg_t i2c_config = {
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    instance->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(instance->ctrl_if, ESP_ERR_NO_MEM, TAG,
                        "create ES8389 control interface failed");

    es8389_codec_cfg_t codec_config = {
        .ctrl_if = instance->ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = mode,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = true,
        .mclk_div = mclk_multiple,
    };
    instance->codec_if = es8389_codec_new(&codec_config);
    if (!instance->codec_if) {
        audio_codec_delete_ctrl_if(instance->ctrl_if);
        memset(instance, 0, sizeof(*instance));
        ESP_LOGE(TAG, "create ES8389 codec interface failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t t_panel_es8389_configure_rect_output(esp_codec_dev_handle_t playback_dev)
{
    ESP_RETURN_ON_FALSE(playback_dev, ESP_ERR_INVALID_ARG, TAG,
                        "playback device is NULL");
    int value = 0;
    ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(playback_dev,
                                               ES8389_DAC_MISC_CONTROL_2_REG,
                                               &value),
                        TAG, "read DAC output control failed");
    value |= ES8389_DAC2_INVERT_MASK;
    return esp_codec_dev_write_reg(playback_dev,
                                   ES8389_DAC_MISC_CONTROL_2_REG, value);
}

void t_panel_es8389_destroy(t_panel_audio_chip_instance_t *instance)
{
    if (!instance) {
        return;
    }
    if (instance->codec_if) {
        audio_codec_delete_codec_if(instance->codec_if);
    }
    if (instance->ctrl_if) {
        audio_codec_delete_ctrl_if(instance->ctrl_if);
    }
    memset(instance, 0, sizeof(*instance));
}
