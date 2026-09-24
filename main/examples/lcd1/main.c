#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_H_RES 720
#define LCD_V_RES 720

#define BOARD_I2C_PORT 0
#define BOARD_I2C_SDA_GPIO 3
#define BOARD_I2C_SCL_GPIO 2
#define XL9555_I2C_ADDRESS 0x20
#define XL9555_LCD_RESET_PIN 3
#define LCD_BACKLIGHT_GPIO 34

#define MIPI_PHY_LDO_CHANNEL 3
#define MIPI_PHY_LDO_VOLTAGE_MV 2500
#define MIPI_DSI_LANE_COUNT 2
#define MIPI_DSI_LANE_BIT_RATE_MBPS 500
#define LCD_DPI_CLOCK_MHZ 36

#define CALIBRATION_CIRCLE_RED 0x00
#define CALIBRATION_CIRCLE_GREEN 0x00
#define CALIBRATION_CIRCLE_BLUE 0x00
#define CALIBRATION_BACKGROUND_RED 0xFF
#define CALIBRATION_BACKGROUND_GREEN 0xFF
#define CALIBRATION_BACKGROUND_BLUE 0xFF

// Set to 1 for RGB565, or 0 for RGB888.
#define LCD_USE_RGB565 0

#if LCD_USE_RGB565
#define LCD_PIXEL_FORMAT LCD_COLOR_PIXEL_FORMAT_RGB565
#define LCD_BITS_PER_PIXEL 16
#define LCD_PIXEL_FORMAT_NAME "RGB565 (experimental)"
#else
#define LCD_PIXEL_FORMAT LCD_COLOR_PIXEL_FORMAT_RGB888
#define LCD_BITS_PER_PIXEL 24
#define LCD_PIXEL_FORMAT_NAME "RGB888"
#endif

typedef struct {
    uint8_t command;
    uint8_t value;
} lcd_register_t;

static const char *TAG = "lcd1";

