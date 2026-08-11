#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/ppa.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "lvgl_page_manager.h"
#include "sgm38121.h"
#include "T_Panle_P4_board_config.h"
#include "ui.h"

static const char *TAG = "factory_camera";

#define CAM_VIDEO_DEVICE ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAM_VIDEO_BUFFER_COUNT 3
#define CAM_PREVIEW_FPS 25
#define CAM_UI_BYTES_PER_PIXEL 3
#define CAM_UI_FRAME_BUFFER_COUNT 3
#define CAM_PHOTO_SAVE_WIDTH 1280
#define CAM_PHOTO_SAVE_HEIGHT 720

// #define CAM_UI_FRAME_INTERVAL_US 125000  // 8 fps
// #define CAM_UI_FRAME_INTERVAL_US 100000  // 10 fps
// #define CAM_UI_FRAME_INTERVAL_US 66666   // 15 fps
// #define CAM_UI_FRAME_INTERVAL_US 50000   // 20 fps
#define CAM_UI_FRAME_INTERVAL_US 33333   // 30 fps

#define CAM_UI_DRAW_CHUNK_LINES 16
#define CAM_PREVIEW_TASK_STACK_SIZE 8192
#define CAM_PHOTO_TASK_STACK_SIZE 4096
#define CAM_SD_MOUNT_POINT "/sdcard"
#define CAM_PHOTO_DIR "/sdcard/photo"
#define MIPI_CSI_PHY_PWR_LDO_CHAN 3
#define MIPI_CSI_PHY_PWR_VOLTAGE_MV 2500
#define CAM_CACHE_ALIGN 64
#define CAM_ALIGN_UP(value, align) (((value) + ((align) - 1)) & ~((align) - 1))

typedef struct
{
    int fd;
    void *buffers[CAM_VIDEO_BUFFER_COUNT];
    size_t buffer_lengths[CAM_VIDEO_BUFFER_COUNT];
    uint32_t width;
    uint32_t height;
    uint32_t pix_format;
    uint32_t bytesperline;
    uint32_t sizeimage;
    bool streaming;
} camera_stream_t;

static sgm38121_handle_t s_camera_pmic;
static esp_ldo_channel_handle_t s_camera_ldo_mipi_phy;
static lcd_driver_t *s_camera_lcd;
static camera_stream_t s_camera_stream = {
    .fd = -1,
};
static TaskHandle_t s_camera_preview_task;
static volatile bool s_camera_preview_stop;
static volatile bool s_camera_preview_draw_enabled;
static uint8_t *s_camera_frame_buf[CAM_UI_FRAME_BUFFER_COUNT];
static uint8_t *s_camera_draw_bounce_buf;
static SemaphoreHandle_t s_camera_frame_lock;
static int s_camera_latest_frame_index = -1;
static volatile bool s_camera_photo_request;
static volatile bool s_camera_photo_saving;
static uint32_t s_camera_photo_count;
static int s_camera_preview_x = CAMERA_UI_PREVIEW_X;
static int s_camera_preview_y = CAMERA_UI_PREVIEW_Y;
static int s_camera_preview_w = CAMERA_UI_PREVIEW_WIDTH;
static int s_camera_preview_h = CAMERA_UI_PREVIEW_HEIGHT;
#if CONFIG_IDF_TARGET_ESP32P4
static ppa_client_handle_t s_camera_ppa_srm;
static bool s_camera_ppa_disabled;
static bool s_camera_ppa_disable_logged;
static bool s_camera_ppa_frame_buffers_dma_capable = true;
#endif

