#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658c_driver.h"
#include "t_panel_p4_bsp.h"

static const char *TAG = "qmi8658c_example";

void app_main(void)
{
    t_panel_p4_bsp_t bsp = {0};
    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));

    t_panel_qmi8658c_t imu = {0};
    qmi8658c_config_t config = T_PANEL_QMI8658C_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(t_panel_qmi8658c_init(&imu,
                                         t_panel_p4_bsp_get_i2c_bus(&bsp),
                                         T_PANEL_QMI8658C_ADDR_AUTO,
                                         &config));
    ESP_LOGI(TAG, "QMI8658C ready at 0x%02X, revision 0x%02X",
             imu.address, imu.revision);
    ESP_ERROR_CHECK(t_panel_qmi8658c_enable_data_ready_interrupt(&imu,
                                                                 QMI8658C_INT_PIN));
    ESP_LOGI(TAG, "data-ready interrupt enabled on GPIO%d, level=%d",
             QMI8658C_INT_PIN, gpio_get_level(QMI8658C_INT_PIN));

    uint32_t sample_count = 0;
    while (true) {
        const esp_err_t ret = t_panel_qmi8658c_wait_for_data(&imu, 1000);
        if (ret == ESP_ERR_TIMEOUT) {
            bool accelerometer_ready = false;
            bool gyroscope_ready = false;
            const esp_err_t status_ret = t_panel_qmi8658c_data_ready(
                &imu, &accelerometer_ready, &gyroscope_ready);
            ESP_LOGW(TAG,
                     "interrupt timeout: GPIO%d=%d, status=%s, acc_ready=%d, gyro_ready=%d",
                     QMI8658C_INT_PIN, gpio_get_level(QMI8658C_INT_PIN),
                     esp_err_to_name(status_ret), accelerometer_ready,
                     gyroscope_ready);
            continue;
        }
        ESP_ERROR_CHECK(ret);

        qmi8658c_data_t data = {0};
        ESP_ERROR_CHECK(t_panel_qmi8658c_read(&imu, &data));
        if (++sample_count % 10 == 0) {
            ESP_LOGI(TAG,
                     "irq=%lu, acc[g] x=%.3f y=%.3f z=%.3f, gyro[dps] x=%.2f y=%.2f z=%.2f, temp=%.2f C",
                     (unsigned long)imu.interrupt_count,
                     data.acc.x, data.acc.y, data.acc.z,
                     data.gyro.x, data.gyro.y, data.gyro.z,
                     data.temperature);
        }
    }
}
