#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_controller.h"

static const char *TAG = "touch_ili9882";

#define ILITEK_I2C_ADDRESS                  0x41
#define ILITEK_I2C_CLOCK_HZ                 400000
#define ILITEK_I2C_TIMEOUT_MS               100
#define ILITEK_COMMAND_DELAY_MS             5
#define ILITEK_COMMAND_RETRIES              3
#define ILITEK_RESET_BOOT_DELAY_MS          65

#define ILITEK_SCREEN_WIDTH                 720
#define ILITEK_SCREEN_HEIGHT                1440
#define ILITEK_LOW_RESOLUTION_MAX           2048
#define ILITEK_HW_MAX_POINTS                10

#define ILITEK_CMD_READ_DATA_CTRL           0xF6
#define ILITEK_CMD_GET_TP_INFO              0x20
#define ILITEK_CMD_GET_FW_VERSION           0x21
#define ILITEK_CMD_GET_PROTOCOL_VERSION     0x22
#define ILITEK_CMD_GET_CORE_VERSION         0x23
#define ILITEK_CMD_GET_CORE_VERSION_NEW     0x24
#define ILITEK_CMD_GET_PANEL_INFO           0x29
#define ILITEK_CMD_GET_REPORT_FORMAT        0x37

#define ILITEK_PACKET_DEMO                  0x5A
#define ILITEK_PACKET_DEMO_HIGH_RESOLUTION  0x5B
#define ILITEK_DEMO_PACKET_LEN              43
#define ILITEK_DEMO_HIGH_RES_PACKET_LEN     72
#define ILITEK_DEMO_HIGH_RES_INFO_LEN       3

#define ILITEK_REPORT_LOW_RESOLUTION        0
#define ILITEK_REPORT_HIGH_RESOLUTION       1
#define ILITEK_CUSTOMER_TYPE_OFF            0x1F
#define ILITEK_CUSTOMER_TYPE_OFF_3BIT       0x07
#define ILITEK_PEN_TYPE_OFF                 0x03

#define ILITEK_CORE_VERSION_1430            0x01040300U
#define ILITEK_CORE_VERSION_1470            0x01040700U
#define ILITEK_CORE_VERSION_1700            0x01070000U

#define ILITEK_ICE_OPEN                     0x25
#define ILITEK_ICE_CLOSE                    0x1B
#define ILITEK_ICE_PID_ADDRESS              0x04009CU
#define ILITEK_ICE_FLASH_CS_ADDRESS         0x041000U
#define ILITEK_ICE_FLASH_DUAL_MODE_ADDRESS  0x041003U
#define ILITEK_EXPECTED_CHIP_ID             0x9882U

typedef struct {
    uint8_t chip_id[2];
    uint8_t fw_version[8];
    uint8_t protocol_version[3];
    uint32_t core_version;
    uint16_t x_resolution;
    uint16_t y_resolution;
    uint16_t raw_x_max;
    uint16_t raw_y_max;
    uint8_t report_resolution;
    uint8_t packet_length;
    bool coordinates_are_pixels;
    bool chip_id_valid;
    bool irq_registered;
    volatile bool report_pending;
} ilitek_touch_state_t;

static ilitek_touch_state_t *get_state(const touch_panel_t *handle)
{
    return handle ? (ilitek_touch_state_t *)handle->driver_handle : NULL;
}

static esp_err_t ilitek_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data,
                                  size_t length)
{
    return i2c_master_transmit(dev, data, length, ILITEK_I2C_TIMEOUT_MS);
}

static esp_err_t ilitek_i2c_read(i2c_master_dev_handle_t dev, uint8_t *data, size_t length)
{
    return i2c_master_receive(dev, data, length, ILITEK_I2C_TIMEOUT_MS);
}