static esp_err_t camera_pmic_init(i2c_master_bus_handle_t bus_handle)
{
    ESP_RETURN_ON_ERROR(sgm38121_init(&s_camera_pmic, bus_handle, SGM38121_I2C_ADDR), TAG, "SGM38121 init failed");

    ESP_RETURN_ON_ERROR(sgm38121_set_dvdd1_voltage(&s_camera_pmic, 1500), TAG, "Set DVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd1_voltage(&s_camera_pmic, 2800), TAG, "Set AVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd2_voltage(&s_camera_pmic, 3300), TAG, "Set AVDD2 failed");

    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&s_camera_pmic, SGM38121_CH_DVDD1, SGM38121_SEQ_SLOT_1), TAG, "Set DVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&s_camera_pmic, SGM38121_CH_AVDD1, SGM38121_SEQ_SLOT_2), TAG, "Set AVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&s_camera_pmic, SGM38121_CH_AVDD2, SGM38121_SEQ_SLOT_3), TAG, "Set AVDD2 sequence failed");

    ESP_RETURN_ON_ERROR(sgm38121_seq_powerup(&s_camera_pmic), TAG, "Camera power-up sequence failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t camera_init(i2c_master_bus_handle_t i2c_bus, lcd_driver_t *lcd)
{
    ESP_RETURN_ON_FALSE(i2c_bus != NULL && lcd != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid camera init handle");
    s_camera_lcd = lcd;

    ESP_RETURN_ON_ERROR(camera_pmic_init(i2c_bus), TAG, "Camera PMIC init failed");

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_CSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_CSI_PHY_PWR_VOLTAGE_MV,
    };

    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &s_camera_ldo_mipi_phy);
    if (ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "MIPI PHY LDO channel already acquired, continue");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "MIPI PHY LDO acquire failed");
    ESP_LOGI(TAG, "MIPI PHY LDO%d enabled at %dmV", MIPI_CSI_PHY_PWR_LDO_CHAN, MIPI_CSI_PHY_PWR_VOLTAGE_MV);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = true,
    };
    esp_video_init_config_t video_cfg = {
        .csi = &csi_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_video_init_with_flags(&video_cfg,
                                                  ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP),
                        TAG, "esp_video init failed");
    ESP_LOGI(TAG, "esp_video initialized with MIPI CSI + ISP");
    return ESP_OK;
}

static void camera_stream_close(camera_stream_t *stream)
{
    if (stream == NULL)
    {
        return;
    }

    if (stream->fd >= 0 && stream->streaming)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(stream->fd, VIDIOC_STREAMOFF, &type) != 0)
        {
            ESP_LOGW(TAG, "Camera streamoff failed, errno=%d", errno);
        }
        stream->streaming = false;
    }

    for (uint32_t i = 0; i < CAM_VIDEO_BUFFER_COUNT; i++)
    {
        if (stream->buffers[i] != NULL && stream->buffers[i] != MAP_FAILED)
        {
            munmap(stream->buffers[i], stream->buffer_lengths[i]);
        }
        stream->buffers[i] = NULL;
        stream->buffer_lengths[i] = 0;
    }

    if (stream->fd >= 0)
    {
        close(stream->fd);
        stream->fd = -1;
    }

    stream->width = 0;
    stream->height = 0;
    stream->pix_format = 0;
    stream->bytesperline = 0;
    stream->sizeimage = 0;
}

static uint32_t camera_preview_bytes_per_pixel(uint32_t pix_format)
{
    return pix_format == V4L2_PIX_FMT_RGB24 ? 3 : 2;
}

static size_t camera_stream_min_frame_size(const camera_stream_t *stream)
{
    if (stream == NULL || stream->width == 0 || stream->height == 0)
    {
        return 0;
    }

    uint32_t bpp = camera_preview_bytes_per_pixel(stream->pix_format);
    uint32_t visible_row_bytes = stream->width * bpp;
    uint32_t stride = stream->bytesperline ? stream->bytesperline : visible_row_bytes;
    return (size_t)stride * (stream->height - 1U) + visible_row_bytes;
}

static esp_err_t camera_get_current_format(camera_stream_t *stream)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    ESP_RETURN_ON_FALSE(stream != NULL && stream->fd >= 0, ESP_ERR_INVALID_ARG, TAG, "Invalid stream");
    ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "Get camera current format failed, errno=%d", errno);

    stream->width = format.fmt.pix.width;
    stream->height = format.fmt.pix.height;
    stream->pix_format = format.fmt.pix.pixelformat;
    stream->bytesperline = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline :
                           stream->width * camera_preview_bytes_per_pixel(stream->pix_format);
    stream->sizeimage = format.fmt.pix.sizeimage;

    ESP_LOGI(TAG,
             "Camera current format: %ux%u " V4L2_FMT_STR ", stride=%" PRIu32 ", sizeimage=%" PRIu32,
             stream->width,
             stream->height,
             V4L2_FMT_STR_ARG(stream->pix_format),
             stream->bytesperline,
             stream->sizeimage);

    return ESP_OK;
}

