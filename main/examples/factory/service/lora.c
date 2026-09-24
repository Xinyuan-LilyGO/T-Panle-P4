#include "lora.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_io_expander_xl9555.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_app.h"

static const char *TAG = "factory_lora";

#define LORA_TASK_STACK_SIZE       6144
#define LORA_MODE_SWITCH_WAIT_MS   2000
#define LORA_TASK_STOP_POLL_MS     20
#define LORA_PACKET_MAX_LEN        256
#define LORA_TEXT_MAX_LEN          256
#define LORA_SUB_GHZ_MAX_POWER_DBM 22
#define LORA_24_GHZ_MAX_POWER_DBM  5
#define LORA_HF_MIN_FREQUENCY_MHZ  1900.0f

static volatile bool s_irq_flag;
static volatile factory_lora_mode_t s_mode = FACTORY_LORA_MODE_STOP;
static factory_lora_mode_t s_pending_mode = FACTORY_LORA_MODE_STOP;
static TaskHandle_t s_tx_task_handle;
static TaskHandle_t s_rx_task_handle;
static bool s_switch_pending;
static bool s_tx_busy;
static int64_t s_switch_start_us;
static int s_tx_interval_s = 1;
static int8_t s_output_power_dbm = 22;
static char s_once_text[LORA_TEXT_MAX_LEN];
static bool s_once_pending;
static factory_lora_callbacks_t s_callbacks;

static void lora_tx_task(void *arg);
static void lora_rx_task(void *arg);

static int apply_output_power(void)
{
    int state = lora_app_standby();
    if (state != 0)
    {
        ESP_LOGE(TAG, "standby before TX power failed, state:%d", state);
        return state;
    }

    state = lora_app_set_output_power(s_output_power_dbm);
    if (state != 0)
    {
        ESP_LOGE(TAG, "apply TX power %ddBm failed, state:%d",
                 s_output_power_dbm, state);
    }
    return state;
}

static void notify_status(factory_lora_mode_t mode, int state, const char *message)
{
    if (s_callbacks.status)
    {
        s_callbacks.status(mode, state, message, s_callbacks.user_data);
    }
}

static void lora_irq_cb(lora_app_irq_event_t event, void *user_data)
{
    (void)event;
    (void)user_data;
    s_irq_flag = true;
}

esp_err_t lora_init(esp_io_expander_handle_t io_expander)
{
    const uint32_t pin_mask = (1UL << XL9555_LORA_PWR_EN);
    ESP_RETURN_ON_FALSE(io_expander != NULL, ESP_ERR_INVALID_STATE, TAG, "XL9555 not initialized");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, pin_mask, IO_EXPANDER_OUTPUT),
                        TAG,
                        "set LoRa power pin direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, pin_mask, 1),
                        TAG,
                        "enable LoRa power failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    int state = lora_app_start();
    if (state != ESP_OK)
    {
        ESP_LOGE(TAG, "LoRa init failed, code %d", state);
        return ESP_FAIL;
    }

    lora_app_set_irq_callback(lora_irq_cb, NULL);
    lora_app_set_crc(false);
    lora_app_set_invert_iq(false);
    if (lora_app_get_chip() == LORA_APP_CHIP_SX1262)
    {
        lora_app_set_current_limit(140.0f);
    }
    else if (lora_app_get_chip() == LORA_APP_CHIP_SX1276)
    {
        lora_app_set_current_limit(240.0f);
    }
    state = lora_app_standby();
    if (state != 0)
    {
        ESP_LOGE(TAG, "LoRa standby failed, state:%d", state);
        return ESP_FAIL;
    }
    s_mode = FACTORY_LORA_MODE_STOP;
    ESP_LOGI(TAG, "LoRa ready on XL9555 pin %d", XL9555_LORA_PWR_EN);
    return ESP_OK;
}

void factory_lora_set_callbacks(const factory_lora_callbacks_t *callbacks)
{
    if (callbacks)
    {
        s_callbacks = *callbacks;
    }
    else
    {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }
}

