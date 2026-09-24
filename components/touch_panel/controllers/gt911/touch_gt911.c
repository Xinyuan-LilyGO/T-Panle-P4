#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_controller.h"

static const char *TAG = "touch_gt911";

static esp_err_t gt911_hardware_reset(esp_io_expander_handle_t expander, int int_level)
{
    ESP_RETURN_ON_FALSE(expander, ESP_ERR_INVALID_ARG, TAG, "invalid IO expander");

    const uint32_t reset_mask = 1UL << XL9555_TP_RST;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, reset_mask, IO_EXPANDER_OUTPUT),
                        TAG, "configure reset failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, reset_mask, 0),
                        TAG, "assert reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    const gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_config), TAG, "configure INT failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_INT, int_level), TAG, "select address failed");
    vTaskDelay(pdMS_TO_TICKS(2));

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, reset_mask, 1),
                        TAG, "release reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT),
                        TAG, "configure INT input failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_lcd_touch_handle_t get_touch(const touch_panel_t *handle)
{
    return handle ? (esp_lcd_touch_handle_t)handle->driver_handle : NULL;
}

esp_err_t touch_controller_init(touch_panel_t *handle, i2c_master_bus_handle_t i2c_bus,
                                esp_io_expander_handle_t expander)
{
    ESP_RETURN_ON_FALSE(handle && i2c_bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    memset(handle, 0, sizeof(*handle));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = 720,
        .y_max = 720,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
    };

    static const struct {
        uint8_t address;
        uint8_t int_level;
    } address_options[] = {
        {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 0},
        {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 1},
    };

    esp_err_t ret = ESP_FAIL;
    for (size_t i = 0; i < sizeof(address_options) / sizeof(address_options[0]); ++i) {
        ESP_RETURN_ON_ERROR(gt911_hardware_reset(expander, address_options[i].int_level),
                            TAG, "hardware reset failed");

        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_config.dev_addr = address_options[i].address;
        io_config.scl_speed_hz = 100000;

        esp_lcd_panel_io_handle_t io = NULL;
        ret = esp_lcd_new_panel_io_i2c_v2(i2c_bus, &io_config, &io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Create I2C IO for address 0x%02x failed: %s",
                     address_options[i].address, esp_err_to_name(ret));
            continue;
        }

        esp_lcd_touch_handle_t touch = NULL;
        ret = esp_lcd_touch_new_i2c_gt911(io, &touch_config, &touch);
        if (ret == ESP_OK) {
            handle->driver_handle = touch;
            handle->io_handle = io;
            ESP_LOGI(TAG, "GT911 initialized at address 0x%02x", address_options[i].address);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "GT911 did not respond at address 0x%02x: %s",
                 address_options[i].address, esp_err_to_name(ret));
        esp_lcd_panel_io_del(io);
    }

    ESP_LOGE(TAG, "GT911 not found at address 0x5d or 0x14");
    return ret;
}

esp_err_t touch_controller_soft_reset(touch_panel_t *handle)
{
    return get_touch(handle) ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}

esp_err_t touch_controller_get_chip_id(touch_panel_t *handle, uint8_t *id)
{
    if (id) {
        id[0] = 0;
        id[1] = 0;
    }
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_touch(handle) ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}

esp_err_t touch_controller_get_fw_version(touch_panel_t *handle, uint8_t *version)
{
    if (version) {
        memset(version, 0, 8);
    }
    if (!version) {
        return ESP_ERR_INVALID_ARG;
    }
    return get_touch(handle) ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}

esp_err_t touch_controller_get_resolution(touch_panel_t *handle, uint16_t *x_res, uint16_t *y_res)
{
    ESP_RETURN_ON_FALSE(get_touch(handle), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(x_res && y_res, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    *x_res = 720;
    *y_res = 720;
    return ESP_OK;
}

esp_err_t touch_controller_get_multiple_points(touch_panel_t *handle, touch_panel_data_t *data)
{
    esp_lcd_touch_handle_t touch = get_touch(handle);
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    memset(data, 0, sizeof(*data));

    ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(touch), TAG, "read data failed");
    esp_lcd_touch_point_data_t points[TOUCH_PANEL_MAX_POINTS] = {0};
    uint8_t point_count = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(touch, points, &point_count,
                                               TOUCH_PANEL_MAX_POINTS),
                        TAG, "get data failed");
    if (point_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    data->finger_count = point_count;
    for (uint8_t i = 0; i < point_count; ++i) {
        data->points[i].x = points[i].x;
        data->points[i].y = points[i].y;
        data->points[i].strength = points[i].strength;
    }
    return ESP_OK;
}

esp_err_t touch_controller_get_single_point(touch_panel_t *handle, touch_panel_data_t *data,
                                            uint8_t finger_num)
{
    ESP_RETURN_ON_FALSE(finger_num > 0 && finger_num <= TOUCH_PANEL_MAX_POINTS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid finger number");
    esp_err_t ret = touch_controller_get_multiple_points(handle, data);
    if (ret != ESP_OK) {
        return ret;
    }
    return data->finger_count >= finger_num ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t touch_controller_get_edge(touch_panel_t *handle, bool *edge_detected)
{
    ESP_RETURN_ON_FALSE(get_touch(handle), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(edge_detected, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    *edge_detected = false;
    return ESP_OK;
}