static esp_err_t camera_set_preview_format(camera_stream_t *stream, uint32_t width, uint32_t height, uint32_t pix_format)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = width,
            .height = height,
            .pixelformat = pix_format,
        },
    };

    ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_S_FMT, &format) == 0,
                        ESP_FAIL, TAG, "Set camera format failed, errno=%d", errno);

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "Get camera format failed, errno=%d", errno);

    stream->width = format.fmt.pix.width;
    stream->height = format.fmt.pix.height;
    stream->pix_format = format.fmt.pix.pixelformat;
    stream->bytesperline = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline :
                           stream->width * camera_preview_bytes_per_pixel(stream->pix_format);
    stream->sizeimage = format.fmt.pix.sizeimage;

    ESP_LOGI(TAG,
             "Camera preview format: %ux%u " V4L2_FMT_STR ", stride=%" PRIu32 ", sizeimage=%" PRIu32,
             stream->width,
             stream->height,
             V4L2_FMT_STR_ARG(stream->pix_format),
             stream->bytesperline,
             format.fmt.pix.sizeimage);

    ESP_RETURN_ON_FALSE(stream->width > 0 &&
                            stream->height > 0 &&
                            stream->pix_format == pix_format,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported camera format after S_FMT");
    return ESP_OK;
}

static esp_err_t camera_set_preview_fps(int fd)
{
    struct v4l2_streamparm sparm = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .parm.capture = {
            .capability = V4L2_CAP_TIMEPERFRAME,
            .timeperframe = {
                .numerator = 1,
                .denominator = CAM_PREVIEW_FPS,
            },
        },
    };

    if (ioctl(fd, VIDIOC_S_PARM, &sparm) != 0)
    {
        ESP_LOGW(TAG, "Set camera FPS=%d failed, continue, errno=%d", CAM_PREVIEW_FPS, errno);
    }
    return ESP_OK;
}

static esp_err_t camera_request_buffers(camera_stream_t *stream)
{
    struct v4l2_requestbuffers req = {
        .count = CAM_VIDEO_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "Request camera buffers failed, errno=%d", errno);
    ESP_RETURN_ON_FALSE(req.count >= CAM_VIDEO_BUFFER_COUNT,
                        ESP_ERR_NO_MEM, TAG, "Only %" PRIu32 " camera buffers allocated", req.count);

    for (uint32_t i = 0; i < CAM_VIDEO_BUFFER_COUNT; i++)
    {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_QUERYBUF, &buf) == 0,
                            ESP_FAIL, TAG, "Query camera buffer[%" PRIu32 "] failed, errno=%d", i, errno);

        stream->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, stream->fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(stream->buffers[i] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG, "Map camera buffer[%" PRIu32 "] failed", i);
        stream->buffer_lengths[i] = buf.length;

        ESP_RETURN_ON_FALSE(ioctl(stream->fd, VIDIOC_QBUF, &buf) == 0,
                            ESP_FAIL, TAG, "Queue camera buffer[%" PRIu32 "] failed, errno=%d", i, errno);
    }
    return ESP_OK;
}

static esp_err_t camera_set_stream_format_with_fallback(camera_stream_t *stream, uint32_t width, uint32_t height)
{
    esp_err_t ret = camera_set_preview_format(stream, width, height, V4L2_PIX_FMT_RGB24);
    if (ret == ESP_OK)
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "RGB24 %ux%u is not available, fallback to RGB565", width, height);
    ret = camera_set_preview_format(stream, width, height, V4L2_PIX_FMT_RGB565);
    if (ret == ESP_OK)
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Requested camera size %ux%u is not available, trying current sensor default", width, height);
    ESP_RETURN_ON_ERROR(camera_get_current_format(stream), TAG, "Get current camera format failed");

    uint32_t default_width = stream->width;
    uint32_t default_height = stream->height;

    ret = camera_set_preview_format(stream, default_width, default_height, V4L2_PIX_FMT_RGB24);
    if (ret == ESP_OK)
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "RGB24 %ux%u is not available, fallback to RGB565", default_width, default_height);
    ret = camera_set_preview_format(stream, default_width, default_height, V4L2_PIX_FMT_RGB565);
    if (ret == ESP_OK)
    {
        return ESP_OK;
    }

    return ret;
}

