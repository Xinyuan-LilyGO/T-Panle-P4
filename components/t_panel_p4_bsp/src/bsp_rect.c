#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp_private.h"

static const char *TAG = "t_panel_p4_bsp";

#define CAMERA_FLASH_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define CAMERA_FLASH_LEDC_TIMER      LEDC_TIMER_2
#define CAMERA_FLASH_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define CAMERA_FLASH_LEDC_FREQ_HZ    20000
#define CAMERA_FLASH_LEDC_MAX_DUTY   ((1U << 10) - 1U)

static const gpio_num_t s_camera_flash_gpio[T_PANEL_P4_CAMERA_FLASH_COUNT] = {
    [T_PANEL_P4_CAMERA_FLASH_1] = (gpio_num_t)CAMERA_FALSH1_EN_PIN,
    [T_PANEL_P4_CAMERA_FLASH_2] = (gpio_num_t)CAMERA_FALSH2_EN_PIN,
};

static const ledc_channel_t s_camera_flash_channel[T_PANEL_P4_CAMERA_FLASH_COUNT] = {
    [T_PANEL_P4_CAMERA_FLASH_1] = LEDC_CHANNEL_4,
    [T_PANEL_P4_CAMERA_FLASH_2] = LEDC_CHANNEL_5,
};

esp_err_t t_panel_p4_bsp_board_init(t_panel_p4_bsp_t *bsp)
{
    bsp->io_expander = NULL;
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_board_deinit(t_panel_p4_bsp_t *bsp)
{
    (void)bsp;
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_board_speaker_set(t_panel_p4_bsp_t *bsp, bool enabled)
{
    (void)bsp;
    esp_err_t ret = gpio_set_direction((gpio_num_t)AUDIO_PA_EN_PIN, GPIO_MODE_OUTPUT);
    if (ret == ESP_OK) {
        ret = gpio_set_level((gpio_num_t)AUDIO_PA_EN_PIN, enabled ? 1 : 0);
    }
    return ret;
}

esp_err_t t_panel_p4_bsp_camera_flash_pwm_init(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp && bsp->i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "BSP is not initialized");
    if (bsp->camera_flash_pwm_initialized) {
        return ESP_OK;
    }

    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << CAMERA_FALSH1_EN_PIN) |
                        (1ULL << CAMERA_FALSH2_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG,
                        "configure camera flash GPIOs failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)CAMERA_FALSH1_EN_PIN, 0),
                        TAG, "disable camera flash 1 failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)CAMERA_FALSH2_EN_PIN, 0),
                        TAG, "disable camera flash 2 failed");

    const ledc_timer_config_t timer_config = {
        .speed_mode = CAMERA_FLASH_LEDC_MODE,
        .duty_resolution = CAMERA_FLASH_LEDC_RESOLUTION,
        .timer_num = CAMERA_FLASH_LEDC_TIMER,
        .freq_hz = CAMERA_FLASH_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG,
                        "configure camera flash PWM timer failed");

    for (unsigned i = 0; i < T_PANEL_P4_CAMERA_FLASH_COUNT; ++i) {
        const ledc_channel_config_t channel_config = {
            .gpio_num = s_camera_flash_gpio[i],
            .speed_mode = CAMERA_FLASH_LEDC_MODE,
            .channel = s_camera_flash_channel[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = CAMERA_FLASH_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        esp_err_t ret = ledc_channel_config(&channel_config);
        if (ret != ESP_OK) {
            for (unsigned configured = 0; configured < i; ++configured) {
                ledc_stop(CAMERA_FLASH_LEDC_MODE,
                          s_camera_flash_channel[configured], 0);
            }
            return ret;
        }
    }

    bsp->camera_flash_pwm_initialized = true;
    memset(bsp->camera_flash_brightness, 0,
           sizeof(bsp->camera_flash_brightness));
    ESP_LOGI(TAG, "Camera flash PWM ready: GPIO%d/GPIO%d, %d Hz",
             CAMERA_FALSH1_EN_PIN, CAMERA_FALSH2_EN_PIN,
             CAMERA_FLASH_LEDC_FREQ_HZ);
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_camera_flash_set_brightness(
    t_panel_p4_bsp_t *bsp, t_panel_p4_camera_flash_t flash, uint8_t percent)
{
    ESP_RETURN_ON_FALSE(bsp && bsp->camera_flash_pwm_initialized,
                        ESP_ERR_INVALID_STATE, TAG,
                        "camera flash PWM is not initialized");
    ESP_RETURN_ON_FALSE(flash >= T_PANEL_P4_CAMERA_FLASH_1 &&
                            flash < T_PANEL_P4_CAMERA_FLASH_COUNT &&
                            percent <= 100,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid camera flash brightness");

    const uint32_t duty = CAMERA_FLASH_LEDC_MAX_DUTY * percent / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(CAMERA_FLASH_LEDC_MODE,
                                      s_camera_flash_channel[flash], duty),
                        TAG, "set camera flash duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(CAMERA_FLASH_LEDC_MODE,
                                         s_camera_flash_channel[flash]),
                        TAG, "update camera flash duty failed");
    bsp->camera_flash_brightness[flash] = percent;
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_camera_flash_set_all_brightness(
    t_panel_p4_bsp_t *bsp, uint8_t percent)
{
    for (unsigned i = 0; i < T_PANEL_P4_CAMERA_FLASH_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(t_panel_p4_bsp_camera_flash_set_brightness(
                                bsp, (t_panel_p4_camera_flash_t)i, percent),
                            TAG, "set all camera flashes failed");
    }
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_camera_flash_pwm_deinit(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    if (!bsp->camera_flash_pwm_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    for (unsigned i = 0; i < T_PANEL_P4_CAMERA_FLASH_COUNT; ++i) {
        const esp_err_t stop_ret = ledc_stop(CAMERA_FLASH_LEDC_MODE,
                                             s_camera_flash_channel[i], 0);
        if (ret == ESP_OK) {
            ret = stop_ret;
        }
        gpio_reset_pin(s_camera_flash_gpio[i]);
    }
    bsp->camera_flash_pwm_initialized = false;
    memset(bsp->camera_flash_brightness, 0,
           sizeof(bsp->camera_flash_brightness));
    return ret;
}
