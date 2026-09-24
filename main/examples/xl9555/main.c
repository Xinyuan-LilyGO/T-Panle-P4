#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"

static const char *TAG = "xl9555";

void app_main()
{
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    // Create the XL9555 expander instance on the specified I2C address.
    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(bus_handle, XL9555_I2C_ADDR, &expander));
    uint32_t pin_mask = (1UL << XL9555_SPK_CRTL);
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, pin_mask, IO_EXPANDER_OUTPUT));
    bool level = false;

    while (1)
    {
        ESP_LOGD(TAG, "app_main!");
        level = !level;
        ESP_ERROR_CHECK(esp_io_expander_set_level(expander, pin_mask, level));
        ESP_LOGI(TAG, "Set pin %d to %d", (int)XL9555_SPK_CRTL, level);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}