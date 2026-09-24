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

static const char *TAG = "radio";

#define LR2021_TEST_MODE_PERIODIC_TX 0
#define LR2021_TEST_MODE_CW 1

// Select LR2021_TEST_MODE_CW for power measurements, or
// LR2021_TEST_MODE_PERIODIC_TX for the original packet transmission test.
#define LR2021_TEST_MODE LR2021_TEST_MODE_PERIODIC_TX

#define LR2021_TEST_FREQUENCY_MHZ 2400.0f
#define LR2021_TEST_POWER_DBM 8
#define LR2021_PACKET_INTERVAL_MS 1000
#define LR2021_CW_FORCE_DIO7_HIGH 0

#if LR2021_CW_FORCE_DIO7_HIGH && LR2021_TEST_MODE != LR2021_TEST_MODE_CW
#error "LR2021_CW_FORCE_DIO7_HIGH is only safe in CW test mode"
#endif

static const uint32_t rfswitch_dio_pins[] = {
    RADIOLIB_NC, RADIOLIB_LR2021_DIO6,
    RADIOLIB_LR2021_DIO7, RADIOLIB_NC, RADIOLIB_NC};

static const Module::RfSwitchMode_t rfswitch_table[] = {
    // mode              DIO5 DIO6 DIO7
    {LR2021::MODE_STBY, {LOW, LOW, LOW}},
    {LR2021::MODE_RX, {LOW, LOW, LOW}},
    {LR2021::MODE_TX, {LOW, LOW, LOW}},
    {LR2021::MODE_RX_HF, {LOW, HIGH, LOW}},
    {LR2021::MODE_TX_HF, {LOW, LOW, HIGH}},
    END_OF_MODE_TABLE,
};

static LR2021PaTableEntry_t paOptTableLf[RADIOLIB_LR2021_PA_TABLE_LEN] = {
    // The 915 MHz reference table only specifies 10 to 22 dBm.
    // Keep the RadioLib defaults for -9 to +9 dBm.
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 8  }, // -9 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 1  }, // -8 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 3  }, // -7 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 5  }, // -6 dBm
    { .paDutyCycle = 1, .paSlices = 2, .paVal = 13 }, // -5 dBm
    { .paDutyCycle = 2, .paSlices = 1, .paVal = 13 }, // -4 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 11 }, // -3 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 13 }, // -2 dBm
    { .paDutyCycle = 3, .paSlices = 1, .paVal = 12 }, // -1 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 18 }, //  0 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 20 }, //  1 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 23 }, //  2 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 27 }, //  3 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 33 }, //  4 dBm
    { .paDutyCycle = 1, .paSlices = 2, .paVal = 26 }, //  5 dBm
    { .paDutyCycle = 1, .paSlices = 2, .paVal = 31 }, //  6 dBm
    { .paDutyCycle = 1, .paSlices = 3, .paVal = 27 }, //  7 dBm
    { .paDutyCycle = 1, .paSlices = 1, .paVal = 37 }, //  8 dBm
    { .paDutyCycle = 1, .paSlices = 2, .paVal = 40 }, //  9 dBm
    // 10 to 22 dBm use the supplied 915 MHz values. paVal = TX_PARAM * 2.
    { .paDutyCycle = 2, .paSlices = 1, .paVal = 32 },// 10 dBm
    { .paDutyCycle = 2, .paSlices = 2, .paVal = 32 },// 11 dBm
    { .paDutyCycle = 5, .paSlices = 1, .paVal = 30 },// 12 dBm
    { .paDutyCycle = 4, .paSlices = 3, .paVal = 31 },// 13 dBm
    { .paDutyCycle = 4, .paSlices = 2, .paVal = 34 },// 14 dBm
    { .paDutyCycle = 5, .paSlices = 4, .paVal = 33 },// 15 dBm 
    { .paDutyCycle = 4, .paSlices = 4, .paVal = 36 },// 16 dBm
    { .paDutyCycle = 5, .paSlices = 6, .paVal = 36 },// 17 dBm
    { .paDutyCycle = 5, .paSlices = 6, .paVal = 38 },// 18 dBm
    { .paDutyCycle = 6, .paSlices = 6, .paVal = 39 },// 19 dBm
    { .paDutyCycle = 6, .paSlices = 6, .paVal = 41 },// 20 dBm
    { .paDutyCycle = 7, .paSlices = 7, .paVal = 42 },// 21 dBm
    { .paDutyCycle = 7, .paSlices = 6, .paVal = 44 },// 22 dBm
};

Esp32p4Hal *hal = new Esp32p4Hal(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
LR2021 radio = new Module(hal, LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);
volatile bool transmittedFlag = false;

static void halt_on_error(const char *operation, int state)
{
    if (state == RADIOLIB_ERR_NONE)
    {
        return;
    }

    ESP_LOGE(TAG, "%s failed, code:%d", operation, state);
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void IRAM_ATTR setFlag(void)
{
    transmittedFlag = true;
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "[LR2021] Initializing ... ");
    radio.irqDioNum = LORA_IRQ_DIO_NUM;
    if(LR2021_TEST_FREQUENCY_MHZ < 1000.0 )
        radio.setPaTable(paOptTableLf, false);

    int state = radio.begin(LR2021_TEST_FREQUENCY_MHZ,
                            406.0f,
                            10,
                            5,
                            RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE,
                            LR2021_TEST_POWER_DBM,
                            8,
                            3.3f);
    halt_on_error("begin", state);

    radio.setPacketSentAction(setFlag);
    radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);
    state = radio.setOutputPower(LR2021_TEST_POWER_DBM);
    halt_on_error("setOutputPower", state);

    ESP_LOGI(TAG, "LR2021 ready: %.1fMHz power:%ddBm mode:%s",
             (double)LR2021_TEST_FREQUENCY_MHZ,
             LR2021_TEST_POWER_DBM,
             LR2021_TEST_MODE == LR2021_TEST_MODE_CW ? "CW" : "periodic TX");

#if LR2021_TEST_MODE == LR2021_TEST_MODE_CW
    state = radio.transmitDirect();
    halt_on_error("transmitDirect(CW)", state);

    ESP_LOGI(TAG, "CW transmitting continuously; reset the board to stop");
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#elif LR2021_TEST_MODE == LR2021_TEST_MODE_PERIODIC_TX
    char buf[32];
    snprintf(buf, sizeof(buf), "Hello World!");
    state = radio.startTransmit(buf);
    halt_on_error("startTransmit", state);

    int n = 0;
    while (true)
    {
        if (transmittedFlag)
        {
            transmittedFlag = false;
            uint32_t irq = radio.getIrqStatus();
            ESP_LOGI(TAG, "GPIO%d interrupt fired: level=%d irq=0x%08" PRIx32,
                     LORA_IRQ, gpio_get_level((gpio_num_t)LORA_IRQ), irq);
            radio.finishTransmit();
            vTaskDelay(pdMS_TO_TICKS(LR2021_PACKET_INTERVAL_MS));
            snprintf(buf, sizeof(buf), "Hello World! #%d", n++);
            state = radio.startTransmit(buf);
            halt_on_error("startTransmit", state);
            
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
#error "Invalid LR2021_TEST_MODE"
#endif
}
