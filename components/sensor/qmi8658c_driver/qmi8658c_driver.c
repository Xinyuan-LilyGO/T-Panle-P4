#include "qmi8658c_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define QMI8658C_WHO_AM_I_VALUE 0x05
#define QMI8658C_CTRL1_INT1_EN  0x08
#define QMI8658C_CTRL1_INT2_EN  0x10
#define QMI8658C_CTRL1_INT_EN   (QMI8658C_CTRL1_INT1_EN | QMI8658C_CTRL1_INT2_EN)
#define QMI8658C_CTRL7_DRDY_DIS 0x20
#define QMI8658C_CTRL8           0x09
#define QMI8658C_CTRL8_HANDSHAKE_STATUS 0x80
#define QMI8658C_FIFO_CTRL      0x14
#define QMI8658C_FIFO_MODE_MASK 0x03
#define QMI8658C_STATUS_INT     0x2D
#define QMI8658C_STATUS0        0x2E
#define QMI8658C_RESET_RESULT   0x4D
#define QMI8658C_RESET_DONE     0x80
#define QMI8658C_RESET_TIMEOUT_MS 500

static const char *TAG = "qmi8658c_driver";

static esp_err_t write_register(t_panel_qmi8658c_t *handle,
                                uint8_t reg,
                                uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_transmit(handle->device, command, sizeof(command), -1);
}

static esp_err_t read_register(t_panel_qmi8658c_t *handle,
                               uint8_t reg,
                               uint8_t *value)
{
    return i2c_master_transmit_receive(handle->device, &reg, 1, value, 1, -1);
}

static esp_err_t read_registers(t_panel_qmi8658c_t *handle,
                                uint8_t reg,
                                uint8_t *data,
                                size_t length)
{
    return i2c_master_transmit_receive(handle->device, &reg, 1, data, length, -1);
}

static bool config_is_valid(const qmi8658c_config_t *config)
{
    const bool accelerometer_odr_valid = config &&
        (config->acc_odr <= QMI8658C_ACC_ODR_31_25 ||
         (config->acc_odr >= QMI8658C_ACC_ODR_128 &&
          config->acc_odr <= QMI8658C_ACC_ODR_3));
    return config &&
           config->mode >= QMI8658C_MODE_ACC_ONLY &&
           config->mode <= QMI8658C_MODE_DUAL &&
           config->acc_scale <= QMI8658C_ACC_SCALE_16G &&
           accelerometer_odr_valid &&
           config->gyro_scale <= QMI8658C_GYRO_SCALE_2048DPS &&
           config->gyro_odr <= QMI8658C_GYRO_ODR_31_25;
}

static uint16_t accelerometer_sensitivity(qmi8658c_acc_scale_t scale)
{
    static const uint16_t values[] = {
        ACC_SCALE_SENSITIVITY_2G,
        ACC_SCALE_SENSITIVITY_4G,
        ACC_SCALE_SENSITIVITY_8G,
        ACC_SCALE_SENSITIVITY_16G,
    };
    return values[scale];
}

static uint16_t gyroscope_sensitivity(qmi8658c_gyro_scale_t scale)
{
    static const uint16_t values[] = {
        GYRO_SCALE_SENSITIVITY_16DPS,
        GYRO_SCALE_SENSITIVITY_32DPS,
        GYRO_SCALE_SENSITIVITY_64DPS,
        GYRO_SCALE_SENSITIVITY_128DPS,
        GYRO_SCALE_SENSITIVITY_256DPS,
        GYRO_SCALE_SENSITIVITY_512DPS,
        GYRO_SCALE_SENSITIVITY_1024DPS,
        GYRO_SCALE_SENSITIVITY_2048DPS,
    };
    return values[scale];
}

static esp_err_t reset_device(t_panel_qmi8658c_t *handle)
{
    esp_err_t ret = write_register(handle, QMI8658_RESET, 0xB0);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint32_t elapsed = 0; elapsed < QMI8658C_RESET_TIMEOUT_MS; elapsed += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t result = 0;
        ret = read_register(handle, QMI8658C_RESET_RESULT, &result);
        if (ret == ESP_OK && result == QMI8658C_RESET_DONE) {
            return ESP_OK;
        }
    }
    return ret == ESP_OK ? ESP_ERR_TIMEOUT : ret;
}

