#include "button.h"

#include <inttypes.h>

#include "board_config.h"
#include "button_gpio.h"
#include "display_panel.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"

#define BUTTON_LONG_PRESS_MS 1500
#define BUTTON_SHORT_PRESS_MS 180

static const char *TAG = "factory_button";
static button_handle_t s_button;
static bool s_screen_on = true;
static factory_button_event_cb_t s_event_callback;
static void *s_event_user_data;

static void button_single_click_cb(void *button_handle, void *user_data)
{
    (void)button_handle;
    (void)user_data;

    bool turn_on = !s_screen_on;
    esp_err_t ret = display_panel_backlight_set(turn_on);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "set screen %s failed: %s",
                 turn_on ? "on" : "off", esp_err_to_name(ret));
        return;
    }

    s_screen_on = turn_on;
    ESP_LOGI(TAG, "single click: screen %s", s_screen_on ? "on" : "off");
}

static void button_double_click_cb(void *button_handle, void *user_data)
{
    (void)button_handle;
    (void)user_data;
    ESP_LOGI(TAG, "double click");
    if (s_event_callback)
    {
        s_event_callback(FACTORY_BUTTON_EVENT_DOUBLE_CLICK, s_event_user_data);
    }
}

static void button_long_press_cb(void *button_handle, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "long press: %" PRIu32 "ms",
             iot_button_get_pressed_time(button_handle));
    if (s_event_callback)
    {
        s_event_callback(FACTORY_BUTTON_EVENT_LONG_PRESS, s_event_user_data);
    }
}

esp_err_t button_init(void)
{
    if (s_button != NULL)
    {
        return ESP_OK;
    }

    const button_config_t button_config = {
        .long_press_time = BUTTON_LONG_PRESS_MS,
        .short_press_time = BUTTON_SHORT_PRESS_MS,
    };
    const button_gpio_config_t gpio_config = {
        .gpio_num = BUTTO_PIN,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };

    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &gpio_config,
                                                    &s_button),
                        TAG, "create GPIO button failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(s_button, BUTTON_SINGLE_CLICK,
                                                NULL, button_single_click_cb, NULL),
                        TAG, "register single click failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(s_button, BUTTON_DOUBLE_CLICK,
                                                NULL, button_double_click_cb, NULL),
                        TAG, "register double click failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(s_button, BUTTON_LONG_PRESS_START,
                                                NULL, button_long_press_cb, NULL),
                        TAG, "register long press failed");

    s_screen_on = true;
    ESP_LOGI(TAG, "button ready: GPIO%d, active low", BUTTO_PIN);
    return ESP_OK;
}

void button_set_event_callback(factory_button_event_cb_t callback,
                               void *user_data)
{
    s_event_callback = callback;
    s_event_user_data = user_data;
}

bool button_is_screen_on(void)
{
    return s_screen_on;
}
