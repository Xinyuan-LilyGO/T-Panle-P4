#include "qmc6309_driver.h"

#include <cmath>
#include <new>

#include "SensorQMC6309.hpp"

namespace {

constexpr uint8_t QMC6309_REG_DATA_X_LSB = 0x01;
constexpr uint8_t QMC6309_REG_STATUS = 0x09;
constexpr uint8_t QMC6309_STATUS_DATA_READY = 0x01;
constexpr uint8_t QMC6309_STATUS_OVERFLOW = 0x02;
constexpr float DEGREES_PER_RADIAN = 57.29577951308232f;

struct Qmc6309Context {
    SensorQMC6309 sensor;
    qmc6309_config_t config;
    bool data_ready_latched = false;
    bool overflow_latched = false;
};

Qmc6309Context *get_context(qmc6309_handle_t *handle)
{
    return handle ? static_cast<Qmc6309Context *>(handle->driver) : nullptr;
}

bool convert_mode(qmc6309_mode_t source, OperationMode &target)
{
    switch (source) {
    case QMC6309_MODE_SUSPEND:
        target = OperationMode::SUSPEND;
        return true;
    case QMC6309_MODE_NORMAL:
        target = OperationMode::NORMAL;
        return true;
    case QMC6309_MODE_SINGLE:
        target = OperationMode::SINGLE_MEASUREMENT;
        return true;
    case QMC6309_MODE_CONTINUOUS:
        target = OperationMode::CONTINUOUS_MEASUREMENT;
        return true;
    default:
        return false;
    }
}

bool convert_range(qmc6309_range_t source, MagFullScaleRange &target)
{
    switch (source) {
    case QMC6309_RANGE_8_GAUSS:
        target = MagFullScaleRange::FS_8G;
        return true;
    case QMC6309_RANGE_16_GAUSS:
        target = MagFullScaleRange::FS_16G;
        return true;
    case QMC6309_RANGE_32_GAUSS:
        target = MagFullScaleRange::FS_32G;
        return true;
    default:
        return false;
    }
}

bool convert_osr(qmc6309_osr_t source, MagOverSampleRatio &target)
{
    switch (source) {
    case QMC6309_OSR_1:
        target = MagOverSampleRatio::OSR_1;
        return true;
    case QMC6309_OSR_2:
        target = MagOverSampleRatio::OSR_2;
        return true;
    case QMC6309_OSR_4:
        target = MagOverSampleRatio::OSR_4;
        return true;
    case QMC6309_OSR_8:
        target = MagOverSampleRatio::OSR_8;
        return true;
    default:
        return false;
    }
}

float sensitivity_for_range(qmc6309_range_t range)
{
    switch (range) {
    case QMC6309_RANGE_8_GAUSS:
        return 0.00025f;
    case QMC6309_RANGE_16_GAUSS:
        return 0.0005f;
    case QMC6309_RANGE_32_GAUSS:
        return 0.001f;
    default:
        return 0.0f;
    }
}

float calculate_heading(float x, float y, float declination_degrees)
{
    float heading = std::atan2(y, x) * DEGREES_PER_RADIAN +
                    declination_degrees;
    heading = std::fmod(heading, 360.0f);
    return heading < 0.0f ? heading + 360.0f : heading;
}

esp_err_t configure(Qmc6309Context *context, const qmc6309_config_t *config)
{
    if (!context || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    OperationMode mode;
    MagFullScaleRange range;
    MagOverSampleRatio osr;
    if (!convert_mode(config->mode, mode) ||
        !convert_range(config->range, range) ||
        !convert_osr(config->oversampling, osr)) {
        return ESP_ERR_INVALID_ARG;
    }

    const float odr = static_cast<float>(config->output_data_rate);
    if (odr != 1.0f && odr != 10.0f && odr != 50.0f &&
        odr != 100.0f && odr != 200.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->sensor.configMagnetometer(mode, range, odr, osr)) {
        return ESP_FAIL;
    }
    context->sensor.setDeclination(config->declination_degrees);
    context->config = *config;
    context->data_ready_latched = false;
    context->overflow_latched = false;
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t qmc6309_init(qmc6309_handle_t *handle,
                                    i2c_master_bus_handle_t bus,
                                    uint8_t address,
                                    const qmc6309_config_t *config)
{
    if (!handle || !bus || !config || address > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->driver) {
        return ESP_ERR_INVALID_STATE;
    }

    Qmc6309Context *context = new (std::nothrow) Qmc6309Context();
    if (!context) {
        return ESP_ERR_NO_MEM;
    }
    if (!context->sensor.begin(bus, address)) {
        delete context;
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t ret = configure(context, config);
    if (ret != ESP_OK) {
        delete context;
        return ret;
    }

    handle->driver = context;
    return ESP_OK;
}

extern "C" esp_err_t qmc6309_deinit(qmc6309_handle_t *handle)
{
    Qmc6309Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }

    delete context;
    handle->driver = nullptr;
    return ESP_OK;
}

extern "C" esp_err_t qmc6309_configure(qmc6309_handle_t *handle,
                                         const qmc6309_config_t *config)
{
    Qmc6309Context *context = get_context(handle);
    return context ? configure(context, config) : ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t qmc6309_data_ready(qmc6309_handle_t *handle, bool *ready)
{
    Qmc6309Context *context = get_context(handle);
    if (!context || !ready) {
        return context ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    if (context->data_ready_latched) {
        *ready = true;
        return ESP_OK;
    }

    const int status = context->sensor.readReg(QMC6309_REG_STATUS);
    if (status < 0) {
        return ESP_FAIL;
    }

    /* Preserve DRDY because reading the status register can consume it. */
    context->data_ready_latched =
        (status & QMC6309_STATUS_DATA_READY) != 0;
    if (context->data_ready_latched) {
        context->overflow_latched =
            (status & QMC6309_STATUS_OVERFLOW) != 0;
    }
    *ready = context->data_ready_latched;
    return ESP_OK;
}

extern "C" esp_err_t qmc6309_read(qmc6309_handle_t *handle,
                                    qmc6309_data_t *data)
{
    Qmc6309Context *context = get_context(handle);
    if (!context || !data) {
        return context ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    bool overflow = context->overflow_latched;
    if (!context->data_ready_latched) {
        const int status = context->sensor.readReg(QMC6309_REG_STATUS);
        if (status < 0) {
            return ESP_FAIL;
        }
        if ((status & QMC6309_STATUS_DATA_READY) == 0) {
            return ESP_ERR_NOT_FINISHED;
        }
        overflow = (status & QMC6309_STATUS_OVERFLOW) != 0;
    }

    context->data_ready_latched = false;
    context->overflow_latched = false;

    uint8_t buffer[6] = {0};
    if (context->sensor.readRegBuff(QMC6309_REG_DATA_X_LSB,
                                    buffer,
                                    sizeof(buffer)) < 0) {
        return ESP_FAIL;
    }

    data->raw_x = static_cast<int16_t>((static_cast<uint16_t>(buffer[1]) << 8) |
                                       buffer[0]);
    data->raw_y = static_cast<int16_t>((static_cast<uint16_t>(buffer[3]) << 8) |
                                       buffer[2]);
    data->raw_z = static_cast<int16_t>((static_cast<uint16_t>(buffer[5]) << 8) |
                                       buffer[4]);

    const float sensitivity = sensitivity_for_range(context->config.range);
    if (sensitivity == 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }
    data->x_gauss = static_cast<float>(data->raw_x) * sensitivity;
    data->y_gauss = static_cast<float>(data->raw_y) * sensitivity;
    data->z_gauss = static_cast<float>(data->raw_z) * sensitivity;
    data->heading_degrees = calculate_heading(data->x_gauss,
                                              data->y_gauss,
                                              context->config.declination_degrees);
    data->overflow = overflow;
    return ESP_OK;
}

extern "C" esp_err_t qmc6309_self_test(qmc6309_handle_t *handle,
                                         int16_t *x_result,
                                         int16_t *y_result,
                                         int16_t *z_result)
{
    Qmc6309Context *context = get_context(handle);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!x_result || !y_result || !z_result) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool passed = context->sensor.selfTest(*x_result, *y_result, *z_result);
    const esp_err_t restore_ret = configure(context, &context->config);
    if (restore_ret != ESP_OK) {
        return restore_ret;
    }
    return passed ? ESP_OK : ESP_FAIL;
}
