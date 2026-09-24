#include "camera.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "camera_ov2710.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/ppa.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "lvgl_page_manager.h"
#include "board_config.h"

static const char *TAG = "factory_camera";

#define CAM_VIDEO_BUFFER_COUNT 3
#define CAM_PREVIEW_FPS 25
#define CAM_LCD_BYTES_PER_PIXEL (DISPLAY_PANEL_BITS_PER_PIXEL / 8U)
#define CAM_RGB888_BYTES_PER_PIXEL 3U
#define CAM_UI_FRAME_BUFFER_COUNT 2
#define CAM_PHOTO_SAVE_WIDTH 1280
#define CAM_PHOTO_SAVE_HEIGHT 720

// #define CAM_UI_FRAME_INTERVAL_US 66666   // 15 fps
#define CAM_UI_FRAME_INTERVAL_US 33333   // up to 30 fps, limited by the sensor

#define CAM_UI_DRAW_CHUNK_LINES 16
#define CAM_PREVIEW_TASK_STACK_SIZE 8192
#define CAM_PHOTO_TASK_STACK_SIZE 4096
#define CAM_SD_MOUNT_POINT "/sdcard"
#define CAM_PHOTO_DIR "/sdcard/photo"
#define CAM_CACHE_ALIGN 64
#define CAM_ALIGN_UP(value, align) (((value) + ((align) - 1)) & ~((align) - 1))

static t_panel_p4_bsp_t *s_camera_bsp;
static display_panel_t *s_camera_lcd;
static camera_ov2710_handle_t s_camera;
static TaskHandle_t s_camera_preview_task;
static volatile bool s_camera_preview_stop;
static volatile bool s_camera_preview_draw_enabled;
static uint8_t *s_camera_frame_buf[CAM_UI_FRAME_BUFFER_COUNT];
static size_t s_camera_frame_buf_size;
static uint8_t *s_camera_draw_bounce_buf;
static SemaphoreHandle_t s_camera_frame_lock;
static int s_camera_latest_frame_index = -1;
static volatile bool s_camera_photo_request;
static volatile bool s_camera_photo_saving;
static uint32_t s_camera_photo_count;
static volatile uint32_t s_camera_preview_width;
static volatile uint32_t s_camera_preview_height;
static volatile uint32_t s_camera_preview_fps_x10;
static camera_preview_frame_cb_t s_camera_preview_frame_cb;
static volatile esp_err_t s_camera_last_photo_result = ESP_ERR_INVALID_STATE;
static uint8_t *s_camera_last_photo_thumb;
static uint32_t s_camera_last_photo_generation;
static int s_camera_preview_x = CAMERA_UI_PREVIEW_X;
static int s_camera_preview_y = CAMERA_UI_PREVIEW_Y;
static int s_camera_preview_w = CAMERA_UI_PREVIEW_WIDTH;
static int s_camera_preview_h = CAMERA_UI_PREVIEW_HEIGHT;
static bool s_camera_preview_direct_crop;
#if CONFIG_IDF_TARGET_ESP32P4
static ppa_client_handle_t s_camera_ppa_srm;
static bool s_camera_ppa_disabled;
static bool s_camera_ppa_disable_logged;
static bool s_camera_ppa_frame_buffers_dma_capable = true;
#endif

esp_err_t camera_init(t_panel_p4_bsp_t *bsp, display_panel_t *display)
{
    ESP_RETURN_ON_FALSE(bsp != NULL && display != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid camera init handle");
    ESP_RETURN_ON_FALSE(t_panel_p4_bsp_get_i2c_bus(bsp) != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "BSP I2C bus is not initialized");
    s_camera_bsp = bsp;
    s_camera_lcd = display;
    ESP_LOGI(TAG, "Camera service ready; OV2710 starts on preview entry");
    return ESP_OK;
}

static uint32_t camera_preview_bytes_per_pixel(uint32_t pix_format)
{
    return pix_format == V4L2_PIX_FMT_RGB24 ? 3 : 2;
}

static uint32_t camera_frame_v4l2_format(camera_ov2710_pixel_format_t format)
{
    return format == CAMERA_OV2710_PIXEL_FORMAT_RGB888
               ? V4L2_PIX_FMT_RGB24
               : V4L2_PIX_FMT_RGB565;
}

static esp_err_t camera_component_open(uint32_t width, uint32_t height)
{
    ESP_RETURN_ON_FALSE(s_camera_bsp != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Camera BSP handle is unavailable");

    const uint32_t sizes[][2] = {
        {width, height},
        {1280, 720},
        {1920, 1080},
    };
    esp_err_t ret = ESP_FAIL;

    for (size_t size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++)
    {
        if (size_index > 0 && sizes[size_index][0] == sizes[0][0] &&
            sizes[size_index][1] == sizes[0][1])
        {
            continue;
        }

        for (int format_index = 0; format_index < 2; format_index++)
        {
            camera_ov2710_config_t config = {
                .width = sizes[size_index][0],
                .height = sizes[size_index][1],
                .fps = CAM_PREVIEW_FPS,
                .buffer_count = CAM_VIDEO_BUFFER_COUNT,
                .pixel_format = format_index == 0
                                    ? CAMERA_OV2710_PIXEL_FORMAT_RGB565
                                    : CAMERA_OV2710_PIXEL_FORMAT_RGB888,
                .log_supported_formats = size_index == 0 && format_index == 0,
            };

            ret = camera_ov2710_init(s_camera_bsp, &config, &s_camera);
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "OV2710 component ready: %ux%u, %s, %u FPS",
                         config.width,
                         config.height,
                         config.pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB888
                             ? "RGB888"
                             : "RGB565",
                         config.fps);
                return ESP_OK;
            }

            s_camera = NULL;
            ESP_LOGW(TAG, "OV2710 %ux%u %s init failed: %s",
                     config.width,
                     config.height,
                     config.pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB888
                         ? "RGB888"
                         : "RGB565",
                     esp_err_to_name(ret));
        }
    }

    return ret;
}

