/*
 * ESP32-P4 host test for ESP32-C5 running ESP-Hosted slave firmware.
 *
 * This example intentionally uses the ESP-Hosted protocol layer instead of
 * sending raw ESSL packets. If the C5 runs ESP-Hosted, raw ESSL packets are
 * interpreted as malformed ESP-Hosted frames and the C5 logs len/offset errors.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "eh_host_feat_wifi.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

static const char *TAG = "esp32p4_host";

static EventGroupHandle_t s_wifi_event_group;
static bool s_time_sync_task_started = false;

#define WIFI_CONNECTED_BIT BIT0
#define TIME_SYNCED_BIT BIT1

static void time_sync_task(void *arg)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK)
    {
        ESP_LOGW(TAG, "SNTP sync timeout, retrying");
    }

    ESP_LOGI(TAG, "SNTP time synchronized");
    xEventGroupSetBits(s_wifi_event_group, TIME_SYNCED_BIT);
    vTaskDelete(NULL);
}

static void time_print_task(void *arg)
{
    ESP_LOGI(TAG, "Time print task started");
    xEventGroupWaitBits(s_wifi_event_group, TIME_SYNCED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (1)
    {
        time_t now = 0;
        struct tm timeinfo = {0};
        char strftime_buf[64] = {0};

        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC+8", &timeinfo);
        ESP_LOGI(TAG, "Current time: %s", strftime_buf);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wifi_set_sta_band(void)
{
#if CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
    esp_err_t ret = esp_wifi_set_country_code("CN", true);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set Wi-Fi country failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set Wi-Fi band mode failed: %s", esp_err_to_name(ret));
        return;
    }

    wifi_band_mode_t band_mode = 0;
    ret = esp_wifi_get_band_mode(&band_mode);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi band mode: %s",
                 band_mode == WIFI_BAND_MODE_2G_ONLY ? "2.4G only" : band_mode == WIFI_BAND_MODE_5G_ONLY ? "5G only"
                                                                 : band_mode == WIFI_BAND_MODE_AUTO      ? "2.4G + 5G"
                                                                                                         : "unknown");
    }
#endif
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Wi-Fi started, connecting");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Wi-Fi connected, waiting for IP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, retrying", event->reason);
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_time_sync_task_started)
        {
            s_time_sync_task_started = true;
            xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 5, NULL);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_hosted_init());             // Initialize ESP-Hosted
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave()); // Connect to ESP-Hosted slave
#if !CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_AUTO_INIT
    ESP_ERROR_CHECK(eh_host_feat_wifi_init() == 0 ? ESP_OK : ESP_FAIL);
#endif

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(esp_netif_init());                // Initialize ESP-Netif
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Create default event loop
    esp_netif_create_default_wifi_sta();              // Create default Wi-Fi station interface
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "xinyuandianzi",
            .password = "AA15994823428",
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_set_sta_band();
    ESP_LOGI(TAG, "Wi-Fi start requested, connecting directly");

    xTaskCreate(time_print_task, "time_print", 4096, NULL, 5, NULL);
}
