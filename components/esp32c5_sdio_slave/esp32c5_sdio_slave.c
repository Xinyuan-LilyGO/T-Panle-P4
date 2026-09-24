#include "esp32c5_sdio_slave.h"

#include <stdlib.h>
#include <string.h>

#include "eh_host_feat_wifi.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define WIFI_STARTUP_MAX_TX_POWER_QDBM 64
#define WIFI_LOW_POWER_TX_POWER_QDBM   32
#define WIFI_CONNECTED_BIT             BIT0
#define WIFI_SCAN_DONE_BIT             BIT1
#define C5_CONTROL_LOCK_TIMEOUT_MS      1000
#define C5_RESTART_PULSE_MS             20

#ifndef CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM 10
#endif
#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM 32
#endif
#ifndef CONFIG_WIFI_RMT_TX_BUFFER_TYPE
#define CONFIG_WIFI_RMT_TX_BUFFER_TYPE 1
#endif
#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF
#define CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF 1
#endif
#ifndef CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM
#define CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM 7
#endif

static const char *TAG = "esp32c5_sdio";

static esp32c5_sdio_slave_config_t s_config;
static SemaphoreHandle_t s_control_mutex;
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static volatile bool s_initialized;
static volatile bool s_enabled;
static volatile bool s_ready;
static volatile bool s_sleeping;
static bool s_hosted_initialized;
static bool s_wifi_feature_initialized;
static bool s_wifi_initialized;
static bool s_wifi_started;
static volatile bool s_wifi_enabled;
static volatile bool s_wifi_connected;
static volatile bool s_wifi_manual_scan;
static volatile bool s_wifi_manual_connect;
static bool s_wifi_resume_enabled = true;
static char s_initial_ssid[33];
static char s_initial_password[65];

static int c5_active_level(void)
{
    return s_config.enable_active_high ? 1 : 0;
}

static esp_err_t c5_set_hardware_enabled(bool enabled)
{
    if (s_config.enable_gpio == GPIO_NUM_NC)
    {
        return ESP_OK;
    }

    esp_err_t ret = gpio_set_direction(s_config.enable_gpio, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = gpio_set_level(s_config.enable_gpio,
                         enabled ? c5_active_level() : !c5_active_level());
    if (ret == ESP_OK && enabled && s_config.enable_delay_ms > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(s_config.enable_delay_ms));
    }
    return ret;
}

static const char *wifi_auth_type_name(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "SEC";
    }
}

static void cstring_copy(char *destination,
                         size_t destination_size,
                         const char *source)
{
    if (destination == NULL || destination_size == 0)
    {
        return;
    }
    memset(destination, 0, destination_size);
    if (source != NULL)
    {
        size_t source_length = strlen(source);
        size_t copy_length = source_length < destination_size - 1
                                 ? source_length
                                 : destination_size - 1;
        memcpy(destination, source, copy_length);
    }
}

static void wifi_set_error(char *err, size_t err_len, const char *message)
{
    cstring_copy(err, err_len, message);
}

static void wifi_config_field_copy(uint8_t *destination,
                                   size_t destination_size,
                                   const char *source)
{
    if (source == NULL || destination_size == 0)
    {
        return;
    }
    size_t source_length = strlen(source);
    size_t copy_length = source_length < destination_size
                             ? source_length
                             : destination_size;
    memcpy(destination, source, copy_length);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        s_wifi_connected = false;
        if (!s_wifi_enabled || !s_ready)
        {
            return;
        }
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN)
        {
            ESP_LOGW(TAG, "Wi-Fi start connect failed: %s", esp_err_to_name(ret));
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_wifi_connected = false;
        ESP_LOGI(TAG, "Wi-Fi connected, waiting for IP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        s_wifi_manual_connect = false;
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, automatic reconnect disabled",
                 event != NULL ? event->reason : -1);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event != NULL)
        {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        s_wifi_connected = true;
        s_wifi_manual_connect = false;
        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_runtime_create(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        return ret;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT,
                                   ESP_EVENT_ANY_ID,
                                   wifi_event_handler,
                                   NULL),
        TAG, "register Wi-Fi event handler failed");
    ret = esp_event_handler_register(IP_EVENT,
                                     IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK)
    {
        (void)esp_event_handler_unregister(WIFI_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           wifi_event_handler);
    }
    return ret;
}

