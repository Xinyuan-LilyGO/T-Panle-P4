/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_ili9882.h"

#define ILI9882_CMD_PAGE       0xFF
#define ILI9882_PAGE_USER      0x00
#define ILI9882_CMD_SHLR_BIT   (1 << 0)
#define ILI9882_CMD_UPDN_BIT   (1 << 1)

#define ESP_LCD_ILI9882_VER_MAJOR 1
#define ESP_LCD_ILI9882_VER_MINOR 1
#define ESP_LCD_ILI9882_VER_PATCH 0

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const ili9882_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    uint8_t lane_num;
    struct {
        unsigned int reset_level : 1;
    } flags;
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} ili9882_panel_t;

static const char *TAG = "ili9882";

static esp_err_t panel_ili9882_del(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9882_init(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9882_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9882_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_ili9882_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_ili9882_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_ili9882(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_ILI9882_VER_MAJOR,
             ESP_LCD_ILI9882_VER_MINOR, ESP_LCD_ILI9882_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG,
                        TAG, "invalid arguments");

    ili9882_vendor_config_t *vendor_config =
        (ili9882_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config &&
                            vendor_config->mipi_config.dsi_bus,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vendor config");
    ESP_RETURN_ON_FALSE(vendor_config->init_cmds && vendor_config->init_cmds_size,
                        ESP_ERR_INVALID_ARG, TAG, "panel init commands are required");
    ESP_RETURN_ON_FALSE(vendor_config->mipi_config.lane_num == 2,
                        ESP_ERR_NOT_SUPPORTED, TAG, "only 2-lane DSI is supported");

    esp_err_t ret = ESP_OK;
    ili9882_panel_t *ili9882 = calloc(1, sizeof(ili9882_panel_t));
    ESP_RETURN_ON_FALSE(ili9882, ESP_ERR_NO_MEM, TAG, "no mem for ili9882 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG,
                          "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        ili9882->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        ili9882->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        ili9882->colmod_val = 0x55;
        break;
    case 18:
        ili9882->colmod_val = 0x66;
        break;
    case 24:
        ili9882->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported pixel width");
        break;
    }

    ili9882->io = io;
    ili9882->init_cmds = vendor_config->init_cmds;
    ili9882->init_cmds_size = vendor_config->init_cmds_size;
    ili9882->lane_num = vendor_config->mipi_config.lane_num;
    ili9882->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ili9882->flags.reset_level = panel_dev_config->flags.reset_active_high;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus,
                              vendor_config->mipi_config.dpi_config, &panel_handle),
        err, TAG, "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    ili9882->del = panel_handle->del;
    ili9882->init = panel_handle->init;
    panel_handle->del = panel_ili9882_del;
    panel_handle->init = panel_ili9882_init;
    panel_handle->reset = panel_ili9882_reset;
    panel_handle->mirror = panel_ili9882_mirror;
    panel_handle->invert_color = panel_ili9882_invert_color;
    panel_handle->disp_on_off = panel_ili9882_disp_on_off;
    panel_handle->user_data = ili9882;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new ili9882 panel @%p", ili9882);
    return ESP_OK;

err:
    if (ili9882) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(ili9882);
    }
    return ret;
}

