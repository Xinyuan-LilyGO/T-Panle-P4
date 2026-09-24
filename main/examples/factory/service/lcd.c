#include "lcd.h"

#include "esp_check.h"
#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 1
#include "esp_heap_caps.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "system.h"
#include "touch_panel.h"

#define LVGL_TICK_PERIOD_MS 2
#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 1
#define LVGL_DRAW_BUFFER_LINES (DISPLAY_PANEL_V_RES / 10)
#define LVGL_DRAW_BUFFER_ALIGNMENT 64
#endif
#define LVGL_REFRESH_TIMEOUT_MS 100

static const char *TAG = "factory_lcd";
static display_panel_t s_lcd;
static touch_panel_t s_touch;
static touch_panel_data_t s_touch_data;
#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 1
static volatile bool s_lvgl_flush_pending;
#endif

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_panel_data_t touch_data = {0};
    esp_err_t ret = touch_panel_get_multiple_points(&s_touch, &touch_data);
    s_touch_data = touch_data;

    if (ret == ESP_OK && touch_data.finger_count > 0)
    {
        data->point.x = touch_data.points[0].x;
        data->point.y = touch_data.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    display_panel_t *panel = lv_display_get_user_data(disp);

#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 2
    (void)area;
    if (!lv_display_flush_is_last(disp))
    {
        lv_display_flush_ready(disp);
        return;
    }

    esp_err_t ret = display_panel_draw_bitmap(panel,
                                               0, 0,
                                               DISPLAY_PANEL_H_RES,
                                               DISPLAY_PANEL_V_RES,
                                               px_map);
    if (ret == ESP_OK)
    {
        ret = display_panel_wait_refresh_done(panel, LVGL_REFRESH_TIMEOUT_MS);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LVGL frame present failed: %s", esp_err_to_name(ret));
    }
    else
    {
        system_lcd_flush_record();
    }
    lv_display_flush_ready(disp);
#else
    s_lvgl_flush_pending = true;

    esp_err_t ret = display_panel_draw_bitmap_async(panel,
                                                    area->x1, area->y1,
                                                    area->x2 + 1, area->y2 + 1,
                                                    px_map);
    if (ret != ESP_OK)
    {
        s_lvgl_flush_pending = false;
        ESP_LOGE(TAG, "LVGL flush failed: %s", esp_err_to_name(ret));
        lv_display_flush_ready(disp);
        return;
    }

    if (lv_display_flush_is_last(disp))
    {
        system_lcd_flush_record();
    }
#endif
}

#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 1
static bool lvgl_flush_ready_cb(void *user_ctx)
{
    lv_display_t *display = (lv_display_t *)user_ctx;
    if (!s_lvgl_flush_pending || display == NULL)
    {
        return false;
    }

    s_lvgl_flush_pending = false;
    lv_display_flush_ready(display);
    return false;
}
#endif

static esp_err_t lvgl_port_init(void)
{
    lv_init();

    lv_display_t *display = lv_display_create(DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_NO_MEM, TAG, "LVGL display create failed");
#if CONFIG_DISPLAY_PANEL_RGB565
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
#elif CONFIG_DISPLAY_PANEL_RGB888
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
#endif
    lv_display_set_user_data(display, &s_lcd);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

#if CONFIG_DISPLAY_PANEL_FRAME_BUFFER_COUNT == 2
    ESP_RETURN_ON_FALSE(s_lcd.frame_buffer_count == 2 &&
                            s_lcd.frame_buffers[0] != NULL &&
                            s_lcd.frame_buffers[1] != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "two complete DPI frame buffers are required");

    const size_t frame_buffer_size = (size_t)DISPLAY_PANEL_H_RES *
                                     DISPLAY_PANEL_V_RES *
                                     (DISPLAY_PANEL_BITS_PER_PIXEL / 8U);
    void *back_frame_buffer = display_panel_get_next_frame_buffer(&s_lcd);
    void *front_frame_buffer = back_frame_buffer == s_lcd.frame_buffers[0]
                                   ? s_lcd.frame_buffers[1]
                                   : s_lcd.frame_buffers[0];
    lv_display_set_buffers(display,
                           back_frame_buffer,
                           front_frame_buffer,
                           frame_buffer_size,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    ESP_LOGI(TAG, "LVGL direct render buffers: %u bytes x2 DPI frame buffers",
             (unsigned)frame_buffer_size);
#else
    const size_t draw_buffer_size = (size_t)DISPLAY_PANEL_H_RES *
                                    LVGL_DRAW_BUFFER_LINES *
                                    (DISPLAY_PANEL_BITS_PER_PIXEL / 8U);
    const uint32_t draw_buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    void *draw_buffer_1 = heap_caps_aligned_calloc(LVGL_DRAW_BUFFER_ALIGNMENT,
                                                   1,
                                                   draw_buffer_size,
                                                   draw_buffer_caps);
    void *draw_buffer_2 = heap_caps_aligned_calloc(LVGL_DRAW_BUFFER_ALIGNMENT,
                                                   1,
                                                   draw_buffer_size,
                                                   draw_buffer_caps);
    if (draw_buffer_1 == NULL || draw_buffer_2 == NULL)
    {
        if (draw_buffer_1 != NULL)
        {
            heap_caps_free(draw_buffer_1);
        }
        if (draw_buffer_2 != NULL)
        {
            heap_caps_free(draw_buffer_2);
        }
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(display,
                           draw_buffer_1,
                           draw_buffer_2,
                           draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    display_panel_set_color_trans_done_cb(&s_lcd, lvgl_flush_ready_cb, display);
    ESP_LOGI(TAG, "LVGL partial render buffers: %u lines, %u bytes x2 in PSRAM",
             (unsigned)LVGL_DRAW_BUFFER_LINES,
             (unsigned)draw_buffer_size);
#endif
    lv_display_set_default(display);

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_ERR_NO_MEM, TAG, "LVGL input device create failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    lv_indev_set_gesture_min_distance(indev, 160);
    lv_indev_set_gesture_min_velocity(indev, 10);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG,
                        "create LVGL tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer,
                                                 LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "start LVGL tick failed");
    return ESP_OK;
}

esp_err_t lcd_init(i2c_master_bus_handle_t i2c_bus,
                   esp_io_expander_handle_t io_expander)
{
    ESP_RETURN_ON_ERROR(display_panel_init(&s_lcd, io_expander), TAG, "display init failed");
    ESP_RETURN_ON_ERROR(display_panel_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(display_panel_set_brightness(80), TAG, "set brightness failed");
    ESP_RETURN_ON_ERROR(touch_panel_init(&s_touch, i2c_bus, io_expander), TAG,
                        "touch init failed");
    return lvgl_port_init();
}

display_panel_t *lcd_get_display(void)
{
    return &s_lcd;
}

bool lcd_touch_data_get(touch_panel_data_t *data)
{
    if (data == NULL)
    {
        return false;
    }

    *data = s_touch_data;
    return true;
}