static esp_err_t ilitek_command_read(i2c_master_dev_handle_t dev, uint8_t command,
                                     bool send_preamble, uint8_t *response,
                                     size_t response_length)
{
    const uint8_t preamble[] = {ILITEK_CMD_READ_DATA_CTRL, command};
    esp_err_t last_error = ESP_FAIL;

    for (int retry = 0; retry < ILITEK_COMMAND_RETRIES; ++retry) {
        if (send_preamble) {
            last_error = ilitek_i2c_write(dev, preamble, sizeof(preamble));
            if (last_error != ESP_OK) {
                continue;
            }
        }

        last_error = ilitek_i2c_write(dev, &command, 1);
        if (last_error != ESP_OK) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(ILITEK_COMMAND_DELAY_MS));
        memset(response, 0, response_length);
        last_error = ilitek_i2c_read(dev, response, response_length);
        if (last_error == ESP_OK && response[0] == command) {
            return ESP_OK;
        }
        if (last_error == ESP_OK) {
            last_error = ESP_ERR_INVALID_RESPONSE;
        }
    }

    return last_error;
}

static esp_err_t ilitek_hardware_reset(void)
{
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << TOUCH_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "configure reset GPIO failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST_PIN, 1), TAG, "set reset high failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST_PIN, 0), TAG, "set reset low failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST_PIN, 1), TAG, "release reset failed");
    vTaskDelay(pdMS_TO_TICKS(ILITEK_RESET_BOOT_DELAY_MS));
    return ESP_OK;
}

static esp_err_t ilitek_read_chip_id(touch_panel_t *handle, ilitek_touch_state_t *state)
{
    const uint8_t ice_open[] = {ILITEK_ICE_OPEN, 0x62, 0x10, 0x18};
    const uint8_t ice_close[] = {ILITEK_ICE_CLOSE, 0x62, 0x10, 0x18};
    const uint8_t pid_address[] = {
        ILITEK_ICE_OPEN,
        (uint8_t)(ILITEK_ICE_PID_ADDRESS),
        (uint8_t)(ILITEK_ICE_PID_ADDRESS >> 8),
        (uint8_t)(ILITEK_ICE_PID_ADDRESS >> 16),
    };
    const uint8_t flash_cs_high[] = {
        ILITEK_ICE_OPEN,
        (uint8_t)(ILITEK_ICE_FLASH_CS_ADDRESS),
        (uint8_t)(ILITEK_ICE_FLASH_CS_ADDRESS >> 8),
        (uint8_t)(ILITEK_ICE_FLASH_CS_ADDRESS >> 16),
        0x01,
    };
    const uint8_t flash_dual_mode_off[] = {
        ILITEK_ICE_OPEN,
        (uint8_t)(ILITEK_ICE_FLASH_DUAL_MODE_ADDRESS),
        (uint8_t)(ILITEK_ICE_FLASH_DUAL_MODE_ADDRESS >> 8),
        (uint8_t)(ILITEK_ICE_FLASH_DUAL_MODE_ADDRESS >> 16),
        0x00,
    };
    uint8_t pid_data[4] = {0};

    ESP_RETURN_ON_ERROR(ilitek_i2c_write(handle->i2c_dev, ice_open, sizeof(ice_open)),
                        TAG, "enter ICE mode failed");
    vTaskDelay(pdMS_TO_TICKS(1));

    esp_err_t ret = ilitek_i2c_write(handle->i2c_dev, flash_cs_high,
                                     sizeof(flash_cs_high));
    if (ret == ESP_OK) {
        ret = ilitek_i2c_write(handle->i2c_dev, flash_dual_mode_off,
                               sizeof(flash_dual_mode_off));
    }
    if (ret == ESP_OK) {
        ret = ilitek_i2c_write(handle->i2c_dev, pid_address, sizeof(pid_address));
    }
    if (ret == ESP_OK) {
        ret = ilitek_i2c_read(handle->i2c_dev, pid_data, sizeof(pid_data));
    }

    esp_err_t close_ret = ilitek_i2c_write(handle->i2c_dev, ice_close, sizeof(ice_close));
    vTaskDelay(pdMS_TO_TICKS(ILITEK_RESET_BOOT_DELAY_MS));
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(close_ret, TAG, "leave ICE mode failed");

    const uint32_t pid = (uint32_t)pid_data[0] |
                         ((uint32_t)pid_data[1] << 8) |
                         ((uint32_t)pid_data[2] << 16) |
                         ((uint32_t)pid_data[3] << 24);
    const uint16_t chip_id = (uint16_t)(pid >> 16);
    state->chip_id[0] = (uint8_t)(chip_id >> 8);
    state->chip_id[1] = (uint8_t)chip_id;
    state->chip_id_valid = true;

    ESP_LOGI(TAG, "PID: 0x%08" PRIX32 ", chip ID: 0x%04X", pid, chip_id);
    return chip_id == ILITEK_EXPECTED_CHIP_ID ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ilitek_read_protocol_version(touch_panel_t *handle,
                                              ilitek_touch_state_t *state)
{
    uint8_t response[4];
    ESP_RETURN_ON_ERROR(
        ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_PROTOCOL_VERSION, true,
                            response, sizeof(response)),
        TAG, "read protocol version failed");

    memcpy(state->protocol_version, &response[1], sizeof(state->protocol_version));
    ESP_LOGI(TAG, "Protocol version: %u.%u.%u", response[1], response[2], response[3]);
    return ESP_OK;
}