static bool camera_frame_buffers_alloc(void)
{
    const size_t frame_size = (size_t)s_camera_preview_w *
                              s_camera_preview_h * CAM_LCD_BYTES_PER_PIXEL;
    const size_t frame_alloc_size = CAM_ALIGN_UP(frame_size, CAM_CACHE_ALIGN);
    const size_t bounce_size = CAMERA_UI_PREVIEW_WIDTH * CAM_UI_DRAW_CHUNK_LINES * CAM_LCD_BYTES_PER_PIXEL;
    const size_t bounce_alloc_size = CAM_ALIGN_UP(bounce_size, CAM_CACHE_ALIGN);

    if (frame_size == 0)
    {
        return false;
    }

    if (s_camera_frame_buf_size < frame_alloc_size)
    {
        for (int i = 0; i < CAM_UI_FRAME_BUFFER_COUNT; i++)
        {
            heap_caps_free(s_camera_frame_buf[i]);
            s_camera_frame_buf[i] = NULL;
        }
        s_camera_frame_buf_size = 0;
    }

    if (s_camera_frame_buf[0] == NULL && s_camera_draw_bounce_buf == NULL)
    {
        bool all_allocated = true;
#if CONFIG_IDF_TARGET_ESP32P4
        s_camera_ppa_frame_buffers_dma_capable = true;
#endif
        for (int i = 0; i < CAM_UI_FRAME_BUFFER_COUNT; i++)
        {
            s_camera_frame_buf[i] = heap_caps_aligned_alloc(
                CAM_CACHE_ALIGN, frame_alloc_size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            if (s_camera_frame_buf[i] == NULL)
            {
                s_camera_frame_buf[i] = heap_caps_aligned_alloc(
                    CAM_CACHE_ALIGN, frame_alloc_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if CONFIG_IDF_TARGET_ESP32P4
                s_camera_ppa_frame_buffers_dma_capable = false;
#endif
            }
            if (s_camera_frame_buf[i] == NULL)
            {
                all_allocated = false;
                break;
            }
        }

        if (all_allocated)
        {
            s_camera_frame_buf_size = frame_alloc_size;
            ESP_LOGI(TAG, "Camera full-frame buffers: %u bytes x%d",
                     (unsigned)frame_alloc_size, CAM_UI_FRAME_BUFFER_COUNT);
        }
        else
        {
            for (int i = 0; i < CAM_UI_FRAME_BUFFER_COUNT; i++)
            {
                heap_caps_free(s_camera_frame_buf[i]);
                s_camera_frame_buf[i] = NULL;
            }
        }
    }

    if (s_camera_frame_buf[0] != NULL && s_camera_frame_buf[1] != NULL)
    {
        return true;
    }

    if (s_camera_draw_bounce_buf == NULL)
    {
        s_camera_draw_bounce_buf = heap_caps_aligned_alloc(
            CAM_CACHE_ALIGN, bounce_alloc_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_camera_draw_bounce_buf != NULL)
        {
            ESP_LOGW(TAG, "Camera full-frame allocation failed; using %u-byte strip fallback",
                     (unsigned)bounce_alloc_size);
        }
    }

    return s_camera_draw_bounce_buf != NULL;
}

static bool camera_frame_lock_init(void)
{
    if (s_camera_frame_lock != NULL)
    {
        return true;
    }

    s_camera_frame_lock = xSemaphoreCreateMutex();
    if (s_camera_frame_lock == NULL)
    {
        ESP_LOGE(TAG, "Camera frame lock create failed");
        return false;
    }
    return true;
}

static int camera_frame_buffer_next_write(void)
{
    static int next = 0;

    int index = next;
    next = (next + 1) % CAM_UI_FRAME_BUFFER_COUNT;
    return index;
}

#if CONFIG_IDF_TARGET_ESP32P4
static esp_err_t camera_ppa_init(void)
{
    if (s_camera_ppa_disabled)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_camera_ppa_srm != NULL)
    {
        return ESP_OK;
    }

    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_64,
    };
    esp_err_t ret = ppa_register_client(&ppa_config, &s_camera_ppa_srm);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Camera PPA SRM enabled, burst=64");
    }
    else if (!s_camera_ppa_disable_logged)
    {
        ESP_LOGW(TAG, "Camera PPA SRM unavailable: %s, fallback to CPU scale", esp_err_to_name(ret));
        s_camera_ppa_disable_logged = true;
        s_camera_ppa_disabled = true;
    }
    return ret;
}

static bool camera_ppa_srm_color_mode(uint32_t pix_format, ppa_srm_color_mode_t *mode)
{
    if (mode == NULL)
    {
        return false;
    }

    if (pix_format == V4L2_PIX_FMT_RGB24)
    {
        *mode = PPA_SRM_COLOR_MODE_RGB888;
        return true;
    }
    if (pix_format == V4L2_PIX_FMT_RGB565)
    {
        *mode = PPA_SRM_COLOR_MODE_RGB565;
        return true;
    }
    return false;
}

static esp_err_t camera_scale_to_lcd_ppa(const uint8_t *src,
                                         uint32_t src_w,
                                         uint32_t src_h,
                                         uint32_t src_stride,
                                         uint8_t *dst,
                                         uint32_t dst_w,
                                         uint32_t dst_h,
                                         uint32_t pix_format)
{
    ppa_srm_color_mode_t in_cm;
    uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);

    ESP_RETURN_ON_FALSE(src != NULL && dst != NULL && src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid PPA scale args");
    if (src_stride != src_w * src_bpp)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_camera_ppa_frame_buffers_dma_capable)
    {
        s_camera_ppa_disabled = true;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!camera_ppa_srm_color_mode(pix_format, &in_cm))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = camera_ppa_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = src_w;
    uint32_t crop_h = src_h;

    if ((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w)
    {
        crop_w = (uint32_t)(((uint64_t)src_h * dst_w) / dst_h);
        crop_x = (src_w - crop_w) / 2;
    }
    else if ((uint64_t)src_w * dst_h < (uint64_t)src_h * dst_w)
    {
        crop_h = (uint32_t)(((uint64_t)src_w * dst_h) / dst_w);
        crop_y = (src_h - crop_h) / 2;
    }

    ppa_srm_oper_config_t srm_config = {
        .in = {
            .buffer = src,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = crop_w,
            .block_h = crop_h,
            .block_offset_x = crop_x,
            .block_offset_y = crop_y,
            .srm_cm = in_cm,
        },
        .out = {
            .buffer = dst,
            .buffer_size = CAM_ALIGN_UP(dst_w * dst_h * CAM_LCD_BYTES_PER_PIXEL, CAM_CACHE_ALIGN),
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
#if CONFIG_DISPLAY_PANEL_RGB565
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
#else
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
#endif
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)dst_w / (float)crop_w,
        .scale_y = (float)dst_h / (float)crop_h,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(s_camera_ppa_srm, &srm_config);
}
#endif

static inline void camera_rgb565_to_rgb888(uint16_t rgb565, uint8_t *dst)
{
    uint8_t b = (uint8_t)(rgb565 & 0x001f);
    uint8_t g = (uint8_t)((rgb565 >> 5) & 0x003f);
    uint8_t r = (uint8_t)((rgb565 >> 11) & 0x001f);

    dst[0] = (uint8_t)((r << 3) | (r >> 2));
    dst[1] = (uint8_t)((g << 2) | (g >> 4));
    dst[2] = (uint8_t)((b << 3) | (b >> 2));
}

static inline uint16_t camera_rgb24_to_rgb565(const uint8_t *src)
{
    return ((uint16_t)(src[0] & 0xF8U) << 8) |
           ((uint16_t)(src[1] & 0xFCU) << 3) |
           ((uint16_t)src[2] >> 3);
}

static inline void camera_rgb24_to_rgb888(const uint8_t *src, uint8_t *dst)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static inline void camera_rgb565_to_lcd(uint16_t rgb565, uint8_t *dst)
{
#if CONFIG_DISPLAY_PANEL_RGB565
    dst[0] = (uint8_t)(rgb565 & 0xFFU);
    dst[1] = (uint8_t)(rgb565 >> 8);
#else
    camera_rgb565_to_rgb888(rgb565, dst);
#endif
}

static inline void camera_rgb24_to_lcd(const uint8_t *src, uint8_t *dst)
{
#if CONFIG_DISPLAY_PANEL_RGB565
    camera_rgb565_to_lcd(camera_rgb24_to_rgb565(src), dst);
#else
    camera_rgb24_to_rgb888(src, dst);
#endif
}

static void camera_scale_step_to_lcd(const uint8_t *src,
                                     uint32_t src_w,
                                     uint32_t src_stride,
                                     uint8_t *dst,
                                     uint32_t dst_w,
                                     uint32_t dst_h,
                                     uint32_t step_x,
                                     uint32_t step_y,
                                     uint32_t pix_format)
{
    uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);

    for (uint32_t y = 0; y < dst_h; y++)
    {
        const uint8_t *src_row = src + ((size_t)y * step_y * src_stride);
        uint8_t *dst_row = dst + ((size_t)y * dst_w * CAM_LCD_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < dst_w; x++)
        {
            const uint8_t *s = src_row + ((size_t)x * step_x * src_bpp);
            uint8_t *d = dst_row + ((size_t)x * CAM_LCD_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd(s, d);
            }
            else
            {
                uint16_t rgb565 = s[0] | ((uint16_t)s[1] << 8);
                camera_rgb565_to_lcd(rgb565, d);
            }
        }
    }
}

