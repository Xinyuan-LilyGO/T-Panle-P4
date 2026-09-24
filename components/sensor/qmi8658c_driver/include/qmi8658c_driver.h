#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "qmi8658c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T_PANEL_QMI8658C_ADDR_AUTO 0x00
#define T_PANEL_QMI8658C_ADDR_LOW  0x6A
#define T_PANEL_QMI8658C_ADDR_HIGH 0x6B

#define T_PANEL_QMI8658C_CONFIG_DEFAULT()       \
    {                                           \
        .mode = QMI8658C_MODE_DUAL,             \
        .acc_scale = QMI8658C_ACC_SCALE_4G,     \
        .acc_odr = QMI8658C_ACC_ODR_125,        \
        .gyro_scale = QMI8658C_GYRO_SCALE_512DPS, \
        .gyro_odr = QMI8658C_GYRO_ODR_125,      \
    }

typedef struct {
    i2c_master_dev_handle_t device;
    qmi8658c_config_t config;
    uint16_t accelerometer_sensitivity;
    uint16_t gyroscope_sensitivity;
    uint8_t address;
    uint8_t who_am_i;
    uint8_t revision;
    gpio_num_t interrupt_gpio;
    void *interrupt_semaphore;
    volatile uint32_t interrupt_count;
    bool interrupt_enabled;
    bool initialized;
} t_panel_qmi8658c_t;

esp_err_t t_panel_qmi8658c_init(t_panel_qmi8658c_t *handle,
                                i2c_master_bus_handle_t bus,
                                uint8_t address,
                                const qmi8658c_config_t *config);
esp_err_t t_panel_qmi8658c_deinit(t_panel_qmi8658c_t *handle);
esp_err_t t_panel_qmi8658c_configure(t_panel_qmi8658c_t *handle,
                                     const qmi8658c_config_t *config);
esp_err_t t_panel_qmi8658c_data_ready(t_panel_qmi8658c_t *handle,
                                      bool *accelerometer_ready,
                                      bool *gyroscope_ready);
esp_err_t t_panel_qmi8658c_read(t_panel_qmi8658c_t *handle,
                                qmi8658c_data_t *data);
esp_err_t t_panel_qmi8658c_enable_data_ready_interrupt(
    t_panel_qmi8658c_t *handle,
    gpio_num_t interrupt_gpio);
esp_err_t t_panel_qmi8658c_wait_for_data(t_panel_qmi8658c_t *handle,
                                         uint32_t timeout_ms);
esp_err_t t_panel_qmi8658c_disable_data_ready_interrupt(
    t_panel_qmi8658c_t *handle);
esp_err_t t_panel_qmi8658c_set_power(t_panel_qmi8658c_t *handle, bool enabled);

#ifdef __cplusplus
}
#endif
