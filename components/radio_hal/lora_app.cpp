#include "lora_app.h"

#include <inttypes.h>
#include <string.h>

#include "radio_esp32p4_hal.h"
#include "board_config.h"

static const char *TAG = "lora_app";

static Esp32p4Hal *s_hal = nullptr;
static Module *s_module = nullptr;
static PhysicalLayer *s_radio = nullptr;
static lora_app_config_t s_config = {};
static bool s_config_valid = false;
static bool s_started = false;
static bool s_crc_enabled = true;
static lora_app_irq_cb_t s_irq_callback = nullptr;
static void *s_irq_user_data = nullptr;
static volatile uint32_t s_irq_events = 0;

#define LR2021_SUB_GHZ_MAX_POWER_DBM 22
#define LR2021_24_GHZ_MAX_POWER_DBM 5
#define LR2021_24_GHZ_DEFAULT_BANDWIDTH_KHZ 406.0f

static LR2021PaTableEntry_t paOptTableLf[RADIOLIB_LR2021_PA_TABLE_LEN] = {
    // The 915 MHz reference table only specifies 10 to 22 dBm.
    // Keep the RadioLib defaults for -9 to +9 dBm.
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 8},  // -9 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 1},  // -8 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 3},  // -7 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 5},  // -6 dBm
    {.paDutyCycle = 1, .paSlices = 2, .paVal = 13}, // -5 dBm
    {.paDutyCycle = 2, .paSlices = 1, .paVal = 13}, // -4 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 11}, // -3 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 13}, // -2 dBm
    {.paDutyCycle = 3, .paSlices = 1, .paVal = 12}, // -1 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 18}, //  0 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 20}, //  1 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 23}, //  2 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 27}, //  3 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 33}, //  4 dBm
    {.paDutyCycle = 1, .paSlices = 2, .paVal = 26}, //  5 dBm
    {.paDutyCycle = 1, .paSlices = 2, .paVal = 31}, //  6 dBm
    {.paDutyCycle = 1, .paSlices = 3, .paVal = 27}, //  7 dBm
    {.paDutyCycle = 1, .paSlices = 1, .paVal = 37}, //  8 dBm
    {.paDutyCycle = 1, .paSlices = 2, .paVal = 40}, //  9 dBm
    // 10 to 22 dBm use the supplied 915 MHz values. paVal = TX_PARAM * 2.
    {.paDutyCycle = 2, .paSlices = 1, .paVal = 32}, // 10 dBm
    {.paDutyCycle = 2, .paSlices = 2, .paVal = 32}, // 11 dBm
    {.paDutyCycle = 5, .paSlices = 1, .paVal = 30}, // 12 dBm
    {.paDutyCycle = 4, .paSlices = 3, .paVal = 31}, // 13 dBm
    {.paDutyCycle = 4, .paSlices = 2, .paVal = 34}, // 14 dBm
    {.paDutyCycle = 5, .paSlices = 4, .paVal = 33}, // 15 dBm
    {.paDutyCycle = 4, .paSlices = 4, .paVal = 36}, // 16 dBm
    {.paDutyCycle = 5, .paSlices = 6, .paVal = 36}, // 17 dBm
    {.paDutyCycle = 5, .paSlices = 6, .paVal = 38}, // 18 dBm
    {.paDutyCycle = 6, .paSlices = 6, .paVal = 39}, // 19 dBm
    {.paDutyCycle = 6, .paSlices = 6, .paVal = 41}, // 20 dBm
    {.paDutyCycle = 7, .paSlices = 7, .paVal = 42}, // 21 dBm
    {.paDutyCycle = 7, .paSlices = 6, .paVal = 44}, // 22 dBm
};

static int8_t lora_app_max_output_power(void)
{
    if (s_config.chip != LORA_APP_CHIP_LR2021)
    {
        return 22;
    }
    return s_config.freq_mhz > RADIOLIB_LR2021_LF_CUTOFF_FREQ ? LR2021_24_GHZ_MAX_POWER_DBM : LR2021_SUB_GHZ_MAX_POWER_DBM;
}

static bool lora_app_lr2021_is_high_frequency(float freq_mhz)
{
    return freq_mhz > RADIOLIB_LR2021_LF_CUTOFF_FREQ;
}

