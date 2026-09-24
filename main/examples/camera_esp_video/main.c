/* ESP32-P4 + OV2710 MIPI CSI preview using the camera_ov2710 component. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_ov2710.h"
#include "driver/ppa.h"
#include "display_panel.h"

static const char *TAG = "camera_esp_video";

#define CAM_DISPLAY_FPS 25
#define CAM_DISPLAY_FRAME_INTERVAL_US (1000000 / CAM_DISPLAY_FPS)
#define CAM_PREVIEW_BYTES_PER_PIXEL 2
#define CAM_LCD_BYTES_PER_PIXEL (DISPLAY_PANEL_BITS_PER_PIXEL / 8)
#define CAM_PPA_SCALE_FRACTION_STEPS 16U
#define CAM_PPA_SCALE_MAX_STEPS ((255U * CAM_PPA_SCALE_FRACTION_STEPS) + 15U)
#if DISPLAY_PANEL_BITS_PER_PIXEL == 16
#define CAM_PPA_LCD_COLOR_MODE PPA_SRM_COLOR_MODE_RGB565
#else
#define CAM_PPA_LCD_COLOR_MODE PPA_SRM_COLOR_MODE_RGB888
#endif
#define CAM_LCD_BOTTOM_GUARD_LINES 2
static t_panel_p4_bsp_t bsp = {};
static display_panel_t lcd = {};
static camera_ov2710_handle_t camera = NULL;
static ppa_client_handle_t ppa_srm = NULL;
static bool ppa_disabled = false;
static bool ppa_disable_logged = false;
static bool ppa_geometry_logged = false;

static esp_err_t init_lcd(void)
{
    ESP_RETURN_ON_ERROR(display_panel_init(
                            &lcd, t_panel_p4_bsp_get_io_expander(&bsp)),
                        TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(display_panel_backlight_init(), TAG, "LCD backlight init failed");
    ESP_RETURN_ON_ERROR(display_panel_set_brightness(60), TAG, "LCD backlight set failed");
    ESP_LOGI(TAG, "LCD ready: %ux%u, %ubpp", DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES,
             DISPLAY_PANEL_BITS_PER_PIXEL);

    return ESP_OK;
}

static esp_err_t show_lcd_test_pattern(void)
{
    uint8_t *lcd_fb = display_panel_get_next_frame_buffer(&lcd);
    ESP_RETURN_ON_FALSE(lcd_fb, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    const uint32_t stripe_h = DISPLAY_PANEL_V_RES / 3;
    uint8_t *p = lcd_fb;
    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++)
    {
        uint8_t b = 0xff;
        uint8_t g = 0;
        uint8_t r = 0;

        if (y < stripe_h)
        {
            b = 0;
            r = 0xff;
        }
        else if (y < stripe_h * 2)
        {
            b = 0;
            g = 0xff;
        }

        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++)
        {
#if DISPLAY_PANEL_BITS_PER_PIXEL == 16
            uint16_t rgb565 = (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                                         ((uint16_t)(g & 0xfc) << 3) |
                                         ((uint16_t)b >> 3));
            *p++ = (uint8_t)rgb565;
            *p++ = (uint8_t)(rgb565 >> 8);
#else
            *p++ = r;
            *p++ = g;
            *p++ = b;
#endif
        }
    }

    ESP_RETURN_ON_ERROR(esp_cache_msync(lcd_fb,
                                        (size_t)DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * CAM_LCD_BYTES_PER_PIXEL,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                        TAG, "LCD test pattern cache sync failed");
    ESP_RETURN_ON_ERROR(display_panel_draw_bitmap(&lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                                  DISPLAY_PANEL_V_RES, lcd_fb),
                        TAG, "LCD test pattern draw failed");
    ESP_LOGI(TAG, "LCD test pattern displayed");
    vTaskDelay(pdMS_TO_TICKS(500));

    return ESP_OK;
}

static inline void rgb565_to_lcd(const uint8_t *src, uint8_t *dst)
{
#if DISPLAY_PANEL_BITS_PER_PIXEL == 16
    dst[0] = src[0];
    dst[1] = src[1];
#else
    uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
    uint8_t r = (uint8_t)((rgb565 >> 11) & 0x1f);
    uint8_t g = (uint8_t)((rgb565 >> 5) & 0x3f);
    uint8_t b = (uint8_t)(rgb565 & 0x1f);

    dst[0] = (uint8_t)((r << 3) | (r >> 2));
    dst[1] = (uint8_t)((g << 2) | (g >> 4));
    dst[2] = (uint8_t)((b << 3) | (b >> 2));
#endif
}

static void copy_video_rgb565_to_lcd(const uint8_t *src, uint8_t *dst, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++)
    {
        rgb565_to_lcd(src, dst);
        src += CAM_PREVIEW_BYTES_PER_PIXEL;
        dst += CAM_LCD_BYTES_PER_PIXEL;
    }
}

static void crop_center_rgb565_to_lcd(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint8_t *dst)
{
    const uint32_t crop_x = (src_w - DISPLAY_PANEL_H_RES) / 2;
    const uint32_t crop_y = (src_h - DISPLAY_PANEL_V_RES) / 2;

    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++)
    {
        const uint8_t *src_row = src + (((size_t)(crop_y + y) * src_w + crop_x) * CAM_PREVIEW_BYTES_PER_PIXEL);
        uint8_t *dst_row = dst + ((size_t)y * DISPLAY_PANEL_H_RES * CAM_LCD_BYTES_PER_PIXEL);
        copy_video_rgb565_to_lcd(src_row, dst_row, DISPLAY_PANEL_H_RES);
    }
}

static esp_err_t init_ppa_srm(void)
{
    if (ppa_disabled)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ppa_srm != NULL)
    {
        return ESP_OK;
    }

    ppa_client_config_t config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_8,
    };
    esp_err_t ret = ppa_register_client(&config, &ppa_srm);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "PPA SRM enabled, burst=8, display=%d FPS", CAM_DISPLAY_FPS);
    }
    return ret;
}

static bool calculate_ppa_fill_geometry(uint32_t src_w, uint32_t src_h,
                                        uint32_t *crop_w, uint32_t *crop_h,
                                        float *scale)
{
    const uint32_t dst_w_scaled =
        DISPLAY_PANEL_H_RES * CAM_PPA_SCALE_FRACTION_STEPS;
    const uint32_t dst_h_scaled =
        DISPLAY_PANEL_V_RES * CAM_PPA_SCALE_FRACTION_STEPS;
    uint32_t min_steps_x = (dst_w_scaled + src_w - 1) / src_w;
    uint32_t min_steps_y = (dst_h_scaled + src_h - 1) / src_h;
    uint32_t min_steps = min_steps_x > min_steps_y ? min_steps_x : min_steps_y;
    if (min_steps == 0) {
        min_steps = 1;
    }

    for (uint32_t steps = min_steps;
         steps <= CAM_PPA_SCALE_MAX_STEPS;
         ++steps) {
        if ((dst_w_scaled % steps) != 0 || (dst_h_scaled % steps) != 0) {
            continue;
        }

        uint32_t candidate_w = dst_w_scaled / steps;
        uint32_t candidate_h = dst_h_scaled / steps;
        if (candidate_w <= src_w && candidate_h <= src_h) {
            *crop_w = candidate_w;
            *crop_h = candidate_h;
            *scale = (float)steps / (float)CAM_PPA_SCALE_FRACTION_STEPS;
            return true;
        }
    }
    return false;
}

static void scale_center_rgb565_to_lcd(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint8_t *dst)
{
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = src_w;
    uint32_t crop_h = src_h;

    if ((uint64_t)src_w * DISPLAY_PANEL_V_RES > (uint64_t)src_h * DISPLAY_PANEL_H_RES)
    {
        crop_w = (uint32_t)(((uint64_t)src_h * DISPLAY_PANEL_H_RES) / DISPLAY_PANEL_V_RES);
        crop_x = (src_w - crop_w) / 2;
    }
    else if ((uint64_t)src_w * DISPLAY_PANEL_V_RES < (uint64_t)src_h * DISPLAY_PANEL_H_RES)
    {
        crop_h = (uint32_t)(((uint64_t)src_w * DISPLAY_PANEL_V_RES) / DISPLAY_PANEL_H_RES);
        crop_y = (src_h - crop_h) / 2;
    }

    uint32_t ppa_crop_w;
    uint32_t ppa_crop_h;
    float ppa_scale;
    bool ppa_geometry_valid = calculate_ppa_fill_geometry(
        src_w, src_h, &ppa_crop_w, &ppa_crop_h, &ppa_scale);
    esp_err_t ppa_ret =
        ppa_geometry_valid ? init_ppa_srm() : ESP_ERR_NOT_SUPPORTED;
    if (ppa_ret == ESP_OK)
    {
        uint32_t ppa_crop_x = (src_w - ppa_crop_w) / 2;
        uint32_t ppa_crop_y = (src_h - ppa_crop_h) / 2;
        ppa_srm_oper_config_t config = {
            .in = {
                .buffer = src,
                .pic_w = src_w,
                .pic_h = src_h,
                .block_w = ppa_crop_w,
                .block_h = ppa_crop_h,
                .block_offset_x = ppa_crop_x,
                .block_offset_y = ppa_crop_y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = dst,
                .buffer_size = (size_t)DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES * CAM_LCD_BYTES_PER_PIXEL,
                .pic_w = DISPLAY_PANEL_H_RES,
                .pic_h = DISPLAY_PANEL_V_RES,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = CAM_PPA_LCD_COLOR_MODE,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = ppa_scale,
            .scale_y = ppa_scale,
            .rgb_swap = false,
            .byte_swap = false,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        if (!ppa_geometry_logged) {
            ESP_LOGI(TAG,
                     "PPA fill: %ux%u crop %ux%u at (%u,%u), scale=%.4f -> %ux%u",
                     src_w, src_h, ppa_crop_w, ppa_crop_h,
                     ppa_crop_x, ppa_crop_y, ppa_scale,
                     DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
            ppa_geometry_logged = true;
        }
        ppa_ret = ppa_do_scale_rotate_mirror(ppa_srm, &config);
        if (ppa_ret == ESP_OK)
        {
            return;
        }
    }

    ppa_disabled = true;
    if (!ppa_disable_logged)
    {
        ESP_LOGW(TAG, "PPA scale failed: %s, fallback to CPU scale", esp_err_to_name(ppa_ret));
        ppa_disable_logged = true;
    }

    for (uint32_t y = 0; y < DISPLAY_PANEL_V_RES; y++)
    {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop_h) / DISPLAY_PANEL_V_RES);
        for (uint32_t x = 0; x < DISPLAY_PANEL_H_RES; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / DISPLAY_PANEL_H_RES);
            const uint8_t *s = src + ((size_t)src_y * src_w + src_x) * CAM_PREVIEW_BYTES_PER_PIXEL;
            uint8_t *d = dst + ((size_t)y * DISPLAY_PANEL_H_RES + x) * CAM_LCD_BYTES_PER_PIXEL;

            rgb565_to_lcd(s, d);
        }
    }
}

static void clean_lcd_bottom_edge(uint8_t *lcd_fb)
{
    if (CAM_LCD_BOTTOM_GUARD_LINES == 0 || CAM_LCD_BOTTOM_GUARD_LINES >= DISPLAY_PANEL_V_RES)
    {
        return;
    }

    const size_t line_bytes = DISPLAY_PANEL_H_RES * CAM_LCD_BYTES_PER_PIXEL;
    uint8_t *guard = lcd_fb + ((size_t)(DISPLAY_PANEL_V_RES - CAM_LCD_BOTTOM_GUARD_LINES) * line_bytes);
    memset(guard, 0, CAM_LCD_BOTTOM_GUARD_LINES * line_bytes);
}

static esp_err_t draw_video_frame(const camera_ov2710_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(frame->pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB565,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Camera frame is not RGB565");
    size_t expected_size = (size_t)frame->width * frame->height * CAM_PREVIEW_BYTES_PER_PIXEL;
    ESP_RETURN_ON_FALSE(frame->bytes_used >= expected_size, ESP_ERR_INVALID_SIZE,
                        TAG, "Frame too small: %u < %u",
                        (unsigned)frame->bytes_used, (unsigned)expected_size);
    const uint8_t *video_frame = frame->data;

    uint8_t *lcd_fb = display_panel_get_next_frame_buffer(&lcd);
    ESP_RETURN_ON_FALSE(lcd_fb, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    if (frame->width == DISPLAY_PANEL_H_RES && frame->height == DISPLAY_PANEL_V_RES)
    {
        copy_video_rgb565_to_lcd(video_frame, lcd_fb, (size_t)DISPLAY_PANEL_H_RES * DISPLAY_PANEL_V_RES);
    }
    else if (frame->width >= DISPLAY_PANEL_H_RES &&
             frame->height >= DISPLAY_PANEL_V_RES &&
             frame->height == DISPLAY_PANEL_V_RES)
    {
        crop_center_rgb565_to_lcd(video_frame, frame->width, frame->height, lcd_fb);
    }
    else
    {
        scale_center_rgb565_to_lcd(video_frame, frame->width, frame->height, lcd_fb);
    }

    clean_lcd_bottom_edge(lcd_fb);
    const size_t guard_bytes = (size_t)DISPLAY_PANEL_H_RES * CAM_LCD_BOTTOM_GUARD_LINES * CAM_LCD_BYTES_PER_PIXEL;
    uint8_t *guard = lcd_fb + ((size_t)(DISPLAY_PANEL_V_RES - CAM_LCD_BOTTOM_GUARD_LINES) *
                              DISPLAY_PANEL_H_RES * CAM_LCD_BYTES_PER_PIXEL);
    ESP_RETURN_ON_ERROR(esp_cache_msync(guard, guard_bytes,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                        TAG, "LCD guard cache sync failed");

    ESP_RETURN_ON_ERROR(display_panel_draw_bitmap(&lcd, 0, 0, DISPLAY_PANEL_H_RES,
                                                  DISPLAY_PANEL_V_RES, lcd_fb),
                        TAG, "LCD draw failed");

    return ESP_OK;
}

static esp_err_t preview_loop(void)
{
    uint32_t frame_count = 0;
    int64_t fps_start_us = esp_timer_get_time();
    int64_t next_display_us = fps_start_us;

    while (1)
    {
        camera_ov2710_frame_t frame;
        ESP_RETURN_ON_ERROR(camera_ov2710_get_frame(camera, &frame),
                            TAG, "Get camera frame failed");

        if (!frame.error)
        {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= next_display_us)
            {
                esp_err_t ret = draw_video_frame(&frame);
                if (ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Draw frame failed: %s", esp_err_to_name(ret));
                }
                else
                {
                    frame_count++;
                }

                now_us = esp_timer_get_time();
                next_display_us = now_us + CAM_DISPLAY_FRAME_INTERVAL_US;
                if (now_us - fps_start_us >= 2000000)
                {
                    float fps = (float)frame_count * 1000000.0f / (float)(now_us - fps_start_us);
                    ESP_LOGI(TAG, "Display FPS: %.1f, last bytes=%u",
                             fps, (unsigned)frame.bytes_used);
                    frame_count = 0;
                    fps_start_us = now_us;
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "Video buffer has error flag");
        }

        ESP_RETURN_ON_ERROR(camera_ov2710_return_frame(camera, &frame),
                            TAG, "Return camera frame failed");
        vTaskDelay(1);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "CAM ESP Video example");
    camera_ov2710_config_t camera_config = CAMERA_OV2710_1080P_CONFIG();

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
    ESP_ERROR_CHECK(camera_ov2710_init(&bsp, &camera_config, &camera));
    ESP_ERROR_CHECK(init_lcd());
    ESP_ERROR_CHECK(show_lcd_test_pattern());
    ESP_ERROR_CHECK(preview_loop());
}
