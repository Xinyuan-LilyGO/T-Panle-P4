/*
 * ESP32-P4 JD9365 MIPI DSI 2-Lane LCD Driver Template
 * LCD RST controlled by XL9555 IO expander
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"

#include "driver/ledc.h"
#include "esp_lcd_jd9365.h"
#include "lcd_jd9365_driver.h"
#include "T_Panle_P4_board_config.h"

#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ 5000
#define LCD_BL_LEDC_RESOLUTION LEDC_TIMER_13_BIT

#define LCD_PANEL_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_PANEL_RGB_ORDER_NAME "RGB"

static const char *TAG = "lcd_jd9365";

/* -------------------- XL9555 Reset Control -------------------- */

static esp_err_t lcd_reset_via_xl9555(esp_io_expander_handle_t expander)
{
    uint32_t rst_pin_mask = (1UL << XL9555_LCD_RST);

    // Set RST pin as output
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, rst_pin_mask, IO_EXPANDER_OUTPUT),
                        TAG, "set RST pin dir failed");

    // Reset sequence: pull low -> delay -> pull high
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, rst_pin_mask, 0),
                        TAG, "RST low failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, rst_pin_mask, 1),
                        TAG, "RST high failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "LCD reset done via XL9555");
    return ESP_OK;
}

/* -------------------- DPI refresh callback -------------------- */

static IRAM_ATTR bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    lcd_driver_t *drv = (lcd_driver_t *)user_ctx;
    BaseType_t need_yield = pdFALSE;

    if (drv && drv->color_trans_done) {
        xSemaphoreGiveFromISR(drv->color_trans_done, &need_yield);
    }

    return need_yield == pdTRUE;
}

static IRAM_ATTR bool on_refresh_done(esp_lcd_panel_handle_t panel,
                                      esp_lcd_dpi_panel_event_data_t *edata,
                                      void *user_ctx)
{
    lcd_driver_t *drv = (lcd_driver_t *)user_ctx;
    BaseType_t need_yield = pdFALSE;

    if (drv && drv->refresh_done)
    {
        xSemaphoreGiveFromISR(drv->refresh_done, &need_yield);
    }

    return need_yield == pdTRUE;
}

/* -------------------- LCD Init -------------------- */

esp_err_t lcd_jd9365_init(lcd_driver_t *drv, esp_io_expander_handle_t expander)
{
    memset(drv, 0, sizeof(lcd_driver_t));
    drv->expander = expander;

    // Step 1: Reset LCD via XL9555
    ESP_RETURN_ON_ERROR(lcd_reset_via_xl9555(drv->expander), TAG, "LCD reset failed");

    // Step 2: Power on MIPI DSI PHY via LDO
    ESP_LOGI(TAG, "Power on MIPI DSI PHY");
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &drv->ldo_mipi_phy);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "MIPI DSI PHY LDO channel already acquired, continue");
    }
    else
    {
        ESP_RETURN_ON_ERROR(ret, TAG, "LDO acquire failed");
    }

    // Step 3: Initialize MIPI DSI bus
    ESP_LOGI(TAG, "Initialize MIPI DSI bus");
    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    bus_config.lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE; // Override to use header value (1500 Mbps)
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &drv->mipi_dsi_bus),
                        TAG, "create DSI bus failed");

    // Step 4: Install panel IO (DBI command interface)
    ESP_LOGI(TAG, "Install MIPI DBI panel IO");
    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(drv->mipi_dsi_bus, &dbi_config, &drv->mipi_dbi_io),
                        TAG, "create DBI IO failed");

    // Step 5: Create JD9365 panel
    ESP_LOGI(TAG, "Install JD9365 LCD driver");
    esp_lcd_dpi_panel_config_t dpi_config = JD9365_720_720_PANEL_60HZ_DPI_CONFIG(LCD_MIPI_DPI_PX_FORMAT);
    dpi_config.dpi_clock_freq_mhz = LCD_DPI_CLOCK_FREQ_MHZ;
    dpi_config.num_fbs = 2;
    jd9365_vendor_config_t vendor_config = {
        .init_cmds = NULL, // Use default init sequence; set custom cmds here if needed
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = drv->mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = LCD_MIPI_DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, // RST is controlled by XL9555, not a direct GPIO
        .rgb_ele_order = LCD_PANEL_RGB_ORDER,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(drv->mipi_dbi_io, &panel_config, &drv->panel_handle),
                        TAG, "create JD9365 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(drv->panel_handle, 2,
                                                           &drv->frame_buffers[0], &drv->frame_buffers[1]),
                        TAG, "get DPI frame buffers failed");
    drv->next_fb = 1;

    // Step 6: Initialize panel (sends init commands via DBI)
    ESP_LOGI(TAG, "Initialize LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(drv->panel_handle), TAG, "panel init failed");

    // Step 7: Turn on display
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(drv->panel_handle, true), TAG, "display on failed");

    // Step 8: Register DPI refresh done callback
    drv->color_trans_done = xSemaphoreCreateBinary();
    drv->refresh_done = xSemaphoreCreateBinary();
    if (drv->color_trans_done == NULL || drv->refresh_done == NULL)
    {
        ESP_LOGE(TAG, "create LCD semaphores failed");
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
        .on_refresh_done = on_refresh_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(drv->panel_handle, &cbs, drv),
                        TAG, "register DPI callbacks failed");

    ESP_LOGI(TAG, "LCD JD9365 initialized successfully (%dx%d, %d-lane MIPI DSI, %dbpp, %dMHz DPI, %s order)",
             LCD_H_RES, LCD_V_RES, LCD_MIPI_DSI_LANE_NUM, LCD_BIT_PER_PIXEL, LCD_DPI_CLOCK_FREQ_MHZ,
             LCD_PANEL_RGB_ORDER_NAME);
    return ESP_OK;
}