static int lora_app_lr2021_configure_rf_switch(LR2021 *radio)
{
    static const uint32_t rf_switch_pins[Module::RFSWITCH_MAX_PINS] = {
        RADIOLIB_NC,
        RADIOLIB_LR2021_DIO6,
        RADIOLIB_LR2021_DIO7,
        RADIOLIB_NC,
        RADIOLIB_NC,
    };
    static const Module::RfSwitchMode_t rf_switch_table[] = {
        {LR2021::MODE_STBY, {LOW, LOW, LOW}},
        {LR2021::MODE_RX, {LOW, LOW, LOW}},
        {LR2021::MODE_TX, {LOW, LOW, LOW}},
        {LR2021::MODE_RX_HF, {LOW, HIGH, LOW}},
        {LR2021::MODE_TX_HF, {LOW, LOW, HIGH}},
        END_OF_MODE_TABLE,
    };

    radio->setRfSwitchTable(rf_switch_pins, rf_switch_table);
    ESP_LOGI(TAG, "LR2021 RF switch: DIO6 mask=0x08 (HF RX), DIO7 mask=0x10 (HF TX)");
    return RADIOLIB_ERR_NONE;
}

static int lora_app_lr2021_config_paopttable(LR2021 *radio, LR2021PaTableEntry_t *paOptTable, bool highFreq)
{
    radio->setPaTable(paOptTable, highFreq);
    return RADIOLIB_ERR_NONE;
}

static void lora_app_lr2021_log_pa_status(int8_t power_dbm)
{
    ESP_LOGI(TAG, "LR2021 PA configured: %s %.1fMHz %ddBm",
             s_config.freq_mhz > RADIOLIB_LR2021_LF_CUTOFF_FREQ ? "HF" : "LF",
             (double)s_config.freq_mhz,
             power_dbm);
}

static bool lora_app_has_radio(void)
{
    return s_radio != nullptr;
}

static int lora_app_require_radio(void)
{
    return lora_app_has_radio() ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_NULL_POINTER;
}

static void lora_app_irq_notify(lora_app_irq_event_t event)
{
    s_irq_events |= (uint32_t)event;
    if (s_irq_callback != nullptr)
    {
        s_irq_callback(event, s_irq_user_data);
    }
}

static void lora_app_packet_received_isr(void)
{
    lora_app_irq_notify(LORA_APP_IRQ_RX_DONE);
}

static void lora_app_packet_sent_isr(void)
{
    lora_app_irq_notify(LORA_APP_IRQ_TX_DONE);
}

static void lora_app_apply_irq_actions(void)
{
    if (s_radio == nullptr)
    {
        return;
    }

    s_radio->clearPacketReceivedAction();
    s_radio->clearPacketSentAction();
    if (s_irq_callback != nullptr)
    {
        s_radio->setPacketReceivedAction(lora_app_packet_received_isr);
        s_radio->setPacketSentAction(lora_app_packet_sent_isr);
    }
}

static float lora_app_default_current_limit(lora_app_chip_t chip)
{
    switch (chip)
    {
    case LORA_APP_CHIP_SX1262:
        return 140.0f;
    case LORA_APP_CHIP_SX1276:
        return 240.0f;
    default:
        return 0.0f;
    }
}

static int lora_app_apply_current_limit(float current_ma)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    if (current_ma <= 0.0f)
    {
        current_ma = lora_app_default_current_limit(s_config.chip);
    }

    if (current_ma <= 0.0f)
    {
        return RADIOLIB_ERR_NONE;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setCurrentLimit((uint8_t)current_ma);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setCurrentLimit(current_ma);
        break;
    default:
        state = RADIOLIB_ERR_UNSUPPORTED;
        break;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.current_limit_ma = current_ma;
    }
    return state;
}

extern "C" void lora_app_default_config(lora_app_config_t *config)
{
    if (config == nullptr)
    {
        return;
    }

#if defined(SX1276_MODUEL)
    config->chip = LORA_APP_CHIP_SX1276;
#elif defined(SX1262_MODUEL)
    config->chip = LORA_APP_CHIP_SX1262;
#elif defined(LR1121_MODUEL)
    config->chip = LORA_APP_CHIP_LR1121;
#elif defined(LR2021_MODUEL)
    config->chip = LORA_APP_CHIP_LR2021;
#else
#error "Please select a LoRa module"
#endif
    config->spi_sck = SPI_SCK_PIN;
    config->spi_miso = SPI_MISO_PIN;
    config->spi_mosi = SPI_MOSI_PIN;
    config->cs = LORA_CS;
    config->irq = LORA_IRQ;
    config->rst = LORA_RST;
    config->busy = LORA_BUSY;
    config->spi_freq_hz = 2000000;
    config->freq_mhz = 868.0f;
    config->bandwidth_khz = 125.0f;
    config->spreading_factor = 7;
    config->coding_rate = 5;
    config->sync_word = 0x12;
    config->output_power_dbm = config->chip == LORA_APP_CHIP_LR2021 ? LR2021_SUB_GHZ_MAX_POWER_DBM : 16;
    config->preamble_len = 12;
    config->tcxo_voltage = 3.3f;
    config->current_limit_ma = lora_app_default_current_limit(config->chip);
    config->invert_iq = false;
}

