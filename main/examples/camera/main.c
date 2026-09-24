/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_io_expander.h"
#if CONFIG_T_PANEL_P4_HAS_XL9555
#include "esp_io_expander_xl9555.h"
#endif
#include "sgm38121.h"

#include "driver/isp.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_sccb_i2c.h"
#include "ov2710.h"
#include "display_panel.h"

static const char *TAG = "camera_example";

#define CAM_TARGET_H_RES 1920
#define CAM_TARGET_V_RES 1080
#define CAM_FRAME_BUFFER_COUNT 4
#define MIPI_CSI_PHY_PWR_LDO_CHAN 3
#define MIPI_CSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define CAM_ENABLE_ISP_PIPELINE 1
#define CAM_ENABLE_TEST_PATTERN 0
#define CAM_OVERRIDE_OV2710_MIPI_CTRL 1
#define CAM_OV2710_MIPI_CTRL_VALUE 0x00
#define CAM_STREAM_SENSOR_BEFORE_CSI 0
#define CAM_FRAME_LOG_INTERVAL 25
#define CAM_LCD_PREVIEW 1
#define CAM_LCD_TARGET_FPS 25
#define CAM_LCD_CONVERT_YIELD_LINES DISPLAY_PANEL_V_RES
#define CAM_LCD_FAST_PREVIEW 1
#define CAM_LCD_FAST_PREVIEW_SCALE 2
#define CAM_YIELD_TICKS 1
#define CAM_LCD_TASK_STACK_SIZE 8192
#define CAM_LCD_TASK_PRIORITY 4
#if CONFIG_FREERTOS_UNICORE
#define CAM_LCD_TASK_CORE 0
#else
#define CAM_LCD_TASK_CORE 1
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static sgm38121_handle_t pmic;
static esp_io_expander_handle_t expander = NULL;
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static display_panel_t lcd = {};

static esp_cam_ctlr_handle_t cam_ctlr = NULL;
static isp_proc_handle_t isp_proc = NULL;
static esp_cam_sensor_device_t *cam_sensor = NULL;
static const esp_cam_sensor_format_t *cam_format = NULL;

static SemaphoreHandle_t frame_ready_sem = NULL;
static uint8_t *frame_buffers[CAM_FRAME_BUFFER_COUNT] = {};
static size_t frame_buffer_size = 0;
static uint8_t next_frame_buffer = 0;

static volatile const uint8_t *last_frame_buffer = NULL;
static volatile size_t last_frame_size = 0;
static volatile uint32_t get_trans_cnt = 0;
static volatile uint32_t finish_cnt = 0;

#if CAM_LCD_PREVIEW
static uint8_t *lcd_framebuffer = NULL;
static size_t lcd_framebuffer_size = 0;
static int64_t next_lcd_frame_time_us = 0;
static uint32_t lcd_frame_interval_us = 0;
static uint16_t lcd_src_x_map[DISPLAY_PANEL_H_RES] = {};
static uint16_t lcd_src_y_map[DISPLAY_PANEL_V_RES] = {};
static TaskHandle_t lcd_preview_task_handle = NULL;
static portMUX_TYPE lcd_preview_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile const uint8_t *lcd_preview_frame = NULL;
static volatile const uint8_t *lcd_preview_active_frame = NULL;
static volatile size_t lcd_preview_frame_size = 0;
static volatile uint32_t lcd_preview_drop_cnt = 0;
#endif

static esp_err_t ov2710_read_reg(uint16_t reg, uint8_t *value)
{
    esp_cam_sensor_reg_val_t reg_val = {
        .regaddr = reg,
    };

    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam_sensor, ESP_CAM_SENSOR_IOC_G_REG, &reg_val),
                        TAG, "Read OV2710 reg 0x%04x failed", reg);
    *value = reg_val.value;
    return ESP_OK;
}

static esp_err_t ov2710_write_reg(uint16_t reg, uint8_t value)
{
    esp_cam_sensor_reg_val_t reg_val = {
        .regaddr = reg,
        .value = value,
    };

    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam_sensor, ESP_CAM_SENSOR_IOC_S_REG, &reg_val),
                        TAG, "Write OV2710 reg 0x%04x failed", reg);
    ESP_LOGI(TAG, "OV2710 reg[0x%04x] <= 0x%02x", reg, value);
    return ESP_OK;
}

