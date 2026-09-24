#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmc6309_driver.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "qmc6309_example";

void app_main(void)
{
    t_panel_p4_bsp_t bsp = {0};
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));

    qmc6309_handle_t magnetometer = {0};
    qmc6309_config_t config = QMC6309_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(qmc6309_init(&magnetometer,
                                t_panel_p4_bsp_get_i2c_bus(&bsp),
                                QMC6309_I2C_ADDR,
                                &config));
    ESP_LOGI(TAG, "QMC6309 ready at 0x%02X", QMC6309_I2C_ADDR);

    while (true) {
        bool ready = false;
        ESP_ERROR_CHECK(qmc6309_data_ready(&magnetometer, &ready));
        if (ready) {
            qmc6309_data_t data = {0};
            const esp_err_t ret = qmc6309_read(&magnetometer, &data);
            if (ret == ESP_ERR_NOT_FINISHED) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_ERROR_CHECK(ret);
            ESP_LOGI(TAG,
                     "field[G] x=%.4f y=%.4f z=%.4f, heading=%.1f deg%s",
                     data.x_gauss, data.y_gauss, data.z_gauss,
                     data.heading_degrees,
                     data.overflow ? " (overflow)" : "");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
