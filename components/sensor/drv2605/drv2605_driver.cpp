#include "drv2605_driver.h"

#include <new>

#include "HapticDrivers.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint8_t DRV2605_REG_STATUS = 0x00;
constexpr uint8_t DRV2605_REG_RATED_VOLTAGE = 0x16;
constexpr uint8_t DRV2605_REG_OVERDRIVE_CLAMP = 0x17;
constexpr uint8_t DRV2605_REG_CONTROL3 = 0x1D;
constexpr uint8_t DRV2605_STATUS_DIAG_RESULT = 0x08;
constexpr uint8_t DRV2605_CONTROL3_ERM_OPEN_LOOP = 0x20;
constexpr uint8_t DRV2605_CONTROL3_LRA_OPEN_LOOP = 0x01;

struct Drv2605Context {
    HapticDriver_DRV2605 driver;
    gpio_num_t enable_gpio = GPIO_NUM_NC;
};

Drv2605Context *get_context(drv2605_handle_t *handle)
{
    return handle ? static_cast<Drv2605Context *>(handle->driver) : nullptr;
}

esp_err_t configure_enable_pin(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) {
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret == ESP_OK) {
        ret = gpio_set_level(pin, 1);
    }
    return ret;
}

esp_err_t run_auto_calibration(Drv2605Context *context)
{
    if (!context->driver.calibrate()) {
        return ESP_FAIL;
    }

    const int status = context->driver.readReg(DRV2605_REG_STATUS);
    if (status < 0 || (status & DRV2605_STATUS_DIAG_RESULT) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t drv2605_init(drv2605_handle_t *handle,
                                    i2c_master_bus_handle_t bus,
                                    const drv2605_config_t *config)
{
    if (!handle || !bus || !config || config->address > 0x7F ||
        config->waveform_library > 7 ||
        (config->actuator != DRV2605_ACTUATOR_ERM &&
         config->actuator != DRV2605_ACTUATOR_LRA)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->driver) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = configure_enable_pin(config->enable_gpio);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    Drv2605Context *context = new (std::nothrow) Drv2605Context();
    if (!context) {
        if (config->enable_gpio != GPIO_NUM_NC) {
            gpio_set_level(config->enable_gpio, 0);
        }
        return ESP_ERR_NO_MEM;
    }
    context->enable_gpio = config->enable_gpio;

    if (!context->driver.begin(bus, config->address)) {
        delete context;
        if (config->enable_gpio != GPIO_NUM_NC) {
            gpio_set_level(config->enable_gpio, 0);
        }
        return ESP_ERR_NOT_FOUND;
    }

    const HapticActuatorType actuator =
        config->actuator == DRV2605_ACTUATOR_LRA
            ? HapticActuatorType::LRA
            : HapticActuatorType::ERM;
    if (!context->driver.setActuatorType(actuator)) {
        delete context;
        if (config->enable_gpio != GPIO_NUM_NC) {
            gpio_set_level(config->enable_gpio, 0);
        }
        return ESP_FAIL;
    }
    context->driver.selectLibrary(config->waveform_library);

    if (context->driver.writeReg(DRV2605_REG_RATED_VOLTAGE,
                                 config->rated_voltage_reg) < 0 ||
        context->driver.writeReg(DRV2605_REG_OVERDRIVE_CLAMP,
                                 config->overdrive_clamp_reg) < 0) {
        delete context;
        if (config->enable_gpio != GPIO_NUM_NC) {
            gpio_set_level(config->enable_gpio, 0);
        }
        return ESP_FAIL;
    }

    const uint8_t open_loop_mask = DRV2605_CONTROL3_ERM_OPEN_LOOP |
                                   DRV2605_CONTROL3_LRA_OPEN_LOOP;
    const uint8_t open_loop_value = config->open_loop
                                        ? (config->actuator == DRV2605_ACTUATOR_ERM
                                               ? DRV2605_CONTROL3_ERM_OPEN_LOOP
                                               : DRV2605_CONTROL3_LRA_OPEN_LOOP)
                                        : 0;
    if (context->driver.updateBits(DRV2605_REG_CONTROL3,
                                   open_loop_mask,
                                   open_loop_value) < 0 ||
        (config->auto_calibrate && run_auto_calibration(context) != ESP_OK)) {
        delete context;
        if (config->enable_gpio != GPIO_NUM_NC) {
            gpio_set_level(config->enable_gpio, 0);
        }
        return ESP_FAIL;
    }

    handle->driver = context;
    return ESP_OK;
}

extern "C" esp_err_t drv2605_deinit(drv2605_handle_t *handle)
{
    Drv2605Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }

    context->driver.stop();
    const gpio_num_t enable_gpio = context->enable_gpio;
    delete context;
    handle->driver = nullptr;
    if (enable_gpio != GPIO_NUM_NC) {
        return gpio_set_level(enable_gpio, 0);
    }
    return ESP_OK;
}

extern "C" esp_err_t drv2605_play_effect(drv2605_handle_t *handle,
                                           uint8_t effect_id)
{
    Drv2605Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }
    if (effect_id < DRV2605_EFFECT_MIN || effect_id > DRV2605_EFFECT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return context->driver.playEffect(effect_id) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t drv2605_set_sequence(drv2605_handle_t *handle,
                                            const uint8_t *effects,
                                            size_t effect_count)
{
    Drv2605Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((!effects && effect_count > 0) || effect_count > DRV2605_SEQUENCE_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->driver.clearSequence()) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < effect_count; ++i) {
        if (effects[i] > DRV2605_EFFECT_MAX ||
            !context->driver.setSequence(i, effects[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

extern "C" esp_err_t drv2605_play_sequence(drv2605_handle_t *handle)
{
    Drv2605Context *context = get_context(handle);
    return !context ? ESP_ERR_INVALID_STATE
                    : (context->driver.playSequence() ? ESP_OK : ESP_FAIL);
}

extern "C" esp_err_t drv2605_stop(drv2605_handle_t *handle)
{
    Drv2605Context *context = get_context(handle);
    return !context ? ESP_ERR_INVALID_STATE
                    : (context->driver.stop() ? ESP_OK : ESP_FAIL);
}

extern "C" esp_err_t drv2605_is_playing(drv2605_handle_t *handle, bool *playing)
{
    Drv2605Context *context = get_context(handle);
    if (!context || !playing) {
        return context ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    *playing = context->driver.isPlaying();
    return ESP_OK;
}

extern "C" esp_err_t drv2605_set_realtime_value(drv2605_handle_t *handle,
                                                  uint8_t value)
{
    Drv2605Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!context->driver.setMode(HapticMode::REAL_TIME_PLAYBACK)) {
        return ESP_FAIL;
    }
    return context->driver.setRealtimeValue(value) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t drv2605_auto_calibrate(drv2605_handle_t *handle)
{
    Drv2605Context *context = get_context(handle);
    return !context ? ESP_ERR_INVALID_STATE : run_auto_calibration(context);
}

extern "C" esp_err_t drv2605_set_standby(drv2605_handle_t *handle, bool standby)
{
    Drv2605Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }
    const HapticMode mode = standby ? HapticMode::STANDBY
                                    : HapticMode::INTERNAL_TRIGGER;
    return context->driver.setMode(mode) ? ESP_OK : ESP_FAIL;
}