static esp_err_t update_register(t_panel_qmi8658c_t *handle,
                                 uint8_t reg,
                                 uint8_t clear_mask,
                                 uint8_t set_bits)
{
    uint8_t value = 0;
    esp_err_t ret = read_register(handle, reg, &value);
    if (ret != ESP_OK) {
        return ret;
    }
    value = (value & (uint8_t)~clear_mask) | set_bits;
    return write_register(handle, reg, value);
}

static void IRAM_ATTR data_ready_isr(void *arg)
{
    t_panel_qmi8658c_t *handle = (t_panel_qmi8658c_t *)arg;
    handle->interrupt_count++;
    SemaphoreHandle_t semaphore =
        (SemaphoreHandle_t)handle->interrupt_semaphore;
    if (semaphore) {
        BaseType_t task_woken = pdFALSE;
        xSemaphoreGiveFromISR(semaphore, &task_woken);
        if (task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static void release_interrupt_resources(t_panel_qmi8658c_t *handle)
{
    if (handle->interrupt_gpio != GPIO_NUM_NC) {
        gpio_intr_disable(handle->interrupt_gpio);
        gpio_isr_handler_remove(handle->interrupt_gpio);
        gpio_set_intr_type(handle->interrupt_gpio, GPIO_INTR_DISABLE);
    }
    if (handle->interrupt_semaphore) {
        vSemaphoreDelete((SemaphoreHandle_t)handle->interrupt_semaphore);
    }
    handle->interrupt_gpio = GPIO_NUM_NC;
    handle->interrupt_semaphore = NULL;
    handle->interrupt_count = 0;
    handle->interrupt_enabled = false;
}

esp_err_t t_panel_qmi8658c_configure(t_panel_qmi8658c_t *handle,
                                     const qmi8658c_config_t *config)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = update_register(handle, QMI8658_CTRL2, 0x7F,
                                    ((uint8_t)config->acc_scale << 4) |
                                    (uint8_t)config->acc_odr);
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658_CTRL3, 0x7F,
                              ((uint8_t)config->gyro_scale << 4) |
                              (uint8_t)config->gyro_odr);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658_CTRL7, 0x03,
                              (uint8_t)config->mode);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    handle->config = *config;
    handle->accelerometer_sensitivity =
        accelerometer_sensitivity(config->acc_scale);
    handle->gyroscope_sensitivity = gyroscope_sensitivity(config->gyro_scale);
    return ESP_OK;
}

esp_err_t t_panel_qmi8658c_init(t_panel_qmi8658c_t *handle,
                                i2c_master_bus_handle_t bus,
                                uint8_t address,
                                const qmi8658c_config_t *config)
{
    if (!handle || !bus || !config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->device || handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t selected_address = address;
    if (selected_address == T_PANEL_QMI8658C_ADDR_AUTO) {
        if (i2c_master_probe(bus, T_PANEL_QMI8658C_ADDR_LOW, 20) == ESP_OK) {
            selected_address = T_PANEL_QMI8658C_ADDR_LOW;
        } else if (i2c_master_probe(bus, T_PANEL_QMI8658C_ADDR_HIGH, 20) == ESP_OK) {
            selected_address = T_PANEL_QMI8658C_ADDR_HIGH;
        } else {
            return ESP_ERR_NOT_FOUND;
        }
    } else if (selected_address != T_PANEL_QMI8658C_ADDR_LOW &&
               selected_address != T_PANEL_QMI8658C_ADDR_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = selected_address,
        .scl_speed_hz = QMI8658C_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &device_config, &handle->device);
    if (ret != ESP_OK) {
        return ret;
    }
    handle->address = selected_address;
    handle->interrupt_gpio = GPIO_NUM_NC;

    ret = reset_device(handle);
    if (ret == ESP_OK) {
        ret = read_register(handle, QMI8658_WHO_AM_I, &handle->who_am_i);
    }
    if (ret == ESP_OK && handle->who_am_i != QMI8658C_WHO_AM_I_VALUE) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        ret = read_register(handle, QMI8658_REVISION, &handle->revision);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658_CTRL1, 0x61, 0x40);
    }
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(handle->device);
        memset(handle, 0, sizeof(*handle));
        return ret;
    }

    handle->initialized = true;
    ret = t_panel_qmi8658c_configure(handle, config);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(handle->device);
        memset(handle, 0, sizeof(*handle));
    }
    return ret;
}