static int lora_app_create_radio(const lora_app_config_t *config)
{
    s_hal = new Esp32p4Hal(config->spi_sck, config->spi_miso, config->spi_mosi, config->spi_freq_hz);
    s_module = new Module(s_hal, config->cs, config->irq, config->rst, config->busy);

    switch (config->chip)
    {
    case LORA_APP_CHIP_SX1276:
        s_radio = new SX1276(s_module);
        break;
    case LORA_APP_CHIP_SX1262:
        s_radio = new SX1262(s_module);
        break;
    case LORA_APP_CHIP_LR1121:
        s_radio = new LR1121(s_module);
        break;
    case LORA_APP_CHIP_LR2021:
        s_radio = new LR2021(s_module);
        break;
    default:
        return RADIOLIB_ERR_INVALID_FUNCTION;
    }

    return RADIOLIB_ERR_NONE;
}

static int lora_app_begin_radio(const lora_app_config_t *config)
{
    switch (config->chip)
    {
    case LORA_APP_CHIP_SX1262:
        return static_cast<SX1262 *>(s_radio)->begin(config->freq_mhz,
                                                     config->bandwidth_khz,
                                                     config->spreading_factor,
                                                     config->coding_rate,
                                                     config->sync_word,
                                                     config->output_power_dbm,
                                                     config->preamble_len,
                                                     config->tcxo_voltage);
    case LORA_APP_CHIP_SX1276:
        return static_cast<SX1276 *>(s_radio)->begin(config->freq_mhz,
                                                     config->bandwidth_khz,
                                                     config->spreading_factor,
                                                     config->coding_rate,
                                                     config->sync_word,
                                                     config->output_power_dbm,
                                                     config->preamble_len);
    case LORA_APP_CHIP_LR1121:
        return static_cast<LR1121 *>(s_radio)->begin(config->freq_mhz,
                                                     config->bandwidth_khz,
                                                     config->spreading_factor,
                                                     config->coding_rate,
                                                     config->sync_word,
                                                     config->output_power_dbm,
                                                     config->preamble_len,
                                                     config->tcxo_voltage);
    case LORA_APP_CHIP_LR2021:
    {
        LR2021 *radio = static_cast<LR2021 *>(s_radio);
#if defined(LORA_IRQ_DIO_NUM)
        radio->irqDioNum = LORA_IRQ_DIO_NUM;
#endif
        ESP_LOGI(TAG, "LR2021 IRQ route: DIO%u -> MCU GPIO%d",
                 (unsigned)radio->irqDioNum, config->irq);

        lora_app_lr2021_config_paopttable(radio, paOptTableLf, false);

        int state = radio->begin(config->freq_mhz,
                                 config->bandwidth_khz,
                                 config->spreading_factor,
                                 config->coding_rate,
                                 config->sync_word,
                                 config->output_power_dbm,
                                 config->preamble_len,
                                 config->tcxo_voltage);
        if (state == RADIOLIB_ERR_NONE)
        {
            state = lora_app_lr2021_configure_rf_switch(radio);
            if (state == RADIOLIB_ERR_NONE)
            {
                lora_app_lr2021_log_pa_status(config->output_power_dbm);
            }
        }
        return state;
    }
    default:
        return RADIOLIB_ERR_INVALID_FUNCTION;
    }
}