static esp_err_t camera_stream_open(camera_stream_t *stream, uint32_t width, uint32_t height)
{
    esp_err_t ret = ESP_OK;
    struct v4l2_capability capability = {0};

    memset(stream, 0, sizeof(*stream));
    stream->fd = -1;

    stream->fd = open(CAM_VIDEO_DEVICE, O_RDWR | O_NONBLOCK);
    ESP_RETURN_ON_FALSE(stream->fd >= 0, ESP_FAIL, TAG, "Open %s failed, errno=%d", CAM_VIDEO_DEVICE, errno);

    ESP_GOTO_ON_FALSE(ioctl(stream->fd, VIDIOC_QUERYCAP, &capability) == 0,
                      ESP_FAIL, err, TAG, "Query camera capability failed, errno=%d", errno);
    ESP_LOGI(TAG, "Camera device: driver=%s, card=%s, bus=%s",
             capability.driver,
             capability.card,
             capability.bus_info);

    ESP_GOTO_ON_ERROR(camera_set_stream_format_with_fallback(stream, width, height),
                      err,
                      TAG,
                      "Set camera preview format failed");
    ESP_GOTO_ON_ERROR(camera_set_preview_fps(stream->fd), err, TAG, "Set camera preview FPS failed");
    ESP_GOTO_ON_ERROR(camera_request_buffers(stream), err, TAG, "Prepare camera buffers failed");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_FALSE(ioctl(stream->fd, VIDIOC_STREAMON, &type) == 0,
                      ESP_FAIL, err, TAG, "Camera streamon failed, errno=%d", errno);
    stream->streaming = true;
    return ESP_OK;

err:
    camera_stream_close(stream);
    return ESP_FAIL;
}

