#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_io_expander.h"

typedef enum
{
    FACTORY_LORA_MODE_STOP = 0,
    FACTORY_LORA_MODE_TX,
    FACTORY_LORA_MODE_RX,
    FACTORY_LORA_MODE_TX_ONCE,
} factory_lora_mode_t;

typedef struct
{
    void (*status)(factory_lora_mode_t mode, int state, const char *message,
                   void *user_data);
    void (*tx_done)(const char *text, int state, void *user_data);
    void (*rx_done)(const char *text, float rssi, float snr, int state,
                    void *user_data);
    void (*tx_finished)(void *user_data);
    void *user_data;
} factory_lora_callbacks_t;

esp_err_t lora_init(esp_io_expander_handle_t io_expander);
void factory_lora_set_callbacks(const factory_lora_callbacks_t *callbacks);
bool factory_lora_request_mode(factory_lora_mode_t mode);
bool factory_lora_send_once(const char *text);
void factory_lora_process(void);
void factory_lora_stop(void);
factory_lora_mode_t factory_lora_get_mode(void);
factory_lora_mode_t factory_lora_get_pending_mode(void);
bool factory_lora_is_switch_pending(void);
void factory_lora_set_tx_interval(int interval_s);
int factory_lora_set_frequency(float frequency_mhz);
int factory_lora_set_bandwidth(float bandwidth_khz);
int factory_lora_set_spreading_factor(uint8_t spreading_factor);
int factory_lora_set_coding_rate(uint8_t coding_rate);
int factory_lora_set_output_power(int8_t power_dbm);
int8_t factory_lora_get_max_output_power(float frequency_mhz);
