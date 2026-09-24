/*
 * Touch Driver for ESP32-P4 T-Panel-P4 (JD9365TX)
 * Based on Jadard TP debugging guide for RTOS
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "touch_jadard.h"
#include "board_config.h"

static const char *TAG = "touch";

#define SINGLE_TOUCH_POINT_SIZE 5
#define TOUCH_POINT_ADDR_OFFSET 3

/* -------------------- Internal helpers -------------------- */

static esp_err_t touch_i2c_read(i2c_master_dev_handle_t dev,
                                const uint8_t *cmd, uint8_t cmd_len,
                                uint8_t *out_buf, uint8_t read_len)
{
    return i2c_master_transmit_receive(dev, cmd, cmd_len, out_buf, read_len, 100);
}

static esp_err_t touch_i2c_write(i2c_master_dev_handle_t dev,
                                 const uint8_t *data, uint8_t data_len)
{
    return i2c_master_transmit(dev, data, data_len, 100);
}

static esp_err_t reset_via_xl9555(esp_io_expander_handle_t expander)
{
    uint32_t rst_pin_mask = (1UL << XL9555_TP_RST);

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, rst_pin_mask, IO_EXPANDER_OUTPUT),
                        TAG, "set TP_RST dir failed");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, rst_pin_mask, 1),
                        TAG, "TP_RST high failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, rst_pin_mask, 0),
                        TAG, "TP_RST low failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, rst_pin_mask, 1),
                        TAG, "TP_RST high failed");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Touch reset done via XL9555");
    return ESP_OK;
}

/* -------------------- Public API -------------------- */

esp_err_t touch_controller_init(touch_panel_t *handle,
                     i2c_master_bus_handle_t i2c_bus,
                     esp_io_expander_handle_t expander)
{
    memset(handle, 0, sizeof(*handle));

    ESP_RETURN_ON_ERROR(reset_via_xl9555(expander), TAG, "touch reset failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 200000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &handle->i2c_dev),
                        TAG, "add I2C device failed");

    vTaskDelay(pdMS_TO_TICKS(200));

    touch_controller_soft_reset(handle);

    handle->touch_info_addr = TOUCH_DATA_ADDR;

    uint8_t chip_id[2] = {0};
    esp_err_t ret = touch_controller_get_chip_id(handle, chip_id);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Chip ID: 0x%02x%02x", chip_id[0], chip_id[1]);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to read Chip ID, continuing anyway");
    }

    uint16_t x_res = 0, y_res = 0;
    touch_controller_get_resolution(handle, &x_res, &y_res);

    uint8_t fw_version[8];
    touch_controller_get_fw_version(handle, fw_version);
    ESP_LOGI(TAG, "FW Version: %02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x",
             fw_version[0], fw_version[1], fw_version[2], fw_version[3],
             fw_version[4], fw_version[5], fw_version[6], fw_version[7]);

    return ESP_OK;
}

esp_err_t touch_controller_soft_reset(touch_panel_t *handle)
{
    uint8_t rst_cmd[] = {
        (uint8_t)(SYSTEM_SOFT_RESET_ADDR >> 24), (uint8_t)(SYSTEM_SOFT_RESET_ADDR >> 16),
        (uint8_t)(SYSTEM_SOFT_RESET_ADDR >> 8), (uint8_t)(SYSTEM_SOFT_RESET_ADDR), 0xA5};
    ESP_RETURN_ON_ERROR(touch_i2c_write(handle->i2c_dev, rst_cmd, sizeof(rst_cmd)),
                        TAG, "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t por_cmd[] = {
        (uint8_t)(POR_INIT_ADDR >> 24), (uint8_t)(POR_INIT_ADDR >> 16),
        (uint8_t)(POR_INIT_ADDR >> 8), (uint8_t)(POR_INIT_ADDR), 0x00};
    ESP_RETURN_ON_ERROR(touch_i2c_write(handle->i2c_dev, por_cmd, sizeof(por_cmd)),
                        TAG, "POR init failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "touch soft reset done");
    return ESP_OK;
}

esp_err_t touch_controller_get_chip_id(touch_panel_t *handle, uint8_t *id)
{
    uint8_t chip_id_cmd[] = {
        (uint8_t)(CHIP_ID_ADDR >> 24),
        (uint8_t)(CHIP_ID_ADDR >> 16),
        (uint8_t)(CHIP_ID_ADDR >> 8),
        (uint8_t)(CHIP_ID_ADDR),
        0x00,
        0x02,
    };
    return touch_i2c_read(handle->i2c_dev, chip_id_cmd, sizeof(chip_id_cmd), id, 2);
}

esp_err_t touch_controller_get_fw_version(touch_panel_t *handle, uint8_t *version)
{
    uint8_t cmd[] = {
        (uint8_t)(FW_VERSION_ADDR >> 24), (uint8_t)(FW_VERSION_ADDR >> 16),
        (uint8_t)(FW_VERSION_ADDR >> 8), (uint8_t)(FW_VERSION_ADDR)};
    return touch_i2c_read(handle->i2c_dev, cmd, sizeof(cmd), version, 8);
}

esp_err_t touch_controller_get_resolution(touch_panel_t *handle, uint16_t *x_res, uint16_t *y_res)
{
    uint8_t cmd[] = {
        (uint8_t)(RESOLUTION_ADDR >> 24), (uint8_t)(RESOLUTION_ADDR >> 16),
        (uint8_t)(RESOLUTION_ADDR >> 8), (uint8_t)(RESOLUTION_ADDR)};
    uint8_t buf[4] = {0};
    ESP_RETURN_ON_ERROR(touch_i2c_read(handle->i2c_dev, cmd, sizeof(cmd), buf, 4),
                        TAG, "read resolution failed");
    *x_res = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    *y_res = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    ESP_LOGI(TAG, "resolution: %dx%d", *x_res, *y_res);
    return ESP_OK;
}

esp_err_t touch_controller_get_single_point(touch_panel_t *handle,
                                 touch_panel_data_t *data,
                                 uint8_t finger_num)
{
    if (finger_num == 0 || finger_num > TOUCH_PANEL_MAX_POINTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(data, 0, sizeof(*data));

    uint32_t addr = TOUCH_DATA_ADDR;

    uint8_t cmd[] = {
        (uint8_t)(addr >> 24),
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr),
        0x00,
        0x04,
    };

    esp_err_t ret = touch_i2c_write(handle->i2c_dev, cmd, sizeof(cmd));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "write touch addr failed");
        return ret;
    }

    esp_rom_delay_us(25);

    uint8_t buf[8] = {0};
    ret = i2c_master_receive(handle->i2c_dev, buf, 8, 100);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "get single point failed");
        return ret;
    }

    ESP_LOGI(TAG, "touch data:");
    for (int i = 0; i < 8; i++)
    {
        printf("0x%x ", buf[i]);
        if (i % 8 == 7)
            printf("\n");
    }
    printf("\n");

    uint16_t x = ((uint16_t)buf[3] << 8) | buf[4];
    uint16_t y = ((uint16_t)buf[5] << 8) | buf[6];

    if (x == 0xFFFF && y == 0xFFFF)
    {
        return ESP_ERR_NOT_FOUND;
    }

    data->finger_count = buf[0];
    data->points[0].x = x;
    data->points[0].y = y;
    data->points[0].strength = buf[4];

    return ESP_OK;
}

