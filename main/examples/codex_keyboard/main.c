/*
 * Codex Micro keyboard UI mock for ESP32-P4 T-Panel-P4.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#if CONFIG_T_PANEL_P4_HAS_XL9555
#include "esp_io_expander_xl9555.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "core/lv_refr.h"
#include "display_panel.h"
#include "touch_panel.h"
#include "board_config.h"

static const char *TAG = "codex_keyboard";

#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_DELAY_MS 2
#define LVGL_BUFFER_LINES 64
#define CODEX_UI_TASK_STACK_SIZE 16384
#define MAX_DIRTY_AREAS 16

#define KEY_W 112
#define KEY_H 112
#define KEY_GAP 28
#define KEY_START_X 54
#define KEY_START_Y 54

#define UI_BG 0xF4FFF9
#define UI_MINT 0x7EF1BA
#define UI_MINT_DARK 0x31D987
#define UI_PANEL 0xC8CFCC
#define UI_PANEL_DARK 0x929A96
#define UI_KEY 0xEEF3F1
#define UI_TEXT 0x202729
#define UI_MUTED 0x59615F
#define UI_DIM 0x6E7774
#define UI_BLACK 0x151A1A

static display_panel_t s_lcd;
static touch_panel_t s_touch;
static SemaphoreHandle_t s_lvgl_lock;
static TaskHandle_t s_lvgl_task_handle;
static uint8_t *s_lcd_front_framebuffer;
static uint8_t *s_lcd_back_framebuffer;
static lv_area_t s_dirty_areas[MAX_DIRTY_AREAS];
static uint32_t s_dirty_area_count;
static bool s_dirty_full_screen;
static touch_panel_data_t s_last_touch_data;

static lv_style_t s_style_key;
static lv_style_t center_border_style;
static lv_grad_dsc_t center_border_grad;
static const lv_color_t center_border_colors[2] = {
    LV_COLOR_MAKE(0xC8, 0xC8, 0xC8), // 左上灰色
    LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // 右下白色
};
static const uint8_t center_border_frac[2] = {
    0,
    40, // 越小，白色越早出现，灰色越少
};

static lv_style_t agent_glow_style;
static lv_grad_dsc_t agent_glow_grad;

static const lv_color_t agent_glow_colors[2] = {
    LV_COLOR_MAKE(0xD0, 0xFF, 0xEA),
    LV_COLOR_MAKE(0x31, 0xD9, 0x87),
};

static void create_codex_keyboard_ui(void);

static esp_err_t board_i2c_init(i2c_master_bus_handle_t *ret_i2c_bus)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, ret_i2c_bus), TAG, "I2C init failed");
    return ESP_OK;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static size_t lcd_framebuffer_size(void)
{
    return (size_t)DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * (DISPLAY_PANEL_BITS_PER_PIXEL / 8);
}

static void dirty_area_add(const lv_area_t *area)
{
    lv_area_t clipped = {
        .x1 = LV_MAX(area->x1, 0),
        .y1 = LV_MAX(area->y1, 0),
        .x2 = LV_MIN(area->x2, DISPLAY_PANEL_H_RES - 1),
        .y2 = LV_MIN(area->y2, DISPLAY_PANEL_V_RES - 1),
    };

    if (clipped.x1 > clipped.x2 || clipped.y1 > clipped.y2 || s_dirty_full_screen)
    {
        return;
    }

    if (clipped.x1 == 0 && clipped.y1 == 0 && clipped.x2 == DISPLAY_PANEL_H_RES - 1 && clipped.y2 == DISPLAY_PANEL_V_RES - 1)
    {
        s_dirty_full_screen = true;
        s_dirty_area_count = 0;
        return;
    }

    if (s_dirty_area_count < MAX_DIRTY_AREAS)
    {
        s_dirty_areas[s_dirty_area_count++] = clipped;
    }
    else
    {
        s_dirty_full_screen = true;
        s_dirty_area_count = 0;
    }
}

static esp_err_t framebuffer_copy_area(uint8_t *dst_fb, const uint8_t *src_fb, const lv_area_t *area)
{
    const size_t pixel_bytes = DISPLAY_PANEL_BITS_PER_PIXEL / 8;
    const size_t stride = (size_t)DISPLAY_PANEL_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)lv_area_get_width(area) * pixel_bytes;
    uint8_t *dst_start = dst_fb + (((size_t)area->y1 * DISPLAY_PANEL_H_RES + area->x1) * pixel_bytes);
    const uint8_t *src_start = src_fb + (((size_t)area->y1 * DISPLAY_PANEL_H_RES + area->x1) * pixel_bytes);

    for (int32_t y = area->y1; y <= area->y2; y++)
    {
        const size_t row_offset = (size_t)(y - area->y1) * stride;
        memcpy(dst_start + row_offset, src_start + row_offset, row_bytes);
    }

    const size_t sync_bytes = ((size_t)(lv_area_get_height(area) - 1) * stride) + row_bytes;
    return esp_cache_msync(dst_start, sync_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t framebuffer_msync_area(uint8_t *fb, const lv_area_t *area)
{
    const size_t pixel_bytes = DISPLAY_PANEL_BITS_PER_PIXEL / 8;
    const size_t stride = (size_t)DISPLAY_PANEL_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)lv_area_get_width(area) * pixel_bytes;
    uint8_t *start = fb + (((size_t)area->y1 * DISPLAY_PANEL_H_RES + area->x1) * pixel_bytes);
    const size_t sync_bytes = ((size_t)(lv_area_get_height(area) - 1) * stride) + row_bytes;

    return esp_cache_msync(start, sync_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static esp_err_t sync_dirty_areas_to_back_buffer(void)
{
    if (s_dirty_full_screen)
    {
        lv_area_t full = {
            .x1 = 0,
            .y1 = 0,
            .x2 = DISPLAY_PANEL_H_RES - 1,
            .y2 = DISPLAY_PANEL_V_RES - 1,
        };
        return framebuffer_copy_area(s_lcd_back_framebuffer, s_lcd_front_framebuffer, &full);
    }

    for (uint32_t i = 0; i < s_dirty_area_count; i++)
    {
        ESP_RETURN_ON_ERROR(framebuffer_copy_area(s_lcd_back_framebuffer, s_lcd_front_framebuffer, &s_dirty_areas[i]),
                            TAG, "sync dirty area failed");
    }

    return ESP_OK;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t width = lv_area_get_width(area);
    const int32_t height = lv_area_get_height(area);
    const size_t pixel_bytes = DISPLAY_PANEL_BITS_PER_PIXEL / 8;
    const size_t framebuffer_stride = (size_t)DISPLAY_PANEL_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)width * pixel_bytes;
    uint8_t *dst_start = s_lcd_back_framebuffer + (((size_t)area->y1 * DISPLAY_PANEL_H_RES + area->x1) * pixel_bytes);

    for (int32_t y = 0; y < height; y++)
    {
        uint8_t *dst = dst_start + ((size_t)y * framebuffer_stride);
        const uint8_t *src = px_map + ((size_t)y * row_bytes);
        memcpy(dst, src, row_bytes);
    }

    dirty_area_add(area);
    esp_err_t ret = framebuffer_msync_area(s_lcd_back_framebuffer, area);
    if (ret == ESP_OK && lv_display_flush_is_last(disp))
    {
        ret = display_panel_draw_bitmap(&s_lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                        DISPLAY_PANEL_V_RES, s_lcd_back_framebuffer);
        if (ret == ESP_OK)
        {
            uint8_t *old_front = s_lcd_front_framebuffer;
            s_lcd_front_framebuffer = s_lcd_back_framebuffer;
            s_lcd_back_framebuffer = old_front;
            ret = sync_dirty_areas_to_back_buffer();
        }

        s_dirty_area_count = 0;
        s_dirty_full_screen = false;
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD refresh failed: %s", esp_err_to_name(ret));
    }

    lv_display_flush_ready(disp);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    touch_panel_data_t touch_data = {0};
    if (touch_panel_get_multiple_points(&s_touch, &touch_data) == ESP_OK && touch_data.finger_count > 0)
    {
        s_last_touch_data = touch_data;
        data->point.x = touch_data.points[0].x;
        data->point.y = touch_data.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        memset(&s_last_touch_data, 0, sizeof(s_last_touch_data));
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (xSemaphoreTake(s_lvgl_lock, portMAX_DELAY) == pdTRUE)
        {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(LVGL_TASK_DELAY_MS));
    }
}

static void codex_ui_task(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    create_codex_keyboard_ui();
    lv_refr_now(NULL);
    xSemaphoreGive(s_lvgl_lock);

    ESP_LOGI(TAG, "Codex keyboard UI created");
    vTaskDelete(NULL);
}

static esp_err_t lvgl_port_init(void)
{
    lv_init();

    const size_t draw_buf_size = (size_t)DISPLAY_PANEL_H_RES * LVGL_BUFFER_LINES * (DISPLAY_PANEL_BITS_PER_PIXEL / 8);
    void *draw_buf_1 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *draw_buf_2 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *draw_buf_mem = "internal SRAM";

    if (!draw_buf_1 || !draw_buf_2)
    {
        if (draw_buf_1)
        {
            heap_caps_free(draw_buf_1);
        }
        if (draw_buf_2)
        {
            heap_caps_free(draw_buf_2);
        }
        draw_buf_1 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        draw_buf_2 = heap_caps_aligned_alloc(64, draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        draw_buf_mem = "PSRAM";
    }

    ESP_RETURN_ON_FALSE(draw_buf_1 && draw_buf_2, ESP_ERR_NO_MEM, TAG, "LVGL draw buffer alloc failed");
    ESP_LOGI(TAG, "LVGL draw buffers: %u bytes x2, %s", (unsigned)draw_buf_size, draw_buf_mem);

    lv_display_t *display = lv_display_create(DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_NO_MEM, TAG, "LVGL display create failed");
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
    lv_display_set_buffers(display, draw_buf_1, draw_buf_2, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display);

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_NO_MEM, TAG, "LVGL input device create failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create LVGL tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "start LVGL tick failed");

    s_lvgl_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_lock != NULL, ESP_ERR_NO_MEM, TAG, "LVGL lock create failed");

    return ESP_OK;
}

static void styles_init(void)
{
    lv_style_init(&s_style_key);
    lv_style_set_radius(&s_style_key, 20);
    lv_style_set_bg_color(&s_style_key, lv_color_hex(UI_KEY));
    lv_style_set_border_width(&s_style_key, 0);
    lv_style_set_border_color(&s_style_key, lv_color_hex(UI_PANEL_DARK));
    lv_style_set_shadow_width(&s_style_key, 14);
    lv_style_set_shadow_offset_y(&s_style_key, 8);
    lv_style_set_shadow_color(&s_style_key, lv_color_hex(0x7D8580));
    lv_style_set_shadow_opa(&s_style_key, 89);
    lv_style_set_pad_all(&s_style_key, 0);

    lv_style_init(&center_border_style);
    lv_grad_init_stops(&center_border_grad,
                       center_border_colors,
                       center_border_frac,
                       NULL,
                       sizeof(center_border_colors) / sizeof(lv_color_t));

    lv_grad_linear_init(&center_border_grad,
                        lv_pct(25),
                        lv_pct(0),
                        lv_pct(50),
                        lv_pct(100),
                        LV_GRAD_EXTEND_REFLECT);

    lv_style_set_bg_grad(&center_border_style, &center_border_grad);
    lv_style_set_bg_opa(&center_border_style, LV_OPA_60);

    lv_style_init(&agent_glow_style);

    lv_grad_init_stops(&agent_glow_grad,
                       agent_glow_colors,
                       NULL,
                       NULL,
                       2);

    lv_grad_radial_init(&agent_glow_grad,
                        LV_GRAD_CENTER,
                        LV_GRAD_CENTER,
                        LV_GRAD_RIGHT,
                        LV_GRAD_BOTTOM,
                        LV_GRAD_EXTEND_PAD);

    lv_style_set_bg_grad(&agent_glow_style, &agent_glow_grad);
    lv_style_set_bg_opa(&agent_glow_style, LV_OPA_COVER);
}

static lv_obj_t *create_cont(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), 140);
    lv_obj_set_pos(cont, x, y);
    lv_obj_set_size(cont, width, height);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(UI_MINT_DARK), 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_60, 0);
    return cont;
}

static lv_obj_t *create_agent_key(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *key = lv_button_create(parent);
    lv_obj_set_size(key, width, height);
    lv_obj_set_pos(key, x, y);
    lv_obj_add_style(key, &s_style_key, 0);

    lv_obj_t *center_border = lv_obj_create(key);
    lv_obj_set_size(center_border, 92, 92);
    lv_obj_center(center_border);
    lv_obj_set_style_radius(center_border, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(center_border, 0, 0);
    lv_obj_add_style(center_border, &center_border_style, 0);
    lv_obj_remove_flag(center_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(center_border, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *agent_label = lv_label_create(key);
    lv_obj_center(agent_label);
    lv_obj_set_style_text_color(agent_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(agent_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(agent_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(agent_label, LV_OPA_30, 0);
    lv_label_set_text(agent_label, "Agent");
    return key;
}

static lv_obj_t *create_key(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *key = lv_button_create(parent);
    lv_obj_set_size(key, width, height);
    lv_obj_set_pos(key, x, y);
    lv_obj_set_style_bg_color(key, lv_color_hex(0xf5f5f5), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(key, 0, 0);
    lv_obj_set_style_radius(key, 20, 0);
    lv_obj_set_scrollbar_mode(key, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_shadow_width(key, 14, 0);
    lv_obj_set_style_shadow_offset_y(key, 8, 0);
    lv_obj_set_style_shadow_color(key, lv_color_hex(0x7D8580), 0);
    lv_obj_set_style_shadow_opa(key, 89, 0);

    lv_obj_t *center_border = lv_obj_create(key);
    lv_obj_set_size(center_border, 92, 92);
    lv_obj_set_style_border_width(center_border, 1, 0);
    lv_obj_center(center_border);
    lv_obj_set_style_radius(center_border, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(center_border, 0, 0);
    lv_obj_add_style(center_border, &center_border_style, 0);
    lv_obj_remove_flag(center_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(center_border, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *center_circle = lv_obj_create(key);
    lv_obj_set_size(center_circle, 68, 68);
    lv_obj_center(center_circle);
    lv_obj_set_style_radius(center_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_circle, lv_color_hex(0xDDE5E1), 0);
    lv_obj_set_style_bg_opa(center_circle, LV_OPA_30, 0);
    lv_obj_set_style_border_width(center_circle, 0, 0);
    lv_obj_set_style_pad_all(center_circle, 0, 0);
    lv_obj_remove_flag(center_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(center_circle, LV_OBJ_FLAG_CLICKABLE);

    return key;
}

static lv_obj_t *create_black_circle(lv_obj_t *parent)
{
    lv_obj_t *circle_1 = lv_obj_create(parent);
    lv_obj_set_size(circle_1, 40, 40);
    lv_obj_set_style_radius(circle_1, 20, 0);
    lv_obj_set_style_bg_color(circle_1, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_opa(circle_1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle_1, 0, 0);
    lv_obj_set_scrollbar_mode(circle_1, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *circle_2 = lv_obj_create(circle_1);
    lv_obj_set_size(circle_2, 24, 24);
    lv_obj_set_style_radius(circle_2, 12, 0);
    lv_obj_set_style_bg_color(circle_2, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(circle_2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle_2, 0, 0);
    lv_obj_align(circle_2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scrollbar_mode(circle_2, LV_SCROLLBAR_MODE_OFF);

    return circle_1;
}

static lv_obj_t *create_recessed_hole(lv_obj_t *parent, int x, int y, int size)
{
    // 外层凹槽
    lv_obj_t *outer = lv_obj_create(parent);
    lv_obj_set_pos(outer, x, y);
    lv_obj_set_size(outer, size, size);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0xDDE3E0), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(outer, 1, 0);
    lv_obj_set_style_border_color(outer, lv_color_hex(0xAEB7B3), 0);
    lv_obj_set_style_border_opa(outer, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(outer, 0, 0);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);

    // 右下阴影，让外圈像压下去
    lv_obj_set_style_shadow_width(outer, 10, 0);
    lv_obj_set_style_shadow_offset_y(outer, 3, 0);
    lv_obj_set_style_shadow_color(outer, lv_color_hex(0x8A948F), 0);
    lv_obj_set_style_shadow_opa(outer, LV_OPA_30, 0);

    // 内层浅色高光圈
    lv_obj_t *inner_ring = lv_obj_create(outer);
    lv_obj_set_size(inner_ring, size - 12, size - 12);
    lv_obj_center(inner_ring);
    lv_obj_set_style_radius(inner_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_ring, lv_color_hex(0xEEF4F1), 0);
    lv_obj_set_style_bg_opa(inner_ring, LV_OPA_70, 0);
    lv_obj_set_style_border_width(inner_ring, 1, 0);
    lv_obj_set_style_border_color(inner_ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(inner_ring, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(inner_ring, 0, 0);
    lv_obj_clear_flag(inner_ring, LV_OBJ_FLAG_SCROLLABLE);

    // 黑色圆孔
    lv_obj_t *hole = lv_obj_create(outer);
    lv_obj_set_size(hole, size - 30, size - 30);
    lv_obj_center(hole);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, lv_color_hex(0x020303), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hole, 0, 0);
    lv_obj_set_style_pad_all(hole, 0, 0);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);

    return outer;
}

static lv_obj_t *create_led(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color)
{
    lv_obj_t *agent_led = lv_obj_create(parent);
    lv_obj_set_size(agent_led, width, height);
    lv_obj_set_pos(agent_led, x, y);
    lv_obj_set_style_bg_color(agent_led, color, 0);
    lv_obj_set_style_bg_opa(agent_led, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(agent_led, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(agent_led, 0, 0);
    lv_obj_set_style_border_width(agent_led, 0, 0);
    lv_obj_set_style_shadow_width(agent_led, 0, 0);

    lv_obj_set_style_radius(agent_led, height / 2, 0);
    lv_obj_set_style_shadow_width(agent_led, 8, 0);
    lv_obj_set_style_shadow_color(agent_led, color, 0);
    lv_obj_set_style_shadow_opa(agent_led, LV_OPA_50, 0);

    return agent_led;
}

static lv_obj_t *create_key_icon(lv_obj_t *parent, const char *icon, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static void create_codex_keyboard_ui(void)
{
    styles_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *main_cont = lv_obj_create(scr);
    lv_obj_set_size(main_cont, 640, 640);
    lv_obj_align(main_cont, LV_ALIGN_TOP_LEFT, 40, 40);
    lv_obj_set_style_radius(main_cont, 36, 0);
    lv_obj_set_style_border_width(main_cont, 0, 0);
    lv_obj_set_style_border_color(main_cont, lv_color_hex(UI_PANEL_DARK), 0);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_set_scrollbar_mode(main_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *circle_1 = create_black_circle(main_cont);
    lv_obj_align(circle_1, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_t *circle_2 = create_black_circle(main_cont);
    lv_obj_align(circle_2, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_t *circle_3 = create_black_circle(main_cont);
    lv_obj_align(circle_3, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_t *circle_4 = create_black_circle(main_cont);
    lv_obj_align(circle_4, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

    // row0
    lv_obj_t *white_circle = lv_obj_create(main_cont);
    lv_obj_set_pos(white_circle, KEY_START_X, KEY_START_Y);
    lv_obj_set_size(white_circle, KEY_W, KEY_W);
    lv_obj_set_style_radius(white_circle, KEY_W / 2, 0);
    lv_obj_set_style_bg_color(white_circle, lv_color_hex(UI_KEY), 0);
    lv_obj_set_style_bg_opa(white_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(white_circle, 0, 0);
    lv_obj_set_scrollbar_mode(white_circle, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_shadow_width(white_circle, 18, 0);
    lv_obj_set_style_shadow_offset_y(white_circle, 10, 0);
    lv_obj_set_style_shadow_color(white_circle, lv_color_hex(0x6F7874), 0);
    lv_obj_set_style_shadow_opa(white_circle, LV_OPA_40, 0);

    lv_obj_t *inner = lv_obj_create(white_circle);
    lv_obj_set_size(inner, KEY_W - 20, KEY_W - 20);
    lv_obj_center(inner);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner, lv_color_hex(0xF8FCFA), 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_50, 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(inner, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mark = lv_obj_create(white_circle);
    lv_obj_set_size(mark, KEY_W - 24, 20);
    lv_obj_center(mark);
    lv_obj_set_style_radius(mark, 15, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(0xA8B1AD), 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_shadow_width(mark, 8, 0);
    lv_obj_set_style_shadow_color(mark, lv_color_hex(0x707A75), 0);
    lv_obj_set_style_shadow_opa(mark, LV_OPA_30, 0);
    lv_obj_set_style_transform_pivot_x(mark, 44, 0);
    lv_obj_set_style_transform_pivot_y(mark, 0, 0);
    lv_obj_set_style_transform_rotation(mark, -450, 0);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *black_circle_cont = lv_obj_create(main_cont);
    lv_obj_set_pos(black_circle_cont, 474, KEY_START_Y);
    lv_obj_set_size(black_circle_cont, KEY_W, KEY_W);
    lv_obj_set_style_radius(black_circle_cont, 20, 0);
    lv_obj_set_style_bg_color(black_circle_cont, lv_color_hex(UI_KEY), 0);
    lv_obj_set_style_bg_opa(black_circle_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(black_circle_cont, 0, 0);
    lv_obj_set_scrollbar_mode(black_circle_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_shadow_width(black_circle_cont, 18, 0);
    lv_obj_set_style_shadow_offset_y(black_circle_cont, 10, 0);
    lv_obj_set_style_shadow_color(black_circle_cont, lv_color_hex(0x6F7874), 0);
    lv_obj_set_style_shadow_opa(black_circle_cont, LV_OPA_40, 0);

    lv_obj_t *black_circle = create_black_circle(black_circle_cont);
    lv_obj_align(black_circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(black_circle, 100, 100);
    lv_obj_set_style_radius(black_circle, 50, 0);
    lv_obj_set_style_bg_color(black_circle, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_bg_opa(black_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(black_circle, 0, 0);
    lv_obj_set_scrollbar_mode(black_circle, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *inner_1 = lv_obj_create(black_circle);
    lv_obj_set_size(inner_1, 100 - 20, 100 - 20);
    lv_obj_center(inner_1);
    lv_obj_set_style_radius(inner_1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_1, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_bg_opa(inner_1, LV_OPA_50, 0);
    lv_obj_set_style_border_width(inner_1, 0, 0);
    lv_obj_set_style_border_opa(inner_1, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(inner_1, 0, 0);
    lv_obj_remove_flag(inner_1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *line = lv_obj_create(black_circle);
    lv_obj_set_size(line, 80, 4);
    lv_obj_center(line);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x333A38), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_transform_pivot_x(line, 40, 0);
    lv_obj_set_style_transform_pivot_y(line, 0, 0);
    lv_obj_set_style_transform_rotation(line, 450, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *line_2 = lv_obj_create(black_circle);
    lv_obj_set_size(line_2, 80, 4);
    lv_obj_center(line_2);
    lv_obj_set_style_radius(line_2, 2, 0);
    lv_obj_set_style_bg_color(line_2, lv_color_hex(0x333A38), 0);
    lv_obj_set_style_border_width(line_2, 0, 0);
    lv_obj_set_style_transform_pivot_x(line_2, 40, 0);
    lv_obj_set_style_transform_pivot_y(line_2, 0, 0);
    lv_obj_set_style_transform_rotation(line_2, -450, 0);
    lv_obj_remove_flag(line_2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *agent_event_1 = create_agent_key(main_cont, 194, KEY_START_Y, KEY_W, KEY_H);
    lv_obj_add_style(agent_event_1, &agent_glow_style, 0);
    lv_obj_set_style_shadow_width(agent_event_1, 26, 0);
    lv_obj_set_style_shadow_spread(agent_event_1, 4, 0);
    lv_obj_set_style_shadow_color(agent_event_1, lv_color_hex(UI_MINT), 0);
    lv_obj_set_style_shadow_opa(agent_event_1, LV_OPA_60, 0);

    lv_obj_t *agent_event_2 = create_agent_key(main_cont, 334, KEY_START_Y, KEY_W, KEY_H);

    // row1
    lv_obj_t *agent_event_4 = create_agent_key(main_cont, 54, 194, KEY_W, KEY_H);
    lv_obj_t *agent_event_5 = create_agent_key(main_cont, 194, 194, KEY_W, KEY_H);
    lv_obj_t *agent_event_6 = create_agent_key(main_cont, 334, 194, KEY_W, KEY_H);
    lv_obj_t *agent_event_7 = create_agent_key(main_cont, 474, 194, KEY_W, KEY_H);

    // row2
    lv_obj_t *agent_flash = create_key(main_cont, 54, 334, KEY_W, KEY_H);
    create_key_icon(agent_flash, LV_SYMBOL_CHARGE, &lv_font_montserrat_36);
    lv_obj_t *agent_true = create_key(main_cont, 194, 334, KEY_W, KEY_H);
    create_key_icon(agent_true, LV_SYMBOL_OK, &lv_font_montserrat_36);
    lv_obj_t *agent_false = create_key(main_cont, 334, 334, KEY_W, KEY_H);
    create_key_icon(agent_false, LV_SYMBOL_CLOSE, &lv_font_montserrat_36);
    lv_obj_t *agent_choose = create_key(main_cont, 474, 334, KEY_W, KEY_H);
    create_key_icon(agent_choose, LV_SYMBOL_UPLOAD, &lv_font_montserrat_36);

    // row3
    create_recessed_hole(main_cont, 70, 500, 76);
    lv_obj_t *agent_event_voide = create_key(main_cont, 194, 474, KEY_W * 2 + KEY_GAP, KEY_H);
    create_key_icon(agent_event_voide, LV_SYMBOL_AUDIO, &lv_font_montserrat_32);
    lv_obj_t *agent_codex_wakeup = create_key(main_cont, 474, 474, KEY_W, KEY_H);
    create_key_icon(agent_codex_wakeup, LV_SYMBOL_SETTINGS, &lv_font_montserrat_36);

    create_led(main_cont, 20, 515, 28, 8, lv_color_hex(0x7EF1BA));
    create_led(main_cont, 20, 535, 28, 8, lv_color_hex(0x0069ed));
    create_led(main_cont, 20, 555, 28, 8, lv_color_hex(0xe20059));

    lv_obj_t *right = lv_label_create(main_cont);
    lv_label_set_text(right, "You can just build things");
    lv_obj_set_style_text_font(right, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(right, lv_color_hex(0x59615F), 0);
    lv_obj_set_style_transform_pivot_x(right, 0, 0);
    lv_obj_set_style_transform_pivot_y(right, 0, 0);
    lv_obj_set_style_transform_rotation(right, 900, 0);
    lv_obj_set_pos(right, 620, 180);

    lv_obj_t *left = lv_label_create(main_cont);
    lv_label_set_text(left, "Work Louder | OpenAI 2026");
    lv_obj_set_style_text_font(left, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(left, lv_color_hex(0x59615F), 0);
    lv_obj_set_style_transform_pivot_x(left, 0, 0);
    lv_obj_set_style_transform_pivot_y(left, 0, 0);
    lv_obj_set_style_transform_rotation(left, -900, 0);
    lv_obj_set_style_text_opa(right, LV_OPA_70, 0);
    lv_obj_set_style_text_opa(left, LV_OPA_70, 0);
    lv_obj_set_pos(left, 20, 420);

    lv_obj_t *bottom = lv_label_create(main_cont);
    lv_label_set_text(bottom, "Let's build");
    lv_obj_set_style_text_font(bottom, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bottom, lv_color_hex(0x59615F), 0);
    lv_obj_set_style_text_opa(bottom, LV_OPA_60, 0);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void app_main(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(board_i2c_init(&i2c_bus));

    esp_io_expander_handle_t expander = NULL;
#if CONFIG_T_PANEL_P4_HAS_XL9555
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));
#endif

    ESP_ERROR_CHECK(display_panel_init(&s_lcd, expander));
    ESP_ERROR_CHECK(display_panel_backlight_init());
    ESP_ERROR_CHECK(display_panel_set_brightness(45));
    ESP_ERROR_CHECK(touch_panel_init(&s_touch, i2c_bus, expander));

    s_lcd_front_framebuffer = display_panel_get_next_frame_buffer(&s_lcd);
    ESP_ERROR_CHECK(s_lcd_front_framebuffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_lcd_back_framebuffer = (s_lcd_front_framebuffer == s_lcd.frame_buffers[0]) ? s_lcd.frame_buffers[1] : s_lcd.frame_buffers[0];
    ESP_ERROR_CHECK(s_lcd_back_framebuffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    memset(s_lcd_front_framebuffer, 0, lcd_framebuffer_size());
    memset(s_lcd_back_framebuffer, 0, lcd_framebuffer_size());
    ESP_ERROR_CHECK(esp_cache_msync(s_lcd_front_framebuffer,
                                    lcd_framebuffer_size(),
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    ESP_ERROR_CHECK(esp_cache_msync(s_lcd_back_framebuffer,
                                    lcd_framebuffer_size(),
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    ESP_ERROR_CHECK(display_panel_draw_bitmap(&s_lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                              DISPLAY_PANEL_V_RES, s_lcd_front_framebuffer));

    ESP_ERROR_CHECK(lvgl_port_init());

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, &s_lvgl_task_handle);
    xTaskCreate(codex_ui_task, "codex_ui", CODEX_UI_TASK_STACK_SIZE, NULL, 4, NULL);
    ESP_LOGI(TAG, "Codex keyboard UI started");
}