static void ov2710_dump_regs(const char *stage)
{
    static const uint16_t regs[] = {
        0x3008, 0x300e, 0x300f, 0x3011,
        0x3017, 0x3018, 0x4201, 0x4202,
        0x4800, 0x4801, 0x380c, 0x380d,
        0x380e, 0x380f,
    };

    ESP_LOGI(TAG, "OV2710 register dump (%s)", stage);
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        uint8_t value = 0;
        esp_err_t ret = ov2710_read_reg(regs[i], &value);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  [0x%04x] = 0x%02x", regs[i], value);
        } else {
            ESP_LOGW(TAG, "  [0x%04x] read failed: %s", regs[i], esp_err_to_name(ret));
        }
    }
}

static bool IRAM_ATTR on_csi_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    get_trans_cnt++;
    uint8_t selected_buffer = next_frame_buffer;
#if CAM_LCD_PREVIEW
    const uint8_t *pending_frame = (const uint8_t *)lcd_preview_frame;
    const uint8_t *active_frame = (const uint8_t *)lcd_preview_active_frame;
    for (uint8_t i = 0; i < CAM_FRAME_BUFFER_COUNT; i++) {
        uint8_t candidate = (uint8_t)((next_frame_buffer + i) % CAM_FRAME_BUFFER_COUNT);
        const uint8_t *candidate_buffer = frame_buffers[candidate];
        if (candidate_buffer != pending_frame && candidate_buffer != active_frame) {
            selected_buffer = candidate;
            break;
        }
    }
#endif
    trans->buffer = frame_buffers[selected_buffer];
    trans->buflen = frame_buffer_size;
    next_frame_buffer = (selected_buffer + 1) % CAM_FRAME_BUFFER_COUNT;
    return false;
}

static bool IRAM_ATTR on_csi_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    finish_cnt++;
    last_frame_buffer = (const uint8_t *)trans->buffer;
    last_frame_size = trans->received_size;

    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(frame_ready_sem, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static size_t get_frame_size_bytes(const esp_cam_sensor_format_t *format)
{
    uint32_t bits_per_pixel = 0;

    switch (format->format) {
    case ESP_CAM_SENSOR_PIXFORMAT_RAW8:
        bits_per_pixel = 8;
        break;
    case ESP_CAM_SENSOR_PIXFORMAT_RAW10:
        bits_per_pixel = 10;
        break;
    case ESP_CAM_SENSOR_PIXFORMAT_RAW12:
        bits_per_pixel = 12;
        break;
    case ESP_CAM_SENSOR_PIXFORMAT_RGB565:
        bits_per_pixel = 16;
        break;
    default:
        bits_per_pixel = 16;
        break;
    }

    return ((size_t)format->width * format->height * bits_per_pixel + 7) / 8;
}

#if CAM_LCD_PREVIEW
static inline uint8_t get_raw10_msb8_pixel(const uint8_t *raw10, uint32_t width, uint32_t x, uint32_t y)
{
    size_t pixel_index = (size_t)y * width + x;
    size_t group_index = (pixel_index / 4) * 5;
    return raw10[group_index + (pixel_index & 0x03)];
}

static void convert_raw10_bggr_to_lcd_rgb888(const uint8_t *src, uint8_t *dst)
{
    const uint32_t src_w = cam_format->width;

#if CAM_LCD_FAST_PREVIEW
    const uint32_t scale = CAM_LCD_FAST_PREVIEW_SCALE;
    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y += scale) {
        uint32_t src_y = lcd_src_y_map[y];
        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x += scale) {
            uint32_t src_x = lcd_src_x_map[x];
            uint8_t b = get_raw10_msb8_pixel(src, src_w, src_x, src_y);
            uint8_t g0 = get_raw10_msb8_pixel(src, src_w, src_x + 1, src_y);
            uint8_t g1 = get_raw10_msb8_pixel(src, src_w, src_x, src_y + 1);
            uint8_t r = get_raw10_msb8_pixel(src, src_w, src_x + 1, src_y + 1);
            uint8_t g = (uint8_t)(((uint16_t)g0 + g1) >> 1);

            for (uint32_t yy = 0; yy < scale; yy++) {
                uint8_t *p = dst + ((size_t)(y + yy) * DISPLAY_PANEL_H_RES + x) * 3;
                for (uint32_t xx = 0; xx < scale; xx++) {
                    p[0] = b;
                    p[1] = g;
                    p[2] = r;
                    p += 3;
                }
            }
        }
    }
