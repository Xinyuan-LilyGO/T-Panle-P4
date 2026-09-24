#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        LORA_APP_CHIP_SX1262 = 0,
        LORA_APP_CHIP_SX1276,
        LORA_APP_CHIP_LR1121,
        LORA_APP_CHIP_LR2021,
    } lora_app_chip_t;

    typedef enum
    {
        LORA_APP_IRQ_RX_DONE = (1U << 0),
        LORA_APP_IRQ_TX_DONE = (1U << 1),
    } lora_app_irq_event_t;

    typedef void (*lora_app_irq_cb_t)(lora_app_irq_event_t event, void *user_data);

    typedef struct
    {
        lora_app_chip_t chip;
        int8_t spi_sck;
        int8_t spi_miso;
        int8_t spi_mosi;
        int8_t cs;
        int8_t irq;
        int8_t rst;
        int8_t busy;
        uint32_t spi_freq_hz;
        float freq_mhz;
        float bandwidth_khz;
        uint8_t spreading_factor;
        uint8_t coding_rate;
        uint8_t sync_word;
        int8_t output_power_dbm;
        uint16_t preamble_len;
        float tcxo_voltage;
        float current_limit_ma;
        bool invert_iq;
    } lora_app_config_t;

    void lora_app_default_config(lora_app_config_t *config);
    int lora_app_init(const lora_app_config_t *config);
    int lora_app_start(void);
    void lora_app_stop(void);
    bool lora_app_is_started(void);
    lora_app_chip_t lora_app_get_chip(void);
    void lora_app_set_irq_callback(lora_app_irq_cb_t callback, void *user_data);
    uint32_t lora_app_get_and_clear_irq_events(void);

    int lora_app_transmit(const uint8_t *data, size_t len);
    int lora_app_transmit_text(const char *text);
    int lora_app_start_transmit(const uint8_t *data, size_t len);
    int lora_app_finish_transmit(void);
    int lora_app_receive(uint8_t *data, size_t len, uint32_t timeout_ms);
    int lora_app_start_receive(void);
    int lora_app_read_data(uint8_t *data, size_t len);
    size_t lora_app_get_packet_length(void);

    int lora_app_standby(void);
    int lora_app_sleep(void);
    int lora_app_scan_channel(void);

    int lora_app_set_frequency(float freq_mhz);
    int lora_app_set_bandwidth(float bandwidth_khz);
    int lora_app_set_spreading_factor(uint8_t spreading_factor);
    int lora_app_set_coding_rate(uint8_t coding_rate);
    int lora_app_set_output_power(int8_t power_dbm);
    int8_t lora_app_get_max_output_power(void);
    int lora_app_set_current_limit(float current_ma);
    int lora_app_set_sync_word(uint8_t sync_word);
    int lora_app_set_preamble_length(size_t preamble_len);
    int lora_app_set_crc(bool enabled);
    int lora_app_set_invert_iq(bool enabled);

    float lora_app_get_rssi(void);
    float lora_app_get_snr(void);
    uint8_t lora_app_random_byte(void);

#ifdef __cplusplus
}
#endif