esp_err_t t_panel_qmi8658c_deinit(t_panel_qmi8658c_t *handle)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = t_panel_qmi8658c_disable_data_ready_interrupt(handle);
    const esp_err_t power_ret = t_panel_qmi8658c_set_power(handle, false);
    const esp_err_t remove_ret = i2c_master_bus_rm_device(handle->device);
    memset(handle, 0, sizeof(*handle));
    if (ret != ESP_OK) {
        return ret;
    }
    return power_ret != ESP_OK ? power_ret : remove_ret;
}

esp_err_t t_panel_qmi8658c_data_ready(t_panel_qmi8658c_t *handle,
                                      bool *accelerometer_ready,
                                      bool *gyroscope_ready)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!accelerometer_ready || !gyroscope_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    const esp_err_t ret = read_register(handle, QMI8658C_STATUS0, &status);
    if (ret == ESP_OK) {
        *accelerometer_ready = (status & 0x01) != 0;
        *gyroscope_ready = (status & 0x02) != 0;
    }
    return ret;
}

esp_err_t t_panel_qmi8658c_read(t_panel_qmi8658c_t *handle,
                                qmi8658c_data_t *data)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[14] = {0};
    esp_err_t ret = read_registers(handle, QMI8658_TEMP_L, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    const int16_t temperature = (int16_t)((uint16_t)raw[1] << 8 | raw[0]);
    const int16_t acc_x = (int16_t)((uint16_t)raw[3] << 8 | raw[2]);
    const int16_t acc_y = (int16_t)((uint16_t)raw[5] << 8 | raw[4]);
    const int16_t acc_z = (int16_t)((uint16_t)raw[7] << 8 | raw[6]);
    const int16_t gyro_x = (int16_t)((uint16_t)raw[9] << 8 | raw[8]);
    const int16_t gyro_y = (int16_t)((uint16_t)raw[11] << 8 | raw[10]);
    const int16_t gyro_z = (int16_t)((uint16_t)raw[13] << 8 | raw[12]);

    data->temperature = (float)temperature / TEMPERATURE_SENSOR_RESOLUTION;
    data->acc.x = (float)acc_x / handle->accelerometer_sensitivity;
    data->acc.y = (float)acc_y / handle->accelerometer_sensitivity;
    data->acc.z = (float)acc_z / handle->accelerometer_sensitivity;
    data->gyro.x = (float)gyro_x / handle->gyroscope_sensitivity;
    data->gyro.y = (float)gyro_y / handle->gyroscope_sensitivity;
    data->gyro.z = (float)gyro_z / handle->gyroscope_sensitivity;
    return ESP_OK;
}