#else
    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++) {
        uint32_t src_y = lcd_src_y_map[y];
        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++) {
            uint32_t src_x = lcd_src_x_map[x];
            uint8_t b = get_raw10_msb8_pixel(src, src_w, src_x, src_y);
            uint8_t g0 = get_raw10_msb8_pixel(src, src_w, src_x + 1, src_y);
            uint8_t g1 = get_raw10_msb8_pixel(src, src_w, src_x, src_y + 1);
            uint8_t r = get_raw10_msb8_pixel(src, src_w, src_x + 1, src_y + 1);
            uint8_t g = (uint8_t)(((uint16_t)g0 + g1) >> 1);
            uint8_t *p = dst + ((size_t)y * DISPLAY_PANEL_H_RES + x) * 3;
            p[0] = b;
            p[1] = g;
            p[2] = r;
        }
        if (((y + 1) % CAM_LCD_CONVERT_YIELD_LINES) == 0) {
            vTaskDelay(CAM_YIELD_TICKS);
        }
    }
#endif

    // Hide unstable edge pixels at the LCD boundary and make sure the DPI DMA sees CPU writes.
    if (DISPLAY_PANEL_V_RES > 1) {
        uint8_t *last_line = dst + ((size_t)(DISPLAY_PANEL_V_RES - 1) * DISPLAY_PANEL_H_RES * 3);
        const uint8_t *prev_line = dst + ((size_t)(DISPLAY_PANEL_V_RES - 2) * DISPLAY_PANEL_H_RES * 3);
        memcpy(last_line, prev_line, DISPLAY_PANEL_H_RES * 3);
    }
    esp_cache_msync(dst, lcd_framebuffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void init_lcd_preview_maps(void)
{
    const uint32_t src_w = cam_format->width;
    const uint32_t src_h = cam_format->height;
    const uint32_t crop = src_h;
    const uint32_t crop_x = (src_w - crop) / 2;

    for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++) {
        lcd_src_x_map[x] = (uint16_t)((crop_x + (uint32_t)(((uint64_t)x * crop) / DISPLAY_PANEL_H_RES)) & ~1U);
    }
    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++) {
        lcd_src_y_map[y] = (uint16_t)(((uint64_t)y * crop) / DISPLAY_PANEL_V_RES) & ~1U;
    }
}

static void lcd_preview_submit_frame(const uint8_t *frame, size_t frame_size)
{
    if (!lcd_preview_task_handle || !frame) {
        return;
    }

    portENTER_CRITICAL(&lcd_preview_lock);
    if (lcd_preview_frame != NULL) {
        lcd_preview_drop_cnt++;
    }
    lcd_preview_frame = frame;
    lcd_preview_frame_size = frame_size;
    portEXIT_CRITICAL(&lcd_preview_lock);

    xTaskNotifyGive(lcd_preview_task_handle);
}

static const uint8_t *lcd_preview_take_frame(size_t *frame_size)
{
    const uint8_t *frame = NULL;

    portENTER_CRITICAL(&lcd_preview_lock);
    frame = (const uint8_t *)lcd_preview_frame;
    *frame_size = lcd_preview_frame_size;
    lcd_preview_frame = NULL;
    lcd_preview_frame_size = 0;
    lcd_preview_active_frame = frame;
    portEXIT_CRITICAL(&lcd_preview_lock);

    return frame;
}

static void lcd_preview_release_frame(const uint8_t *frame)
{
    portENTER_CRITICAL(&lcd_preview_lock);
    if (lcd_preview_active_frame == frame) {
        lcd_preview_active_frame = NULL;
    }
    portEXIT_CRITICAL(&lcd_preview_lock);
}

static void lcd_preview_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        size_t frame_size = 0;
        const uint8_t *src = lcd_preview_take_frame(&frame_size);
        if (!src || frame_size < frame_buffer_size) {
            lcd_preview_release_frame(src);
            vTaskDelay(CAM_YIELD_TICKS);
            continue;
        }

        lcd_framebuffer = display_panel_get_next_frame_buffer(&lcd);
        convert_raw10_bggr_to_lcd_rgb888(src, lcd_framebuffer);
        lcd_preview_release_frame(src);
        esp_err_t ret = display_panel_draw_bitmap(&lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                                  DISPLAY_PANEL_V_RES, lcd_framebuffer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD draw failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(CAM_YIELD_TICKS);
    }
}

