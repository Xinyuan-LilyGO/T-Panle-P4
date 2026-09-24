#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "camera_flash_led";

#define FADE_STEP_PERCENT 10
#define FADE_STEP_DELAY_MS 100
#define FLASH_PULSE_MS 200
#define SEQUENCE_DELAY_MS 1000

static t_panel_p4_bsp_t s_bsp;

static void fade_flash(t_panel_p4_camera_flash_t flash)
{
    for (uint8_t brightness = 0; brightness <= 100;
         brightness += FADE_STEP_PERCENT) {
        ESP_ERROR_CHECK(t_panel_p4_bsp_camera_flash_set_brightness(
            &s_bsp, flash, brightness));
        vTaskDelay(pdMS_TO_TICKS(FADE_STEP_DELAY_MS));
    }
    ESP_ERROR_CHECK(t_panel_p4_bsp_camera_flash_set_brightness(
        &s_bsp, flash, 0));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Rect camera flash LED PWM example");
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&s_bsp));
    ESP_ERROR_CHECK(t_panel_p4_bsp_camera_flash_pwm_init(&s_bsp));

    while (true) {
        ESP_LOGI(TAG, "Fade flash 1");
        fade_flash(T_PANEL_P4_CAMERA_FLASH_1);

        ESP_LOGI(TAG, "Fade flash 2");
        fade_flash(T_PANEL_P4_CAMERA_FLASH_2);

        ESP_LOGI(TAG, "Pulse both flashes at 100%%");
        ESP_ERROR_CHECK(t_panel_p4_bsp_camera_flash_set_all_brightness(
            &s_bsp, 100));
        vTaskDelay(pdMS_TO_TICKS(FLASH_PULSE_MS));
        ESP_ERROR_CHECK(t_panel_p4_bsp_camera_flash_set_all_brightness(
            &s_bsp, 0));

        vTaskDelay(pdMS_TO_TICKS(SEQUENCE_DELAY_MS));
    }
}
