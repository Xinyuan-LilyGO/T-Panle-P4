/*
 * ESP32-P4 display panel driver
 * LCD panel driver for the T-Panel-P4 board variants.
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

#include "driver/ledc.h"
#include "display_panel.h"
#include "display_panel_profile.h"
#if CONFIG_T_PANEL_P4_BOARD_RECT
#include "esp_lcd_ili9882.h"
#include "hal/axi_icm_ll.h"
#include "hal/dw_gdma_ll.h"
#else
#include "esp_io_expander.h"
#include "esp_lcd_jd9365.h"
#endif
#include "board_config.h"

#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ 30000
#define LCD_BL_LEDC_RESOLUTION LEDC_TIMER_11_BIT
#define DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN 3
#define DISPLAY_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define DISPLAY_DSI_DMA_AXI_READ_PRIORITY 15
#if CONFIG_T_PANEL_P4_BOARD_RECT
#define DISPLAY_LCD_BL_PWM_GPIO LCD_BL_PWM_PIN
#else
#define DISPLAY_LCD_BL_PWM_GPIO LCD_BL_PWM
#endif

static const char *TAG = "display_panel";

/* -------------------- TE Input -------------------- */

#if CONFIG_T_PANEL_P4_BOARD_RECT
static esp_err_t lcd_te_gpio_init(void)
{
    const gpio_config_t te_gpio_config = {
        .pin_bit_mask = 1ULL << LCD_TE_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&te_gpio_config), TAG,
                        "configure LCD TE GPIO failed");
    ESP_LOGI(TAG, "LCD TE input configured on GPIO%d", LCD_TE_PIN);
    return ESP_OK;
}

static void lcd_dsi_memory_qos_init(void)
{
    /* The DPI engine continuously reads its framebuffer through DW-GDMA's
     * memory master port. Give those reads priority over competing PSRAM
     * traffic so the DSI bridge FIFO cannot underrun during UI redraws. */
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(DW_GDMA_LL_MASTER_PORT_MEMORY,
                                            0,
                                            DISPLAY_DSI_DMA_AXI_READ_PRIORITY);
    ESP_LOGI(TAG, "LCD DW-GDMA AXI read priority set to %d",
             DISPLAY_DSI_DMA_AXI_READ_PRIORITY);
}
#endif

/* -------------------- XL9555 Reset Control -------------------- */

#if !CONFIG_T_PANEL_P4_BOARD_RECT
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
#endif

/* -------------------- DPI refresh callback -------------------- */

static IRAM_ATTR bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    display_panel_t *drv = (display_panel_t *)user_ctx;
    BaseType_t need_yield = pdFALSE;

    if (drv && drv->color_trans_done)
    {
        xSemaphoreGiveFromISR(drv->color_trans_done, &need_yield);
    }
    if (drv && drv->color_trans_done_cb &&
        drv->color_trans_done_cb(drv->color_trans_done_user_ctx))
    {
        need_yield = pdTRUE;
    }

    return need_yield == pdTRUE;
}

static IRAM_ATTR bool on_refresh_done(esp_lcd_panel_handle_t panel,
                                      esp_lcd_dpi_panel_event_data_t *edata,
                                      void *user_ctx)
{
    display_panel_t *drv = (display_panel_t *)user_ctx;
    BaseType_t need_yield = pdFALSE;

    if (drv && drv->refresh_done)
    {
        xSemaphoreGiveFromISR(drv->refresh_done, &need_yield);
    }

    return need_yield == pdTRUE;
}

/* -------------------- LCD Init -------------------- */

esp_err_t display_panel_init(display_panel_t *drv, esp_io_expander_handle_t reset_expander)
{
    ESP_RETURN_ON_FALSE(drv != NULL, ESP_ERR_INVALID_ARG, TAG, "display handle is NULL");
    memset(drv, 0, sizeof(*drv));
    drv->expander = reset_expander;

#if CONFIG_T_PANEL_P4_BOARD_RECT
    // Step 1: Configure the panel TE output as an MCU input.
    ESP_RETURN_ON_ERROR(lcd_te_gpio_init(), TAG, "LCD TE GPIO init failed");
    lcd_dsi_memory_qos_init();
#else
    // Step 1: Reset LCD via XL9555
    ESP_RETURN_ON_FALSE(drv->expander != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "LCD reset IO expander is NULL");
    ESP_RETURN_ON_ERROR(lcd_reset_via_xl9555(drv->expander), TAG, "LCD reset failed");
#endif

    // Step 2: Power on MIPI DSI PHY via LDO
    ESP_LOGI(TAG, "Power on MIPI DSI PHY");
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DISPLAY_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = DISPLAY_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
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
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = display_panel_profile.lane_num,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = display_panel_profile.lane_bit_rate_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &drv->mipi_dsi_bus),
                        TAG, "create DSI bus failed");

    // Step 4: Install panel IO (DBI command interface)
    ESP_LOGI(TAG, "Install MIPI DBI panel IO");