static esp_err_t init_lcd_preview(void)
{
    lcd_framebuffer_size = DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * (DISPLAY_PANEL_BITS_PER_PIXEL / 8);
    ESP_RETURN_ON_ERROR(display_panel_init(&lcd, expander), TAG, "LCD init failed");
    lcd_framebuffer = display_panel_get_next_frame_buffer(&lcd);
    ESP_RETURN_ON_FALSE(lcd_framebuffer, ESP_ERR_NO_MEM, TAG, "Failed to get LCD framebuffer");
    ESP_RETURN_ON_ERROR(display_panel_backlight_init(), TAG, "LCD backlight init failed");
    ESP_RETURN_ON_ERROR(display_panel_set_brightness(60), TAG, "LCD backlight set failed");
    ESP_LOGI(TAG, "LCD preview buffer ready: %ux%u, %u bytes",
             DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES, (unsigned)lcd_framebuffer_size);

    lcd_frame_interval_us = 1000000 / CAM_LCD_TARGET_FPS;
    next_lcd_frame_time_us = esp_timer_get_time();
    init_lcd_preview_maps();
    ESP_LOGI(TAG, "LCD preview target FPS: %u", CAM_LCD_TARGET_FPS);

    BaseType_t task_ret = xTaskCreatePinnedToCore(lcd_preview_task,
                                                  "lcd_preview",
                                                  CAM_LCD_TASK_STACK_SIZE,
                                                  NULL,
                                                  CAM_LCD_TASK_PRIORITY,
                                                  &lcd_preview_task_handle,
                                                  CAM_LCD_TASK_CORE);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "Create LCD preview task failed");
    ESP_LOGI(TAG, "LCD preview task started on CPU%d", CAM_LCD_TASK_CORE);

    return ESP_OK;
}
#endif

static esp_err_t init_camera_sensor(i2c_master_bus_handle_t i2c_bus)
{
    esp_sccb_io_handle_t sccb_handle = NULL;
    sccb_i2c_config_t sccb_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV2710_SCCB_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(sccb_new_i2c_io(i2c_bus, &sccb_cfg, &sccb_handle), TAG, "SCCB I2C IO create failed");

    esp_cam_sensor_config_t cam_cfg = {
        .sccb_handle = sccb_handle,
        .xclk_pin = -1,
        .xclk_freq_hz = 0,
        .reset_pin = -1,
        .pwdn_pin = -1,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };

    cam_sensor = ov2710_detect(&cam_cfg);
    ESP_RETURN_ON_FALSE(cam_sensor, ESP_FAIL, TAG, "OV2710 detect failed");
    ESP_LOGI(TAG, "Detected camera sensor: %s", esp_cam_sensor_get_name(cam_sensor));

    esp_cam_sensor_id_t chip_id = {};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam_sensor, ESP_CAM_SENSOR_IOC_G_CHIP_ID, &chip_id), TAG, "Get chip id failed");
    ESP_LOGI(TAG, "OV2710 chip id: pid=0x%04" PRIx16 ", ver=0x%02x", chip_id.pid, chip_id.ver);

    esp_cam_sensor_format_array_t fmt_array = {};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(cam_sensor, &fmt_array), TAG, "Query sensor formats failed");

    for (int i = 0; i < fmt_array.count; i++) {
        const esp_cam_sensor_format_t *fmt = &fmt_array.format_array[i];
        ESP_LOGI(TAG, "format[%d]: %s, %ux%u, %" PRIu32 " lane, %" PRIu32 " Mbps/lane",
                 i,
                 fmt->name,
                 fmt->width,
                 fmt->height,
                 fmt->mipi_info.lane_num,
                 fmt->mipi_info.mipi_clk / 1000000);
    }

    for (int i = 0; i < fmt_array.count; i++) {
        const esp_cam_sensor_format_t *fmt = &fmt_array.format_array[i];
        if (fmt->port == ESP_CAM_SENSOR_MIPI_CSI &&
                fmt->format == ESP_CAM_SENSOR_PIXFORMAT_RAW10 &&
                fmt->width == CAM_TARGET_H_RES &&
                fmt->height == CAM_TARGET_V_RES &&
                fmt->mipi_info.lane_num == 1) {
            cam_format = fmt;
            break;
        }
    }

    if (!cam_format && fmt_array.count > 0) {
        cam_format = &fmt_array.format_array[0];
        ESP_LOGW(TAG, "Target 1920x1080 RAW10 1-lane format not found, using %s", cam_format->name);
    }

    ESP_RETURN_ON_FALSE(cam_format, ESP_FAIL, TAG, "No camera format available");
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(cam_sensor, cam_format), TAG, "Set sensor format failed");

    frame_buffer_size = get_frame_size_bytes(cam_format);
    ESP_LOGI(TAG, "Selected %s, frame buffer size=%u bytes",
             cam_format->name, (unsigned)frame_buffer_size);

    return ESP_OK;
}