static bool start_mode(factory_lora_mode_t mode)
{
    if (mode == FACTORY_LORA_MODE_TX || mode == FACTORY_LORA_MODE_TX_ONCE)
    {
        s_mode = mode;
        s_irq_flag = true;
        if (s_tx_task_handle == NULL &&
            xTaskCreate(lora_tx_task, "lora_tx_task", LORA_TASK_STACK_SIZE,
                        NULL, 3, &s_tx_task_handle) != pdPASS)
        {
            s_mode = FACTORY_LORA_MODE_STOP;
            ESP_LOGE(TAG, "create lora_tx_task failed");
            return false;
        }
        return true;
    }

    if (mode == FACTORY_LORA_MODE_RX)
    {
        s_mode = mode;
        if (s_rx_task_handle == NULL &&
            xTaskCreate(lora_rx_task, "lora_rx_task", LORA_TASK_STACK_SIZE,
                        NULL, 3, &s_rx_task_handle) != pdPASS)
        {
            s_mode = FACTORY_LORA_MODE_STOP;
            ESP_LOGE(TAG, "create lora_rx_task failed");
            return false;
        }
    }
    return true;
}

bool factory_lora_request_mode(factory_lora_mode_t mode)
{
    if (mode < FACTORY_LORA_MODE_STOP || mode > FACTORY_LORA_MODE_TX_ONCE)
    {
        return false;
    }

    s_irq_flag = false;
    s_mode = FACTORY_LORA_MODE_STOP;
    s_pending_mode = mode;
    s_switch_pending = true;
    s_switch_start_us = esp_timer_get_time();
    notify_status(FACTORY_LORA_MODE_STOP, 0,
                  mode == FACTORY_LORA_MODE_STOP ? "STOP requested" : "switching mode");
    return true;
}

bool factory_lora_send_once(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }
    snprintf(s_once_text, sizeof(s_once_text), "%s", text);
    s_once_pending = true;
    return factory_lora_request_mode(FACTORY_LORA_MODE_TX_ONCE);
}

void factory_lora_process(void)
{
    if (!s_switch_pending)
    {
        return;
    }
    if (s_tx_task_handle != NULL || s_rx_task_handle != NULL)
    {
        if ((esp_timer_get_time() - s_switch_start_us) / 1000 > LORA_MODE_SWITCH_WAIT_MS)
        {
            s_switch_pending = false;
            notify_status(FACTORY_LORA_MODE_STOP, ESP_ERR_TIMEOUT, "mode switch timeout");
        }
        return;
    }

    factory_lora_mode_t mode = s_pending_mode;
    s_switch_pending = false;
    int state = lora_app_is_started() ? lora_app_standby() : 0;
    notify_status(FACTORY_LORA_MODE_STOP, state,
                  state == 0 ? "standby state:0" : "standby failed");
    if (state == 0 && mode != FACTORY_LORA_MODE_STOP)
    {
        start_mode(mode);
    }
}

void factory_lora_stop(void)
{
    s_irq_flag = false;
    s_mode = FACTORY_LORA_MODE_STOP;
    s_switch_pending = false;
    s_once_pending = false;
    if (s_tx_task_handle == NULL && s_rx_task_handle == NULL && !s_tx_busy)
    {
        int state = lora_app_is_started() ? lora_app_standby() : 0;
        notify_status(FACTORY_LORA_MODE_STOP, state,
                      state == 0 ? "standby state:0" : "standby failed");
    }
}

factory_lora_mode_t factory_lora_get_mode(void)
{
    return s_mode;
}

factory_lora_mode_t factory_lora_get_pending_mode(void)
{
    return s_pending_mode;
}

bool factory_lora_is_switch_pending(void)
{
    return s_switch_pending;
}

void factory_lora_set_tx_interval(int interval_s)
{
    s_tx_interval_s = interval_s > 0 ? interval_s : 1;
}

int factory_lora_set_frequency(float frequency_mhz)
{
    return lora_app_set_frequency(frequency_mhz);
}

int factory_lora_set_bandwidth(float bandwidth_khz)
{
    return lora_app_set_bandwidth(bandwidth_khz);
}

int factory_lora_set_spreading_factor(uint8_t spreading_factor)
{
    return lora_app_set_spreading_factor(spreading_factor);
}

int factory_lora_set_coding_rate(uint8_t coding_rate)
{
    return lora_app_set_coding_rate(coding_rate);
}

