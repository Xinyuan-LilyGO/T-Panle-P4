#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    FACTORY_BUTTON_EVENT_DOUBLE_CLICK = 0,
    FACTORY_BUTTON_EVENT_LONG_PRESS,
} factory_button_event_t;

typedef void (*factory_button_event_cb_t)(factory_button_event_t event,
                                          void *user_data);

esp_err_t button_init(void);
void button_set_event_callback(factory_button_event_cb_t callback,
                               void *user_data);
bool button_is_screen_on(void);

#ifdef __cplusplus
}
#endif