static esp_err_t init_csi_controller(void)
{
    ESP_RETURN_ON_FALSE(cam_format, ESP_ERR_INVALID_STATE, TAG, "Camera format is not selected");

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = cam_format->width,
        .v_res = cam_format->height,
        .data_lane_num = cam_format->mipi_info.lane_num,
        .lane_bit_rate_mbps = cam_format->mipi_info.mipi_clk / 1000000,
        .input_data_color_type = CAM_CTLR_COLOR_RAW10,
        .output_data_color_type = CAM_CTLR_COLOR_RAW10,
        .queue_items = 1,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };

    ESP_LOGI(TAG, "CSI config: %ux%u RAW10, %" PRIu32 " lane, %" PRIu32 " Mbps/lane",
             cam_format->width,
             cam_format->height,
             cam_format->mipi_info.lane_num,
             cam_format->mipi_info.mipi_clk / 1000000);

    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &cam_ctlr), TAG, "CSI controller create failed");

    esp_cam_ctlr_evt_cbs_t csi_cbs = {
        .on_get_new_trans = on_csi_get_new_trans,
        .on_trans_finished = on_csi_trans_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(cam_ctlr, &csi_cbs, NULL), TAG, "Register CSI callbacks failed");

    for (int i = 0; i < CAM_FRAME_BUFFER_COUNT; i++) {
        frame_buffers[i] = heap_caps_aligned_calloc(64, 1, frame_buffer_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(frame_buffers[i], ESP_ERR_NO_MEM, TAG, "Failed to allocate frame buffer[%d]", i);
    }

    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(cam_ctlr), TAG, "CSI enable failed");

    return ESP_OK;
}

static esp_err_t init_isp_pipeline(void)
{
#if CAM_ENABLE_ISP_PIPELINE
    ESP_RETURN_ON_FALSE(cam_format, ESP_ERR_INVALID_STATE, TAG, "Camera format is not selected");

    esp_isp_processor_cfg_t isp_cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 160 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW10,
        .output_data_color_type = ISP_COLOR_RAW10,
        .has_line_start_packet = cam_format->mipi_info.line_sync_en,
        .has_line_end_packet = cam_format->mipi_info.line_sync_en,
        .h_res = cam_format->width,
        .v_res = cam_format->height,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .flags = {
            .bypass_isp = true,
        },
    };

    ESP_LOGI(TAG, "ISP config: %ux%u RAW10 bypass, line_sync=%d",
             cam_format->width, cam_format->height, cam_format->mipi_info.line_sync_en);

    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &isp_proc), TAG, "ISP processor create failed");
    ESP_LOGI(TAG, "ISP processor created in bypass mode");
#endif

    return ESP_OK;
}

static esp_err_t init_mipi_csi_phy_ldo(void)
{
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_CSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_CSI_PHY_PWR_LDO_VOLTAGE_MV,
    };

    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MIPI PHY LDO channel already acquired, continue");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "MIPI PHY LDO acquire failed");
    ESP_LOGI(TAG, "MIPI CSI PHY LDO%d enabled at %dmV",
             MIPI_CSI_PHY_PWR_LDO_CHAN, MIPI_CSI_PHY_PWR_LDO_VOLTAGE_MV);
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

