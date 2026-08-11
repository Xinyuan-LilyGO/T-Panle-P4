/*
 * LVGL example for ESP32-P4 T-Panel-P4.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "core/lv_refr.h"
#include "lcd_jd9365_driver.h"
#include "lcd_jd9365_touch.h"
#include "T_Panle_P4_board_config.h"


static const char *TAG = "lvgl_example";

#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_DELAY_MS 2
#define LVGL_BUFFER_LINES 64
#define ANIM_BOX_SIZE 56
#define ANIM_X_MIN 40
#define ANIM_X_MAX (LCD_H_RES - ANIM_BOX_SIZE - 40)
#define ANIM_LABEL_UPDATE_PERIOD_MS 200
#define MAX_DIRTY_AREAS 16

static lcd_driver_t s_lcd;
static touch_handle_t s_touch;
static SemaphoreHandle_t s_lvgl_lock;
static TaskHandle_t s_lvgl_task_handle;
static lv_obj_t *s_anim_ball;
static lv_obj_t *s_anim_bar;
static lv_obj_t *s_anim_label;
static lv_obj_t *s_touch_label;
static uint8_t *s_lcd_front_framebuffer;
static uint8_t *s_lcd_back_framebuffer;
static lv_area_t s_dirty_areas[MAX_DIRTY_AREAS];
static uint32_t s_dirty_area_count;
static bool s_dirty_full_screen;
static int32_t s_anim_x;
static touch_data_t s_last_touch_data;

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
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static size_t lcd_framebuffer_size(void)
{
    return (size_t)LCD_H_RES * LCD_V_RES * (LCD_BIT_PER_PIXEL / 8);
}

static void dirty_area_add(const lv_area_t *area)
{
    lv_area_t clipped = {
        .x1 = LV_MAX(area->x1, 0),
        .y1 = LV_MAX(area->y1, 0),
        .x2 = LV_MIN(area->x2, LCD_H_RES - 1),
        .y2 = LV_MIN(area->y2, LCD_V_RES - 1),
    };

    if (clipped.x1 > clipped.x2 || clipped.y1 > clipped.y2)
    {
        return;
    }

    if (s_dirty_full_screen)
    {
        return;
    }

    if (clipped.x1 == 0 && clipped.y1 == 0 && clipped.x2 == LCD_H_RES - 1 && clipped.y2 == LCD_V_RES - 1)
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
    const size_t pixel_bytes = LCD_BIT_PER_PIXEL / 8;
    const size_t stride = (size_t)LCD_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)lv_area_get_width(area) * pixel_bytes;
    uint8_t *dst_start = dst_fb + (((size_t)area->y1 * LCD_H_RES + area->x1) * pixel_bytes);
    const uint8_t *src_start = src_fb + (((size_t)area->y1 * LCD_H_RES + area->x1) * pixel_bytes);

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
    const size_t pixel_bytes = LCD_BIT_PER_PIXEL / 8;
    const size_t stride = (size_t)LCD_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)lv_area_get_width(area) * pixel_bytes;
    uint8_t *start = fb + (((size_t)area->y1 * LCD_H_RES + area->x1) * pixel_bytes);
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
            .x2 = LCD_H_RES - 1,
            .y2 = LCD_V_RES - 1,
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
    static bool logged = false;
    static bool logged_done = false;
    const int32_t width = lv_area_get_width(area);
    const int32_t height = lv_area_get_height(area);
    const size_t pixel_bytes = LCD_BIT_PER_PIXEL / 8;
    const size_t framebuffer_stride = (size_t)LCD_H_RES * pixel_bytes;
    const size_t row_bytes = (size_t)width * (LCD_BIT_PER_PIXEL / 8);
    uint8_t *dst_start = s_lcd_back_framebuffer + (((size_t)area->y1 * LCD_H_RES + area->x1) * pixel_bytes);

    if (!logged)
    {
        logged = true;
        ESP_LOGI(TAG, "LVGL first flush: area=(%ld,%ld)-(%ld,%ld), buffer=%p",
                 (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2, px_map);
    }

    for (int32_t y = 0; y < height; y++)
    {
        uint8_t *dst = dst_start + ((size_t)y * framebuffer_stride);
        const uint8_t *src = px_map + ((size_t)y * row_bytes);
        memcpy(dst, src, row_bytes);
    }

    dirty_area_add(area);
    esp_err_t ret = framebuffer_msync_area(s_lcd_back_framebuffer, area);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(ret));
    }
    else if (!logged_done)
    {
        logged_done = true;
        ESP_LOGI(TAG, "LVGL first flush done");
    }

    if (ret == ESP_OK && lv_display_flush_is_last(disp))
    {
        ret = lcd_jd9365_draw_bitmap(&s_lcd, 0, 0, LCD_H_RES, LCD_V_RES, s_lcd_back_framebuffer);
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
    touch_data_t touch_data = {0};

    if (touch_get_multiple_points(&s_touch, &touch_data) == ESP_OK && touch_data.finger_count > 0)
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

static esp_err_t lvgl_port_init(void)
{
    lv_init();

    const size_t draw_buf_size = (size_t)LCD_H_RES * LVGL_BUFFER_LINES * (LCD_BIT_PER_PIXEL / 8);
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

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
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

static void anim_set_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
    s_anim_x = value;
    if (s_anim_bar)
    {
        int32_t pct = ((s_anim_x - ANIM_X_MIN) * 100) / (ANIM_X_MAX - ANIM_X_MIN);
        pct = LV_CLAMP(0, pct, 100);
        lv_bar_set_value(s_anim_bar, pct, LV_ANIM_OFF);
    }
}

static void anim_label_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_anim_label)
    {
        lv_label_set_text_fmt(s_anim_label, "Animation x=%ld", (long)s_anim_x);
    }
}

static void touch_label_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_touch_label)
    {
        return;
    }

    char buf[256];
    int len = snprintf(buf, sizeof(buf), "Touch fingers: %u%s\n",
                       (unsigned)s_last_touch_data.finger_count,
                       s_last_touch_data.edge_touch ? " edge" : "");

    if (s_last_touch_data.finger_count == 0)
    {
        snprintf(buf + len, sizeof(buf) - len,
                 "P1: --\nP2: --\nP3: --\nP4: --\nP5: --");
    }
    else
    {
        uint8_t count = s_last_touch_data.finger_count;
        if (count > MAX_TOUCH_POINTS)
        {
            count = MAX_TOUCH_POINTS;
        }

        for (uint8_t i = 0; i < MAX_TOUCH_POINTS; i++)
        {
            if (i < count)
            {
                len += snprintf(buf + len, sizeof(buf) - len,
                                "P%u: x=%3u y=%3u p=%3u\n",
                                (unsigned)(i + 1),
                                (unsigned)s_last_touch_data.points[i].x,
                                (unsigned)s_last_touch_data.points[i].y,
                                (unsigned)s_last_touch_data.points[i].pressure);
            }
            else
            {
                len += snprintf(buf + len, sizeof(buf) - len,
                                "P%u: --\n", (unsigned)(i + 1));
            }

            if (len >= (int)sizeof(buf))
            {
                break;
            }
        }
    }

    lv_label_set_text(s_touch_label, buf);
}

static void create_demo_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "T-Panel-P4 LVGL Ready");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Lightweight partial-refresh animation test");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, 520);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 118);

    lv_obj_t *touch_panel = lv_obj_create(scr);
    lv_obj_clear_flag(touch_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(touch_panel, 260, 124);
    lv_obj_set_style_radius(touch_panel, 0, 0);
    lv_obj_set_style_bg_color(touch_panel, lv_color_hex(0x152238), 0);
    lv_obj_set_style_border_width(touch_panel, 0, 0);
    lv_obj_set_style_pad_all(touch_panel, 12, 0);
    lv_obj_align(touch_panel, LV_ALIGN_TOP_LEFT, 24, 156);

    s_touch_label = lv_label_create(touch_panel);
    lv_label_set_text(s_touch_label, "Touch fingers: 0\nP1: --\nP2: --\nP3: --\nP4: --\nP5: --");
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_touch_label, LV_FONT_DEFAULT, 0);
    lv_obj_align(s_touch_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *track = lv_obj_create(scr);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, 640, 72);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x2d3b53), 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, -38);

    s_anim_ball = lv_obj_create(scr);
    lv_obj_clear_flag(s_anim_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_anim_ball, ANIM_BOX_SIZE, ANIM_BOX_SIZE);
    lv_obj_set_style_radius(s_anim_ball, 0, 0);
    lv_obj_set_style_bg_color(s_anim_ball, lv_color_hex(0x1f6feb), 0);
    lv_obj_set_style_border_width(s_anim_ball, 0, 0);
    lv_obj_set_y(s_anim_ball, 296);

    s_anim_bar = lv_bar_create(scr);
    lv_obj_set_size(s_anim_bar, 420, 24);
    lv_obj_align(s_anim_bar, LV_ALIGN_BOTTOM_MID, 0, -92);
    lv_bar_set_range(s_anim_bar, 0, 100);
    lv_obj_set_style_radius(s_anim_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_anim_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_anim_bar, lv_color_hex(0x2d3b53), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_anim_bar, lv_color_hex(0x2ea043), LV_PART_INDICATOR);

    s_anim_label = lv_label_create(scr);
    lv_label_set_text(s_anim_label, "Animation x=0");
    lv_obj_set_style_text_color(s_anim_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(s_anim_label, LV_ALIGN_BOTTOM_MID, 0, -54);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_anim_ball);
    lv_anim_set_values(&anim, ANIM_X_MIN, ANIM_X_MAX);
    lv_anim_set_duration(&anim, 1400);
    lv_anim_set_reverse_duration(&anim, 1400);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, anim_set_x_cb);
    lv_anim_start(&anim);

    lv_timer_create(anim_label_timer_cb, ANIM_LABEL_UPDATE_PERIOD_MS, NULL);
    lv_timer_create(touch_label_timer_cb, 50, NULL);
}

void app_main(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(board_i2c_init(&i2c_bus));

    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));

    ESP_ERROR_CHECK(lcd_jd9365_init(&s_lcd, expander));
    ESP_ERROR_CHECK(lcd_backlight_init());
    ESP_ERROR_CHECK(lcd_backlight_set_brightness(30));
    ESP_ERROR_CHECK(touch_init(&s_touch, i2c_bus, expander));

    s_lcd_front_framebuffer = lcd_jd9365_get_next_frame_buffer(&s_lcd);
    ESP_ERROR_CHECK(s_lcd_front_framebuffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_lcd_back_framebuffer = (s_lcd_front_framebuffer == s_lcd.frame_buffers[0]) ?
                             s_lcd.frame_buffers[1] : s_lcd.frame_buffers[0];
    ESP_ERROR_CHECK(s_lcd_back_framebuffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    memset(s_lcd_front_framebuffer, 0, lcd_framebuffer_size());
    memset(s_lcd_back_framebuffer, 0, lcd_framebuffer_size());
    ESP_ERROR_CHECK(esp_cache_msync(s_lcd_front_framebuffer,
                                    lcd_framebuffer_size(),
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    ESP_ERROR_CHECK(esp_cache_msync(s_lcd_back_framebuffer,
                                    lcd_framebuffer_size(),
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    ESP_ERROR_CHECK(lcd_jd9365_draw_bitmap(&s_lcd, 0, 0, LCD_H_RES, LCD_V_RES, s_lcd_front_framebuffer));

    ESP_ERROR_CHECK(lvgl_port_init());

    xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    create_demo_ui();
    lv_refr_now(NULL);
    xSemaphoreGive(s_lvgl_lock);
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, &s_lvgl_task_handle);

    ESP_LOGI(TAG, "LVGL example started");
}