#if CONFIG_T_PANEL_P4_BOARD_RECT
    esp_lcd_dbi_io_config_t dbi_config = ILI9882_PANEL_IO_DBI_CONFIG();
#else
    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(drv->mipi_dsi_bus, &dbi_config, &drv->mipi_dbi_io),
                        TAG, "create DBI IO failed");

    ESP_LOGI(TAG, "Install %s driver", display_panel_profile.name);
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = display_panel_profile.pixel_clock_mhz,
        .virtual_channel = 0,
#if DISPLAY_PANEL_BITS_PER_PIXEL == 16
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
#else
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
#endif
        .num_fbs = CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT,
        .video_timing = {
            .h_size = display_panel_profile.width,
            .v_size = display_panel_profile.height,
            .hsync_back_porch = display_panel_profile.hbp,
            .hsync_pulse_width = display_panel_profile.hsync,
            .hsync_front_porch = display_panel_profile.hfp,
            .vsync_back_porch = display_panel_profile.vbp,
            .vsync_pulse_width = display_panel_profile.vsync,
            .vsync_front_porch = display_panel_profile.vfp,
        },
        .flags.use_dma2d = true,
    };
#if CONFIG_T_PANEL_P4_BOARD_RECT
    ili9882_vendor_config_t vendor_config = {
        .init_cmds = (const ili9882_lcd_init_cmd_t *)display_panel_profile.init_cmds,
        .init_cmds_size = display_panel_profile.init_cmds_count,
#else
    jd9365_vendor_config_t vendor_config = {
        .init_cmds = (const jd9365_lcd_init_cmd_t *)display_panel_profile.init_cmds,
        .init_cmds_size = display_panel_profile.init_cmds_count,
#endif
        .mipi_config = {
            .dsi_bus = drv->mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = display_panel_profile.lane_num,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
#if CONFIG_T_PANEL_P4_BOARD_RECT
        .reset_gpio_num = LCD_RST_PIN,
#else
        .reset_gpio_num = -1,
#endif
        .rgb_ele_order = display_panel_profile.rgb_ele_order,
        .bits_per_pixel = display_panel_profile.bits_per_pixel,
        .vendor_config = &vendor_config,
    };
#if CONFIG_T_PANEL_P4_BOARD_RECT
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9882(drv->mipi_dbi_io, &panel_config, &drv->panel_handle),
                        TAG, "create ILI9882 panel failed");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(drv->mipi_dbi_io, &panel_config, &drv->panel_handle),
                        TAG, "create JD9365 panel failed");
#endif
#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 2
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(drv->panel_handle, 2,
                                                           &drv->frame_buffers[0],
                                                           &drv->frame_buffers[1]),
                        TAG, "get DPI frame buffers failed");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(drv->panel_handle, 1,
                                                           &drv->frame_buffers[0]),
                        TAG, "get DPI frame buffer failed");
#endif
    drv->frame_buffer_count = CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT;
    drv->next_fb = drv->frame_buffer_count > 1 ? 1 : 0;

    // Step 6: Initialize panel (sends init commands via DBI)
    ESP_LOGI(TAG, "Initialize LCD panel");
#if CONFIG_T_PANEL_P4_BOARD_RECT
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(drv->panel_handle), TAG, "reset ILI9882 panel failed");
#endif
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
    xSemaphoreGive(drv->color_trans_done);

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
        .on_refresh_done = on_refresh_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(drv->panel_handle, &cbs, drv),
                        TAG, "register DPI callbacks failed");

    ESP_LOGI(TAG, "%s initialized (%ux%u, %u-lane MIPI DSI, %ubpp, %uMHz DPI)",
             display_panel_profile.name, display_panel_profile.width, display_panel_profile.height,
             display_panel_profile.lane_num, display_panel_profile.bits_per_pixel,
             display_panel_profile.pixel_clock_mhz
        );
    return ESP_OK;
}