esp_err_t touch_controller_get_multiple_points(touch_panel_t *handle,
                                    touch_panel_data_t *data)
{
    memset(data, 0, sizeof(*data));

    const uint8_t total_size = TOUCH_POINT_ADDR_OFFSET +
                               TOUCH_PANEL_MAX_POINTS * SINGLE_TOUCH_POINT_SIZE;
    uint8_t buf[TOUCH_POINT_ADDR_OFFSET + TOUCH_PANEL_MAX_POINTS * SINGLE_TOUCH_POINT_SIZE] = {0};

    uint8_t cmd[] = {
        (uint8_t)(handle->touch_info_addr >> 24), (uint8_t)(handle->touch_info_addr >> 16),
        (uint8_t)(handle->touch_info_addr >> 8), (uint8_t)(handle->touch_info_addr),
        0x00, 0x04};
    esp_err_t ret = touch_i2c_read(handle->i2c_dev, cmd, sizeof(cmd), buf, total_size);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "get multiple points failed");
        return ret;
    }

    if (buf[0] == 0 || buf[0] > TOUCH_PANEL_MAX_POINTS)
    {
        return ESP_ERR_NOT_FOUND;
    }

    data->finger_count = buf[0];

    for (uint8_t i = 0; i < data->finger_count; i++)
    {
        uint8_t offset = TOUCH_POINT_ADDR_OFFSET + i * SINGLE_TOUCH_POINT_SIZE;
        data->points[i].x = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
        data->points[i].y = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
        data->points[i].strength = buf[offset + 4];
    }

    uint8_t last = data->finger_count - 1;
    if (data->points[last].x == 0xFFFF &&
        data->points[last].y == 0xFFFF &&
        data->points[last].strength == 0)
    {
        data->edge_touch = true;
    }

    return ESP_OK;
}

esp_err_t touch_controller_get_edge(touch_panel_t *handle, bool *edge_detected)
{
    *edge_detected = false;

    const uint8_t total_size = TOUCH_POINT_ADDR_OFFSET +
                               TOUCH_PANEL_MAX_POINTS * SINGLE_TOUCH_POINT_SIZE;
    uint8_t buf[TOUCH_POINT_ADDR_OFFSET + TOUCH_PANEL_MAX_POINTS * SINGLE_TOUCH_POINT_SIZE] = {0};

    uint8_t cmd[] = {
        (uint8_t)(handle->touch_info_addr >> 24), (uint8_t)(handle->touch_info_addr >> 16),
        (uint8_t)(handle->touch_info_addr >> 8), (uint8_t)(handle->touch_info_addr)};
    esp_err_t ret = touch_i2c_read(handle->i2c_dev, cmd, sizeof(cmd), buf, total_size);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "get edge touch failed");
        return ret;
    }

    if (buf[0] == 0)
    {
        return ESP_OK;
    }

    uint8_t offset = TOUCH_POINT_ADDR_OFFSET + (buf[0] - 1) * SINGLE_TOUCH_POINT_SIZE;
    uint16_t x = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
    uint16_t y = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];

    if (x == 0xFFFF && y == 0xFFFF && buf[offset + 4] == 0)
    {
        *edge_detected = true;
    }

    return ESP_OK;
}
