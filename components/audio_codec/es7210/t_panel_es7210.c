#include <string.h>

#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es7210_adc.h"
#include "t_panel_audio_codec_internal.h"

static const char *TAG = "t_panel_es7210";

esp_err_t t_panel_es7210_create(i2c_master_bus_handle_t i2c_bus,
                                uint8_t mic_mask,
                                uint16_t mclk_multiple,
                                t_panel_audio_chip_instance_t *instance)
{
    const uint8_t valid_mics = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                               ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    ESP_RETURN_ON_FALSE(i2c_bus && instance && mic_mask &&
                            !(mic_mask & ~valid_mics),
                        ESP_ERR_INVALID_ARG, TAG, "invalid ES7210 arguments");
    memset(instance, 0, sizeof(*instance));

    audio_codec_i2c_cfg_t i2c_config = {
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    instance->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(instance->ctrl_if, ESP_ERR_NO_MEM, TAG,
                        "create ES7210 control interface failed");

    es7210_codec_cfg_t codec_config = {
        .ctrl_if = instance->ctrl_if,
        .master_mode = false,
        .mic_selected = mic_mask,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = mclk_multiple,
    };
    instance->codec_if = es7210_codec_new(&codec_config);
    if (!instance->codec_if) {
        audio_codec_delete_ctrl_if(instance->ctrl_if);
        memset(instance, 0, sizeof(*instance));
        ESP_LOGE(TAG, "create ES7210 codec interface failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void t_panel_es7210_destroy(t_panel_audio_chip_instance_t *instance)
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