extern "C" int lora_app_init(const lora_app_config_t *config)
{
    lora_app_stop();

    if (config == nullptr)
    {
        if (!s_config_valid)
        {
            lora_app_default_config(&s_config);
        }
    }
    else
    {
        s_config = *config;
    }
    s_config_valid = true;

    int state = lora_app_create_radio(&s_config);
    if (state != RADIOLIB_ERR_NONE)
    {
        lora_app_stop();
        return state;
    }

    ESP_LOGI(TAG, "Initializing chip:%d freq:%.1fMHz bw:%.1fkHz sf:%u cr:%u",
             s_config.chip,
             (double)s_config.freq_mhz,
             (double)s_config.bandwidth_khz,
             s_config.spreading_factor,
             s_config.coding_rate);

    state = lora_app_begin_radio(&s_config);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGE(TAG, "init failed, code %d", state);
        lora_app_stop();
        return state;
    }

    state = lora_app_apply_current_limit(s_config.current_limit_ma);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGE(TAG, "set current limit failed, code %d", state);
        lora_app_stop();
        return state;
    }
    if (s_config.current_limit_ma > 0.0f)
    {
        ESP_LOGI(TAG, "current limit %.1fmA", (double)s_config.current_limit_ma);
    }

    state = lora_app_set_crc(s_crc_enabled);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGE(TAG, "restore CRC setting failed, code %d", state);
        lora_app_stop();
        return state;
    }

    state = s_radio->invertIQ(s_config.invert_iq);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGE(TAG, "set invert IQ failed, code %d", state);
        lora_app_stop();
        return state;
    }

    s_started = true;
    lora_app_apply_irq_actions();
    ESP_LOGI(TAG, "init success");
    return RADIOLIB_ERR_NONE;
}

extern "C" int lora_app_start(void)
{
    if (s_started)
    {
        return RADIOLIB_ERR_NONE;
    }

    lora_app_config_t config;
    if (s_config_valid)
    {
        config = s_config;
    }
    else
    {
        lora_app_default_config(&config);
    }
    return lora_app_init(&config);
}

extern "C" void lora_app_stop(void)
{
    if (s_radio != nullptr)
    {
        s_radio->clearPacketReceivedAction();
        s_radio->clearPacketSentAction();
        if (s_started)
        {
            s_radio->standby();
        }
        delete s_radio;
        s_radio = nullptr;
    }

    if (s_module != nullptr)
    {
        delete s_module;
        s_module = nullptr;
    }

    if (s_hal != nullptr)
    {
        s_hal->term();
        delete s_hal;
        s_hal = nullptr;
    }

    s_started = false;
}

extern "C" bool lora_app_is_started(void)
{
    return s_started;
}

extern "C" lora_app_chip_t lora_app_get_chip(void)
{
    return s_config.chip;
}

extern "C" void lora_app_set_irq_callback(lora_app_irq_cb_t callback, void *user_data)
{
    s_irq_callback = callback;
    s_irq_user_data = user_data;
    lora_app_apply_irq_actions();
}

extern "C" uint32_t lora_app_get_and_clear_irq_events(void)
{
    uint32_t events = s_irq_events;
    s_irq_events = 0;
    return events;
}

extern "C" int lora_app_transmit(const uint8_t *data, size_t len)
{
    if (data == nullptr)
    {
        return RADIOLIB_ERR_NULL_POINTER;
    }

    int state = lora_app_start();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    state = s_radio->transmit(data, len);

    ESP_LOGI(TAG, "transmit len:%u state:%d", (unsigned)len, state);
    return state;
}

extern "C" int lora_app_transmit_text(const char *text)
{
    if (text == nullptr)
    {
        return RADIOLIB_ERR_NULL_POINTER;
    }

    return lora_app_transmit(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

extern "C" int lora_app_start_transmit(const uint8_t *data, size_t len)
{
    if (data == nullptr)
    {
        return RADIOLIB_ERR_NULL_POINTER;
    }

    int state = lora_app_start();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    return s_radio->startTransmit(data, len);
}

extern "C" int lora_app_finish_transmit(void)
{
    int state = lora_app_require_radio();
    return state == RADIOLIB_ERR_NONE ? s_radio->finishTransmit() : state;
}

extern "C" int lora_app_receive(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }
    if (data == nullptr)
    {
        return RADIOLIB_ERR_NULL_POINTER;
    }

    return s_radio->receive(data, len, timeout_ms);
}

extern "C" int lora_app_start_receive(void)
{
    int state = lora_app_require_radio();
    return state == RADIOLIB_ERR_NONE ? s_radio->startReceive() : state;
}

extern "C" int lora_app_read_data(uint8_t *data, size_t len)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }
    if (data == nullptr)
    {
        return RADIOLIB_ERR_NULL_POINTER;
    }

    return s_radio->readData(data, len);
}

extern "C" size_t lora_app_get_packet_length(void)
{
    return lora_app_has_radio() ? s_radio->getPacketLength() : 0;
}

extern "C" int lora_app_standby(void)
{
    int state = lora_app_require_radio();
    return state == RADIOLIB_ERR_NONE ? s_radio->standby() : state;
}

