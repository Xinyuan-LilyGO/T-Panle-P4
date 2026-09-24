#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "radio_esp32p4_hal.h"
#include "board_config.h"

// #define LORA_TX 1

static const char *TAG = "radio";

Esp32p4Hal *hal = new Esp32p4Hal(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
SX1276 radio = new Module(hal, LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

volatile bool receivedFlag = false;
void setFlag(void)
{
    // we got a packet, set the flag
    receivedFlag = true;
}

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
    ESP_LOGI(TAG, "[SX1276] Initializing ... ");
    int16_t state = radio.begin(868.0);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGI(TAG, "failed, code %d\n", state);
        while (true)
        {
            hal->delay(1000);
        }
    }
    ESP_LOGI(TAG, "success!\n");
    radio.setPacketReceivedAction(setFlag);

    radio.setBandwidth(125.0);
    radio.setSpreadingFactor(7);
    radio.setCodingRate(5);
    radio.setPreambleLength(12);
    radio.setSyncWord(0x12);
    radio.setCRC(false);
    radio.invertIQ(false);

    radio.setOutputPower(20);
    radio.setCurrentLimit(240);
#if LORA_TX
    while (1)
    {
        ESP_LOGI(TAG, "[SX1276] Transmitting packet ... ");
        state = radio.transmit("Hello World!");
        if (state == RADIOLIB_ERR_NONE)
        {
            // the packet was successfully transmitted
            ESP_LOGI(TAG, "success!");
        }
        else
        {
            ESP_LOGI(TAG, "failed, code %d\n", state);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
#else
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGI(TAG, "failed, code %d\n", state);
    }
    ESP_LOGI(TAG, "[SX1276] Waiting for incoming transmission ... ");

    while (1)
    {
        if (receivedFlag)
        {
            receivedFlag = false;

            // you can receive data as an Arduino String
            uint8_t str[32];
            state = radio.readData(str, sizeof(str));

            // you can also receive data as byte array
            /*
              byte byteArr[8];
              int state = radio.receive(byteArr, 8);
            */

            if (state == RADIOLIB_ERR_NONE)
            {
                // packet was successfully received
                ESP_LOGI(TAG, "success!");

                // print the data of the packet
                ESP_LOGI(TAG, "[SX1276] Data:%s", str);

                // print the RSSI (Received Signal Strength Indicator)
                // of the last received packet
                ESP_LOGI(TAG, "[SX1276] RSSI:%.2f dBm", radio.getRSSI());

                // print the SNR (Signal-to-Noise Ratio)
                // of the last received packet
                ESP_LOGI(TAG, "[SX1276] SNR:%.2f dB", radio.getSNR());
            }
            else if (state == RADIOLIB_ERR_RX_TIMEOUT)
            {
                // timeout occurred while waiting for a packet
                ESP_LOGI(TAG, "timeout!");
            }
            else if (state == RADIOLIB_ERR_CRC_MISMATCH)
            {
                // packet was received, but is malformed
                ESP_LOGI(TAG, "CRC error!");
            }
            else
            {
                // some other error occurred
                ESP_LOGI(TAG, "failed, code %d", state);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
#endif
}