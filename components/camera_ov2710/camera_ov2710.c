#include "camera_ov2710.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sgm38121.h"

#define CAMERA_OV2710_DEVICE ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAMERA_OV2710_SCCB_FREQ_HZ 400000

static const char *TAG = "camera_ov2710";

struct camera_ov2710 {
    int fd;
    void *buffers[CAMERA_OV2710_MAX_BUFFER_COUNT];
    size_t buffer_lengths[CAMERA_OV2710_MAX_BUFFER_COUNT];
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t v4l2_pixel_format;
    camera_ov2710_pixel_format_t pixel_format;
    bool streaming;
    bool video_initialized;
    bool pmic_initialized;
    sgm38121_handle_t pmic;
};

static uint32_t camera_pixel_format_to_v4l2(camera_ov2710_pixel_format_t format)
{
    return format == CAMERA_OV2710_PIXEL_FORMAT_RGB888
               ? V4L2_PIX_FMT_RGB24
               : V4L2_PIX_FMT_RGB565;
}

static esp_err_t camera_power_init(struct camera_ov2710 *camera,
                                   i2c_master_bus_handle_t i2c_bus)
{
    ESP_RETURN_ON_ERROR(sgm38121_init(&camera->pmic, i2c_bus, SGM38121_I2C_ADDR),
                        TAG, "SGM38121 init failed");
    camera->pmic_initialized = true;

    ESP_RETURN_ON_ERROR(sgm38121_set_dvdd1_voltage(&camera->pmic, 1500),
                        TAG, "set DVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd1_voltage(&camera->pmic, 2800),
                        TAG, "set AVDD1 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_avdd2_voltage(&camera->pmic, 3300),
                        TAG, "set AVDD2 failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&camera->pmic, SGM38121_CH_DVDD1,
                                              SGM38121_SEQ_SLOT_1),
                        TAG, "set DVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&camera->pmic, SGM38121_CH_AVDD1,
                                              SGM38121_SEQ_SLOT_2),
                        TAG, "set AVDD1 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_set_sequence(&camera->pmic, SGM38121_CH_AVDD2,
                                              SGM38121_SEQ_SLOT_3),
                        TAG, "set AVDD2 sequence failed");
    ESP_RETURN_ON_ERROR(sgm38121_seq_powerup(&camera->pmic),
                        TAG, "camera power-up sequence failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

static esp_err_t camera_video_init(struct camera_ov2710 *camera,
                                   i2c_master_bus_handle_t i2c_bus)
{
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = CAMERA_OV2710_SCCB_FREQ_HZ,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = true,
    };
    esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    ESP_RETURN_ON_ERROR(esp_video_init_with_flags(
                            &video_config,
                            ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP),
                        TAG, "esp_video init failed");
    camera->video_initialized = true;
    ESP_LOGI(TAG, "OV2710 initialized with MIPI CSI + ISP");
    return ESP_OK;
}

static void camera_log_formats(int fd)
{
    for (uint32_t index = 0;; ++index) {
        struct v4l2_fmtdesc format = {
            .index = index,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &format) != 0) {
            break;
        }
        ESP_LOGI(TAG, "format[%" PRIu32 "]: " V4L2_FMT_STR " %s",
                 index, V4L2_FMT_STR_ARG(format.pixelformat), format.description);
    }
}

static esp_err_t camera_set_format(struct camera_ov2710 *camera,
                                   const camera_ov2710_config_t *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = config->width,
            .height = config->height,
            .pixelformat = camera_pixel_format_to_v4l2(config->pixel_format),
        },
    };

    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_S_FMT, &format) == 0,
                        ESP_FAIL, TAG, "set format failed, errno=%d", errno);

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "get format failed, errno=%d", errno);

    camera->width = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;
    const uint32_t bytes_per_pixel =
        config->pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB888 ? 3U : 2U;
    camera->bytes_per_line = format.fmt.pix.bytesperline
                                 ? format.fmt.pix.bytesperline
                                 : camera->width * bytes_per_pixel;
    camera->v4l2_pixel_format = format.fmt.pix.pixelformat;
    camera->pixel_format = config->pixel_format;
    ESP_LOGI(TAG, "stream format: %ux%u " V4L2_FMT_STR
             ", stride=%" PRIu32 ", size=%" PRIu32,
             camera->width, camera->height,
             V4L2_FMT_STR_ARG(camera->v4l2_pixel_format),
             format.fmt.pix.bytesperline, format.fmt.pix.sizeimage);

    ESP_RETURN_ON_FALSE(camera->width == config->width &&
                            camera->height == config->height &&
                            camera->v4l2_pixel_format ==
                                camera_pixel_format_to_v4l2(config->pixel_format),
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "unexpected format returned by esp_video");
    return ESP_OK;
}

