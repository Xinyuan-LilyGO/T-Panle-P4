#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "radio_esp32p4_hal.h"
#include "T_Panle_P4_board_config.h"

static const char *TAG = "radio";

Esp32p4Hal *hal = new Esp32p4Hal(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
SX1262 radio = new Module(hal, LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

static void lora_power_enable(void)
{
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = 0;
    bus_cfg.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
    bus_cfg.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(bus_handle, XL9555_I2C_ADDR, &expander));

    const uint32_t pin_mask = 1UL << XL9555_LORA_PWR_EN;
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, pin_mask, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, pin_mask, 1));
    ESP_LOGI(TAG, "LoRa power enabled via XL9555 pin %d", XL9555_LORA_PWR_EN);
}

extern "C" void app_main()
{
    lora_power_enable();
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "[SX1262] Initializing ... ");
    int state = radio.begin(868.0);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGI(TAG, "failed, code %d\n", state);
        while (true)
        {
            hal->delay(1000);
        }
    }
    ESP_LOGI(TAG, "success!\n");

    radio.setTCXO(3.3);
    radio.setOutputPower(22);
    radio.setCurrentLimit(140);

    while (1)
    {
        ESP_LOGI(TAG, "[SX1262] Transmitting packet ... ");
        state = radio.transmit("Hello World!");
        if (state == RADIOLIB_ERR_NONE)
        {
            ESP_LOGI(TAG, "success!");
        }
        else
        {
            ESP_LOGI(TAG, "failed, code %d\n", state);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