static esp_err_t ilitek_read_core_version(touch_panel_t *handle, ilitek_touch_state_t *state)
{
    uint8_t response[5];
    esp_err_t ret = ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_CORE_VERSION_NEW,
                                        true, response, sizeof(response));
    if (ret != ESP_OK) {
        ret = ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_CORE_VERSION,
                                  true, response, 4);
        if (ret != ESP_OK) {
            return ret;
        }
        response[4] = 0;
    }

    state->core_version = ((uint32_t)response[1] << 24) |
                          ((uint32_t)response[2] << 16) |
                          ((uint32_t)response[3] << 8) |
                          response[4];
    ESP_LOGI(TAG, "Core version: %u.%u.%u.%u",
             response[1], response[2], response[3], response[4]);
    return ESP_OK;
}

static esp_err_t ilitek_read_fw_version(touch_panel_t *handle, ilitek_touch_state_t *state)
{
    uint8_t response[9];
    ESP_RETURN_ON_ERROR(
        ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_FW_VERSION, true,
                            response, sizeof(response)),
        TAG, "read firmware version failed");

    memcpy(state->fw_version, &response[1], sizeof(state->fw_version));
    ESP_LOGI(TAG, "FW AP: %u.%u.%u.%u, MP: %u.%u.%u.%u",
             response[1], response[2], response[3], response[4],
             response[5], response[6], response[7], response[8]);
    return ESP_OK;
}

static esp_err_t ilitek_read_tp_info(touch_panel_t *handle, ilitek_touch_state_t *state)
{
    uint8_t response[14];
    ESP_RETURN_ON_ERROR(
        ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_TP_INFO, true,
                            response, sizeof(response)),
        TAG, "read touch information failed");

    state->raw_x_max = (uint16_t)response[3] | ((uint16_t)response[4] << 8);
    state->raw_y_max = (uint16_t)response[5] | ((uint16_t)response[6] << 8);
    ESP_LOGI(TAG, "Touch range: %ux%u, channels: %ux%u",
             state->raw_x_max, state->raw_y_max, response[7], response[8]);
    return ESP_OK;
}

static esp_err_t ilitek_read_panel_info(touch_panel_t *handle, ilitek_touch_state_t *state)
{
    uint8_t response[6];
    const size_t response_length = state->core_version >= ILITEK_CORE_VERSION_1430 ? 6 : 5;
    ESP_RETURN_ON_ERROR(
        ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_PANEL_INFO, false,
                            response, response_length),
        TAG, "read panel information failed");

    const uint16_t x_resolution = ((uint16_t)response[1] << 8) | response[2];
    const uint16_t y_resolution = ((uint16_t)response[3] << 8) | response[4];
    if (x_resolution != 0 && y_resolution != 0) {
        state->x_resolution = x_resolution;
        state->y_resolution = y_resolution;
    }
    state->coordinates_are_pixels = response_length == 6 && response[5] != 0;
    ESP_LOGI(TAG, "Panel resolution: %ux%u, pixel coordinates: %s",
             state->x_resolution, state->y_resolution,
             state->coordinates_are_pixels ? "yes" : "no");
    return ESP_OK;
}

