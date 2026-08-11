/*
 * ESP32-P4 + OV2710 MIPI CSI preview through esp_video/ISP.
 *
 * This example keeps the low-level CSI example untouched and uses the V4L2-like
 * esp_video path instead. The CSI video device starts the ISP internally when a
 * non-RAW format such as RGB24 is requested from a RAW10 sensor.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "T_Panle_P4_board_config.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "sgm38121.h"

#include "lcd_jd9365_driver.h"

static const char *TAG = "camera_esp_video";

#define CAM_VIDEO_DEVICE              ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAM_VIDEO_BUFFER_COUNT        3
#define CAM_SENSOR_WIDTH              1280
#define CAM_SENSOR_HEIGHT             720
#define CAM_PREVIEW_WIDTH             LCD_H_RES
#define CAM_PREVIEW_HEIGHT            LCD_V_RES
#define CAM_PREVIEW_FPS               25
#define CAM_PREVIEW_PIX_FORMAT        V4L2_PIX_FMT_RGB565
#define CAM_PREVIEW_BYTES_PER_PIXEL   2
#define CAM_LCD_BYTES_PER_PIXEL       (LCD_BIT_PER_PIXEL / 8)
#define CAM_LCD_BOTTOM_GUARD_LINES    2
#define CAM_PREVIEW_CROP_LEFT         ((CAM_SENSOR_WIDTH - CAM_PREVIEW_WIDTH) / 2)
#define CAM_PREVIEW_CROP_TOP          ((CAM_SENSOR_HEIGHT - CAM_PREVIEW_HEIGHT) / 2)
#define MIPI_CSI_PHY_PWR_LDO_CHAN     3
#define MIPI_CSI_PHY_PWR_VOLTAGE_MV   2500

typedef struct {
    int fd;
    void *buffers[CAM_VIDEO_BUFFER_COUNT];
    size_t buffer_lengths[CAM_VIDEO_BUFFER_COUNT];
    uint32_t width;
    uint32_t height;
    uint32_t pix_format;
    bool streaming;
} video_stream_t;

static sgm38121_handle_t pmic;
static esp_io_expander_handle_t expander = NULL;
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static lcd_driver_t lcd = {};
static video_stream_t stream = {
    .fd = -1,
};
static bool use_isp_crop = false;

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

static esp_err_t init_mipi_phy_ldo(void)
{
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_CSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_CSI_PHY_PWR_VOLTAGE_MV,
    };

    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MIPI PHY LDO channel already acquired, continue");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "MIPI PHY LDO acquire failed");
    ESP_LOGI(TAG, "MIPI PHY LDO%d enabled at %dmV", MIPI_CSI_PHY_PWR_LDO_CHAN, MIPI_CSI_PHY_PWR_VOLTAGE_MV);
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

static esp_err_t init_board_i2c(i2c_master_bus_handle_t *ret_bus)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, ret_bus), TAG, "I2C bus init failed");
    return ESP_OK;
}

static esp_err_t init_video_system(i2c_master_bus_handle_t i2c_bus)
{
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

static esp_err_t init_lcd(void)
{
    ESP_RETURN_ON_ERROR(lcd_jd9365_init(&lcd, expander), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "LCD backlight init failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_set_brightness(60), TAG, "LCD backlight set failed");
    ESP_LOGI(TAG, "LCD ready: %ux%u BGR888", LCD_H_RES, LCD_V_RES);

    return ESP_OK;
}

static esp_err_t show_lcd_test_pattern(void)
{
    uint8_t *lcd_fb = lcd_jd9365_get_next_frame_buffer(&lcd);
    ESP_RETURN_ON_FALSE(lcd_fb, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    const uint32_t stripe_h = LCD_V_RES / 3;
    uint8_t *p = lcd_fb;
    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        uint8_t b = 0xff;
        uint8_t g = 0;
        uint8_t r = 0;

        if (y < stripe_h) {
            b = 0;
            r = 0xff;
        } else if (y < stripe_h * 2) {
            b = 0;
            g = 0xff;
        }

        for (uint32_t x = 0; x < LCD_H_RES; x++) {
            *p++ = b;
            *p++ = g;
            *p++ = r;
        }
    }

    ESP_RETURN_ON_ERROR(lcd_jd9365_draw_bitmap(&lcd, 0, 0, LCD_H_RES, LCD_V_RES, lcd_fb),
                        TAG, "LCD test pattern draw failed");
    ESP_LOGI(TAG, "LCD BGR888 test pattern displayed");
    vTaskDelay(pdMS_TO_TICKS(500));

    return ESP_OK;
}

static void log_video_formats(int fd)
{
    struct v4l2_fmtdesc fmtdesc = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    for (uint32_t i = 0; ; i++) {
        memset(&fmtdesc, 0, sizeof(fmtdesc));
        fmtdesc.index = i;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
            break;
        }

        ESP_LOGI(TAG, "format[%" PRIu32 "]: " V4L2_FMT_STR " %s",
                 i, V4L2_FMT_STR_ARG(fmtdesc.pixelformat), fmtdesc.description);
    }
}

static esp_err_t set_center_crop(int fd)
{
    struct v4l2_selection selection = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .target = V4L2_SEL_TGT_CROP,
        .r = {
            .left = CAM_PREVIEW_CROP_LEFT,
            .top = CAM_PREVIEW_CROP_TOP,
            .width = CAM_PREVIEW_WIDTH,
            .height = CAM_PREVIEW_HEIGHT,
        },
    };

    if (ioctl(fd, VIDIOC_S_SELECTION, &selection) != 0) {
        ESP_LOGW(TAG, "ISP crop is not available, use sensor-size ISP output and software center crop/scale, errno=%d", errno);
        use_isp_crop = false;
        return ESP_OK;
    }

    use_isp_crop = true;
    ESP_LOGI(TAG, "Center crop set: left=%d, top=%d, %dx%d",
             selection.r.left, selection.r.top, selection.r.width, selection.r.height);

    return ESP_OK;
}

static esp_err_t set_preview_format(video_stream_t *s)
{
    uint32_t width = use_isp_crop ? CAM_PREVIEW_WIDTH : CAM_SENSOR_WIDTH;
    uint32_t height = use_isp_crop ? CAM_PREVIEW_HEIGHT : CAM_SENSOR_HEIGHT;
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = width,
            .height = height,
            .pixelformat = CAM_PREVIEW_PIX_FORMAT,
        },
    };

    ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_S_FMT, &format) == 0,
                        ESP_FAIL, TAG, "Set preview format failed, errno=%d", errno);

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "Get preview format failed, errno=%d", errno);

    s->width = format.fmt.pix.width;
    s->height = format.fmt.pix.height;
    s->pix_format = format.fmt.pix.pixelformat;

    ESP_LOGI(TAG, "Preview format: %ux%u " V4L2_FMT_STR ", sizeimage=%" PRIu32,
             s->width, s->height, V4L2_FMT_STR_ARG(s->pix_format), format.fmt.pix.sizeimage);
    ESP_RETURN_ON_FALSE(s->width == width &&
                        s->height == height &&
                        s->pix_format == CAM_PREVIEW_PIX_FORMAT,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Unexpected video format after S_FMT");

    return ESP_OK;
}

static esp_err_t set_preview_fps(int fd)
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

    if (ioctl(fd, VIDIOC_S_PARM, &sparm) != 0) {
        ESP_LOGW(TAG, "Set FPS=%d failed, continue with sensor default, errno=%d", CAM_PREVIEW_FPS, errno);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Preview FPS requested: %d", CAM_PREVIEW_FPS);
    return ESP_OK;
}

static esp_err_t request_and_queue_buffers(video_stream_t *s)
{
    struct v4l2_requestbuffers req = {
        .count = CAM_VIDEO_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "Request video buffers failed, errno=%d", errno);
    ESP_RETURN_ON_FALSE(req.count >= CAM_VIDEO_BUFFER_COUNT,
                        ESP_ERR_NO_MEM, TAG, "Only %" PRIu32 " buffers allocated", req.count);

    for (uint32_t i = 0; i < CAM_VIDEO_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_QUERYBUF, &buf) == 0,
                            ESP_FAIL, TAG, "Query buffer[%" PRIu32 "] failed, errno=%d", i, errno);

        s->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s->buffers[i] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG, "Map buffer[%" PRIu32 "] failed", i);
        s->buffer_lengths[i] = buf.length;

        ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_QBUF, &buf) == 0,
                            ESP_FAIL, TAG, "Queue buffer[%" PRIu32 "] failed, errno=%d", i, errno);
    }

    ESP_LOGI(TAG, "Video buffers queued: %d", CAM_VIDEO_BUFFER_COUNT);
    return ESP_OK;
}

static esp_err_t open_video_stream(video_stream_t *s)
{
    struct v4l2_capability capability = {};

    s->fd = open(CAM_VIDEO_DEVICE, O_RDWR);
    ESP_RETURN_ON_FALSE(s->fd >= 0, ESP_FAIL, TAG, "Open %s failed, errno=%d", CAM_VIDEO_DEVICE, errno);

    ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_QUERYCAP, &capability) == 0,
                        ESP_FAIL, TAG, "Query capability failed, errno=%d", errno);
    ESP_LOGI(TAG, "Video device: driver=%s, card=%s, bus=%s",
             capability.driver, capability.card, capability.bus_info);
    log_video_formats(s->fd);

    ESP_RETURN_ON_ERROR(set_center_crop(s->fd), TAG, "Set crop failed");
    ESP_RETURN_ON_ERROR(set_preview_format(s), TAG, "Set preview format failed");
    ESP_RETURN_ON_ERROR(set_preview_fps(s->fd), TAG, "Set FPS failed");
    ESP_RETURN_ON_ERROR(request_and_queue_buffers(s), TAG, "Queue buffers failed");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "Start stream failed, errno=%d", errno);
    s->streaming = true;
    ESP_LOGI(TAG, "Video stream started");

    return ESP_OK;
}

static inline void rgb565_to_lcd_bgr888(const uint8_t *src, uint8_t *dst)
{
    uint16_t rgb565 = src[0] | ((uint16_t)src[1] << 8);
    dst[0] = (uint8_t)(((rgb565 & 0x001f) * 255) / 31);
    dst[1] = (uint8_t)((((rgb565 >> 5) & 0x003f) * 255) / 63);
    dst[2] = (uint8_t)((((rgb565 >> 11) & 0x001f) * 255) / 31);
}

static void copy_video_rgb565_to_lcd(const uint8_t *src, uint8_t *dst, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++) {
        rgb565_to_lcd_bgr888(src, dst);
        src += CAM_PREVIEW_BYTES_PER_PIXEL;
        dst += CAM_LCD_BYTES_PER_PIXEL;
    }
}

static void crop_center_rgb565_to_lcd(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint8_t *dst)
{
    const uint32_t crop_x = (src_w - LCD_H_RES) / 2;
    const uint32_t crop_y = (src_h - LCD_V_RES) / 2;

    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        const uint8_t *src_row = src + (((size_t)(crop_y + y) * src_w + crop_x) * CAM_PREVIEW_BYTES_PER_PIXEL);
        uint8_t *dst_row = dst + ((size_t)y * LCD_H_RES * CAM_LCD_BYTES_PER_PIXEL);
        copy_video_rgb565_to_lcd(src_row, dst_row, LCD_H_RES);
    }
}

static void scale_center_rgb565_to_lcd(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint8_t *dst)
{
    uint32_t crop = src_h;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;

    if (src_w > src_h) {
        crop_x = (src_w - src_h) / 2;
    } else if (src_h > src_w) {
        crop = src_w;
        crop_y = (src_h - src_w) / 2;
    }

    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        uint32_t src_y = crop_y + (uint32_t)(((uint64_t)y * crop) / LCD_V_RES);
        for (uint32_t x = 0; x < LCD_H_RES; x++) {
            uint32_t src_x = crop_x + (uint32_t)(((uint64_t)x * crop) / LCD_H_RES);
            const uint8_t *s = src + ((size_t)src_y * src_w + src_x) * CAM_PREVIEW_BYTES_PER_PIXEL;
            uint8_t *d = dst + ((size_t)y * LCD_H_RES + x) * CAM_LCD_BYTES_PER_PIXEL;

            rgb565_to_lcd_bgr888(s, d);
        }
    }
}

static void clean_lcd_bottom_edge(uint8_t *lcd_fb)
{
    if (CAM_LCD_BOTTOM_GUARD_LINES == 0 || CAM_LCD_BOTTOM_GUARD_LINES >= LCD_V_RES) {
        return;
    }

    const size_t line_bytes = LCD_H_RES * CAM_LCD_BYTES_PER_PIXEL;
    uint8_t *guard = lcd_fb + ((size_t)(LCD_V_RES - CAM_LCD_BOTTOM_GUARD_LINES) * line_bytes);
    memset(guard, 0, CAM_LCD_BOTTOM_GUARD_LINES * line_bytes);
}

static esp_err_t draw_video_frame(video_stream_t *s, const uint8_t *video_frame, size_t bytesused)
{
    size_t expected_size = (size_t)s->width * s->height * CAM_PREVIEW_BYTES_PER_PIXEL;
    ESP_RETURN_ON_FALSE(bytesused >= expected_size, ESP_ERR_INVALID_SIZE,
                        TAG, "Frame too small: %u < %u", (unsigned)bytesused, (unsigned)expected_size);

    uint8_t *lcd_fb = lcd_jd9365_get_next_frame_buffer(&lcd);
    ESP_RETURN_ON_FALSE(lcd_fb, ESP_ERR_INVALID_STATE, TAG, "LCD framebuffer is NULL");

    if (s->width == LCD_H_RES && s->height == LCD_V_RES) {
        copy_video_rgb565_to_lcd(video_frame, lcd_fb, (size_t)LCD_H_RES * LCD_V_RES);
    } else if (s->width >= LCD_H_RES && s->height >= LCD_V_RES && s->height == LCD_V_RES) {
        crop_center_rgb565_to_lcd(video_frame, s->width, s->height, lcd_fb);
    } else {
        scale_center_rgb565_to_lcd(video_frame, s->width, s->height, lcd_fb);
    }

    clean_lcd_bottom_edge(lcd_fb);
    ESP_RETURN_ON_ERROR(esp_cache_msync(lcd_fb,
                                        LCD_H_RES * LCD_V_RES * CAM_LCD_BYTES_PER_PIXEL,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                        TAG, "LCD framebuffer cache sync failed");

    ESP_RETURN_ON_ERROR(lcd_jd9365_draw_bitmap(&lcd, 0, 0, LCD_H_RES, LCD_V_RES, lcd_fb),
                        TAG, "LCD draw failed");

    return ESP_OK;
}

static esp_err_t preview_loop(video_stream_t *s)
{
    uint32_t frame_count = 0;
    int64_t fps_start_us = esp_timer_get_time();

    while (1) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_DQBUF, &buf) == 0,
                            ESP_FAIL, TAG, "DQBUF failed, errno=%d", errno);

        if ((buf.flags & V4L2_BUF_FLAG_DONE) && buf.index < CAM_VIDEO_BUFFER_COUNT) {
            esp_err_t ret = draw_video_frame(s, (const uint8_t *)s->buffers[buf.index], buf.bytesused);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Draw frame failed: %s", esp_err_to_name(ret));
            }

            frame_count++;
            int64_t now_us = esp_timer_get_time();
            if (now_us - fps_start_us >= 2000000) {
                float fps = (float)frame_count * 1000000.0f / (float)(now_us - fps_start_us);
                ESP_LOGI(TAG, "Preview FPS: %.1f, last bytes=%" PRIu32, fps, buf.bytesused);
                frame_count = 0;
                fps_start_us = now_us;
            }
        } else if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            ESP_LOGW(TAG, "Video buffer has error flag");
        }

        ESP_RETURN_ON_FALSE(ioctl(s->fd, VIDIOC_QBUF, &buf) == 0,
                            ESP_FAIL, TAG, "QBUF failed, errno=%d", errno);
        vTaskDelay(1);
    }
}

void app_main(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;

    ESP_ERROR_CHECK(init_board_i2c(&i2c_bus));
    ESP_ERROR_CHECK(init_sgm38121(i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(i2c_bus, XL9555_I2C_ADDR, &expander));
    ESP_ERROR_CHECK(init_mipi_phy_ldo());
    ESP_ERROR_CHECK(init_video_system(i2c_bus));
    ESP_ERROR_CHECK(init_lcd());
    ESP_ERROR_CHECK(show_lcd_test_pattern());
    ESP_ERROR_CHECK(open_video_stream(&stream));
    ESP_ERROR_CHECK(preview_loop(&stream));
}