static esp_err_t camera_set_fps(struct camera_ov2710 *camera, uint32_t fps)
{
    struct v4l2_streamparm parameters = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .parm.capture = {
            .capability = V4L2_CAP_TIMEPERFRAME,
            .timeperframe = {
                .numerator = 1,
                .denominator = fps,
            },
        },
    };
    if (ioctl(camera->fd, VIDIOC_S_PARM, &parameters) != 0) {
        ESP_LOGW(TAG, "set FPS=%" PRIu32 " failed, use sensor default, errno=%d",
                 fps, errno);
    } else {
        ESP_LOGI(TAG, "stream FPS requested: %" PRIu32, fps);
    }
    return ESP_OK;
}

static esp_err_t camera_prepare_buffers(struct camera_ov2710 *camera,
                                        uint32_t buffer_count)
{
    struct v4l2_requestbuffers request = {
        .count = buffer_count,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_REQBUFS, &request) == 0,
                        ESP_FAIL, TAG, "request buffers failed, errno=%d", errno);
    ESP_RETURN_ON_FALSE(request.count >= buffer_count,
                        ESP_ERR_NO_MEM, TAG, "only %" PRIu32 " buffers allocated",
                        request.count);

    camera->buffer_count = buffer_count;
    for (uint32_t index = 0; index < buffer_count; ++index) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = index,
        };
        ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) == 0,
                            ESP_FAIL, TAG,
                            "query buffer[%" PRIu32 "] failed, errno=%d",
                            index, errno);

        camera->buffers[index] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, camera->fd, buffer.m.offset);
        ESP_RETURN_ON_FALSE(camera->buffers[index] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG,
                            "map buffer[%" PRIu32 "] failed", index);
        camera->buffer_lengths[index] = buffer.length;
        ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_QBUF, &buffer) == 0,
                            ESP_FAIL, TAG,
                            "queue buffer[%" PRIu32 "] failed, errno=%d",
                            index, errno);
    }
    ESP_LOGI(TAG, "video buffers queued: %" PRIu32, buffer_count);
    return ESP_OK;
}

static esp_err_t camera_stream_open(struct camera_ov2710 *camera,
                                    const camera_ov2710_config_t *config)
{
    struct v4l2_capability capability = {0};
    camera->fd = open(CAMERA_OV2710_DEVICE, O_RDWR);
    ESP_RETURN_ON_FALSE(camera->fd >= 0, ESP_FAIL, TAG,
                        "open %s failed, errno=%d", CAMERA_OV2710_DEVICE, errno);
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_QUERYCAP, &capability) == 0,
                        ESP_FAIL, TAG, "query capability failed, errno=%d", errno);
    ESP_LOGI(TAG, "device: driver=%s, card=%s, bus=%s",
             capability.driver, capability.card, capability.bus_info);

    if (config->log_supported_formats) {
        camera_log_formats(camera->fd);
    }
    ESP_RETURN_ON_ERROR(camera_set_format(camera, config),
                        TAG, "set stream format failed");
    ESP_RETURN_ON_ERROR(camera_set_fps(camera, config->fps),
                        TAG, "set stream FPS failed");
    ESP_RETURN_ON_ERROR(camera_prepare_buffers(camera, config->buffer_count),
                        TAG, "prepare stream buffers failed");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "start stream failed, errno=%d", errno);
    camera->streaming = true;
    ESP_LOGI(TAG, "OV2710 stream started");
    return ESP_OK;
}