void *lcd_jd9365_get_next_frame_buffer(lcd_driver_t *drv)
{
    return drv->frame_buffers[drv->next_fb];
}

/* -------------------- LCD Deinit -------------------- */

esp_err_t lcd_jd9365_deinit(lcd_driver_t *drv)
{
    if (drv->panel_handle)
    {
        esp_lcd_panel_disp_on_off(drv->panel_handle, false);
        esp_lcd_panel_del(drv->panel_handle);
        drv->panel_handle = NULL;
    }
    if (drv->mipi_dbi_io)
    {
        esp_lcd_panel_io_del(drv->mipi_dbi_io);
        drv->mipi_dbi_io = NULL;
    }
    if (drv->mipi_dsi_bus)
    {
        esp_lcd_del_dsi_bus(drv->mipi_dsi_bus);
        drv->mipi_dsi_bus = NULL;
    }
    if (drv->ldo_mipi_phy)
    {
        esp_ldo_release_channel(drv->ldo_mipi_phy);
        drv->ldo_mipi_phy = NULL;
    }
    if (drv->refresh_done)
    {
        vSemaphoreDelete(drv->refresh_done);
        drv->refresh_done = NULL;
    }
    if (drv->color_trans_done) {
        vSemaphoreDelete(drv->color_trans_done);
        drv->color_trans_done = NULL;
    }    
    if (drv->refresh_done) {
        vSemaphoreDelete(drv->refresh_done);
        drv->refresh_done = NULL;
    }

    ESP_LOGI(TAG, "LCD JD9365 deinitialized");
    return ESP_OK;
}

/* -------------------- LCD Draw -------------------- */

esp_err_t lcd_jd9365_draw_bitmap(lcd_driver_t *drv, int x_start, int y_start,
                                 int x_end, int y_end, const void *color_data)
{
    while (xSemaphoreTake(drv->color_trans_done, 0) == pdTRUE); // Wait for previous transfer done

    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(drv->panel_handle, x_start, y_start, x_end, y_end, color_data),
                        TAG, "draw bitmap failed");
    if (color_data == drv->frame_buffers[drv->next_fb])
    {
        drv->next_fb = (drv->next_fb + 1) % 2;
    }
    // Wait for DMA transfer done
    xSemaphoreTake(drv->color_trans_done, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t lcd_jd9365_wait_refresh_done(lcd_driver_t *drv, uint32_t timeout_ms)
{
    if (drv == NULL || drv->refresh_done == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (xSemaphoreTake(drv->refresh_done, 0) == pdTRUE) {
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(drv->refresh_done, timeout_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* -------------------- Backlight Control (LEDC PWM) -------------------- */

esp_err_t lcd_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BL_LEDC_RESOLUTION,
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num = LCD_BL_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");

    ESP_LOGI(TAG, "Backlight PWM initialized on GPIO %d", LCD_BL_PWM);
    return ESP_OK;
}

esp_err_t lcd_backlight_set(bool on)
{
    uint32_t duty = on ? ((1 << LCD_BL_LEDC_RESOLUTION) - 1) : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL),
                        TAG, "update duty failed");
    return ESP_OK;
}

esp_err_t lcd_backlight_set_brightness(uint8_t percent)
{
    if (percent > 100)
        percent = 100;
    uint32_t max_duty = (1 << LCD_BL_LEDC_RESOLUTION) - 1;
    uint32_t duty = max_duty * percent / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL),
                        TAG, "update duty failed");
    ESP_LOGI(TAG, "Backlight brightness: %d%%", percent);
    return ESP_OK;
}
