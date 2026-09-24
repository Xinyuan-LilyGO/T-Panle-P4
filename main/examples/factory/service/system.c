
#include "system.h"

#include <stdlib.h>
#include <time.h>

#include "sdkconfig.h"
#include "bmu.h"
#include "display_panel.h"
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
#include "esp32c5_sdio_slave.h"
#endif
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_T_PANEL_P4_HAS_LORA
#include "lora_app.h"
#endif

static const char *TAG = "factory_system";
static volatile uint32_t s_lcd_flush_count;
static bool s_time_sync_started;

#define FACTORY_BRIGHTNESS_MIN 5
#define FACTORY_TIMEZONE "CST-8"
#define FACTORY_SNTP_SERVER "ntp.aliyun.com"

static void factory_time_sync_cb(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    char text[32] = {0};
    if (localtime_r(&now, &timeinfo) != NULL)
    {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "System time synchronized: %s", text);
    }
}

static void factory_time_ip_event_handler(void *arg,
                                          esp_event_base_t event_base,
                                          int32_t event_id,
                                          void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    esp_err_t ret = esp_netif_sntp_start();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Start SNTP after got IP failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t factory_time_sync_start(void)
{
    if (s_time_sync_started)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(setenv("TZ", FACTORY_TIMEZONE, 1) == 0,
                        ESP_FAIL, TAG, "set timezone failed");
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(FACTORY_SNTP_SERVER);
    config.start = false;
    config.sync_cb = factory_time_sync_cb;
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     factory_time_ip_event_handler, NULL);
    if (ret != ESP_OK)
    {
        esp_netif_sntp_deinit();
        return ret;
    }

    s_time_sync_started = true;
    ESP_LOGI(TAG, "SNTP ready: server=%s timezone=%s",
             FACTORY_SNTP_SERVER, FACTORY_TIMEZONE);
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
    if (esp32c5_sdio_slave_wifi_is_connected())
    {
        factory_time_ip_event_handler(NULL, IP_EVENT,
                                      IP_EVENT_STA_GOT_IP, NULL);
    }
#endif
    return ESP_OK;
}

void system_lcd_flush_record(void)
{
    s_lcd_flush_count++;
}

static int percent_used(size_t free_size, size_t total_size)
{
    if (total_size == 0 || free_size > total_size)
    {
        return -1;
    }

    return (int)(((total_size - free_size) * 100) / total_size);
}

bool status_bar_info_get(status_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    int battery_percent = -1;
    bool battery_charging = false;
    bmu_battery_summary_get(&battery_percent, &battery_charging);

    bool wifi_connected = false;
    bool bluetooth_enabled = false;
    bool bluetooth_connected = false;
#if CONFIG_T_PANEL_P4_HAS_ESP32C5
    wifi_connected = esp32c5_sdio_slave_wifi_is_connected();
#endif

    *status = (status_info_t){
        .wifi_connected = wifi_connected,
        .bluetooth_enabled = bluetooth_enabled,
        .bluetooth_connected = bluetooth_connected,
        .battery_charging = battery_charging,
        .battery_percent = battery_percent,
    };
    return true;
}

bool factory_power_enter_ship_mode(void)
{
    if (!bmu_is_ready())
    {
        ESP_LOGW(TAG, "Cannot enter ship mode: BMU is not ready");
        return false;
    }

    ESP_LOGW(TAG, "Entering AXP517 ship mode");
    display_panel_set_brightness(FACTORY_BRIGHTNESS_MIN);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!bmu_enter_ship_mode())
    {
        return false;
    }
    return true;
}

void factory_power_restart(void)
{
    ESP_LOGW(TAG, "Restart requested from settings page");
    display_panel_set_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

bool factory_power_enter_low_power(void)
{
    bool ok = true;

    ESP_LOGI(TAG, "Entering low power profile");

    if (display_panel_set_brightness(FACTORY_BRIGHTNESS_MIN) != ESP_OK)
    {
        ok = false;
    }

#if CONFIG_T_PANEL_P4_HAS_ESP32C5
    if (!esp32c5_sdio_slave_wifi_enter_low_power())
    {
        ok = false;
    }
#endif

#if CONFIG_T_PANEL_P4_HAS_LORA
    if (lora_app_is_started() && lora_app_sleep() != ESP_OK)
    {
        ESP_LOGW(TAG, "LoRa sleep request failed");
    }
#endif

    if (!bmu_enter_low_power())
    {
        ok = false;
    }

    return ok;
}

bool factory_power_exit_low_power(void)
{
    bool ok = true;

    ESP_LOGI(TAG, "Leaving low power profile");

#if CONFIG_T_PANEL_P4_HAS_ESP32C5
    if (!esp32c5_sdio_slave_wifi_exit_low_power())
    {
        ok = false;
    }
#endif

#if CONFIG_T_PANEL_P4_HAS_LORA
    if (lora_app_is_started() && lora_app_standby() != ESP_OK)
    {
        ESP_LOGW(TAG, "LoRa standby request failed");
    }
#endif

    if (!bmu_exit_low_power())
    {
        ok = false;
    }

    return ok;
}

bool home_status_info_get(home_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    static bool s_status_inited = false;
    static uint32_t s_last_flush_count = 0;
    static int64_t s_last_sample_us = 0;

    int64_t now_us = esp_timer_get_time();
    if (!s_status_inited)
    {
        s_last_flush_count = s_lcd_flush_count;
        s_last_sample_us = now_us;
        s_status_inited = true;
    }

    uint32_t flush_count = s_lcd_flush_count;
    uint32_t flush_delta = flush_count - s_last_flush_count;
    int64_t elapsed_us = now_us - s_last_sample_us;
    int fps = -1;
    if (elapsed_us > 0)
    {
        fps = (int)((flush_delta * 1000000LL + elapsed_us / 2) / elapsed_us);
    }

    size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    int temp_c_x10 = -1;
    bmu_die_temperature_get(&temp_c_x10);

    *status = (home_info_t){
        .fps = fps,
        .sram_percent = percent_used(sram_free, sram_total),
        .psram_percent = percent_used(psram_free, psram_total),
        .sram_free_kb = (uint32_t)(sram_free / 1024),
        .psram_free_kb = (uint32_t)(psram_free / 1024),
        .temp_c_x10 = temp_c_x10,
    };
    s_last_flush_count = flush_count;
    s_last_sample_us = now_us;

    return true;
}
