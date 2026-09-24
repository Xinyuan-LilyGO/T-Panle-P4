#include <stdbool.h>

#include "drv2605_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "drv2605_example";

void app_main(void)
{
    t_panel_p4_bsp_t bsp = {0};
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));

    drv2605_handle_t haptic = {0};
    drv2605_config_t config = DRV2605_CONFIG_DEFAULT(MOTOR_EN_PIN);

    /* These values are for a 3 V ERM. Match them to the connected motor. */
    config.rated_voltage_reg = DRV2605_ERM_RATED_VOLTAGE_MV(3000);
    config.overdrive_clamp_reg = DRV2605_ERM_OVERDRIVE_CLAMP_MV(3300);
    config.auto_calibrate = true;
    ESP_ERROR_CHECK(drv2605_init(&haptic,
                                t_panel_p4_bsp_get_i2c_bus(&bsp),
                                &config));
    ESP_LOGI(TAG, "DRV2605 ready at 0x%02X", config.address);

    while (true) {
        ESP_ERROR_CHECK(drv2605_play_effect(&haptic, 47));

        bool playing = true;
        while (playing) {
            ESP_ERROR_CHECK(drv2605_is_playing(&haptic, &playing));
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        ESP_LOGI(TAG, "effect 47 (Buzz 1, 100%%) completed");
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_ERROR_CHECK(drv2605_set_realtime_value(
            &haptic, DRV2605_RTP_MAX_FORWARD));
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(drv2605_set_realtime_value(&haptic, DRV2605_RTP_STOP));
        ESP_LOGI(TAG, "500 ms full-scale RTP completed");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