static void camera_scale_to_lcd(const uint8_t *src,
                                uint32_t src_w,
                                uint32_t src_h,
                                uint32_t src_stride,
                                uint8_t *dst,
                                uint32_t dst_w,
                                uint32_t dst_h,
                                uint32_t pix_format)
{
    uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);

    if (src_w == dst_w * 2 && src_h == dst_h * 2)
    {
        camera_scale_step_to_lcd(src, src_w, src_stride, dst, dst_w, dst_h, 2, 2, pix_format);
        return;
    }

    if (src_w == dst_w * 3 && src_h == dst_h * 3)
    {
        camera_scale_step_to_lcd(src, src_w, src_stride, dst, dst_w, dst_h, 3, 3, pix_format);
        return;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t ppa_ret = camera_scale_to_lcd_ppa(src, src_w, src_h, src_stride, dst, dst_w, dst_h, pix_format);
    if (ppa_ret == ESP_OK)
    {
        return;
    }

    s_camera_ppa_disabled = true;
    if (!s_camera_ppa_disable_logged)
    {
        ESP_LOGW(TAG, "Camera PPA scale failed: %s, fallback to CPU scale", esp_err_to_name(ppa_ret));
        s_camera_ppa_disable_logged = true;
    }
#endif

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = src_w;
    uint32_t crop_h = src_h;

    if (s_camera_preview_direct_crop && src_w >= dst_w && src_h >= dst_h)
    {
        /* Show a centered source window at 1:1 pixels, without scaling. */
        crop_w = dst_w;
        crop_h = dst_h;
        crop_x = (src_w - crop_w) / 2U;
        crop_y = (src_h - crop_h) / 2U;
    }
    else if ((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w)
    {
        crop_w = (uint32_t)(((uint64_t)src_h * dst_w) / dst_h);
        crop_x = (src_w - crop_w) / 2;
    }
    else if ((uint64_t)src_w * dst_h < (uint64_t)src_h * dst_w)
    {
        crop_h = (uint32_t)(((uint64_t)src_w * dst_h) / dst_w);
        crop_y = (src_h - crop_h) / 2;
    }

    for (uint32_t y = 0; y < dst_h; y++)
    {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop_h) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / dst_w);
            const uint8_t *s = src + ((size_t)src_y * src_stride) + ((size_t)src_x * src_bpp);
            uint8_t *d = dst + (((size_t)y * dst_w + x) * CAM_LCD_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd(s, d);
            }
            else
            {
                uint16_t rgb565 = s[0] | ((uint16_t)s[1] << 8);
                camera_rgb565_to_lcd(rgb565, d);
            }
        }
    }
}

