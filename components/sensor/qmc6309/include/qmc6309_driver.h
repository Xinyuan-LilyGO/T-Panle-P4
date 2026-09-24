#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMC6309_I2C_ADDR 0x7C

typedef enum {
    QMC6309_MODE_SUSPEND = 0,
    QMC6309_MODE_NORMAL,
    QMC6309_MODE_SINGLE,
    QMC6309_MODE_CONTINUOUS,
} qmc6309_mode_t;

typedef enum {
    QMC6309_RANGE_8_GAUSS = 8,
    QMC6309_RANGE_16_GAUSS = 16,
    QMC6309_RANGE_32_GAUSS = 32,
} qmc6309_range_t;

typedef enum {
    QMC6309_ODR_1_HZ = 1,
    QMC6309_ODR_10_HZ = 10,
    QMC6309_ODR_50_HZ = 50,
    QMC6309_ODR_100_HZ = 100,
    QMC6309_ODR_200_HZ = 200,
} qmc6309_odr_t;

typedef enum {
    QMC6309_OSR_1 = 1,
    QMC6309_OSR_2 = 2,
    QMC6309_OSR_4 = 4,
    QMC6309_OSR_8 = 8,
} qmc6309_osr_t;

typedef struct {
    qmc6309_mode_t mode;
    qmc6309_range_t range;
    qmc6309_odr_t output_data_rate;
    qmc6309_osr_t oversampling;
    float declination_degrees;
} qmc6309_config_t;

#define QMC6309_CONFIG_DEFAULT()                 \
    {                                            \
        .mode = QMC6309_MODE_CONTINUOUS,         \
        .range = QMC6309_RANGE_8_GAUSS,          \
        .output_data_rate = QMC6309_ODR_50_HZ,   \
        .oversampling = QMC6309_OSR_8,           \
        .declination_degrees = 0.0f,             \
    }

typedef struct {
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    float x_gauss;
    float y_gauss;
    float z_gauss;
    float heading_degrees;
    bool overflow;
} qmc6309_data_t;

typedef struct {
    void *driver;
} qmc6309_handle_t;

esp_err_t qmc6309_init(qmc6309_handle_t *handle,
                       i2c_master_bus_handle_t bus,
                       uint8_t address,
                       const qmc6309_config_t *config);
esp_err_t qmc6309_deinit(qmc6309_handle_t *handle);
esp_err_t qmc6309_configure(qmc6309_handle_t *handle,
                            const qmc6309_config_t *config);
esp_err_t qmc6309_data_ready(qmc6309_handle_t *handle, bool *ready);
esp_err_t qmc6309_read(qmc6309_handle_t *handle, qmc6309_data_t *data);
esp_err_t qmc6309_self_test(qmc6309_handle_t *handle,
                            int16_t *x_result,
                            int16_t *y_result,
                            int16_t *z_result);

#ifdef __cplusplus
}
#endif