static bool camera_frame_buffers_alloc(void)
{
    const size_t frame_size = CAMERA_UI_PREVIEW_WIDTH * CAMERA_UI_PREVIEW_HEIGHT * CAM_UI_BYTES_PER_PIXEL;
    const size_t frame_alloc_size = CAM_ALIGN_UP(frame_size, CAM_CACHE_ALIGN);
    const size_t bounce_size = CAMERA_UI_PREVIEW_WIDTH * CAM_UI_DRAW_CHUNK_LINES * CAM_UI_BYTES_PER_PIXEL;
    const size_t bounce_alloc_size = CAM_ALIGN_UP(bounce_size, CAM_CACHE_ALIGN);

    for (int i = 0; i < CAM_UI_FRAME_BUFFER_COUNT; i++)
    {
        if (s_camera_frame_buf[i] == NULL)
        {
            s_camera_frame_buf[i] = heap_caps_aligned_alloc(CAM_CACHE_ALIGN,
                                                            frame_alloc_size,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            if (s_camera_frame_buf[i] == NULL)
            {
                s_camera_frame_buf[i] = heap_caps_aligned_alloc(CAM_CACHE_ALIGN,
                                                                frame_alloc_size,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if CONFIG_IDF_TARGET_ESP32P4
                s_camera_ppa_frame_buffers_dma_capable = false;
#endif
                if (s_camera_frame_buf[i] == NULL)
                {
                    ESP_LOGE(TAG, "Camera UI frame buffer alloc failed");
                    return false;
                }
            }
        }
    }

    if (s_camera_draw_bounce_buf == NULL)
    {
        s_camera_draw_bounce_buf = heap_caps_aligned_alloc(CAM_CACHE_ALIGN,
                                                           bounce_alloc_size,
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_camera_draw_bounce_buf == NULL)
        {
            ESP_LOGW(TAG, "Camera LCD bounce buffer alloc failed, direct PSRAM draw will be used");
        }
        else
        {
            ESP_LOGI(TAG, "Camera LCD bounce buffer: %u bytes internal", (unsigned)bounce_alloc_size);
        }
    }
    return true;
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

static esp_err_t camera_scale_to_lcd_rgb888_ppa(const uint8_t *src,
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
            .buffer_size = CAM_ALIGN_UP(dst_w * dst_h * CAM_UI_BYTES_PER_PIXEL, CAM_CACHE_ALIGN),
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
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

static inline void camera_rgb565_to_lcd_rgb888(uint16_t rgb565, uint8_t *dst)
{
    uint8_t b = (uint8_t)(rgb565 & 0x001f);
    uint8_t g = (uint8_t)((rgb565 >> 5) & 0x003f);
    uint8_t r = (uint8_t)((rgb565 >> 11) & 0x001f);

    dst[0] = (uint8_t)((r << 3) | (r >> 2));
    dst[1] = (uint8_t)((g << 2) | (g >> 4));
    dst[2] = (uint8_t)((b << 3) | (b >> 2));
}

static inline void camera_rgb24_to_lcd_rgb888(const uint8_t *src, uint8_t *dst)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static void camera_scale_step_to_lcd_rgb888(const uint8_t *src,
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
        uint8_t *dst_row = dst + ((size_t)y * dst_w * CAM_UI_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < dst_w; x++)
        {
            const uint8_t *s = src_row + ((size_t)x * step_x * src_bpp);
            uint8_t *d = dst_row + ((size_t)x * CAM_UI_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd_rgb888(s, d);
            }
            else
            {
                uint16_t rgb565 = s[0] | ((uint16_t)s[1] << 8);
                camera_rgb565_to_lcd_rgb888(rgb565, d);
            }
        }
    }
}

static void camera_scale_to_lcd_rgb888(const uint8_t *src,
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
        camera_scale_step_to_lcd_rgb888(src, src_w, src_stride, dst, dst_w, dst_h, 2, 2, pix_format);
        return;
    }

    if (src_w == dst_w * 3 && src_h == dst_h * 3)
    {
        camera_scale_step_to_lcd_rgb888(src, src_w, src_stride, dst, dst_w, dst_h, 3, 3, pix_format);
        return;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t ppa_ret = camera_scale_to_lcd_rgb888_ppa(src, src_w, src_h, src_stride, dst, dst_w, dst_h, pix_format);
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

    for (uint32_t y = 0; y < dst_h; y++)
    {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop_h) / dst_h);
        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / dst_w);
            const uint8_t *s = src + ((size_t)src_y * src_stride) + ((size_t)src_x * src_bpp);
            uint8_t *d = dst + (((size_t)y * dst_w + x) * CAM_UI_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd_rgb888(s, d);
            }
            else
            {
                uint16_t rgb565 = s[0] | ((uint16_t)s[1] << 8);
                camera_rgb565_to_lcd_rgb888(rgb565, d);
            }
        }
    }
}

