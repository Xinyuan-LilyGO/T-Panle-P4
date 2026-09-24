#include "ui_internal.h"

static const char *TAG = "[UI]";

void app_page_destroy_async_cb(void *user_data)
{
    PageType id = (PageType)(uintptr_t)user_data;
    ui_page_destroy(id);
}

void app_page_back_gesture(Page *page, GestureDirection direction)
{
    ESP_LOGI(TAG, "app_page_back_gesture page:%d direction:%d", page->id, direction);
    if (direction == GESTURE_RIGHT)
    {
        ui_page_switch_async(PAGE_HOME);
    }
}

void create_page_ui(void)
{
#if CONFIG_T_PANEL_P4_BOARD_STANDARD
    start_page_register();
    home_page_register();
    music_page_register();
    record_page_register();
    camera_page_register();
    lora_page_register();
    file_page_register();
    settings_page_register();
    self_test_page_register();
#elif CONFIG_T_PANEL_P4_BOARD_ROUND
    start_page_register();
    record_page_register();
    home_round_page_register();
    music_page_register();
    camera_page_register();
    lora_page_register();
    file_page_register();
    settings_page_register();
    self_test_page_register();
#elif CONFIG_T_PANEL_P4_BOARD_RECT
    start_page_register();
    home_page_register();
    music_page_register();
    record_page_register();
    camera_page_register();
    file_page_register();
    sensor_page_register();
    settings_page_register();
    self_test_page_register();

#endif
}