static esp_err_t ilitek_read_report_format(touch_panel_t *handle,
                                           ilitek_touch_state_t *state)
{
    if (state->core_version < ILITEK_CORE_VERSION_1470) {
        return ESP_OK;
    }

    uint8_t response[2];
    ESP_RETURN_ON_ERROR(
        ilitek_command_read(handle->i2c_dev, ILITEK_CMD_GET_REPORT_FORMAT, false,
                            response, sizeof(response)),
        TAG, "read report format failed");

    state->report_resolution = response[1] & 0x07;
    const uint8_t customer_type = state->core_version >= ILITEK_CORE_VERSION_1700
                                      ? (response[1] >> 3) & 0x07
                                      : response[1] >> 3;
    const uint8_t expected_customer_type = state->core_version >= ILITEK_CORE_VERSION_1700
                                               ? ILITEK_CUSTOMER_TYPE_OFF_3BIT
                                               : ILITEK_CUSTOMER_TYPE_OFF;
    const uint8_t pen_type = state->core_version >= ILITEK_CORE_VERSION_1700
                                 ? response[1] >> 6
                                 : 0;

    ESP_RETURN_ON_FALSE(state->report_resolution <= ILITEK_REPORT_HIGH_RESOLUTION,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported report resolution %u",
                        state->report_resolution);
    const uint8_t expected_pen_type = state->core_version >= ILITEK_CORE_VERSION_1700
                                          ? ILITEK_PEN_TYPE_OFF
                                          : 0;
    ESP_RETURN_ON_FALSE(customer_type == expected_customer_type &&
                            pen_type == expected_pen_type,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "customer/pen report format is not supported (customer=%u pen=%u)",
                        customer_type, pen_type);

    state->packet_length = state->report_resolution == ILITEK_REPORT_HIGH_RESOLUTION
                               ? ILITEK_DEMO_HIGH_RES_PACKET_LEN
                               : ILITEK_DEMO_PACKET_LEN;
    ESP_LOGI(TAG, "Report format: %s resolution, %u-byte packet",
             state->report_resolution == ILITEK_REPORT_HIGH_RESOLUTION ? "high" : "low",
             state->packet_length);
    return ESP_OK;
}

static void IRAM_ATTR ilitek_touch_isr(void *arg)
{
    ilitek_touch_state_t *state = (ilitek_touch_state_t *)arg;
    state->report_pending = true;
}

static esp_err_t ilitek_configure_interrupt(ilitek_touch_state_t *state)
{
    const gpio_config_t interrupt_config = {
        .pin_bit_mask = 1ULL << TOUCH_INT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_config), TAG, "configure interrupt GPIO failed");

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(TOUCH_INT_PIN, ilitek_touch_isr, state),
                        TAG, "install touch ISR failed");
    state->irq_registered = true;
    state->report_pending = false;
    return ESP_OK;
}

static uint8_t ilitek_packet_checksum(const uint8_t *packet, size_t length)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum -= packet[i];
    }
    return checksum;
}

static uint16_t ilitek_scale_coordinate(uint16_t value, uint16_t input_max,
                                        uint16_t output_resolution, bool already_pixels)
{
    if (output_resolution == 0) {
        return 0;
    }

    uint32_t scaled = value;
    if (!already_pixels && input_max != 0) {
        scaled = (uint32_t)(((uint64_t)value * output_resolution) / input_max);
    }
    if (scaled >= output_resolution) {
        scaled = output_resolution - 1;
    }
    return (uint16_t)scaled;
}