static void wifi_runtime_destroy(void)
{
    (void)esp_event_handler_unregister(IP_EVENT,
                                       IP_EVENT_STA_GOT_IP,
                                       wifi_event_handler);
    (void)esp_event_handler_unregister(WIFI_EVENT,
                                       ESP_EVENT_ANY_ID,
                                       wifi_event_handler);
    if (s_sta_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
}

static esp_err_t wifi_driver_start(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&config);
    if (ret != ESP_OK)
    {
        return ret;
    }
    s_wifi_initialized = true;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        goto fail;
    }

    if (s_initial_ssid[0] != '\0')
    {
        wifi_config_t wifi_config = {0};
        wifi_config_field_copy(wifi_config.sta.ssid,
                               sizeof(wifi_config.sta.ssid),
                               s_initial_ssid);
        wifi_config_field_copy(wifi_config.sta.password,
                               sizeof(wifi_config.sta.password),
                               s_initial_password);
        wifi_config.sta.threshold.authmode =
            s_config.initial_authmode == WIFI_AUTH_OPEN
                ? WIFI_AUTH_OPEN
                : WIFI_AUTH_WPA_PSK;
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK)
        {
            goto fail;
        }
    }

    (void)esp_wifi_set_max_tx_power(WIFI_STARTUP_MAX_TX_POWER_QDBM);
    s_ready = true;
    s_wifi_enabled = true;
    ret = esp_wifi_start();
    if (ret == ESP_OK)
    {
        s_wifi_started = true;
        return ESP_OK;
    }

fail:
    s_ready = false;
    s_wifi_enabled = false;
    (void)esp_wifi_deinit();
    s_wifi_initialized = false;
    return ret;
}

static bool transport_down(void)
{
    bool ok = true;

    s_ready = false;
    s_wifi_enabled = false;
    s_wifi_manual_scan = true;
    if (s_wifi_initialized)
    {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_wifi_started)
        {
            esp_err_t ret = esp_wifi_stop();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED)
            {
                ok = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t ret = esp_wifi_deinit();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT)
        {
            ESP_LOGW(TAG, "Wi-Fi deinit returned: %s", esp_err_to_name(ret));
            ok = false;
        }
    }
    s_wifi_started = false;
    s_wifi_initialized = false;

#if !CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_AUTO_INIT
    if (s_wifi_feature_initialized)
    {
        if (eh_host_feat_wifi_deinit() != 0)
        {
            ok = false;
        }
    }
#endif
    s_wifi_feature_initialized = false;

    if (s_hosted_initialized)
    {
        if (esp_hosted_deinit() != ESP_OK)
        {
            ok = false;
        }
    }
    s_hosted_initialized = false;
    s_wifi_manual_scan = false;
    s_wifi_manual_connect = false;
    s_wifi_connected = false;
    if (s_wifi_event_group != NULL)
    {
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_SCAN_DONE_BIT);
    }
    return ok;
}

static esp_err_t transport_up(bool enable_wifi)
{
    esp_err_t ret = esp_hosted_init();
    if (ret != ESP_OK)
    {
        return ret;
    }
    s_hosted_initialized = true;

    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK)
    {
        goto fail;
    }

#if !CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_AUTO_INIT
    if (eh_host_feat_wifi_init() != 0)
    {
        ret = ESP_FAIL;
        goto fail;
    }
    s_wifi_feature_initialized = true;
#endif

    ret = wifi_driver_start();
    if (ret != ESP_OK)
    {
        goto fail;
    }

    if (!enable_wifi && !esp32c5_sdio_slave_wifi_set_enabled(false))
    {
        ret = ESP_FAIL;
        goto fail;
    }
    return ESP_OK;

fail:
    (void)transport_down();
    return ret;
}

esp_err_t esp32c5_sdio_slave_init(const esp32c5_sdio_slave_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "configuration is required");
    if (s_control_mutex == NULL)
    {
        s_control_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_control_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "create control mutex failed");
    }
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_control_mutex,
                       pdMS_TO_TICKS(C5_CONTROL_LOCK_TIMEOUT_MS)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "control lock timeout");

    if (s_initialized)
    {
        xSemaphoreGive(s_control_mutex);
        return ESP_OK;
    }

    s_config = *config;
    cstring_copy(s_initial_ssid, sizeof(s_initial_ssid), config->initial_ssid);
    cstring_copy(s_initial_password, sizeof(s_initial_password),
                 config->initial_password);

    esp_err_t ret = c5_set_hardware_enabled(true);
    if (ret == ESP_OK)
    {
        s_enabled = true;
        ret = wifi_runtime_create();
    }
    if (ret == ESP_OK)
    {
        ret = transport_up(true);
    }

    if (ret == ESP_OK)
    {
        s_initialized = true;
        s_sleeping = false;
        ESP_LOGI(TAG, "ESP32-C5 SDIO Wi-Fi ready");
    }
    else
    {
        (void)transport_down();
        wifi_runtime_destroy();
        (void)c5_set_hardware_enabled(false);
        s_enabled = false;
        ESP_LOGE(TAG, "ESP32-C5 initialization failed: %s",
                 esp_err_to_name(ret));
    }

    xSemaphoreGive(s_control_mutex);
    return ret;
}

