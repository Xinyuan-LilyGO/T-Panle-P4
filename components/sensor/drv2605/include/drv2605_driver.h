#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRV2605_I2C_ADDR 0x5A
#define DRV2605_EFFECT_MIN 1
#define DRV2605_EFFECT_MAX 117
#define DRV2605_SEQUENCE_SLOTS 8
#define DRV2605_RTP_STOP 0x00
#define DRV2605_RTP_MAX_FORWARD 0x7F

/* DRV2605 datasheet equations for an ERM actuator. */
#define DRV2605_ERM_RATED_VOLTAGE_MV(mv) \
    ((uint8_t)((((uint32_t)(mv) * 1000U) + 10665U) / 21330U))
#define DRV2605_ERM_OVERDRIVE_CLAMP_MV(mv) \
    ((uint8_t)((((uint32_t)(mv) * 1000U) + 10980U) / 21960U))

typedef enum {
    DRV2605_ACTUATOR_ERM = 0,
    DRV2605_ACTUATOR_LRA,
} drv2605_actuator_t;

typedef struct {
    uint8_t address;
    gpio_num_t enable_gpio;
    drv2605_actuator_t actuator;
    uint8_t waveform_library;
    uint8_t rated_voltage_reg;
    uint8_t overdrive_clamp_reg;
    bool open_loop;
    bool auto_calibrate;
} drv2605_config_t;

#define DRV2605_CONFIG_DEFAULT(enable_pin)                         \
    {                                                              \
        .address = DRV2605_I2C_ADDR,                               \
        .enable_gpio = (enable_pin),                               \
        .actuator = DRV2605_ACTUATOR_ERM,                           \
        .waveform_library = 1,                                     \
        .rated_voltage_reg = DRV2605_ERM_RATED_VOLTAGE_MV(3000),   \
        .overdrive_clamp_reg =                                     \
            DRV2605_ERM_OVERDRIVE_CLAMP_MV(3300),                  \
        .open_loop = true,                                         \
        .auto_calibrate = false,                                   \
    }

typedef struct {
    void *driver;
} drv2605_handle_t;

esp_err_t drv2605_init(drv2605_handle_t *handle,
                       i2c_master_bus_handle_t bus,
                       const drv2605_config_t *config);
esp_err_t drv2605_deinit(drv2605_handle_t *handle);
esp_err_t drv2605_play_effect(drv2605_handle_t *handle, uint8_t effect_id);
esp_err_t drv2605_set_sequence(drv2605_handle_t *handle,
                               const uint8_t *effects,
                               size_t effect_count);
esp_err_t drv2605_play_sequence(drv2605_handle_t *handle);
esp_err_t drv2605_stop(drv2605_handle_t *handle);
esp_err_t drv2605_is_playing(drv2605_handle_t *handle, bool *playing);
esp_err_t drv2605_set_realtime_value(drv2605_handle_t *handle, uint8_t value);
esp_err_t drv2605_auto_calibrate(drv2605_handle_t *handle);
esp_err_t drv2605_set_standby(drv2605_handle_t *handle, bool standby);

#ifdef __cplusplus
}
#endif
