#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "radio_esp32p4_hal.h"
#include "T_Panle_P4_board_config.h"

static const char *TAG = "radio";

static const uint32_t rfswitch_dio_pins[] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
    RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};

static const Module::RfSwitchMode_t rfswitch_table[] = {
    // mode                  DIO5  DIO6
    {LR11x0::MODE_STBY, {LOW, LOW}},
    {LR11x0::MODE_RX, {HIGH, LOW}},
    {LR11x0::MODE_TX, {LOW, HIGH}},
    {LR11x0::MODE_TX_HP, {LOW, HIGH}},
    {LR11x0::MODE_TX_HF, {LOW, LOW}},
    {LR11x0::MODE_GNSS, {LOW, LOW}},
    {LR11x0::MODE_WIFI, {LOW, LOW}},
    END_OF_MODE_TABLE,
};

Esp32p4Hal *hal = new Esp32p4Hal(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
LR1121 radio = new Module(hal, LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

extern "C" void app_main()
{
    ESP_LOGI(TAG, "[LR1121] Initializing ... ");
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
    radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);

    while (1)
    {
        ESP_LOGI(TAG, "[LR1121] Transmitting packet ... ");
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