static esp_err_t start_ov2710_stream(void)
{
#if CAM_ENABLE_TEST_PATTERN
    int test_pattern_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam_sensor, ESP_CAM_SENSOR_IOC_S_TEST_PATTERN, &test_pattern_on),
                        TAG, "Enable OV2710 test pattern failed");
    ESP_LOGI(TAG, "OV2710 test pattern enabled");
#endif

    ov2710_dump_regs("before stream on");

    int stream_on = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on),
                        TAG, "OV2710 stream on failed");
    vTaskDelay(pdMS_TO_TICKS(20));

#if CAM_OVERRIDE_OV2710_MIPI_CTRL
    ESP_RETURN_ON_ERROR(ov2710_write_reg(0x4800, CAM_OV2710_MIPI_CTRL_VALUE),
                        TAG, "Override OV2710 MIPI control failed");
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    ov2710_dump_regs("after stream on");
    ESP_LOGI(TAG, "Camera streaming started");

    return ESP_OK;
}

static esp_err_t init_sgm38121(i2c_master_bus_handle_t bus_handle)
{
    ESP_RETURN_ON_ERROR(sgm38121_init(&pmic, bus_handle, SGM38121_I2C_ADDR), TAG, "SGM38121 init failed");

    ESP_RETURN_ON_ERROR(sgm38121_set_dvdd1_voltage(&pmic, 1500), TAG, "Set DVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd1_voltage(&pmic, 2800), TAG, "Set AVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd2_voltage(&pmic, 3300), TAG, "Set AVDD2 failed");

    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&pmic, SGM38121_CH_DVDD1, SGM38121_SEQ_SLOT_1), TAG, "Set DVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&pmic, SGM38121_CH_AVDD1, SGM38121_SEQ_SLOT_2), TAG, "Set AVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&pmic, SGM38121_CH_AVDD2, SGM38121_SEQ_SLOT_3), TAG, "Set AVDD2 sequence failed");

    ESP_RETURN_ON_ERROR(sgm38121_seq_powerup(&pmic), TAG, "Camera power-up sequence failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

void app_main(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    ESP_ERROR_CHECK(init_sgm38121(i2c_bus));
#if CONFIG_T_PANEL_P4_HAS_XL9555
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));
#endif

    ESP_ERROR_CHECK(init_camera_sensor(i2c_bus));
    ESP_ERROR_CHECK(init_mipi_csi_phy_ldo());
#if CAM_LCD_PREVIEW
    ESP_ERROR_CHECK(init_lcd_preview());
#endif

    frame_ready_sem = xSemaphoreCreateBinary();
    assert(frame_ready_sem);

    ESP_ERROR_CHECK(init_csi_controller());
    ESP_ERROR_CHECK(init_isp_pipeline());

#if CAM_STREAM_SENSOR_BEFORE_CSI
    ESP_ERROR_CHECK(start_ov2710_stream());
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    ESP_LOGI(TAG, "Starting CSI controller");
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam_ctlr));

#if !CAM_STREAM_SENSOR_BEFORE_CSI
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(start_ov2710_stream());
#endif

    while (1) {
        if (xSemaphoreTake(frame_ready_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            const uint8_t *src = (const uint8_t *)last_frame_buffer;
            if (!src) {
                continue;
            }

            if (finish_cnt <= 3 || (finish_cnt % CAM_FRAME_LOG_INTERVAL) == 0) {
                ESP_LOGI(TAG, "Frame %" PRIu32 ": size=%u bytes, first=%02x %02x %02x %02x %02x %02x %02x %02x, get=%" PRIu32,
                         finish_cnt,
                         (unsigned)last_frame_size,
                         src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                         get_trans_cnt);
            }

#if CAM_LCD_PREVIEW
            int64_t now_us = esp_timer_get_time();
            if (now_us >= next_lcd_frame_time_us) {
                next_lcd_frame_time_us += lcd_frame_interval_us;
                if (now_us > next_lcd_frame_time_us + lcd_frame_interval_us) {
                    next_lcd_frame_time_us = now_us + lcd_frame_interval_us;
                }
                lcd_preview_submit_frame(src, last_frame_size);
            }
#endif
            vTaskDelay(CAM_YIELD_TICKS);
        } else {
            ESP_LOGW(TAG, "No frame received: get=%" PRIu32 ", finish=%" PRIu32,
                     get_trans_cnt, finish_cnt);
        }
    }
}