static esp_err_t panel_ili9882_del(esp_lcd_panel_t *panel)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;

    ESP_RETURN_ON_ERROR(ili9882->del(panel), TAG, "del ili9882 panel failed");
    if (ili9882->reset_gpio_num >= 0) {
        gpio_reset_pin(ili9882->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del ili9882 panel @%p", ili9882);
    free(ili9882);
    return ESP_OK;
}

static esp_err_t panel_ili9882_init(esp_lcd_panel_t *panel)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ili9882->io;
    bool is_user_page = true;
    bool is_cmd_overwritten = false;

    uint8_t id[3] = {0};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(io, 0x04, id, sizeof(id)),
                        TAG, "read ID failed");
    ESP_LOGI(TAG, "LCD ID: %02X %02X %02X", id[0], id[1], id[2]);

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(            io, ILI9882_CMD_PAGE,            (uint8_t[]){0x98, 0x82, ILI9882_PAGE_USER}, 3),
        TAG, "select user page failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL,
                                  (uint8_t[]){ili9882->madctl_val}, 1),
        TAG, "set MADCTL failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD,
                                  (uint8_t[]){ili9882->colmod_val}, 1),
        TAG, "set COLMOD failed");

    for (int i = 0; i < ili9882->init_cmds_size; i++) {
        const ili9882_lcd_init_cmd_t *cmd = &ili9882->init_cmds[i];
        const void *cmd_data = cmd->data;
        uint8_t madctl_data = 0;

        if (is_user_page && cmd->data_bytes > 0) {
            switch (cmd->cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                madctl_data = (((const uint8_t *)cmd->data)[0] & ~LCD_CMD_BGR_BIT) |
                               (ili9882->madctl_val & LCD_CMD_BGR_BIT);
                ili9882->madctl_val = madctl_data;
                cmd_data = &madctl_data;
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                ili9882->colmod_val = ((const uint8_t *)cmd->data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG,
                         "The %02Xh command conflicts with the panel configuration",
                         cmd->cmd);
            }
        }

        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(io, cmd->cmd, cmd_data, cmd->data_bytes),
            TAG, "send command %d (0x%02X) failed", i, cmd->cmd);
        vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));

        if (cmd->cmd == ILI9882_CMD_PAGE && cmd->data_bytes >= 3) {
            const uint8_t *page_data = (const uint8_t *)cmd->data;
            is_user_page = page_data[0] == 0x98 && page_data[1] == 0x82 &&
                           page_data[2] == ILI9882_PAGE_USER;
        }
    }
    ESP_LOGD(TAG, "send init commands success");

    ESP_RETURN_ON_ERROR(ili9882->init(panel), TAG,
                        "init MIPI DPI panel failed");
    return ESP_OK;
}

static esp_err_t panel_ili9882_reset(esp_lcd_panel_t *panel)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = ili9882->io;

    if (ili9882->reset_gpio_num >= 0) {
        gpio_set_level(ili9882->reset_gpio_num, !ili9882->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(ili9882->reset_gpio_num, ili9882->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ili9882->reset_gpio_num, !ili9882->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0),
            TAG, "send reset command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_ili9882_invert_color(esp_lcd_panel_t *panel,
                                            bool invert_color_data)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;

    ESP_RETURN_ON_FALSE(ili9882->io, ESP_ERR_INVALID_STATE, TAG,
                        "invalid panel IO");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(ili9882->io, command, NULL, 0),
        TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_ili9882_mirror(esp_lcd_panel_t *panel,
                                     bool mirror_x, bool mirror_y)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;
    uint8_t madctl_val = ili9882->madctl_val;

    ESP_RETURN_ON_FALSE(ili9882->io, ESP_ERR_INVALID_STATE, TAG,
                        "invalid panel IO");

    if (mirror_x) {
        madctl_val |= ILI9882_CMD_SHLR_BIT;
    } else {
        madctl_val &= ~ILI9882_CMD_SHLR_BIT;
    }
    if (mirror_y) {
        madctl_val |= ILI9882_CMD_UPDN_BIT;
    } else {
        madctl_val &= ~ILI9882_CMD_UPDN_BIT;
    }

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(ili9882->io, LCD_CMD_MADCTL,
                                  (uint8_t[]){madctl_val}, 1),
        TAG, "send command failed");
    ili9882->madctl_val = madctl_val;
    return ESP_OK;
}

static esp_err_t panel_ili9882_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ili9882_panel_t *ili9882 = (ili9882_panel_t *)panel->user_data;
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(ili9882->io, command, NULL, 0),
        TAG, "send command failed");
    return ESP_OK;
}
#endif