extern "C" int lora_app_sleep(void)
{
    int state = lora_app_require_radio();
    return state == RADIOLIB_ERR_NONE ? s_radio->sleep() : state;
}

extern "C" int lora_app_scan_channel(void)
{
    int state = lora_app_require_radio();
    return state == RADIOLIB_ERR_NONE ? s_radio->scanChannel() : state;
}

extern "C" int lora_app_set_frequency(float freq_mhz)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    if (s_config.chip == LORA_APP_CHIP_LR2021)
    {
        bool old_high_frequency = lora_app_lr2021_is_high_frequency(s_config.freq_mhz);
        bool new_high_frequency = lora_app_lr2021_is_high_frequency(freq_mhz);

        if (old_high_frequency != new_high_frequency)
        {
            lora_app_config_t old_config = s_config;
            lora_app_config_t new_config = s_config;
            new_config.freq_mhz = freq_mhz;
            if (new_high_frequency)
            {
                new_config.bandwidth_khz = LR2021_24_GHZ_DEFAULT_BANDWIDTH_KHZ;
                if (new_config.output_power_dbm > LR2021_24_GHZ_MAX_POWER_DBM)
                {
                    new_config.output_power_dbm = LR2021_24_GHZ_MAX_POWER_DBM;
                }
            }

            ESP_LOGI(TAG,
                     "LR2021 band switch: %s %.1fMHz -> %s %.1fMHz, reinitialize bw:%.1fkHz power:%ddBm",
                     old_high_frequency ? "HF" : "LF", (double)s_config.freq_mhz,
                     new_high_frequency ? "HF" : "LF", (double)freq_mhz,
                     (double)new_config.bandwidth_khz, new_config.output_power_dbm);

            state = lora_app_init(&new_config);
            if (state != RADIOLIB_ERR_NONE)
            {
                ESP_LOGE(TAG, "LR2021 band switch reinitialize failed, state:%d", state);
                int restore_state = lora_app_init(&old_config);
                if (restore_state != RADIOLIB_ERR_NONE)
                {
                    ESP_LOGE(TAG, "LR2021 previous band restore failed, state:%d", restore_state);
                }
                else
                {
                    ESP_LOGW(TAG, "LR2021 restored previous %s %.1fMHz configuration",
                             old_high_frequency ? "HF" : "LF", (double)old_config.freq_mhz);
                }
            }
            return state;
        }

        LR2021 *radio = static_cast<LR2021 *>(s_radio);
        state = radio->standby();
        if (state != RADIOLIB_ERR_NONE)
        {
            ESP_LOGE(TAG, "LR2021 standby before frequency change failed, state:%d", state);
            return state;
        }
        state = lora_app_lr2021_configure_rf_switch(radio);
        if (state != RADIOLIB_ERR_NONE)
        {
            return state;
        }
    }

    state = s_radio->setFrequency(freq_mhz);
    if (state != RADIOLIB_ERR_NONE)
    {
        ESP_LOGE(TAG, "set frequency %.1fMHz failed, state:%d", (double)freq_mhz, state);
        return state;
    }

    s_config.freq_mhz = freq_mhz;
    if (s_config.chip == LORA_APP_CHIP_LR2021)
    {
        int8_t max_power = lora_app_max_output_power();
        if (s_config.output_power_dbm > max_power)
        {
            s_config.output_power_dbm = max_power;
        }
        state = s_radio->setOutputPower(s_config.output_power_dbm);
        if (state != RADIOLIB_ERR_NONE)
        {
            ESP_LOGE(TAG, "set output power after frequency change failed, state:%d", state);
            return state;
        }
        lora_app_lr2021_log_pa_status(s_config.output_power_dbm);
    }
    return RADIOLIB_ERR_NONE;
}

extern "C" int lora_app_set_bandwidth(float bandwidth_khz)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setBandwidth(bandwidth_khz);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setBandwidth(bandwidth_khz);
        break;
    case LORA_APP_CHIP_LR1121:
        state = static_cast<LR1121 *>(s_radio)->setBandwidth(bandwidth_khz);
        break;
    case LORA_APP_CHIP_LR2021:
        state = static_cast<LR2021 *>(s_radio)->setBandwidth(bandwidth_khz);
        break;
    default:
        state = RADIOLIB_ERR_INVALID_FUNCTION;
        break;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.bandwidth_khz = bandwidth_khz;
    }
    return state;
}

