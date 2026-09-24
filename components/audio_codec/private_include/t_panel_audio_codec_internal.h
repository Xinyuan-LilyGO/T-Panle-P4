#pragma once

#include "audio_codec_ctrl_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_if_t *codec_if;
} t_panel_audio_chip_instance_t;

esp_err_t t_panel_es8389_create(i2c_master_bus_handle_t i2c_bus,
                                const audio_codec_gpio_if_t *gpio_if,
                                esp_codec_dec_work_mode_t mode,
                                uint16_t mclk_multiple,
                                t_panel_audio_chip_instance_t *instance);
esp_err_t t_panel_es8389_configure_rect_output(esp_codec_dev_handle_t playback_dev);
void t_panel_es8389_destroy(t_panel_audio_chip_instance_t *instance);

esp_err_t t_panel_es7210_create(i2c_master_bus_handle_t i2c_bus,
                                uint8_t mic_mask,
                                uint16_t mclk_multiple,
                                t_panel_audio_chip_instance_t *instance);
void t_panel_es7210_destroy(t_panel_audio_chip_instance_t *instance);