static esp_err_t camera_preview_draw_direct(const uint8_t *frame)
{
    esp_err_t ret;
    int x = s_camera_preview_x;
    int y = s_camera_preview_y;
    int w = s_camera_preview_w;
    int h = s_camera_preview_h;
    const size_t frame_stride = CAMERA_UI_PREVIEW_WIDTH * CAM_UI_BYTES_PER_PIXEL;
    const size_t row_bytes = (size_t)w * CAM_UI_BYTES_PER_PIXEL;

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

            ret = lcd_jd9365_draw_bitmap(s_camera_lcd,
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
                                          CAMERA_UI_PREVIEW_WIDTH * CAMERA_UI_PREVIEW_HEIGHT * CAM_UI_BYTES_PER_PIXEL,
                                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                          draw_done,
                          TAG,
                          "Camera direct frame cache sync failed");
        ret = lcd_jd9365_draw_bitmap(s_camera_lcd,
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
    const uint32_t row_stride = (width * CAM_UI_BYTES_PER_PIXEL + 3U) & ~3U;
    const uint32_t visible_row_bytes = width * CAM_UI_BYTES_PER_PIXEL;
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
                camera_rgb565_to_lcd_rgb888(rgb565, rgb888);
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
        uint8_t *dst_row = dst_frame + ((size_t)y * dst_w * CAM_UI_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < dst_w; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / dst_w);
            const uint8_t *src = src_row + ((size_t)src_x * src_bpp);
            uint8_t *dst = dst_row + ((size_t)x * CAM_UI_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd_rgb888(src, dst);
            }
            else
            {
                uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
                camera_rgb565_to_lcd_rgb888(rgb565, dst);
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
    const size_t thumb_size = (size_t)CAMERA_UI_LAST_PHOTO_WIDTH * CAMERA_UI_LAST_PHOTO_HEIGHT * CAM_UI_BYTES_PER_PIXEL;

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
        uint8_t *dst_row = thumb + ((size_t)y * CAMERA_UI_LAST_PHOTO_WIDTH * CAM_UI_BYTES_PER_PIXEL);

        for (uint32_t x = 0; x < CAMERA_UI_LAST_PHOTO_WIDTH; x++)
        {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop_w) / CAMERA_UI_LAST_PHOTO_WIDTH);
            const uint8_t *src = src_row + ((size_t)src_x * src_bpp);
            uint8_t *dst = dst_row + ((size_t)x * CAM_UI_BYTES_PER_PIXEL);

            if (pix_format == V4L2_PIX_FMT_RGB24)
            {
                camera_rgb24_to_lcd_rgb888(src, dst);
            }
            else
            {
                uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
                camera_rgb565_to_lcd_rgb888(rgb565, dst);
            }
        }
    }

    camera_page_set_last_photo(thumb, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    free(thumb);
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
        camera_page_set_storage_ready(ret == ESP_OK);
        if (ret == ESP_OK)
        {
            camera_photo_make_path(path, sizeof(path), photo->shot_no);
            int64_t save_start_us = esp_timer_get_time();
            ret = camera_photo_write_bmp(path, photo->frame, photo->width, photo->height, photo->pix_format);
            int64_t save_ms = (esp_timer_get_time() - save_start_us) / 1000;
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "Photo saved: %s (%" PRId64 " ms)", path, save_ms);
                camera_page_set_shot_count(photo->shot_no);
                camera_page_set_status("SAVED", true);
            }
        }
    }

    if (ret != ESP_OK)
    {
        camera_page_set_status(ret == ESP_ERR_NOT_FOUND ? "NO SD" : "SAVE ERR", false);
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

    const size_t frame_size = (size_t)save_w * save_h * CAM_UI_BYTES_PER_PIXEL;
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

    s_camera_photo_saving = true;
    s_camera_photo_request = true;
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
            uint8_t *px = s_camera_draw_bounce_buf + ((size_t)i * CAM_UI_BYTES_PER_PIXEL);
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }

        esp_err_t sync_ret = esp_cache_msync(s_camera_draw_bounce_buf,
                                             (size_t)w * lines * CAM_UI_BYTES_PER_PIXEL,
                                             ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (sync_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Camera clear cache sync failed: %s", esp_err_to_name(sync_ret));
            break;
        }

        esp_err_t draw_ret = lcd_jd9365_draw_bitmap(s_camera_lcd,
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
        camera_page_set_status("NO MEM", false);
        s_camera_preview_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (camera_stream_open(&s_camera_stream, width, height) != ESP_OK)
    {
        camera_page_set_status("NO CAM", false);
        s_camera_preview_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    camera_page_set_status("STREAM", true);
    camera_page_set_preview_info(s_camera_stream.width, s_camera_stream.height, 0);
    ESP_LOGI(TAG, "Camera preview started, request=%ux%u actual=%ux%u",
             width,
             height,
             s_camera_stream.width,
             s_camera_stream.height);

    while (!s_camera_preview_stop)
    {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_camera_stream.fd, VIDIOC_DQBUF, &buf) != 0)
        {
            if (errno == EAGAIN)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            ESP_LOGW(TAG, "Camera DQBUF failed, errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if ((buf.flags & V4L2_BUF_FLAG_DONE) && buf.index < CAM_VIDEO_BUFFER_COUNT)
        {
            size_t expected = camera_stream_min_frame_size(&s_camera_stream);
            int64_t now_us = esp_timer_get_time();

            if (s_camera_preview_stop)
            {
                if (ioctl(s_camera_stream.fd, VIDIOC_QBUF, &buf) != 0)
                {
                    ESP_LOGW(TAG, "Camera QBUF before stop failed, errno=%d", errno);
                }
                break;
            }

            bool should_draw = s_camera_preview_draw_enabled &&
                               buf.bytesused >= expected &&
                               now_us - last_ui_frame_us >= CAM_UI_FRAME_INTERVAL_US;
            if (s_camera_photo_request && buf.bytesused >= expected)
            {
                esp_err_t photo_ret = camera_photo_submit_frame((const uint8_t *)s_camera_stream.buffers[buf.index],
                                                                s_camera_stream.width,
                                                                s_camera_stream.height,
                                                                s_camera_stream.pix_format,
                                                                s_camera_stream.bytesperline,
                                                                buf.bytesused);
                s_camera_photo_request = false;
                if (photo_ret != ESP_OK)
                {
                    s_camera_photo_saving = false;
                    camera_page_set_status(photo_ret == ESP_ERR_NO_MEM ? "NO MEM" : "SAVE ERR", false);
                    ESP_LOGW(TAG, "Submit photo frame failed: %s", esp_err_to_name(photo_ret));
                }
                else
                {
                    ESP_LOGI(TAG,
                             "Photo capture frame: %ux%u " V4L2_FMT_STR,
                             s_camera_stream.width,
                             s_camera_stream.height,
                             V4L2_FMT_STR_ARG(s_camera_stream.pix_format));
                }
            }

            if (should_draw)
            {
                int write_index = camera_frame_buffer_next_write();
                if (write_index >= 0)
                {
                    bool frame_ready = false;
                    if (xSemaphoreTake(s_camera_frame_lock, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        camera_scale_to_lcd_rgb888((const uint8_t *)s_camera_stream.buffers[buf.index],
                                                   s_camera_stream.width,
                                                   s_camera_stream.height,
                                                   s_camera_stream.bytesperline,
                                                   s_camera_frame_buf[write_index],
                                                   CAMERA_UI_PREVIEW_WIDTH,
                                                   CAMERA_UI_PREVIEW_HEIGHT,
                                                   s_camera_stream.pix_format);
                        s_camera_latest_frame_index = write_index;
                        frame_ready = true;
                        xSemaphoreGive(s_camera_frame_lock);
                    }
                    if (s_camera_preview_draw_enabled && !s_camera_preview_stop)
                    {
                        if (frame_ready)
                        {
                            camera_preview_draw_direct(s_camera_frame_buf[write_index]);
                            last_ui_frame_us = now_us;
                            frame_count++;
                        }
                    }
                }
            }

            if (now_us - fps_start_us >= 2000000)
            {
                float fps = (float)frame_count * 1000000.0f / (float)(now_us - fps_start_us);
                uint32_t fps_x10 = (uint32_t)(fps * 10.0f + 0.5f);
                camera_page_set_preview_info(s_camera_stream.width, s_camera_stream.height, fps_x10);
                ESP_LOGI(TAG, "Camera preview draw FPS: %.1f", fps);
                frame_count = 0;
                fps_start_us = now_us;
            }
        }
        else if (buf.flags & V4L2_BUF_FLAG_ERROR)
        {
            ESP_LOGW(TAG, "Camera buffer has error flag");
        }

        if (ioctl(s_camera_stream.fd, VIDIOC_QBUF, &buf) != 0)
        {
            ESP_LOGW(TAG, "Camera QBUF failed, errno=%d", errno);
        }
        vTaskDelay(1);
    }

    camera_stream_close(&s_camera_stream);
    ESP_LOGI(TAG, "Camera preview stopped");
    if (s_camera_photo_request)
    {
        s_camera_photo_request = false;
        s_camera_photo_saving = false;
        camera_page_set_status("STOPPED", false);
    }
    s_camera_preview_draw_enabled = false;
    s_camera_preview_stop = false;
    s_camera_preview_task = NULL;
    vTaskDelete(NULL);
}

bool camera_preview_start(uint32_t width, uint32_t height)
{
    if (s_camera_preview_task != NULL)
    {
        camera_page_set_status(s_camera_preview_stop ? "STOPPING" : "STREAM", !s_camera_preview_stop);
        return !s_camera_preview_stop;
    }

    s_camera_preview_stop = false;
    s_camera_preview_draw_enabled = true;
    s_camera_latest_frame_index = -1;
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
        camera_page_set_status("TASK ERR", false);
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