extern "C" int lora_app_set_spreading_factor(uint8_t spreading_factor)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setSpreadingFactor(spreading_factor);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setSpreadingFactor(spreading_factor);
        break;
    case LORA_APP_CHIP_LR1121:
        state = static_cast<LR1121 *>(s_radio)->setSpreadingFactor(spreading_factor);
        break;
    case LORA_APP_CHIP_LR2021:
        state = static_cast<LR2021 *>(s_radio)->setSpreadingFactor(spreading_factor);
        break;
    default:
        state = RADIOLIB_ERR_INVALID_FUNCTION;
        break;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.spreading_factor = spreading_factor;
    }
    return state;
}

extern "C" int lora_app_set_coding_rate(uint8_t coding_rate)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setCodingRate(coding_rate);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setCodingRate(coding_rate);
        break;
    case LORA_APP_CHIP_LR1121:
        state = static_cast<LR1121 *>(s_radio)->setCodingRate(coding_rate);
        break;
    case LORA_APP_CHIP_LR2021:
        state = static_cast<LR2021 *>(s_radio)->setCodingRate(coding_rate);
        break;
    default:
        state = RADIOLIB_ERR_INVALID_FUNCTION;
        break;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.coding_rate = coding_rate;
    }
    return state;
}

extern "C" int lora_app_set_output_power(int8_t power_dbm)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    int8_t max_power = lora_app_max_output_power();
    if (power_dbm > max_power)
    {
        ESP_LOGW(TAG, "Clamp output power %ddBm to %ddBm at %.1fMHz",
                 power_dbm, max_power, (double)s_config.freq_mhz);
        power_dbm = max_power;
    }
    state = s_radio->setOutputPower(power_dbm);
    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.output_power_dbm = power_dbm;
        if (s_config.chip == LORA_APP_CHIP_LR2021)
        {
            lora_app_lr2021_log_pa_status(power_dbm);
        }
    }
    return state;
}

extern "C" int8_t lora_app_get_max_output_power(void)
{
    return lora_app_max_output_power();
}

extern "C" int lora_app_set_current_limit(float current_ma)
{
    return lora_app_apply_current_limit(current_ma);
}

extern "C" int lora_app_set_sync_word(uint8_t sync_word)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setSyncWord(sync_word);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setSyncWord(sync_word);
        break;
    case LORA_APP_CHIP_LR1121:
        state = static_cast<LR1121 *>(s_radio)->setSyncWord(sync_word);
        break;
    case LORA_APP_CHIP_LR2021:
        state = static_cast<LR2021 *>(s_radio)->setSyncWord(sync_word);
        break;
    default:
        state = RADIOLIB_ERR_INVALID_FUNCTION;
        break;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.sync_word = sync_word;
    }
    return state;
}

extern "C" int lora_app_set_preamble_length(size_t preamble_len)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    state = s_radio->setPreambleLength(preamble_len);
    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.preamble_len = (uint16_t)preamble_len;
    }
    return state;
}

extern "C" int lora_app_set_crc(bool enabled)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    switch (s_config.chip)
    {
    case LORA_APP_CHIP_SX1276:
        state = static_cast<SX1276 *>(s_radio)->setCRC(enabled);
        break;
    case LORA_APP_CHIP_SX1262:
        state = static_cast<SX1262 *>(s_radio)->setCRC(enabled);
        break;
    case LORA_APP_CHIP_LR1121:
        state = static_cast<LR1121 *>(s_radio)->setCRC(enabled ? 2 : 0);
        break;
    case LORA_APP_CHIP_LR2021:
        state = static_cast<LR2021 *>(s_radio)->setCRC(enabled ? 2 : 0);
        break;
    default:
        return RADIOLIB_ERR_INVALID_FUNCTION;
    }

    if (state == RADIOLIB_ERR_NONE)
    {
        s_crc_enabled = enabled;
    }
    return state;
}

extern "C" int lora_app_set_invert_iq(bool enabled)
{
    int state = lora_app_require_radio();
    if (state != RADIOLIB_ERR_NONE)
    {
        return state;
    }

    state = s_radio->invertIQ(enabled);
    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.invert_iq = enabled;
    }
    return state;
}

extern "C" float lora_app_get_rssi(void)
{
    return lora_app_has_radio() ? s_radio->getRSSI() : 0.0f;
}

extern "C" float lora_app_get_snr(void)
{
    return lora_app_has_radio() ? s_radio->getSNR() : 0.0f;
}

extern "C" uint8_t lora_app_random_byte(void)
{
    return lora_app_has_radio() ? s_radio->randomByte() : 0;
}