static const lcd_register_t s_init_cmd[] = {
    {0xE0, 0x00}, {0xE1, 0x93}, {0xE2, 0x65}, {0xE3, 0xF8}, {0x80, 0x01},
    {0xE0, 0x01}, {0x00, 0x00}, {0x01, 0x41}, {0x03, 0x10}, {0x04, 0x44},
    {0x17, 0x00}, {0x18, 0xE7}, {0x19, 0x00}, {0x1A, 0x00}, {0x1B, 0xE7},
    {0x1C, 0x00}, {0x24, 0xFE}, {0x35, 0x26}, {0x37, 0x09}, {0x38, 0x04},
    {0x39, 0x08}, {0x3A, 0x0A}, {0x3C, 0x78}, {0x3D, 0xFF}, {0x3E, 0xFF},
    {0x3F, 0xFF}, {0x40, 0x04}, {0x41, 0x5A}, {0x42, 0xC7}, {0x43, 0x18},
    {0x44, 0x0B}, {0x45, 0x14}, {0x55, 0x02}, {0x57, 0x49}, {0x59, 0x0A},
    {0x5A, 0x1A}, {0x5B, 0x19}, {0x5D, 0x7F}, {0x5E, 0x51}, {0x5F, 0x3F},
    {0x60, 0x32}, {0x61, 0x2D}, {0x62, 0x21}, {0x63, 0x26}, {0x64, 0x12},
    {0x65, 0x2E}, {0x66, 0x2F}, {0x67, 0x32}, {0x68, 0x52}, {0x69, 0x44},
    {0x6A, 0x4D}, {0x6B, 0x41}, {0x6C, 0x3F}, {0x6D, 0x32}, {0x6E, 0x21},
    {0x6F, 0x00}, {0x70, 0x7F}, {0x71, 0x51}, {0x72, 0x3F}, {0x73, 0x32},
    {0x74, 0x2D}, {0x75, 0x21}, {0x76, 0x26}, {0x77, 0x12}, {0x78, 0x2E},
    {0x79, 0x2F}, {0x7A, 0x32}, {0x7B, 0x52}, {0x7C, 0x44}, {0x7D, 0x4D},
    {0x7E, 0x41}, {0x7F, 0x3F}, {0x80, 0x32}, {0x81, 0x21}, {0x82, 0x00},
    {0xE0, 0x02}, {0x00, 0x5F}, {0x01, 0x5F}, {0x02, 0x5E}, {0x03, 0x5E},
    {0x04, 0x50}, {0x05, 0x48}, {0x06, 0x48}, {0x07, 0x4A}, {0x08, 0x4A},
    {0x09, 0x44}, {0x0A, 0x44}, {0x0B, 0x46}, {0x0C, 0x46}, {0x0D, 0x5F},
    {0x0E, 0x5F}, {0x0F, 0x57}, {0x10, 0x57}, {0x11, 0x77}, {0x12, 0x77},
    {0x13, 0x40}, {0x14, 0x42}, {0x15, 0x5F}, {0x16, 0x5F}, {0x17, 0x5F},
    {0x18, 0x5E}, {0x19, 0x5E}, {0x1A, 0x50}, {0x1B, 0x49}, {0x1C, 0x49},
    {0x1D, 0x4B}, {0x1E, 0x4B}, {0x1F, 0x45}, {0x20, 0x45}, {0x21, 0x47},
    {0x22, 0x47}, {0x23, 0x5F}, {0x24, 0x5F}, {0x25, 0x57}, {0x26, 0x57},
    {0x27, 0x77}, {0x28, 0x77}, {0x29, 0x41}, {0x2A, 0x43}, {0x2B, 0x5F},
    {0x2C, 0x1E}, {0x2D, 0x1E}, {0x2E, 0x1F}, {0x2F, 0x1F}, {0x30, 0x10},
    {0x31, 0x07}, {0x32, 0x07}, {0x33, 0x05}, {0x34, 0x05}, {0x35, 0x0B},
    {0x36, 0x0B}, {0x37, 0x09}, {0x38, 0x09}, {0x39, 0x1F}, {0x3A, 0x1F},
    {0x3B, 0x17}, {0x3C, 0x17}, {0x3D, 0x17}, {0x3E, 0x17}, {0x3F, 0x03},
    {0x40, 0x01}, {0x41, 0x1F}, {0x42, 0x1E}, {0x43, 0x1E}, {0x44, 0x1F},
    {0x45, 0x1F}, {0x46, 0x10}, {0x47, 0x06}, {0x48, 0x06}, {0x49, 0x04},
    {0x4A, 0x04}, {0x4B, 0x0A}, {0x4C, 0x0A}, {0x4D, 0x08}, {0x4E, 0x08},
    {0x4F, 0x1F}, {0x50, 0x1F}, {0x51, 0x17}, {0x52, 0x17}, {0x53, 0x17},
    {0x54, 0x17}, {0x55, 0x02}, {0x56, 0x00}, {0x57, 0x1F}, {0xE0, 0x02},
    {0x58, 0x40}, {0x59, 0x00}, {0x5A, 0x00}, {0x5B, 0x30}, {0x5C, 0x01},
    {0x5D, 0x30}, {0x5E, 0x01}, {0x5F, 0x02}, {0x60, 0x30}, {0x61, 0x03},
    {0x62, 0x04}, {0x63, 0x04}, {0x64, 0x86}, {0x65, 0x42}, {0x66, 0xE0},
    {0x67, 0x73}, {0x68, 0x05}, {0x69, 0x04}, {0x6A, 0x7F}, {0x6B, 0x08},
    {0x6C, 0x00}, {0x6D, 0x04}, {0x6E, 0x04}, {0x6F, 0x88}, {0x75, 0xD9},
    {0x76, 0x00}, {0x77, 0x33}, {0x78, 0x43},
};

static esp_err_t send_registers(esp_lcd_panel_io_handle_t io,
                                const lcd_register_t *registers,
                                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io,
                                                       registers[i].command,
                                                       &registers[i].value,
                                                       1),
                            TAG, "send JD9365 register 0x%02X failed",
                            registers[i].command);
    }
    return ESP_OK;
}

static esp_err_t jd9365_send_init_sequence(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_ERROR(send_registers(io, s_init_cmd,
                                       sizeof(s_init_cmd) / sizeof(s_init_cmd[0])),
                        TAG, "send JD9365 unlock sequence failed");

    const uint8_t user_page = 0x00;
    const uint8_t tear_line = 0x00;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE0, &user_page, 1),
                        TAG, "select JD9365 user page failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0),
                        TAG, "exit JD9365 sleep mode failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0),
                        TAG, "turn JD9365 display on failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    return esp_lcd_panel_io_tx_param(io, 0x35, &tear_line, 1);
}

static esp_err_t board_i2c_init(i2c_master_bus_handle_t *bus)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, bus);
}

static esp_err_t lcd_hardware_reset(esp_io_expander_handle_t expander)
{
    const uint32_t reset_mask = 1UL << XL9555_LCD_RESET_PIN;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, reset_mask,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "configure LCD reset output failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, reset_mask, 0),
                        TAG, "assert LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, reset_mask, 1),
                        TAG, "release LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t lcd_backlight_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure backlight failed");
    return gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
}

