#include "lora_app.h"

#include <string.h>

#include "radio_esp32p4_hal.h"
#include "T_Panle_P4_board_config.h"

static const char *TAG = "lora_app";

static Esp32p4Hal *s_hal = nullptr;
static Module *s_module = nullptr;
static PhysicalLayer *s_radio = nullptr;
static lora_app_config_t s_config = {};
static bool s_config_valid = false;
static bool s_started = false;
static lora_app_irq_cb_t s_irq_callback = nullptr;
static void *s_irq_user_data = nullptr;
static volatile uint32_t s_irq_events = 0;

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

    config->chip = LORA_APP_CHIP_SX1276;
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
    config->output_power_dbm = 16;
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
        return static_cast<LR2021 *>(s_radio)->begin(config->freq_mhz,
                                                     config->bandwidth_khz,
                                                     config->spreading_factor,
                                                     config->coding_rate,
                                                     config->sync_word,
                                                     config->output_power_dbm,
                                                     config->preamble_len,
                                                     config->tcxo_voltage);
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

    state = s_radio->setFrequency(freq_mhz);
    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.freq_mhz = freq_mhz;
    }
    return state;
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

    state = s_radio->setOutputPower(power_dbm);
    if (state == RADIOLIB_ERR_NONE)
    {
        s_config.output_power_dbm = power_dbm;
    }
    return state;
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
        return static_cast<SX1276 *>(s_radio)->setCRC(enabled);
    case LORA_APP_CHIP_SX1262:
        return static_cast<SX1262 *>(s_radio)->setCRC(enabled);
    case LORA_APP_CHIP_LR1121:
        return static_cast<LR1121 *>(s_radio)->setCRC(enabled ? 2 : 0);
    case LORA_APP_CHIP_LR2021:
        return static_cast<LR2021 *>(s_radio)->setCRC(enabled ? 2 : 0);
    default:
        return RADIOLIB_ERR_INVALID_FUNCTION;
    }
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
