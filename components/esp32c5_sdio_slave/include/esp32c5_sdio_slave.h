#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int rssi;
    char type[16];
    wifi_auth_mode_t authmode;
} esp32c5_wifi_ap_info_t;

typedef struct {
    gpio_num_t enable_gpio;
    bool enable_active_high;
    uint32_t enable_delay_ms;
    const char *initial_ssid;
    const char *initial_password;
    wifi_auth_mode_t initial_authmode;
} esp32c5_sdio_slave_config_t;

#define ESP32C5_SDIO_SLAVE_CONFIG_DEFAULT() \
    {                                        \
        .enable_gpio = GPIO_NUM_NC,          \
        .enable_active_high = true,          \
        .enable_delay_ms = 100,              \
        .initial_ssid = NULL,                \
        .initial_password = NULL,            \
        .initial_authmode = 0,               \
    }

esp_err_t esp32c5_sdio_slave_init(const esp32c5_sdio_slave_config_t *config);
esp_err_t esp32c5_sdio_slave_deinit(void);

bool esp32c5_sdio_slave_is_enabled(void);
bool esp32c5_sdio_slave_set_enabled(bool enabled);
bool esp32c5_sdio_slave_is_ready(void);
bool esp32c5_sdio_slave_is_sleeping(void);
bool esp32c5_sdio_slave_set_sleeping(bool sleeping);
bool esp32c5_sdio_slave_restart(void);
bool esp32c5_sdio_slave_probe(uint32_t *chip_id,
                              char *target_name,
                              size_t target_name_len);

bool esp32c5_sdio_slave_wifi_is_enabled(void);
bool esp32c5_sdio_slave_wifi_set_enabled(bool enabled);
bool esp32c5_sdio_slave_wifi_is_connected(void);
bool esp32c5_sdio_slave_wifi_scan(esp32c5_wifi_ap_info_t *aps,
                                  int max_count,
                                  int *out_count,
                                  char *err,
                                  size_t err_len);
bool esp32c5_sdio_slave_wifi_connect(const char *ssid,
                                     const char *password,
                                     wifi_auth_mode_t authmode,
                                     char *err,
                                     size_t err_len);
bool esp32c5_sdio_slave_wifi_enter_low_power(void);
bool esp32c5_sdio_slave_wifi_exit_low_power(void);

#ifdef __cplusplus
}
#endif