int factory_lora_set_output_power(int8_t power_dbm)
{
    s_output_power_dbm = power_dbm;
    if (s_mode == FACTORY_LORA_MODE_TX || s_mode == FACTORY_LORA_MODE_RX ||
        s_mode == FACTORY_LORA_MODE_TX_ONCE || s_tx_busy ||
        s_tx_task_handle != NULL || s_rx_task_handle != NULL || s_switch_pending)
    {
        ESP_LOGI(TAG, "defer TX power %ddBm until next packet", power_dbm);
        return 0;
    }
    return apply_output_power();
}

int8_t factory_lora_get_max_output_power(float frequency_mhz)
{
    if (lora_app_get_chip() == LORA_APP_CHIP_LR2021 &&
        frequency_mhz >= LORA_HF_MIN_FREQUENCY_MHZ)
    {
        return LORA_24_GHZ_MAX_POWER_DBM;
    }
    return LORA_SUB_GHZ_MAX_POWER_DBM;
}

static void lora_tx_task(void *arg)
{
    (void)arg;
    static uint16_t count;
    char tx_text[LORA_TEXT_MAX_LEN];

    while (s_mode == FACTORY_LORA_MODE_TX || s_mode == FACTORY_LORA_MODE_TX_ONCE)
    {
        if (s_irq_flag)
        {
            s_irq_flag = false;
            count++;
            if (s_mode == FACTORY_LORA_MODE_TX_ONCE && s_once_pending)
            {
                snprintf(tx_text, sizeof(tx_text), "%s", s_once_text);
                s_once_pending = false;
            }
            else
            {
                snprintf(tx_text, sizeof(tx_text), "T-Panel-P4 LoRa test #%u",
                         (unsigned)count);
            }

            int state = apply_output_power();
            if (state == 0)
            {
                s_tx_busy = true;
                state = lora_app_transmit_text(tx_text);
                s_tx_busy = false;
            }
            if (s_callbacks.tx_done)
            {
                s_callbacks.tx_done(tx_text, state, s_callbacks.user_data);
            }
            if (s_mode == FACTORY_LORA_MODE_TX_ONCE)
            {
                s_mode = FACTORY_LORA_MODE_STOP;
                break;
            }
        }

        int elapsed_ms = 0;
        while (s_mode == FACTORY_LORA_MODE_TX && elapsed_ms < s_tx_interval_s * 1000)
        {
            vTaskDelay(pdMS_TO_TICKS(LORA_TASK_STOP_POLL_MS));
            elapsed_ms += LORA_TASK_STOP_POLL_MS;
        }
    }

    int state = lora_app_standby();
    notify_status(FACTORY_LORA_MODE_STOP, state,
                  state == 0 ? "standby state:0" : "standby failed");
    s_tx_task_handle = NULL;
    if (s_callbacks.tx_finished)
    {
        s_callbacks.tx_finished(s_callbacks.user_data);
    }
    vTaskDelete(NULL);
}

static void read_rx_packet(void)
{
    size_t packet_len = lora_app_get_packet_length();
    if (packet_len == 0)
    {
        return;
    }

    uint8_t packet[LORA_PACKET_MAX_LEN] = {0};
    size_t read_len = packet_len < sizeof(packet) ? packet_len : sizeof(packet) - 1;
    int state = lora_app_read_data(packet, read_len);
    packet[read_len] = '\0';
    float rssi = state == 0 ? lora_app_get_rssi() : 0.0f;
    float snr = state == 0 ? lora_app_get_snr() : 0.0f;
    if (s_callbacks.rx_done)
    {
        s_callbacks.rx_done((char *)packet, rssi, snr, state, s_callbacks.user_data);
    }
    lora_app_start_receive();
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    int state = lora_app_is_started() ? 0 : lora_app_start();
    if (state == 0)
    {
        state = lora_app_start_receive();
    }
    notify_status(FACTORY_LORA_MODE_RX, state,
                  state == 0 ? "startReceive state:0" : "startReceive failed");
    if (state != 0)
    {
        s_mode = FACTORY_LORA_MODE_STOP;
    }

    while (s_mode == FACTORY_LORA_MODE_RX)
    {
        if (s_irq_flag)
        {
            s_irq_flag = false;
            read_rx_packet();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    state = lora_app_standby();
    notify_status(FACTORY_LORA_MODE_STOP, state,
                  state == 0 ? "standby state:0" : "standby failed");
    s_rx_task_handle = NULL;
    vTaskDelete(NULL);
}