static esp_err_t camera_preview_scale_draw_strips(const uint8_t *src,
                                                  uint32_t src_w,
                                                  uint32_t src_h,
                                                  uint32_t src_stride,
                                                  uint32_t pix_format)
{
    ESP_RETURN_ON_FALSE(src != NULL && s_camera_draw_bounce_buf != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Camera strip draw buffer is unavailable");
    ESP_RETURN_ON_FALSE(s_camera_lcd != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Camera LCD handle is NULL");

    const int x = s_camera_preview_x;
    const int y = s_camera_preview_y;
    const uint32_t dst_w = (uint32_t)s_camera_preview_w;
    const uint32_t dst_h = (uint32_t)s_camera_preview_h;
    const uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = src_w;
    uint32_t crop_h = src_h;

    ESP_RETURN_ON_FALSE(dst_w > 0 && dst_h > 0 && src_stride >= src_w * src_bpp,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid camera strip geometry");

    if (s_camera_preview_direct_crop && src_w >= dst_w && src_h >= dst_h)
    {
        /* Show a centered source window at 1:1 pixels, without scaling. */
        crop_w = dst_w;
        crop_h = dst_h;
        crop_x = (src_w - crop_w) / 2U;
        crop_y = (src_h - crop_h) / 2U;
    }
    else if ((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w)
    {
        crop_w = (uint32_t)(((uint64_t)src_h * dst_w) / dst_h);
        crop_x = (src_w - crop_w) / 2;
    }
    else if ((uint64_t)src_w * dst_h < (uint64_t)src_h * dst_w)
    {
        crop_h = (uint32_t)(((uint64_t)src_w * dst_h) / dst_w);
        crop_y = (src_h - crop_h) / 2;
    }

    const bool integer_scale = crop_w % dst_w == 0 && crop_h % dst_h == 0;
    const uint32_t step_x = s_camera_preview_direct_crop ? 1U :
                             (integer_scale ? crop_w / dst_w : 0U);
    const uint32_t step_y = s_camera_preview_direct_crop ? 1U :
                             (integer_scale ? crop_h / dst_h : 0U);
    esp_err_t ret = ESP_OK;

    ui_lock();
    for (uint32_t row = 0;
         row < dst_h && s_camera_preview_draw_enabled && !s_camera_preview_stop;
         row += CAM_UI_DRAW_CHUNK_LINES)
    {
        uint32_t lines = dst_h - row;
        if (lines > CAM_UI_DRAW_CHUNK_LINES)
        {
            lines = CAM_UI_DRAW_CHUNK_LINES;
        }

        for (uint32_t line = 0; line < lines; line++)
        {
            uint32_t dst_y = row + line;
            uint32_t src_y = crop_y + (integer_scale
                                           ? dst_y * step_y
                                           : (uint32_t)(((uint64_t)dst_y * crop_h) / dst_h));
            const uint8_t *src_row = src + (size_t)src_y * src_stride;
            uint8_t *dst_row = s_camera_draw_bounce_buf +
                               (size_t)line * dst_w * CAM_LCD_BYTES_PER_PIXEL;

            const uint32_t x_step_fp = integer_scale ? 0U :
                                       (uint32_t)(((uint64_t)crop_w << 16) / dst_w);
            uint32_t src_x_fp = (uint32_t)crop_x << 16;
            for (uint32_t col = 0; col < dst_w; col++)
            {
                uint32_t src_x = integer_scale ? crop_x + col * step_x
                                                : (src_x_fp >> 16);
                const uint8_t *source = src_row + (size_t)src_x * src_bpp;
                uint8_t *target = dst_row + (size_t)col * CAM_LCD_BYTES_PER_PIXEL;
                if (pix_format == V4L2_PIX_FMT_RGB24)
                {
                    camera_rgb24_to_lcd(source, target);
                }
                else
                {
                    uint16_t rgb565 = source[0] | ((uint16_t)source[1] << 8);
                    camera_rgb565_to_lcd(rgb565, target);
                }
                src_x_fp += x_step_fp;
            }
        }

        size_t strip_size = (size_t)dst_w * lines * CAM_LCD_BYTES_PER_PIXEL;
        ret = esp_cache_msync(s_camera_draw_bounce_buf,
                              strip_size,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                  ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (ret != ESP_OK)
        {
            break;
        }
        ret = display_panel_draw_bitmap(s_camera_lcd,
                                        x, y + (int)row,
                                        x + (int)dst_w,
                                        y + (int)(row + lines),
                                        s_camera_draw_bounce_buf);
        if (ret != ESP_OK)
        {
            break;
        }

        /* Keep DMA2D writes short enough for the continuously scanning DSI DMA. */
        esp_rom_delay_us(100);
    }
    ui_unlock();

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Camera strip draw failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t camera_preview_draw_direct(const uint8_t *frame)
{
    esp_err_t ret;
    int x = s_camera_preview_x;
    int y = s_camera_preview_y;
    int w = s_camera_preview_w;
    int h = s_camera_preview_h;
    const size_t frame_stride = (size_t)w * CAM_LCD_BYTES_PER_PIXEL;
    const size_t row_bytes = (size_t)w * CAM_LCD_BYTES_PER_PIXEL;

    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "Camera direct frame is NULL");
    ESP_RETURN_ON_FALSE(s_camera_lcd != NULL, ESP_ERR_INVALID_STATE, TAG, "Camera LCD handle is NULL");
    if (!s_camera_preview_draw_enabled || s_camera_preview_stop)
    {
        return ESP_OK;
    }

    ui_lock();
    if (s_camera_draw_bounce_buf)
    {
        ret = ESP_OK;
        for (int row = 0; row < h && s_camera_preview_draw_enabled; row += CAM_UI_DRAW_CHUNK_LINES)
        {
            int lines = h - row;
            if (lines > CAM_UI_DRAW_CHUNK_LINES)
            {
                lines = CAM_UI_DRAW_CHUNK_LINES;
            }

            for (int line = 0; line < lines; line++)
            {
                memcpy(s_camera_draw_bounce_buf + ((size_t)line * row_bytes),
                       frame + ((size_t)(row + line) * frame_stride),
                       row_bytes);
            }

            ESP_GOTO_ON_ERROR(esp_cache_msync(s_camera_draw_bounce_buf,
                                              row_bytes * lines,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                              draw_done,
                              TAG,
                              "Camera bounce buffer cache sync failed");

            ret = display_panel_draw_bitmap(s_camera_lcd,
                                         x,
                                         y + row,
                                         x + w,
                                         y + row + lines,
                                         s_camera_draw_bounce_buf);
            if (ret != ESP_OK)
            {
                break;
            }
        }
    }
    else
    {
        if (!s_camera_preview_draw_enabled)
        {
            ret = ESP_OK;
            goto draw_done;
        }
        ESP_GOTO_ON_ERROR(esp_cache_msync((void *)frame,
                                          (size_t)w * h * CAM_LCD_BYTES_PER_PIXEL,
                                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                          draw_done,
                          TAG,
                          "Camera direct frame cache sync failed");
        ret = display_panel_draw_bitmap(s_camera_lcd,
                                     x,
                                     y,
                                     x + w,
                                     y + h,
                                     frame);
    }

draw_done:
    ui_unlock();

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Camera direct draw failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

typedef struct
{
    uint8_t *frame;
    uint32_t width;
    uint32_t height;
    uint32_t pix_format;
    uint32_t shot_no;
} camera_photo_task_arg_t;

static void camera_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
}

static void camera_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static esp_err_t camera_photo_make_dir(void)
{
    struct stat st = {0};

    if (stat(CAM_SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        ESP_LOGE(TAG, "SD card mount point is not ready: %s errno=%d", CAM_SD_MOUNT_POINT, errno);
        return ESP_ERR_NOT_FOUND;
    }

    if (stat(CAM_PHOTO_DIR, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
        {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Photo path exists but is not a directory: %s", CAM_PHOTO_DIR);
        return ESP_ERR_INVALID_STATE;
    }

    if (errno != ENOENT)
    {
        ESP_LOGE(TAG, "Check photo dir failed: %s errno=%d", CAM_PHOTO_DIR, errno);
        return ESP_FAIL;
    }

    errno = 0;
    if (mkdir(CAM_PHOTO_DIR, 0775) == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Create photo dir failed: %s errno=%d", CAM_PHOTO_DIR, errno);
    return ESP_FAIL;
}

bool camera_storage_is_ready(void)
{
    struct stat st = {0};

    return stat(CAM_SD_MOUNT_POINT, &st) == 0 && S_ISDIR(st.st_mode);
}

static void camera_photo_make_path(char *path, size_t path_size, uint32_t shot_no)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};

    if (now > 1609459200 && localtime_r(&now, &tm_now) != NULL)
    {
        snprintf(path,
                 path_size,
                 CAM_PHOTO_DIR "/IMG_%04d%02d%02d_%02d%02d%02d_%03" PRIu32 ".bmp",
                 tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1,
                 tm_now.tm_mday,
                 tm_now.tm_hour,
                 tm_now.tm_min,
                 tm_now.tm_sec,
                 shot_no % 1000);
    }
    else
    {
        snprintf(path,
                 path_size,
                 CAM_PHOTO_DIR "/IMG_%010" PRIu64 "_%03" PRIu32 ".bmp",
                 (uint64_t)(esp_timer_get_time() / 1000),
                 shot_no % 1000);
    }
}

static esp_err_t camera_photo_write_bmp(const char *path,
                                        const uint8_t *frame,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t pix_format)
{
    const uint32_t row_stride = (width * CAM_RGB888_BYTES_PER_PIXEL + 3U) & ~3U;
    const uint32_t visible_row_bytes = width * CAM_RGB888_BYTES_PER_PIXEL;
    const uint32_t image_size = row_stride * height;
    const uint32_t file_size = 54U + image_size;
    const uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);
    uint8_t header[54] = {0};
    esp_err_t ret = ESP_FAIL;
    uint8_t *row = NULL;
    uint8_t *io_buf = NULL;

    ESP_RETURN_ON_FALSE(path != NULL && frame != NULL && width > 0 && height > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid BMP args");
    ESP_RETURN_ON_FALSE(pix_format == V4L2_PIX_FMT_RGB24 || pix_format == V4L2_PIX_FMT_RGB565,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported photo format: " V4L2_FMT_STR,
                        V4L2_FMT_STR_ARG(pix_format));

    row = heap_caps_malloc(row_stride, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (row == NULL)
    {
        row = heap_caps_malloc(row_stride, MALLOC_CAP_DEFAULT);
    }
    ESP_RETURN_ON_FALSE(row != NULL, ESP_ERR_NO_MEM, TAG, "BMP row buffer alloc failed");

    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Open photo failed: %s errno=%d", path, errno);
        free(row);
        return ESP_FAIL;
    }

    io_buf = heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (io_buf == NULL)
    {
        io_buf = heap_caps_malloc(32 * 1024, MALLOC_CAP_DEFAULT);
    }
    if (io_buf != NULL)
    {
        setvbuf(fp, (char *)io_buf, _IOFBF, 32 * 1024);
    }

    header[0] = 'B';
    header[1] = 'M';
    camera_write_le32(&header[2], file_size);
    camera_write_le32(&header[10], 54);
    camera_write_le32(&header[14], 40);
    camera_write_le32(&header[18], width);
    camera_write_le32(&header[22], height);
    camera_write_le16(&header[26], 1);
    camera_write_le16(&header[28], 24);
    camera_write_le32(&header[34], image_size);

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header))
    {
        goto out;
    }

    for (int y = (int)height - 1; y >= 0; y--)
    {
        const uint8_t *src = frame + ((size_t)y * width * src_bpp);
        for (uint32_t x = 0; x < width; x++)
        {
            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                row[x * 3U + 0U] = src[x * 3U + 2U];
                row[x * 3U + 1U] = src[x * 3U + 1U];
                row[x * 3U + 2U] = src[x * 3U + 0U];
            }
            else
            {
                uint8_t rgb888[3];
                uint16_t rgb565 = src[x * 2U] | ((uint16_t)src[x * 2U + 1U] << 8);
                camera_rgb565_to_rgb888(rgb565, rgb888);
                row[x * 3U + 0U] = rgb888[2];
                row[x * 3U + 1U] = rgb888[1];
                row[x * 3U + 2U] = rgb888[0];
            }
        }
        if (row_stride > visible_row_bytes)
        {
            memset(row + visible_row_bytes, 0, row_stride - visible_row_bytes);
        }
        if (fwrite(row, 1, row_stride, fp) != row_stride)
        {
            goto out;
        }
        if (((uint32_t)y & 0x1fU) == 0)
        {
            vTaskDelay(1);
        }
    }

    ret = ESP_OK;

out:
    if (fclose(fp) != 0 && ret == ESP_OK)
    {
        ret = ESP_FAIL;
    }
    free(io_buf);
    free(row);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Write photo failed: %s", path);
    }
    return ret;
}

static esp_err_t camera_photo_scale_to_rgb888(const uint8_t *src_frame,
                                              uint32_t src_w,
                                              uint32_t src_h,
                                              uint32_t src_stride,
                                              uint32_t pix_format,
                                              uint8_t *dst_frame,
                                              uint32_t dst_w,
                                              uint32_t dst_h)
{
    const uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);

    ESP_RETURN_ON_FALSE(src_frame != NULL && dst_frame != NULL && src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid photo scale args");
    ESP_RETURN_ON_FALSE(pix_format == V4L2_PIX_FMT_RGB24 || pix_format == V4L2_PIX_FMT_RGB565,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported photo scale format: " V4L2_FMT_STR,
                        V4L2_FMT_STR_ARG(pix_format));

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = src_w;
    uint32_t crop_h = src_h;

    if ((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w)
    {
        crop_w = (uint32_t)(((uint64_t)src_h * dst_w) / dst_h);
        crop_x = (src_w - crop_w) / 2U;
    }
    else if ((uint64_t)src_w * dst_h < (uint64_t)src_h * dst_w)
    {
        crop_h = (uint32_t)(((uint64_t)src_w * dst_h) / dst_w);
        crop_y = (src_h - crop_h) / 2U;
    }

    for (uint32_t y = 0; y < dst_h; y++)
    {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop_h) / dst_h);
        const uint8_t *src_row = src_frame + ((size_t)src_y * src_stride);
        uint8_t *dst_row = dst_frame + ((size_t)y * dst_w * CAM_RGB888_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / dst_w);
            const uint8_t *src = src_row + ((size_t)src_x * src_bpp);
            uint8_t *dst = dst_row + ((size_t)x * CAM_RGB888_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_rgb888(src, dst);
            }
            else
            {
                uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
                camera_rgb565_to_rgb888(rgb565, dst);
            }
        }
    }

    return ESP_OK;
}

static esp_err_t camera_photo_update_thumbnail(const uint8_t *frame,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t pix_format)
{
    const uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);
    const size_t thumb_size = (size_t)CAMERA_UI_LAST_PHOTO_WIDTH *
                              CAMERA_UI_LAST_PHOTO_HEIGHT *
                              CAM_RGB888_BYTES_PER_PIXEL;

    ESP_RETURN_ON_FALSE(frame != NULL && width > 0 && height > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid thumbnail frame");
    ESP_RETURN_ON_FALSE(pix_format == V4L2_PIX_FMT_RGB24 || pix_format == V4L2_PIX_FMT_RGB565,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported thumbnail format: " V4L2_FMT_STR,
                        V4L2_FMT_STR_ARG(pix_format));

    uint8_t *thumb = heap_caps_malloc(thumb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (thumb == NULL)
    {
        thumb = heap_caps_malloc(thumb_size, MALLOC_CAP_DEFAULT);
    }
    ESP_RETURN_ON_FALSE(thumb != NULL, ESP_ERR_NO_MEM, TAG, "Thumbnail alloc failed");

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = width;
    uint32_t crop_h = height;

    if ((uint64_t)width * CAMERA_UI_LAST_PHOTO_HEIGHT > (uint64_t)height * CAMERA_UI_LAST_PHOTO_WIDTH)
    {
        crop_w = (uint32_t)(((uint64_t)height * CAMERA_UI_LAST_PHOTO_WIDTH) / CAMERA_UI_LAST_PHOTO_HEIGHT);
        crop_x = (width - crop_w) / 2U;
    }
    else if ((uint64_t)width * CAMERA_UI_LAST_PHOTO_HEIGHT < (uint64_t)height * CAMERA_UI_LAST_PHOTO_WIDTH)
    {
        crop_h = (uint32_t)(((uint64_t)width * CAMERA_UI_LAST_PHOTO_HEIGHT) / CAMERA_UI_LAST_PHOTO_WIDTH);
        crop_y = (height - crop_h) / 2U;
    }

    for (uint32_t y = 0; y < CAMERA_UI_LAST_PHOTO_HEIGHT; y++)
    {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop_h) / CAMERA_UI_LAST_PHOTO_HEIGHT);
        const uint8_t *src_row = frame + ((size_t)src_y * width * src_bpp);
        uint8_t *dst_row = thumb + ((size_t)y * CAMERA_UI_LAST_PHOTO_WIDTH *
                                    CAM_RGB888_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < CAMERA_UI_LAST_PHOTO_WIDTH; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / CAMERA_UI_LAST_PHOTO_WIDTH);
            const uint8_t *src = src_row + ((size_t)src_x * src_bpp);
            uint8_t *dst = dst_row + ((size_t)x * CAM_RGB888_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_rgb888(src, dst);
            }
            else
            {
                uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
                camera_rgb565_to_rgb888(rgb565, dst);
            }
        }
    }

    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreTake(s_camera_frame_lock, portMAX_DELAY);
    }
    free(s_camera_last_photo_thumb);
    s_camera_last_photo_thumb = thumb;
    s_camera_last_photo_generation++;
    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreGive(s_camera_frame_lock);
    }
    return ESP_OK;
}