static esp_err_t ilitek_parse_demo_packet(const ilitek_touch_state_t *state,
                                          const uint8_t *packet, size_t length,
                                          touch_panel_data_t *data)
{
    ESP_RETURN_ON_FALSE(length == state->packet_length, ESP_ERR_INVALID_SIZE, TAG,
                        "unexpected report length");
    ESP_RETURN_ON_FALSE(ilitek_packet_checksum(packet, length - 1) == packet[length - 1],
                        ESP_ERR_INVALID_CRC, TAG, "touch report checksum mismatch");

    const bool high_resolution = state->report_resolution == ILITEK_REPORT_HIGH_RESOLUTION;
    const uint8_t expected_packet_id = high_resolution
                                           ? ILITEK_PACKET_DEMO_HIGH_RESOLUTION
                                           : ILITEK_PACKET_DEMO;
    ESP_RETURN_ON_FALSE(packet[0] == expected_packet_id, ESP_ERR_INVALID_RESPONSE, TAG,
                        "unexpected report ID 0x%02X", packet[0]);

    const size_t first_point_offset = high_resolution
                                          ? 1 + ILITEK_DEMO_HIGH_RES_INFO_LEN
                                          : 1;
    const size_t point_size = high_resolution ? 5 : 4;
    const uint16_t raw_x_max = high_resolution ? state->raw_x_max
                                                : ILITEK_LOW_RESOLUTION_MAX;
    const uint16_t raw_y_max = high_resolution ? state->raw_y_max
                                                : ILITEK_LOW_RESOLUTION_MAX;

    for (uint8_t slot = 0; slot < ILITEK_HW_MAX_POINTS; ++slot) {
        const size_t offset = first_point_offset + slot * point_size;
        uint16_t raw_x;
        uint16_t raw_y;
        uint8_t pressure;

        if (high_resolution) {
            if (packet[offset] == 0xFF && packet[offset + 1] == 0xFF &&
                packet[offset + 2] == 0xFF && packet[offset + 3] == 0xFF) {
                continue;
            }
            raw_x = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
            raw_y = ((uint16_t)packet[offset + 2] << 8) | packet[offset + 3];
            pressure = packet[offset + 4];
        } else {
            if (packet[offset] == 0xFF && packet[offset + 1] == 0xFF &&
                packet[offset + 2] == 0xFF) {
                continue;
            }
            raw_x = ((uint16_t)(packet[offset] & 0xF0) << 4) | packet[offset + 1];
            raw_y = ((uint16_t)(packet[offset] & 0x0F) << 8) | packet[offset + 2];
            pressure = packet[offset + 3];
        }

        if (data->finger_count >= TOUCH_PANEL_MAX_POINTS) {
            continue;
        }
        touch_panel_point_t *point = &data->points[data->finger_count++];
        point->x = ilitek_scale_coordinate(raw_x, raw_x_max, state->x_resolution,
                                           state->coordinates_are_pixels);
        point->y = ilitek_scale_coordinate(raw_y, raw_y_max, state->y_resolution,
                                           state->coordinates_are_pixels);
        point->strength = pressure != 0 ? pressure : 1;
    }

    return data->finger_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t touch_controller_init(touch_panel_t *handle, i2c_master_bus_handle_t i2c_bus,
                                esp_io_expander_handle_t expander)
{
    (void)expander;
    ESP_RETURN_ON_FALSE(handle && i2c_bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    memset(handle, 0, sizeof(*handle));

    ilitek_touch_state_t *state = calloc(1, sizeof(*state));
    ESP_RETURN_ON_FALSE(state, ESP_ERR_NO_MEM, TAG, "allocate driver state failed");
    state->x_resolution = ILITEK_SCREEN_WIDTH;
    state->y_resolution = ILITEK_SCREEN_HEIGHT;
    state->raw_x_max = ILITEK_LOW_RESOLUTION_MAX;
    state->raw_y_max = ILITEK_LOW_RESOLUTION_MAX;
    state->report_resolution = ILITEK_REPORT_LOW_RESOLUTION;
    state->packet_length = ILITEK_DEMO_PACKET_LEN;
    handle->driver_handle = state;

    esp_err_t ret = ilitek_hardware_reset();
    if (ret != ESP_OK) {
        goto fail;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ILITEK_I2C_ADDRESS,
        .scl_speed_hz = ILITEK_I2C_CLOCK_HZ,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &device_config, &handle->i2c_dev);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = ilitek_read_chip_id(handle, state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to confirm chip ID: %s", esp_err_to_name(ret));
    }

    ret = ilitek_read_protocol_version(handle, state);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = ilitek_read_core_version(handle, state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read core version, using legacy report format");
        state->core_version = 0;
    }
    ret = ilitek_read_fw_version(handle, state);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = ilitek_read_tp_info(handle, state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read touch range, using default range");
    }
    ret = ilitek_read_panel_info(handle, state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read panel resolution, using %ux%u",
                 state->x_resolution, state->y_resolution);
    }
    ret = ilitek_read_report_format(handle, state);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        goto fail;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read report format, using 43-byte low-resolution reports");
        state->report_resolution = ILITEK_REPORT_LOW_RESOLUTION;
        state->packet_length = ILITEK_DEMO_PACKET_LEN;
    }
    ret = ilitek_configure_interrupt(state);
    if (ret != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "Ilitek touch initialized at I2C address 0x%02X", ILITEK_I2C_ADDRESS);
    return ESP_OK;

fail:
    if (handle->i2c_dev) {
        i2c_master_bus_rm_device(handle->i2c_dev);
    }
    free(state);
    memset(handle, 0, sizeof(*handle));
    return ret;
}