esp_err_t t_panel_qmi8658c_enable_data_ready_interrupt(
    t_panel_qmi8658c_t *handle,
    gpio_num_t interrupt_gpio)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!GPIO_IS_VALID_GPIO(interrupt_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->interrupt_enabled || handle->interrupt_semaphore) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t semaphore = xSemaphoreCreateBinary();
    if (!semaphore) {
        return ESP_ERR_NO_MEM;
    }
    handle->interrupt_gpio = interrupt_gpio;
    handle->interrupt_semaphore = semaphore;
    handle->interrupt_count = 0;

    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << interrupt_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&gpio_cfg);
    if (ret == ESP_OK) {
        ret = gpio_install_isr_service(0);
        if (ret == ESP_ERR_INVALID_STATE) {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK) {
        ret = gpio_isr_handler_add(interrupt_gpio, data_ready_isr, handle);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658C_FIFO_CTRL,
                              QMI8658C_FIFO_MODE_MASK, 0);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658_CTRL7,
                              QMI8658C_CTRL7_DRDY_DIS,
                              QMI8658C_CTRL7_DRDY_DIS);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658C_CTRL8,
                              QMI8658C_CTRL8_HANDSHAKE_STATUS,
                              QMI8658C_CTRL8_HANDSHAKE_STATUS);
    }
    if (ret == ESP_OK) {
        /* T-Qst diode-ORs INT1 and INT2; both must leave high-Z mode. */
        ret = update_register(handle, QMI8658_CTRL1,
                              QMI8658C_CTRL1_INT_EN,
                              QMI8658C_CTRL1_INT_EN);
    }
    if (ret == ESP_OK) {
        uint8_t stale_data[14];
        ret = read_registers(handle, QMI8658_TEMP_L,
                             stale_data, sizeof(stale_data));
    }
    if (ret == ESP_OK) {
        ret = gpio_set_intr_type(interrupt_gpio, GPIO_INTR_POSEDGE);
    }
    if (ret == ESP_OK) {
        ret = gpio_intr_enable(interrupt_gpio);
    }
    if (ret == ESP_OK) {
        ret = update_register(handle, QMI8658_CTRL7,
                              QMI8658C_CTRL7_DRDY_DIS, 0);
    }
    if (ret != ESP_OK) {
        update_register(handle, QMI8658_CTRL1, QMI8658C_CTRL1_INT_EN, 0);
        update_register(handle, QMI8658C_CTRL8,
                        QMI8658C_CTRL8_HANDSHAKE_STATUS, 0);
        update_register(handle, QMI8658_CTRL7,
                        QMI8658C_CTRL7_DRDY_DIS, 0);
        release_interrupt_resources(handle);
        return ret;
    }

    handle->interrupt_enabled = true;
    uint8_t ctrl1 = 0;
    uint8_t ctrl7 = 0;
    uint8_t ctrl8 = 0;
    uint8_t fifo_ctrl = 0;
    uint8_t status_int = 0;
    read_register(handle, QMI8658_CTRL1, &ctrl1);
    read_register(handle, QMI8658_CTRL7, &ctrl7);
    read_register(handle, QMI8658C_CTRL8, &ctrl8);
    read_register(handle, QMI8658C_FIFO_CTRL, &fifo_ctrl);
    read_register(handle, QMI8658C_STATUS_INT, &status_int);
    ESP_LOGI(TAG,
             "IRQ config: CTRL1=0x%02X CTRL7=0x%02X CTRL8=0x%02X FIFO=0x%02X STATUSINT=0x%02X GPIO%d=%d",
             ctrl1, ctrl7, ctrl8, fifo_ctrl, status_int,
             interrupt_gpio, gpio_get_level(interrupt_gpio));

    if (gpio_get_level(interrupt_gpio) != 0) {
        xSemaphoreGive(semaphore);
    }
    return ESP_OK;
}

esp_err_t t_panel_qmi8658c_wait_for_data(t_panel_qmi8658c_t *handle,
                                         uint32_t timeout_ms)
{
    if (!handle || !handle->initialized || !handle->interrupt_enabled ||
        !handle->interrupt_semaphore) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t timeout_ticks = timeout_ms == UINT32_MAX
                                         ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)handle->interrupt_semaphore,
                          timeout_ticks) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t t_panel_qmi8658c_disable_data_ready_interrupt(
    t_panel_qmi8658c_t *handle)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle->interrupt_enabled && !handle->interrupt_semaphore) {
        return ESP_OK;
    }

    gpio_intr_disable(handle->interrupt_gpio);
    const esp_err_t ret = update_register(handle, QMI8658_CTRL1,
                                          QMI8658C_CTRL1_INT_EN, 0);
    update_register(handle, QMI8658C_CTRL8,
                    QMI8658C_CTRL8_HANDSHAKE_STATUS, 0);
    release_interrupt_resources(handle);
    return ret;
}

esp_err_t t_panel_qmi8658c_set_power(t_panel_qmi8658c_t *handle, bool enabled)
{
    if (!handle || !handle->device || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (enabled) {
        esp_err_t ret = update_register(handle, QMI8658_CTRL1, 0x01, 0x00);
        if (ret != ESP_OK) {
            return ret;
        }
        return update_register(handle, QMI8658_CTRL7, 0x03,
                               (uint8_t)handle->config.mode);
    }

    esp_err_t ret = update_register(handle, QMI8658_CTRL7, 0x03, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }
    return update_register(handle, QMI8658_CTRL1, 0x01, 0x01);
}