static esp_err_t lcd_panel_init(esp_lcd_panel_handle_t *panel,
                                esp_ldo_channel_handle_t *mipi_phy_ldo)
{
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = MIPI_PHY_LDO_CHANNEL,
        .voltage_mv = MIPI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, mipi_phy_ldo),
                        TAG, "acquire MIPI PHY LDO failed");

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = MIPI_DSI_LANE_COUNT,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = MIPI_DSI_LANE_BIT_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus),
                        TAG, "create MIPI DSI bus failed");

    esp_lcd_panel_io_handle_t dbi_io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io),
                        TAG, "create MIPI DBI IO failed");

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_PIXEL_FORMAT,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = 2,
            .hsync_pulse_width = 20,
            .hsync_front_porch = 68,
            .vsync_back_porch = 5,
            .vsync_pulse_width = 16,
            .vsync_front_porch = 50,
        },
        .flags.use_dma2d = true,
    };

    static const jd9365_lcd_init_cmd_t no_driver_init_commands[1] = {0};
    jd9365_vendor_config_t vendor_config = {
        .init_cmds = no_driver_init_commands,
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = MIPI_DSI_LANE_COUNT,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(dbi_io, &panel_config, panel),
                        TAG, "create JD9365 panel failed");
    ESP_RETURN_ON_ERROR(jd9365_send_init_sequence(dbi_io),
                        TAG, "initialize JD9365 registers failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "initialize DPI panel failed");
    return esp_lcd_panel_disp_on_off(*panel, true);
}

static esp_err_t show_calibration_circle(esp_lcd_panel_handle_t panel,
                                         void *frame_buffer)
{
    /* Doubled coordinates keep the 720-pixel circle centered at (359.5, 359.5). */
    const int32_t diameter = LCD_H_RES;
    const int32_t radius_2x = diameter;
    const int32_t radius_squared_4x = radius_2x * radius_2x;

#if LCD_USE_RGB565
    const uint16_t circle_pixel =
        ((uint16_t)(CALIBRATION_CIRCLE_RED >> 3) << 11) |
        ((uint16_t)(CALIBRATION_CIRCLE_GREEN >> 2) << 5) |
        (uint16_t)(CALIBRATION_CIRCLE_BLUE >> 3);
    const uint16_t background_pixel =
        ((uint16_t)(CALIBRATION_BACKGROUND_RED >> 3) << 11) |
        ((uint16_t)(CALIBRATION_BACKGROUND_GREEN >> 2) << 5) |
        (uint16_t)(CALIBRATION_BACKGROUND_BLUE >> 3);
    uint16_t *pixel = (uint16_t *)frame_buffer;

    for (int32_t y = 0; y < LCD_V_RES; y++) {
        const int32_t dy_2x = 2 * y - (LCD_V_RES - 1);
        for (int32_t x = 0; x < LCD_H_RES; x++) {
            const int32_t dx_2x = 2 * x - (LCD_H_RES - 1);
            const bool inside = dx_2x * dx_2x + dy_2x * dy_2x <= radius_squared_4x;
            *pixel++ = inside ? circle_pixel : background_pixel;
        }
    }
#else
    uint8_t *pixel = frame_buffer;

    for (int32_t y = 0; y < LCD_V_RES; y++) {
        const int32_t dy_2x = 2 * y - (LCD_V_RES - 1);
        for (int32_t x = 0; x < LCD_H_RES; x++) {
            const int32_t dx_2x = 2 * x - (LCD_H_RES - 1);
            const bool inside = dx_2x * dx_2x + dy_2x * dy_2x <= radius_squared_4x;
            *pixel++ = inside ? CALIBRATION_CIRCLE_RED : CALIBRATION_BACKGROUND_RED;
            *pixel++ = inside ? CALIBRATION_CIRCLE_GREEN : CALIBRATION_BACKGROUND_GREEN;
            *pixel++ = inside ? CALIBRATION_CIRCLE_BLUE : CALIBRATION_BACKGROUND_BLUE;
        }
    }
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0,
                                                   LCD_H_RES, LCD_V_RES,
                                                   frame_buffer),
                        TAG, "draw calibration circle failed");
    ESP_LOGI(TAG, "Calibration circle displayed: %dx%d, RGB(%u, %u, %u)",
             LCD_H_RES, LCD_V_RES,
             CALIBRATION_CIRCLE_RED,
             CALIBRATION_CIRCLE_GREEN,
             CALIBRATION_CIRCLE_BLUE);
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(lcd_backlight_init());

    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(board_i2c_init(&i2c_bus));

    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus,
                                                    XL9555_I2C_ADDRESS,
                                                    &expander));
    ESP_ERROR_CHECK(lcd_hardware_reset(expander));

    esp_ldo_channel_handle_t mipi_phy_ldo = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_LOGI(TAG, "LCD pixel format: %s, %d bpp,",
             LCD_PIXEL_FORMAT_NAME, LCD_BITS_PER_PIXEL);
    ESP_ERROR_CHECK(lcd_panel_init(&panel, &mipi_phy_ldo));

    void *frame_buffer = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &frame_buffer));
    ESP_ERROR_CHECK(show_calibration_circle(panel, frame_buffer));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT_GPIO, 1));

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