void *display_panel_get_next_frame_buffer(display_panel_t *drv)
{
    if (drv == NULL || drv->frame_buffer_count == 0)
    {
        return NULL;
    }
    return drv->frame_buffers[drv->next_fb];
}

void display_panel_set_color_trans_done_cb(display_panel_t *drv,
                                           display_panel_color_trans_done_cb_t callback,
                                           void *user_ctx)
{
    if (drv == NULL)
    {
        return;
    }
    drv->color_trans_done_cb = callback;
    drv->color_trans_done_user_ctx = user_ctx;
}

/* -------------------- LCD Deinit -------------------- */

esp_err_t display_panel_deinit(display_panel_t *drv)
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
    if (drv->color_trans_done)
    {
        vSemaphoreDelete(drv->color_trans_done);
        drv->color_trans_done = NULL;
    }
    if (drv->refresh_done)
    {
        vSemaphoreDelete(drv->refresh_done);
        drv->refresh_done = NULL;
    }

    ESP_LOGI(TAG, "LCD JD9365 deinitialized");
    return ESP_OK;
}

/* -------------------- LCD Draw -------------------- */

esp_err_t display_panel_draw_bitmap_async(display_panel_t *drv,
                                          int x_start, int y_start,
                                          int x_end, int y_end,
                                          const void *color_data)
{
    ESP_RETURN_ON_FALSE(drv != NULL && drv->panel_handle != NULL &&
                            drv->color_trans_done != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display is not ready");

    xSemaphoreTake(drv->color_trans_done, portMAX_DELAY);

    esp_err_t ret = esp_lcd_panel_draw_bitmap(drv->panel_handle,
                                              x_start, y_start,
                                              x_end, y_end,
                                              color_data);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(drv->color_trans_done);
        ESP_LOGE(TAG, "draw bitmap failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (drv->frame_buffer_count > 1 && color_data == drv->frame_buffers[drv->next_fb])
    {
        drv->next_fb = (drv->next_fb + 1) % drv->frame_buffer_count;
    }
    return ESP_OK;
}

esp_err_t display_panel_draw_bitmap(display_panel_t *drv, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data)
{
    ESP_RETURN_ON_ERROR(display_panel_draw_bitmap_async(drv,
                                                        x_start, y_start,
                                                        x_end, y_end,
                                                        color_data),
                        TAG, "start bitmap draw failed");

    xSemaphoreTake(drv->color_trans_done, portMAX_DELAY);
    xSemaphoreGive(drv->color_trans_done);
    return ESP_OK;
}

esp_err_t display_panel_wait_refresh_done(display_panel_t *drv, uint32_t timeout_ms)
{
    if (drv == NULL || drv->refresh_done == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (xSemaphoreTake(drv->refresh_done, 0) == pdTRUE)
    {
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(drv->refresh_done, timeout_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* -------------------- Backlight Control (LEDC PWM) -------------------- */

static uint8_t s_backlight_brightness_percent = 100;

esp_err_t display_panel_backlight_init(void)
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
        .gpio_num = DISPLAY_LCD_BL_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");

    ESP_LOGI(TAG, "Backlight PWM initialized on GPIO %d", DISPLAY_LCD_BL_PWM_GPIO);
    return ESP_OK;
}

esp_err_t display_panel_backlight_set(bool on)
{
    uint32_t max_duty = (1 << LCD_BL_LEDC_RESOLUTION) - 1;
    uint32_t duty = on ? max_duty * s_backlight_brightness_percent / 100 : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL),
                        TAG, "update duty failed");
    return ESP_OK;
}

esp_err_t display_panel_set_brightness(uint8_t percent)
{
    if (percent > 100)
        percent = 100;
    uint32_t max_duty = (1 << LCD_BL_LEDC_RESOLUTION) - 1;
    uint32_t duty = max_duty * percent / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL),
                        TAG, "update duty failed");
    s_backlight_brightness_percent = percent;
    ESP_LOGI(TAG, "Backlight brightness: %d%%", percent);
    return ESP_OK;
}