esp_err_t camera_ov2710_init(t_panel_p4_bsp_t *bsp,
                             const camera_ov2710_config_t *config,
                             camera_ov2710_handle_t *ret_camera)
{
    ESP_RETURN_ON_FALSE(bsp && config && ret_camera,
                        ESP_ERR_INVALID_ARG, TAG, "invalid init arguments");
    i2c_master_bus_handle_t i2c_bus = t_panel_p4_bsp_get_i2c_bus(bsp);
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "BSP I2C bus is not initialized");
    ESP_RETURN_ON_FALSE(config->width > 0 && config->height > 0 && config->fps > 0 &&
                            config->buffer_count > 0 &&
                            config->buffer_count <= CAMERA_OV2710_MAX_BUFFER_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid stream configuration");
    ESP_RETURN_ON_FALSE(config->pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB565 ||
                            config->pixel_format == CAMERA_OV2710_PIXEL_FORMAT_RGB888,
                        ESP_ERR_INVALID_ARG, TAG, "invalid pixel format");

    struct camera_ov2710 *camera = calloc(1, sizeof(*camera));
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_NO_MEM, TAG, "allocate camera handle failed");
    camera->fd = -1;

    esp_err_t ret = t_panel_p4_bsp_mipi_phy_init(bsp);
    if (ret == ESP_OK) {
        ret = camera_power_init(camera, i2c_bus);
    }
    if (ret == ESP_OK) {
        ret = camera_video_init(camera, i2c_bus);
    }
    if (ret == ESP_OK) {
        ret = camera_stream_open(camera, config);
    }
    if (ret != ESP_OK) {
        camera_ov2710_deinit(camera);
        return ret;
    }

    *ret_camera = camera;
    return ESP_OK;
}

esp_err_t camera_ov2710_get_frame(camera_ov2710_handle_t camera,
                                  camera_ov2710_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(camera && frame && camera->streaming,
                        ESP_ERR_INVALID_ARG, TAG, "invalid get-frame arguments");
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_DQBUF, &buffer) == 0,
                        ESP_FAIL, TAG, "dequeue frame failed, errno=%d", errno);
    ESP_RETURN_ON_FALSE(buffer.index < camera->buffer_count,
                        ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid frame index %" PRIu32, buffer.index);

    *frame = (camera_ov2710_frame_t) {
        .data = camera->buffers[buffer.index],
        .bytes_used = buffer.bytesused,
        .width = camera->width,
        .height = camera->height,
        .bytes_per_line = camera->bytes_per_line,
        .index = buffer.index,
        .pixel_format = camera->pixel_format,
        .error = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0,
    };
    return ESP_OK;
}

esp_err_t camera_ov2710_return_frame(camera_ov2710_handle_t camera,
                                     const camera_ov2710_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(camera && frame && camera->streaming &&
                            frame->index < camera->buffer_count &&
                            frame->data == camera->buffers[frame->index],
                        ESP_ERR_INVALID_ARG, TAG, "invalid return-frame arguments");
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = frame->index,
    };
    ESP_RETURN_ON_FALSE(ioctl(camera->fd, VIDIOC_QBUF, &buffer) == 0,
                        ESP_FAIL, TAG, "return frame failed, errno=%d", errno);
    return ESP_OK;
}

esp_err_t camera_ov2710_deinit(camera_ov2710_handle_t camera)
{
    if (!camera) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    if (camera->streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(camera->fd, VIDIOC_STREAMOFF, &type) != 0) {
            result = ESP_FAIL;
        }
        camera->streaming = false;
    }
    for (uint32_t index = 0; index < camera->buffer_count; ++index) {
        if (camera->buffers[index] && camera->buffers[index] != MAP_FAILED) {
            munmap(camera->buffers[index], camera->buffer_lengths[index]);
        }
    }
    if (camera->fd >= 0) {
        close(camera->fd);
    }
    if (camera->video_initialized) {
        esp_err_t ret = esp_video_deinit_with_flags(
            ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP);
        if (result == ESP_OK) {
            result = ret;
        }
    }
    if (camera->pmic_initialized) {
        esp_err_t ret = sgm38121_seq_shutdown(&camera->pmic);
        if (result == ESP_OK) {
            result = ret;
        }
        ret = sgm38121_deinit(&camera->pmic);
        if (result == ESP_OK) {
            result = ret;
        }
    }
    free(camera);
    return result;
}
