#include "axp517.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t_panel_p4_bsp.h"

#ifdef AXP517_IRQ_PIN
#define AXP517_EXAMPLE_IRQ_GPIO AXP517_IRQ_PIN
#else
#define AXP517_EXAMPLE_IRQ_GPIO AXP517_IRQ_GPIO_UNUSED
#endif

#define AXP517_EXAMPLE_RUNTIME_LOG_ENABLED     true
#define AXP517_EXAMPLE_RUNTIME_LOG_INTERVAL_MS 3000U

void app_main(void)
{
    static t_panel_p4_bsp_t bsp;
    static axp517_handle_t axp517;

    ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
    ESP_ERROR_CHECK(axp517_init_default(&axp517,
                                        t_panel_p4_bsp_get_i2c_bus(&bsp),
                                        AXP517_EXAMPLE_IRQ_GPIO));
    ESP_ERROR_CHECK(axp517_set_runtime_log(
        &axp517,
        AXP517_EXAMPLE_RUNTIME_LOG_ENABLED,
        AXP517_EXAMPLE_RUNTIME_LOG_INTERVAL_MS));

    while (true) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(axp517_process(&axp517));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