esp_err_t touch_controller_soft_reset(touch_panel_t *handle)
{
    ilitek_touch_state_t *state = get_state(handle);
    ESP_RETURN_ON_FALSE(state && handle->i2c_dev, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");

    if (state->irq_registered) {
        gpio_intr_disable(TOUCH_INT_PIN);
    }
    esp_err_t ret = ilitek_hardware_reset();
    state->report_pending = false;
    if (state->irq_registered) {
        gpio_intr_enable(TOUCH_INT_PIN);
    }
    return ret;
}

esp_err_t touch_controller_get_chip_id(touch_panel_t *handle, uint8_t *id)
{
    ilitek_touch_state_t *state = get_state(handle);
    ESP_RETURN_ON_FALSE(state && handle->i2c_dev, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(id, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    ESP_RETURN_ON_FALSE(state->chip_id_valid, ESP_ERR_NOT_FOUND, TAG,
                        "chip ID is unavailable");
    memcpy(id, state->chip_id, sizeof(state->chip_id));
    return ESP_OK;
}

esp_err_t touch_controller_get_fw_version(touch_panel_t *handle, uint8_t *version)
{
    ilitek_touch_state_t *state = get_state(handle);
    ESP_RETURN_ON_FALSE(state && handle->i2c_dev, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(version, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    memcpy(version, state->fw_version, sizeof(state->fw_version));
    return ESP_OK;
}

esp_err_t touch_controller_get_resolution(touch_panel_t *handle, uint16_t *x_res,
                                          uint16_t *y_res)
{
    ilitek_touch_state_t *state = get_state(handle);
    ESP_RETURN_ON_FALSE(state && handle->i2c_dev, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(x_res && y_res, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    *x_res = state->x_resolution;
    *y_res = state->y_resolution;
    return ESP_OK;
}

esp_err_t touch_controller_get_multiple_points(touch_panel_t *handle,
                                               touch_panel_data_t *data)
{
    ilitek_touch_state_t *state = get_state(handle);
    ESP_RETURN_ON_FALSE(state && handle->i2c_dev, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    memset(data, 0, sizeof(*data));

    if (state->irq_registered && !state->report_pending) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t packet[ILITEK_DEMO_HIGH_RES_PACKET_LEN] = {0};
    state->report_pending = false;
    ESP_RETURN_ON_ERROR(ilitek_i2c_read(handle->i2c_dev, packet, state->packet_length),
                        TAG, "read touch report failed");
    return ilitek_parse_demo_packet(state, packet, state->packet_length, data);
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
    ESP_RETURN_ON_FALSE(get_state(handle), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(edge_detected, ESP_ERR_INVALID_ARG, TAG, "invalid output");
    *edge_detected = false;
    return ESP_OK;
}