esp_err_t esp32c5_sdio_slave_deinit(void)
{
    if (s_control_mutex == NULL)
    {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_control_mutex,
                       pdMS_TO_TICKS(C5_CONTROL_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    bool ok = transport_down();
    wifi_runtime_destroy();
    esp_err_t gpio_ret = c5_set_hardware_enabled(false);
    s_initialized = false;
    s_enabled = false;
    s_sleeping = false;
    xSemaphoreGive(s_control_mutex);
    return ok ? gpio_ret : ESP_FAIL;
}

bool esp32c5_sdio_slave_is_enabled(void)
{
    return s_enabled;
}

bool esp32c5_sdio_slave_is_ready(void)
{
    return s_ready;
}

bool esp32c5_sdio_slave_is_sleeping(void)
{
    return s_sleeping;
}

bool esp32c5_sdio_slave_probe(uint32_t *chip_id,
                              char *target_name,
                              size_t target_name_len)
{
    if (chip_id == NULL || target_name == NULL || target_name_len == 0)
    {
        return false;
    }
    *chip_id = 0;
    target_name[0] = '\0';
    if (!s_initialized || !s_enabled || !s_ready || s_sleeping)
    {
        return false;
    }

    return esp_hosted_get_cp_info(chip_id, target_name, target_name_len) == ESP_OK;
}

bool esp32c5_sdio_slave_wifi_is_enabled(void)
{
    return s_wifi_enabled;
}

bool esp32c5_sdio_slave_wifi_is_connected(void)
{
    return s_wifi_connected;
}

bool esp32c5_sdio_slave_wifi_set_enabled(bool enabled)
{
    if (!s_ready || s_sleeping)
    {
        return false;
    }
    if (enabled == s_wifi_enabled)
    {
        return true;
    }

    if (!enabled)
    {
        s_wifi_enabled = false;
        s_wifi_manual_scan = true;
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_disconnect();
        esp_err_t ret = esp_wifi_stop();
        s_wifi_started = false;
        s_wifi_manual_scan = false;
        s_wifi_manual_connect = false;
        s_wifi_connected = false;
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_CONNECTED_BIT | WIFI_SCAN_DONE_BIT);
        }
        return ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_STARTED;
    }

    s_wifi_enabled = true;
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        s_wifi_enabled = false;
        ESP_LOGW(TAG, "enable Wi-Fi failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_wifi_started = true;
    return true;
}

bool esp32c5_sdio_slave_set_enabled(bool enabled)
{
    if (!s_initialized || s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex,
                       pdMS_TO_TICKS(C5_CONTROL_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return false;
    }

    bool ok = true;
    if (enabled && !s_enabled)
    {
        ok = c5_set_hardware_enabled(true) == ESP_OK;
        if (ok)
        {
            ok = transport_up(s_wifi_resume_enabled) == ESP_OK;
        }
        if (!ok)
        {
            (void)c5_set_hardware_enabled(false);
        }
        s_enabled = ok;
        s_sleeping = false;
    }
    else if (!enabled && s_enabled)
    {
        s_wifi_resume_enabled = s_wifi_enabled;
        ok = transport_down();
        if (c5_set_hardware_enabled(false) != ESP_OK)
        {
            ok = false;
        }
        if (ok)
        {
            s_enabled = false;
            s_sleeping = false;
        }
    }

    xSemaphoreGive(s_control_mutex);
    return ok;
}

bool esp32c5_sdio_slave_set_sleeping(bool sleeping)
{
    if (!s_initialized || s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex,
                       pdMS_TO_TICKS(C5_CONTROL_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return false;
    }

    bool ok = s_enabled;
    if (ok && sleeping && !s_sleeping)
    {
        s_wifi_resume_enabled = s_wifi_enabled;
        ok = transport_down();
        if (ok)
        {
            ok = c5_set_hardware_enabled(false) == ESP_OK;
        }
        s_sleeping = ok;
    }
    else if (ok && !sleeping && s_sleeping)
    {
        ok = c5_set_hardware_enabled(true) == ESP_OK;
        if (ok)
        {
            ok = transport_up(s_wifi_resume_enabled) == ESP_OK;
        }
        if (!ok)
        {
            (void)c5_set_hardware_enabled(false);
        }
        s_sleeping = !ok;
    }

    xSemaphoreGive(s_control_mutex);
    return ok;
}

bool esp32c5_sdio_slave_restart(void)
{
    if (!s_initialized || !s_enabled || s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex,
                       pdMS_TO_TICKS(C5_CONTROL_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return false;
    }

    bool wifi_was_enabled = s_sleeping ? s_wifi_resume_enabled : s_wifi_enabled;
    bool ok = transport_down();
    if (c5_set_hardware_enabled(false) != ESP_OK)
    {
        ok = false;
    }
    vTaskDelay(pdMS_TO_TICKS(C5_RESTART_PULSE_MS));
    if (c5_set_hardware_enabled(true) != ESP_OK)
    {
        ok = false;
    }
    if (ok)
    {
        ok = transport_up(wifi_was_enabled) == ESP_OK;
    }
    if (!ok)
    {
        (void)c5_set_hardware_enabled(false);
    }
    s_enabled = ok;
    s_sleeping = false;
    xSemaphoreGive(s_control_mutex);
    return ok;
}

static void wifi_scan_record_store(esp32c5_wifi_ap_info_t *aps,
                                   int max_count,
                                   int *count,
                                   const wifi_ap_record_t *record)
{
    if (*count >= max_count || record == NULL || record->ssid[0] == '\0')
    {
        return;
    }

    esp32c5_wifi_ap_info_t *item = &aps[*count];
    cstring_copy(item->ssid, sizeof(item->ssid), (const char *)record->ssid);
    item->rssi = record->rssi;
    item->authmode = record->authmode;
    cstring_copy(item->type, sizeof(item->type),
                 wifi_auth_type_name(record->authmode));
    (*count)++;
}

bool esp32c5_sdio_slave_wifi_scan(esp32c5_wifi_ap_info_t *aps,
                                  int max_count,
                                  int *out_count,
                                  char *err,
                                  size_t err_len)
{
    if (aps == NULL || max_count <= 0 || out_count == NULL)
    {
        wifi_set_error(err, err_len, "Invalid scan arguments");
        return false;
    }
    *out_count = 0;
    if (!s_ready || !s_wifi_enabled || s_sleeping)
    {
        wifi_set_error(err, err_len, "C5 Wi-Fi is not ready");
        return false;
    }

    s_wifi_manual_scan = true;
    s_wifi_manual_connect = false;
    (void)esp_wifi_disconnect();
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    s_wifi_manual_scan = false;
    if (ret != ESP_OK)
    {
        wifi_set_error(err, err_len, esp_err_to_name(ret));
        return false;
    }

    uint16_t count = (uint16_t)max_count;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (records == NULL)
    {
        wifi_set_error(err, err_len, "No memory for scan results");
        return false;
    }

    ret = esp_wifi_scan_get_ap_records(&count, records);
    if (ret == ESP_OK)
    {
        for (uint16_t i = 0; i < count; ++i)
        {
            wifi_scan_record_store(aps, max_count, out_count, &records[i]);
        }
    }
    free(records);
    if (ret != ESP_OK)
    {
        wifi_set_error(err, err_len, esp_err_to_name(ret));
        return false;
    }
    wifi_set_error(err, err_len, "");
    return true;
}

bool esp32c5_sdio_slave_wifi_connect(const char *ssid,
                                     const char *password,
                                     wifi_auth_mode_t authmode,
                                     char *err,
                                     size_t err_len)
{
    if (ssid == NULL || ssid[0] == '\0' || !s_ready ||
        !s_wifi_enabled || s_sleeping)
    {
        wifi_set_error(err, err_len, "C5 Wi-Fi is not ready");
        return false;
    }

    s_wifi_manual_connect = true;
    wifi_config_t wifi_config = {0};
    wifi_config_field_copy(wifi_config.sta.ssid,
                           sizeof(wifi_config.sta.ssid),
                           ssid);
    wifi_config_field_copy(wifi_config.sta.password,
                           sizeof(wifi_config.sta.password),
                           password);
    wifi_config.sta.threshold.authmode =
        authmode == WIFI_AUTH_OPEN ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    (void)esp_wifi_disconnect();
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK)
    {
        ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN)
        {
            ret = ESP_OK;
        }
    }
    if (ret != ESP_OK)
    {
        s_wifi_manual_connect = false;
        wifi_set_error(err, err_len, esp_err_to_name(ret));
        return false;
    }
    wifi_set_error(err, err_len, "");
    return true;
}

bool esp32c5_sdio_slave_wifi_enter_low_power(void)
{
    if (!s_ready || !s_wifi_enabled)
    {
        return true;
    }
    bool ok = true;
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set Wi-Fi power save failed: %s", esp_err_to_name(ret));
        ok = false;
    }
    ret = esp_wifi_set_max_tx_power(WIFI_LOW_POWER_TX_POWER_QDBM);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "limit Wi-Fi TX power failed: %s", esp_err_to_name(ret));
        ok = false;
    }
    return ok;
}

bool esp32c5_sdio_slave_wifi_exit_low_power(void)
{
    if (!s_ready || !s_wifi_enabled)
    {
        return true;
    }
    bool ok = true;
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "disable Wi-Fi power save failed: %s", esp_err_to_name(ret));
        ok = false;
    }
    ret = esp_wifi_set_max_tx_power(WIFI_STARTUP_MAX_TX_POWER_QDBM);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "restore Wi-Fi TX power failed: %s", esp_err_to_name(ret));
        ok = false;
    }
    return ok;
}