static void camera_photo_task(void *arg)
{
    camera_photo_task_arg_t *photo = (camera_photo_task_arg_t *)arg;
    char path[128];
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (photo != NULL && photo->frame != NULL)
    {
        ret = camera_photo_make_dir();
        if (ret == ESP_OK)
        {
            camera_photo_make_path(path, sizeof(path), photo->shot_no);
            int64_t save_start_us = esp_timer_get_time();
            ret = camera_photo_write_bmp(path, photo->frame, photo->width, photo->height, photo->pix_format);
            int64_t save_ms = (esp_timer_get_time() - save_start_us) / 1000;
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "Photo saved: %s (%" PRId64 " ms)", path, save_ms);
            }
        }
    }

    if (ret != ESP_OK)
    {
        s_camera_last_photo_result = ret;
    }
    else
    {
        s_camera_last_photo_result = ESP_OK;
    }
    if (photo != NULL)
    {
        free(photo->frame);
        free(photo);
    }
    s_camera_photo_request = false;
    s_camera_photo_saving = false;
    vTaskDelete(NULL);
}

static esp_err_t camera_photo_submit_frame(const uint8_t *frame,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t pix_format,
                                           uint32_t src_stride,
                                           size_t bytesused)
{
    const size_t src_bpp = camera_preview_bytes_per_pixel(pix_format);
    uint32_t save_w = width;
    uint32_t save_h = height;

    ESP_RETURN_ON_FALSE(frame != NULL && width > 0 && height > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid photo frame");
    ESP_RETURN_ON_FALSE(pix_format == V4L2_PIX_FMT_RGB24 || pix_format == V4L2_PIX_FMT_RGB565,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported photo source format: " V4L2_FMT_STR,
                        V4L2_FMT_STR_ARG(pix_format));
    const size_t visible_row_bytes = (size_t)width * src_bpp;
    const size_t required_size = (size_t)src_stride * (height - 1U) + visible_row_bytes;
    ESP_RETURN_ON_FALSE(src_stride >= visible_row_bytes, ESP_ERR_INVALID_ARG, TAG, "Invalid photo frame stride");
    ESP_RETURN_ON_FALSE(bytesused >= required_size, ESP_FAIL, TAG, "Photo frame is incomplete");

    if (width >= CAM_PHOTO_SAVE_WIDTH && height >= CAM_PHOTO_SAVE_HEIGHT)
    {
        save_w = CAM_PHOTO_SAVE_WIDTH;
        save_h = CAM_PHOTO_SAVE_HEIGHT;
    }

    camera_photo_task_arg_t *photo = heap_caps_calloc(1, sizeof(*photo), MALLOC_CAP_DEFAULT);
    if (photo == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const size_t frame_size = (size_t)save_w * save_h * CAM_RGB888_BYTES_PER_PIXEL;
    photo->frame = heap_caps_aligned_alloc(CAM_CACHE_ALIGN,
                                           CAM_ALIGN_UP(frame_size, CAM_CACHE_ALIGN),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (photo->frame == NULL)
    {
        photo->frame = heap_caps_malloc(frame_size, MALLOC_CAP_DEFAULT);
    }
    if (photo->frame == NULL)
    {
        ESP_LOGW(TAG,
                 "Photo frame alloc failed: need=%u KB, psram largest=%u KB",
                 (unsigned)(frame_size / 1024U),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
        free(photo);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t scale_ret = camera_photo_scale_to_rgb888(frame,
                                                       width,
                                                       height,
                                                       src_stride,
                                                       pix_format,
                                                       photo->frame,
                                                       save_w,
                                                       save_h);
    if (scale_ret != ESP_OK)
    {
        free(photo->frame);
        free(photo);
        return scale_ret;
    }

    photo->width = save_w;
    photo->height = save_h;
    photo->pix_format = V4L2_PIX_FMT_RGB24;
    photo->shot_no = ++s_camera_photo_count;

    ESP_LOGI(TAG, "Photo save frame: %ux%u -> %ux%u RGB3", width, height, save_w, save_h);
    esp_err_t thumb_ret = camera_photo_update_thumbnail(photo->frame,
                                                        photo->width,
                                                        photo->height,
                                                        photo->pix_format);
    if (thumb_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Update photo thumbnail failed: %s", esp_err_to_name(thumb_ret));
    }

    BaseType_t ok = xTaskCreate(camera_photo_task,
                                "camera_photo",
                                CAM_PHOTO_TASK_STACK_SIZE,
                                photo,
                                2,
                                NULL);
    if (ok != pdPASS)
    {
        s_camera_photo_saving = false;
        s_camera_last_photo_result = ESP_FAIL;
        free(photo->frame);
        free(photo);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t camera_capture_photo_async(void)
{
    if (s_camera_photo_saving || s_camera_photo_request)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_camera_preview_task == NULL || s_camera_preview_stop)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!camera_storage_is_ready())
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_camera_photo_saving = true;
    s_camera_last_photo_result = ESP_ERR_NOT_FINISHED;
    s_camera_photo_request = true;
    return ESP_OK;
}

esp_err_t camera_state_get(camera_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "State is NULL");
    state->width = s_camera_preview_width;
    state->height = s_camera_preview_height;
    state->fps_x10 = s_camera_preview_fps_x10;
    state->photo_count = s_camera_photo_count;
    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreTake(s_camera_frame_lock, portMAX_DELAY);
    }
    state->photo_generation = s_camera_last_photo_generation;
    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreGive(s_camera_frame_lock);
    }
    state->last_photo_result = s_camera_last_photo_result;
    state->running = s_camera_preview_task != NULL && !s_camera_preview_stop;
    state->photo_saving = s_camera_photo_saving || s_camera_photo_request;
    state->storage_ready = camera_storage_is_ready();
    return ESP_OK;
}

esp_err_t camera_last_photo_copy(uint8_t *dst,
                                 size_t dst_size,
                                 uint32_t *width,
                                 uint32_t *height,
                                 uint32_t *generation)
{
    const size_t required = (size_t)CAMERA_UI_LAST_PHOTO_WIDTH *
                            CAMERA_UI_LAST_PHOTO_HEIGHT * CAM_RGB888_BYTES_PER_PIXEL;
    ESP_RETURN_ON_FALSE(dst != NULL && dst_size >= required,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid thumbnail buffer");
    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreTake(s_camera_frame_lock, portMAX_DELAY);
    }
    if (s_camera_last_photo_thumb == NULL)
    {
        if (s_camera_frame_lock != NULL)
        {
            xSemaphoreGive(s_camera_frame_lock);
        }
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dst, s_camera_last_photo_thumb, required);
    if (width != NULL)
    {
        *width = CAMERA_UI_LAST_PHOTO_WIDTH;
    }
    if (height != NULL)
    {
        *height = CAMERA_UI_LAST_PHOTO_HEIGHT;
    }
    if (generation != NULL)
    {
        *generation = s_camera_last_photo_generation;
    }
    if (s_camera_frame_lock != NULL)
    {
        xSemaphoreGive(s_camera_frame_lock);
    }
    return ESP_OK;
}

void camera_preview_clear_area(void)
{
    const int x = s_camera_preview_x;
    const int y = s_camera_preview_y;
    const int w = s_camera_preview_w;
    const int h = s_camera_preview_h;
    const uint8_t r = 0x05;
    const uint8_t g = 0x07;
    const uint8_t b = 0x0B;

    if (s_camera_lcd == NULL || w <= 0 || h <= 0 || !camera_frame_buffers_alloc())
    {
        return;
    }

    if (s_camera_frame_buf[0] != NULL)
    {
        const size_t frame_bytes = (size_t)w * h * CAM_LCD_BYTES_PER_PIXEL;
        for (int i = 0; i < w * h; i++)
        {
            uint8_t *px = s_camera_frame_buf[0] +
                          ((size_t)i * CAM_LCD_BYTES_PER_PIXEL);
            const uint8_t rgb888[3] = {r, g, b};
            camera_rgb24_to_lcd(rgb888, px);
        }

        esp_err_t sync_ret = esp_cache_msync(
            s_camera_frame_buf[0], frame_bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (sync_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Camera clear cache sync failed: %s",
                     esp_err_to_name(sync_ret));
            return;
        }

        ui_lock();
        esp_err_t draw_ret = display_panel_draw_bitmap(s_camera_lcd,
                                                       x, y, x + w, y + h,
                                                       s_camera_frame_buf[0]);
        ui_unlock();
        if (draw_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Camera clear draw failed: %s",
                     esp_err_to_name(draw_ret));
        }
        return;
    }

    ui_lock();
    for (int row = 0; row < h; row += CAM_UI_DRAW_CHUNK_LINES)
    {
        int lines = h - row;
        if (lines > CAM_UI_DRAW_CHUNK_LINES)
        {
            lines = CAM_UI_DRAW_CHUNK_LINES;
        }

        for (int i = 0; i < w * lines; i++)
        {
            uint8_t *px = s_camera_draw_bounce_buf + ((size_t)i * CAM_LCD_BYTES_PER_PIXEL);
            const uint8_t rgb888[3] = {r, g, b};
            camera_rgb24_to_lcd(rgb888, px);
        }

        esp_err_t sync_ret = esp_cache_msync(s_camera_draw_bounce_buf,
                                             (size_t)w * lines * CAM_LCD_BYTES_PER_PIXEL,
                                             ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (sync_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Camera clear cache sync failed: %s", esp_err_to_name(sync_ret));
            break;
        }

        esp_err_t draw_ret = display_panel_draw_bitmap(s_camera_lcd,
                                                    x,
                                                    y + row,
                                                    x + w,
                                                    y + row + lines,
                                                    s_camera_draw_bounce_buf);
        if (draw_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Camera clear draw failed: %s", esp_err_to_name(draw_ret));
            break;
        }
    }
    ui_unlock();
}

void camera_preview_set_area(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    s_camera_preview_x = x;
    s_camera_preview_y = y;
    s_camera_preview_w = w > CAMERA_UI_PREVIEW_WIDTH ? CAMERA_UI_PREVIEW_WIDTH : w;
    s_camera_preview_h = h > CAMERA_UI_PREVIEW_HEIGHT ? CAMERA_UI_PREVIEW_HEIGHT : h;
    ESP_LOGI(TAG,
             "Camera preview area: x=%d y=%d w=%d h=%d",
             s_camera_preview_x,
             s_camera_preview_y,
             s_camera_preview_w,
             s_camera_preview_h);
}

void camera_preview_set_direct_crop(bool enabled)
{
    s_camera_preview_direct_crop = enabled;
}

void camera_preview_set_frame_callback(camera_preview_frame_cb_t callback)
{
    s_camera_preview_frame_cb = callback;
}

static void camera_preview_task(void *arg)
{
    uint32_t width = (uint32_t)((uintptr_t)arg >> 16);
    uint32_t height = (uint32_t)((uintptr_t)arg & 0xffff);
    uint32_t frame_count = 0;
    int64_t fps_start_us = esp_timer_get_time();
    int64_t last_ui_frame_us = 0;

    if (width == 0 || height == 0)
    {
        width = 1280;
        height = 720;
    }

    if (!camera_frame_lock_init() || !camera_frame_buffers_alloc())
    {
        s_camera_preview_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (camera_component_open(width, height) != ESP_OK)
    {
        ESP_LOGE(TAG, "OV2710 component initialization failed");
        s_camera_preview_direct_crop = false;
        s_camera_preview_draw_enabled = false;
        s_camera_preview_stop = false;
        s_camera_preview_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Camera preview started, request=%ux%u", width, height);

    while (!s_camera_preview_stop)
    {
        camera_ov2710_frame_t frame = {0};
        esp_err_t frame_ret = camera_ov2710_get_frame(s_camera, &frame);
        if (frame_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Get OV2710 frame failed: %s", esp_err_to_name(frame_ret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t pix_format = camera_frame_v4l2_format(frame.pixel_format);
        uint32_t src_bpp = camera_preview_bytes_per_pixel(pix_format);
        uint32_t src_stride = frame.bytes_per_line
                                  ? frame.bytes_per_line
                                  : frame.width * src_bpp;
        size_t expected = frame.height > 0
                              ? (size_t)src_stride * (frame.height - 1U) +
                                    (size_t)frame.width * src_bpp
                              : 0;
        int64_t now_us = esp_timer_get_time();

        if (s_camera_preview_stop)
        {
            camera_ov2710_return_frame(s_camera, &frame);
            break;
        }

        if (!frame.error && frame.data != NULL && frame.bytes_used >= expected)
        {
            s_camera_preview_width = frame.width;
            s_camera_preview_height = frame.height;
            bool should_draw = s_camera_preview_draw_enabled &&
                               now_us - last_ui_frame_us >= CAM_UI_FRAME_INTERVAL_US;
            if (s_camera_photo_request)
            {
                esp_err_t photo_ret = camera_photo_submit_frame((const uint8_t *)frame.data,
                                                                frame.width,
                                                                frame.height,
                                                                pix_format,
                                                                src_stride,
                                                                frame.bytes_used);
                s_camera_photo_request = false;
                if (photo_ret != ESP_OK)
                {
                    s_camera_photo_saving = false;
                    s_camera_last_photo_result = photo_ret;
                    ESP_LOGW(TAG, "Submit photo frame failed: %s", esp_err_to_name(photo_ret));
                }
                else
                {
                    ESP_LOGI(TAG,
                             "Photo capture frame: %ux%u " V4L2_FMT_STR,
                             frame.width,
                             frame.height,
                             V4L2_FMT_STR_ARG(pix_format));
                }
            }

            if (should_draw)
            {
                if (s_camera_draw_bounce_buf != NULL)
                {
                    esp_err_t draw_ret = camera_preview_scale_draw_strips(
                        (const uint8_t *)frame.data,
                        frame.width,
                        frame.height,
                        src_stride,
                        pix_format);
                    if (draw_ret == ESP_OK)
                    {
                        last_ui_frame_us = now_us;
                        frame_count++;
                    }
                }
                else
                {
                    int write_index = camera_frame_buffer_next_write();
                    if (write_index >= 0)
                    {
                        bool frame_ready = false;
                        if (xSemaphoreTake(s_camera_frame_lock, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            camera_scale_to_lcd((const uint8_t *)frame.data,
                                                       frame.width,
                                                       frame.height,
                                                       src_stride,
                                                       s_camera_frame_buf[write_index],
                                                       s_camera_preview_w,
                                                       s_camera_preview_h,
                                                       pix_format);
                            s_camera_latest_frame_index = write_index;
                            frame_ready = true;
                            xSemaphoreGive(s_camera_frame_lock);
                        }
                        if (s_camera_preview_draw_enabled && !s_camera_preview_stop && frame_ready)
                        {
                            camera_preview_frame_cb_t frame_cb = s_camera_preview_frame_cb;
                            if (frame_cb != NULL)
                            {
                                frame_cb(s_camera_frame_buf[write_index],
                                         (uint32_t)s_camera_preview_w,
                                         (uint32_t)s_camera_preview_h);
                            }
                            else
                            {
                                camera_preview_draw_direct(s_camera_frame_buf[write_index]);
                            }
                            last_ui_frame_us = now_us;
                            frame_count++;
                        }
                    }
                }
            }

            if (now_us - fps_start_us >= 1000000)
            {
                float fps = (float)frame_count * 1000000.0f / (float)(now_us - fps_start_us);
                s_camera_preview_fps_x10 = (uint32_t)(fps * 10.0f + 0.5f);
                ESP_LOGI(TAG, "Camera preview draw FPS: %.1f", fps);
                frame_count = 0;
                fps_start_us = now_us;
            }
        }
        else if (frame.error)
        {
            ESP_LOGW(TAG, "Camera buffer has error flag");
        }
        else
        {
            ESP_LOGW(TAG, "Camera frame too small: %u < %u",
                     (unsigned)frame.bytes_used, (unsigned)expected);
        }

        frame_ret = camera_ov2710_return_frame(s_camera, &frame);
        if (frame_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Return OV2710 frame failed: %s", esp_err_to_name(frame_ret));
            break;
        }
        vTaskDelay(1);
    }

    if (s_camera != NULL)
    {
        esp_err_t deinit_ret = camera_ov2710_deinit(s_camera);
        if (deinit_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "OV2710 deinit failed: %s", esp_err_to_name(deinit_ret));
        }
        s_camera = NULL;
    }
    ESP_LOGI(TAG, "Camera preview stopped");
    if (s_camera_photo_request)
    {
        s_camera_photo_request = false;
        s_camera_photo_saving = false;
    }
    s_camera_preview_draw_enabled = false;
    s_camera_preview_direct_crop = false;
    s_camera_preview_stop = false;
    s_camera_preview_task = NULL;
    s_camera_preview_fps_x10 = 0;
    s_camera_preview_width = 0;
    s_camera_preview_height = 0;
    vTaskDelete(NULL);
}

bool camera_preview_start(uint32_t width, uint32_t height)
{
    if (s_camera_preview_task != NULL)
    {
        return !s_camera_preview_stop;
    }

    s_camera_preview_stop = false;
    s_camera_preview_draw_enabled = true;
    s_camera_latest_frame_index = -1;
    s_camera_preview_width = 0;
    s_camera_preview_height = 0;
    s_camera_preview_fps_x10 = 0;
    uintptr_t task_arg = (((uintptr_t)width & 0xffff) << 16) | ((uintptr_t)height & 0xffff);
    BaseType_t ok = xTaskCreate(camera_preview_task,
                                "camera_preview",
                                CAM_PREVIEW_TASK_STACK_SIZE,
                                (void *)task_arg,
                                5,
                                &s_camera_preview_task);
    if (ok != pdPASS)
    {
        s_camera_preview_task = NULL;
        return false;
    }
    return true;
}

void camera_preview_stop(void)
{
    s_camera_preview_draw_enabled = false;
    s_camera_preview_stop = true;
}

bool camera_preview_stop_wait(uint32_t timeout_ms)
{
    s_camera_preview_draw_enabled = false;
    s_camera_preview_stop = true;

    if (s_camera_preview_task == NULL)
    {
        return true;
    }

    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (current == s_camera_preview_task)
    {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (s_camera_preview_task != NULL)
    {
        if (timeout_ms > 0 && (xTaskGetTickCount() - start) >= timeout_ticks)
        {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}
