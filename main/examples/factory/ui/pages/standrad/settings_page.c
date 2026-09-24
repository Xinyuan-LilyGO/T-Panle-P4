#include "ui_internal.h"
#include "settings_page.h"

static const char *TAG = "[UI][settings_page]";

#define SET_WIFI_SCAN_MAX_APS 16

#define SET_PAGE_PANEL_PAD 10
#define SET_PAGE_PANEL_HEAD_H 22
#define SET_PAGE_PANEL_ROW_GAP 6
#define SET_PAGE_INFO_LINE_H 20
#define SET_PAGE_SWITCH_ROW_H 48
#define SET_PAGE_ACTION_ROW_H 48
#define SET_PAGE_OPTION_H 72
#define SET_PAGE_OPTION_ROW_GAP 10
#define SET_PAGE_OPTIONS_PAD_Y 8
#define SET_PAGE_OPTION_FAR_W 600
#define SET_PAGE_OPTION_TEXT_W 480
#define SET_PAGE_DETAIL_W 640
#define SET_PAGE_DETAIL_H 600
#define SET_PAGE_DETAIL_X 40
#define SET_PAGE_DETAIL_Y 40
#define SET_PAGE_DETAIL_BODY_H 550
#define SET_PAGE_PMU_PANEL_H 620
#define SET_PAGE_DETAIL_TITLE_W 500
#define SET_PAGE_DETAIL_TEXT_W 500
#define SET_WIFI_DIALOG_W 640
#define SET_WIFI_DIALOG_H 620
#define SET_WIFI_DIALOG_Y 20
#define SET_WIFI_LIST_H 420
#define SET_WIFI_ROW_SSID_W 300
#define SET_WIFI_ROW_RSSI_W 80
#define SET_WIFI_ROW_TYPE_W 120
#define SET_WIFI_DIALOG_TITLE_W 210
#define SET_WIFI_STATUS_W 270
#define SET_WIFI_PASSWORD_W 640
#define SET_WIFI_PASSWORD_H 360
#define SET_WIFI_PASSWORD_Y -8
#define SET_WIFI_PASSWORD_SSID_W 430
#define SET_WIFI_PASSWORD_TA_W 470
#define SET_WIFI_KEYBOARD_H 240

static set_page_ui_t s_set_ui;
static int s_set_brightness_percent = SET_BRIGHTNESS_DEFAULT;
static bool s_set_auto_dim_enabled = false;
static uint32_t s_set_screen_timeout_ms = 30000;
static bool s_display_dimmed = false;
static lv_timer_t *s_display_dim_timer = NULL;
static bool s_display_indev_hooked = false;
static uint32_t s_display_last_activity_ms = 0;
static volatile bool s_set_power_action_busy = false;
static volatile bool s_set_usb_otg_switch_busy = false;
static volatile bool s_set_wifi_switch_busy = false;
static volatile bool s_set_c5_action_busy = false;
static volatile bool s_set_usb_otg_usb_mode = false;
static uint32_t s_set_storage_last_update_ms = 0;
static esp32c5_wifi_ap_info_t s_set_wifi_aps[SET_WIFI_SCAN_MAX_APS];
static int s_set_wifi_ap_count = 0;
static char s_set_wifi_scan_status[64] = "Tap WiFi Remote to scan";
static volatile bool s_set_wifi_scan_busy = false;
static TaskHandle_t s_set_wifi_scan_task_handle = NULL;
static char s_set_wifi_selected_ssid[33];
static wifi_auth_mode_t s_set_wifi_selected_auth = WIFI_AUTH_WPA2_PSK;
static char s_set_wifi_last_connected_ssid[33];

/*********set Page************/
static void set_page_power_status_set(const char *text, uint32_t color);

typedef enum
{
    SET_DETAIL_WIFI = 0,
    SET_DETAIL_DISPLAY,
    SET_DETAIL_STORAGE,
    SET_DETAIL_PMU,
    SET_DETAIL_SYSTEM,
    SET_DETAIL_COUNT,
} set_detail_page_t;

static bool s_set_pmu_ui_updating;

static uint32_t set_page_timeout_ms_from_index(uint32_t index)
{
    static const uint32_t timeout_ms[] = {
        0,
        30000,
        60000,
        300000,
        600000,
    };

    if (index >= sizeof(timeout_ms) / sizeof(timeout_ms[0]))
    {
        return timeout_ms[1];
    }
    return timeout_ms[index];
}

static uint32_t set_page_timeout_index_from_ms(uint32_t timeout_ms)
{
    switch (timeout_ms)
    {
    case 0:
        return 0;
    case 60000:
        return 2;
    case 300000:
        return 3;
    case 600000:
        return 4;
    case 30000:
    default:
        return 1;
    }
}

static void set_page_display_restore(void)
{
    if (!s_display_dimmed)
    {
        return;
    }

    esp_err_t ret = display_panel_set_brightness((uint8_t)s_set_brightness_percent);
    if (ret == ESP_OK)
    {
        s_display_dimmed = false;
    }
    else
    {
        ESP_LOGW(TAG, "Restore backlight failed: %s", esp_err_to_name(ret));
    }
}

static void display_indev_activity_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
    {
        return;
    }

    s_display_last_activity_ms = lv_tick_get();
    set_page_display_restore();
}

static void display_indev_activity_hook(void)
{
    if (s_display_indev_hooked)
    {
        return;
    }

    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL)
    {
        lv_indev_add_event_cb(indev, display_indev_activity_event_cb, LV_EVENT_ALL, NULL);
        s_display_indev_hooked = true;
    }
}

static void display_dim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_indev_activity_hook();

    if (!s_set_auto_dim_enabled || s_set_screen_timeout_ms == 0)
    {
        set_page_display_restore();
        return;
    }

    uint32_t now_ms = lv_tick_get();
    uint32_t inactive_ms = now_ms - s_display_last_activity_ms;
    if (inactive_ms < 1000)
    {
        set_page_display_restore();
        return;
    }

    if (inactive_ms < s_set_screen_timeout_ms)
    {
        return;
    }

    if (s_set_auto_dim_enabled && !s_display_dimmed)
    {
        esp_err_t ret = display_panel_set_brightness((uint8_t)SET_BRIGHTNESS_MIN);
        if (ret == ESP_OK)
        {
            s_display_dimmed = true;
        }
        else
        {
            ESP_LOGW(TAG, "Dim backlight failed: %s", esp_err_to_name(ret));
        }
    }
}

void display_dim_timer_start(void)
{
    if (s_display_dim_timer == NULL)
    {
        s_display_last_activity_ms = lv_tick_get();
        display_indev_activity_hook();
        s_display_dim_timer = lv_timer_create(display_dim_timer_cb, 1000, NULL);
    }
}

static void set_page_brightness_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    int value = (int)lv_slider_get_value(slider);

    if (value < SET_BRIGHTNESS_MIN)
    {
        value = SET_BRIGHTNESS_MIN;
    }
    else if (value > SET_BRIGHTNESS_MAX)
    {
        value = SET_BRIGHTNESS_MAX;
    }

    if (s_set_ui.brightness_label)
    {
        lv_label_set_text_fmt(s_set_ui.brightness_label, "%d%%", value);
    }

    if (value == s_set_brightness_percent)
    {
        set_page_display_restore();
        return;
    }

    esp_err_t ret = display_panel_set_brightness((uint8_t)value);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set backlight brightness failed: %s", esp_err_to_name(ret));
        return;
    }

    s_display_dimmed = false;
    s_set_brightness_percent = value;
}

static void set_page_auto_dim_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(e);
    s_set_auto_dim_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!s_set_auto_dim_enabled)
    {
        set_page_display_restore();
    }
}

static void set_page_screen_timeout_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target_obj(e);
    s_set_screen_timeout_ms = set_page_timeout_ms_from_index(lv_dropdown_get_selected(dropdown));
    set_page_display_restore();
}

static void set_page_usb_otg_storage_status_apply(bool usb_mode, bool ok, const char *text)
{
    if (s_set_ui.storage_label)
    {
        lv_label_set_text(s_set_ui.storage_label, text);
        lv_obj_set_style_text_color(s_set_ui.storage_label,
                                    lv_color_hex(ok ? (usb_mode ? UI_OK : UI_PRIMARY) : UI_ERROR),
                                    0);
    }
    if (s_set_ui.storage_bus_label)
    {
        lv_label_set_text(s_set_ui.storage_bus_label, usb_mode ? "USB MSC" : "SDMMC 4-bit");
        lv_obj_set_style_text_color(s_set_ui.storage_bus_label,
                                    lv_color_hex(usb_mode ? UI_OK : UI_MUTED),
                                    0);
    }
}

static void set_page_usb_otg_storage_status_set(bool usb_mode, bool ok, const char *text)
{
    ui_lock();
    set_page_usb_otg_storage_status_apply(usb_mode, ok, text);
    ui_unlock();
}

static void set_page_storage_info_apply(const factory_storage_info_t *info)
{
    if (info == NULL)
    {
        set_page_usb_otg_storage_status_apply(false, false, "NO SD");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_MUTED), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_MUTED), 0);
        }
        return;
    }

    if (info->usb_mode)
    {
        set_page_usb_otg_storage_status_apply(true, true, "USB MSC");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, "HOST ACCESS");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_OK), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "APP OFF");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    if (!info->app_mounted)
    {
        set_page_usb_otg_storage_status_apply(false, false, "NO SD");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, info->type[0] ? info->type : "--");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_MUTED), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_MUTED), 0);
        }
        return;
    }

    set_page_usb_otg_storage_status_apply(false, true, "SD READY");
    if (s_set_ui.storage_type_label)
    {
        lv_label_set_text(s_set_ui.storage_type_label, info->type[0] ? info->type : "SD CARD");
        lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_TEXT), 0);
    }
    if (s_set_ui.storage_size_label)
    {
        char size_text[56] = {0};
        const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;
        const uint64_t mb = 1024ULL * 1024ULL;
        if (info->total_bytes >= gb)
        {
            uint64_t free_x10 = (info->free_bytes * 10ULL + gb / 2ULL) / gb;
            uint64_t total_x10 = (info->total_bytes * 10ULL + gb / 2ULL) / gb;
            snprintf(size_text,
                     sizeof(size_text),
                     "%" PRIu64 ".%" PRIu64 "/%" PRIu64 ".%" PRIu64 " GB",
                     free_x10 / 10ULL,
                     free_x10 % 10ULL,
                     total_x10 / 10ULL,
                     total_x10 % 10ULL);
        }
        else
        {
            uint64_t free_mb = (info->free_bytes + mb / 2ULL) / mb;
            uint64_t total_mb = (info->total_bytes + mb / 2ULL) / mb;
            snprintf(size_text, sizeof(size_text), "%" PRIu64 "/%" PRIu64 " MB", free_mb, total_mb);
        }
        lv_label_set_text(s_set_ui.storage_size_label, size_text);
        lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_SECONDARY), 0);
    }
    if (s_set_ui.storage_bus_label)
    {
        lv_label_set_text(s_set_ui.storage_bus_label, info->bus[0] ? info->bus : "SDMMC 4-bit");
        lv_obj_set_style_text_color(s_set_ui.storage_bus_label, lv_color_hex(UI_MUTED), 0);
    }
}

static void set_page_storage_refresh(bool force)
{
    uint32_t now = lv_tick_get();
    if (!force && now - s_set_storage_last_update_ms < 2000)
    {
        return;
    }
    s_set_storage_last_update_ms = now;

    factory_storage_info_t info = {0};
    if (storage_info_get(&info))
    {
        s_set_usb_otg_usb_mode = info.usb_mode;
        set_page_storage_info_apply(&info);
    }
    else
    {
        set_page_storage_info_apply(NULL);
    }
}

static void set_page_usb_otg_switch_state_set(bool checked)
{
    ui_lock();
    if (s_set_ui.usb_otg_switch)
    {
        if (checked)
        {
            lv_obj_add_state(s_set_ui.usb_otg_switch, LV_STATE_CHECKED);
        }
        else
        {
            lv_obj_remove_state(s_set_ui.usb_otg_switch, LV_STATE_CHECKED);
        }
    }
    ui_unlock();
}

static void set_page_usb_otg_task(void *arg)
{
    bool usb_mode = (bool)(intptr_t)arg;
    bool ok = usb_otg_msc_set(usb_mode);

    if (ok)
    {
        s_set_usb_otg_usb_mode = usb_mode;
        set_page_usb_otg_storage_status_set(usb_mode, true, usb_mode ? "USB MSC" : "SD READY");
    }
    else
    {
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode,
                                            false,
                                            usb_mode ? "USB FAIL" : "APP FAIL");
    }

    s_set_usb_otg_switch_busy = false;
    vTaskDelete(NULL);
}

static void set_page_usb_otg_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool usb_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (s_set_usb_otg_switch_busy)
    {
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode, false, "USB BUSY");
        return;
    }

    s_set_usb_otg_switch_busy = true;
    set_page_usb_otg_storage_status_set(usb_mode, true, "SWITCHING");
    if (xTaskCreate(set_page_usb_otg_task,
                    "usb_otg_msc",
                    4096,
                    (void *)(intptr_t)usb_mode,
                    5,
                    NULL) != pdPASS)
    {
        s_set_usb_otg_switch_busy = false;
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode, false, "TASK FAIL");
    }
}

static void set_page_switch_checked_set(lv_obj_t *sw, bool checked)
{
    if (sw == NULL)
    {
        return;
    }
    if (checked)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

static void set_page_c5_ui_apply(void)
{
    bool sleeping = esp32c5_sdio_slave_is_sleeping();
    bool ready = esp32c5_sdio_slave_is_ready();
    bool wifi_enabled = esp32c5_sdio_slave_wifi_is_enabled();

    set_page_switch_checked_set(s_set_ui.c5_sleep_switch, sleeping);
    set_page_switch_checked_set(s_set_ui.wifi_enable_switch, wifi_enabled);
    if (s_set_ui.c5_state_label)
    {
        lv_label_set_text(s_set_ui.c5_state_label, sleeping ? "SLEEP" : (ready ? "ACTIVE" : "ERROR"));
        lv_obj_set_style_text_color(s_set_ui.c5_state_label,
                                    lv_color_hex(sleeping ? UI_WARN : (ready ? UI_OK : UI_ERROR)), 0);
    }
    if (s_set_ui.wifi_state_label)
    {
        lv_label_set_text(s_set_ui.wifi_state_label, wifi_enabled ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_set_ui.wifi_state_label,
                                    lv_color_hex(wifi_enabled ? UI_OK : UI_MUTED), 0);
    }
}

static void set_page_c5_ui_refresh(void)
{
    ui_lock();
    set_page_c5_ui_apply();
    ui_unlock();
}

typedef enum
{
    SET_C5_ACTION_WIFI_OFF = 0,
    SET_C5_ACTION_WIFI_ON,
    SET_C5_ACTION_SLEEP,
    SET_C5_ACTION_WAKE,
    SET_C5_ACTION_RESTART,
} set_c5_action_t;

static void set_page_c5_action_task(void *arg)
{
    set_c5_action_t action = (set_c5_action_t)(intptr_t)arg;
    bool ok = false;
    switch (action)
    {
    case SET_C5_ACTION_WIFI_OFF:
        ok = esp32c5_sdio_slave_wifi_set_enabled(false);
        break;
    case SET_C5_ACTION_WIFI_ON:
        ok = esp32c5_sdio_slave_wifi_set_enabled(true);
        break;
    case SET_C5_ACTION_SLEEP:
        ok = esp32c5_sdio_slave_set_sleeping(true);
        break;
    case SET_C5_ACTION_WAKE:
        ok = esp32c5_sdio_slave_set_sleeping(false);
        break;
    case SET_C5_ACTION_RESTART:
        ok = esp32c5_sdio_slave_restart();
        break;
    default:
        break;
    }

    set_page_c5_ui_refresh();
    if (!ok)
    {
        set_page_power_status_set("C5 ERROR", UI_ERROR);
    }
    else if (action == SET_C5_ACTION_RESTART)
    {
        set_page_power_status_set("C5 READY", UI_OK);
    }
    s_set_wifi_switch_busy = false;
    s_set_c5_action_busy = false;
    vTaskDelete(NULL);
}

static bool set_page_c5_task_start(set_c5_action_t action)
{
    if (xTaskCreate(set_page_c5_action_task,
                    "set_c5_action",
                    6144,
                    (void *)(intptr_t)action,
                    5,
                    NULL) == pdPASS)
    {
        return true;
    }
    set_page_power_status_set("TASK FAIL", UI_ERROR);
    return false;
}

static void set_page_wifi_enable_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    if (s_set_wifi_switch_busy || s_set_c5_action_busy)
    {
        set_page_c5_ui_apply();
        return;
    }

    bool enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    s_set_wifi_switch_busy = true;
    if (!set_page_c5_task_start(enabled ? SET_C5_ACTION_WIFI_ON : SET_C5_ACTION_WIFI_OFF))
    {
        s_set_wifi_switch_busy = false;
        set_page_c5_ui_apply();
    }
}

static void set_page_c5_sleep_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    if (s_set_c5_action_busy || s_set_wifi_switch_busy)
    {
        set_page_c5_ui_apply();
        return;
    }

    bool sleeping = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    s_set_c5_action_busy = true;
    set_page_power_status_set(sleeping ? "C5 SLEEP" : "C5 WAKE", UI_WARN);
    if (!set_page_c5_task_start(sleeping ? SET_C5_ACTION_SLEEP : SET_C5_ACTION_WAKE))
    {
        s_set_c5_action_busy = false;
        set_page_c5_ui_apply();
    }
}

static void set_page_c5_restart_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_set_c5_action_busy || s_set_wifi_switch_busy)
    {
        return;
    }
    s_set_c5_action_busy = true;
    set_page_power_status_set("C5 RESTART", UI_WARN);
    if (!set_page_c5_task_start(SET_C5_ACTION_RESTART))
    {
        s_set_c5_action_busy = false;
    }
}

typedef enum
{
    SET_POWER_ACTION_SHIP_MODE = 0,
    SET_POWER_ACTION_RESTART,
} set_power_action_t;

static void set_page_power_status_set(const char *text, uint32_t color)
{
    ui_lock();
    if (s_set_ui.power_status_label)
    {
        lv_label_set_text(s_set_ui.power_status_label, text);
        lv_obj_set_style_text_color(s_set_ui.power_status_label, lv_color_hex(color), 0);
    }
    ui_unlock();
}

static void set_page_power_action_task(void *arg)
{
    set_power_action_t action = (set_power_action_t)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(350));

    switch (action)
    {
    case SET_POWER_ACTION_SHIP_MODE:
    {
        bool ok = factory_power_enter_ship_mode();
        set_page_power_status_set(ok ? "SHIP OK" : "SHIP FAIL", ok ? UI_OK : UI_ERROR);
        s_set_power_action_busy = false;
        break;
    }
    case SET_POWER_ACTION_RESTART:
        factory_power_restart();
        break;
    default:
        s_set_power_action_busy = false;
        break;
    }

    vTaskDelete(NULL);
}

static void set_page_power_action_start(set_power_action_t action)
{
    const char *text = action == SET_POWER_ACTION_SHIP_MODE ? "SHIP MODE" : "RESTARTING";
    uint32_t color = action == SET_POWER_ACTION_SHIP_MODE ? UI_WARN : UI_PRIMARY;

    if (s_set_power_action_busy)
    {
        set_page_power_status_set("BUSY", UI_WARN);
        return;
    }

    s_set_power_action_busy = true;
    set_page_power_status_set(text, color);
    if (xTaskCreate(set_page_power_action_task,
                    "set_power",
                    4096,
                    (void *)(intptr_t)action,
                    5,
                    NULL) != pdPASS)
    {
        s_set_power_action_busy = false;
        set_page_power_status_set("TASK FAIL", UI_ERROR);
    }
}

static void set_page_power_confirm_hide(void)
{
    if (s_set_ui.power_confirm_dialog)
    {
        lv_obj_add_flag(s_set_ui.power_confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_page_power_confirm_no_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        set_page_power_confirm_hide();
    }
}

static void set_page_power_confirm_yes_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    set_power_action_t action = (set_power_action_t)(intptr_t)lv_event_get_user_data(e);
    set_page_power_confirm_hide();
    set_page_power_action_start(action);
}

static void set_page_power_confirm_show(set_power_action_t action)
{
    if (s_set_ui.root == NULL || s_set_power_action_busy)
    {
        return;
    }

    if (s_set_ui.power_confirm_dialog == NULL)
    {
        s_set_ui.power_confirm_dialog = lv_obj_create(s_set_ui.root);
        lv_obj_set_size(s_set_ui.power_confirm_dialog, 460, 220);
        lv_obj_center(s_set_ui.power_confirm_dialog);
        lv_obj_set_style_bg_color(s_set_ui.power_confirm_dialog, lv_color_hex(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(s_set_ui.power_confirm_dialog, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_set_ui.power_confirm_dialog, 1, 0);
        lv_obj_set_style_border_color(s_set_ui.power_confirm_dialog, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_border_opa(s_set_ui.power_confirm_dialog, LV_OPA_30, 0);
        lv_obj_set_style_radius(s_set_ui.power_confirm_dialog, 22, 0);
        lv_obj_set_style_shadow_width(s_set_ui.power_confirm_dialog, 20, 0);
        lv_obj_set_style_shadow_color(s_set_ui.power_confirm_dialog, lv_color_hex(UI_WARN), 0);
        lv_obj_set_style_shadow_opa(s_set_ui.power_confirm_dialog, LV_OPA_30, 0);
        lv_obj_set_style_pad_all(s_set_ui.power_confirm_dialog, 18, 0);
        lv_obj_remove_flag(s_set_ui.power_confirm_dialog, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_clean(s_set_ui.power_confirm_dialog);
    ui_obj_set_flex(s_set_ui.power_confirm_dialog,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    const bool ship_mode = action == SET_POWER_ACTION_SHIP_MODE;
    lv_obj_t *title = ui_label_create(s_set_ui.power_confirm_dialog,
                                      ship_mode ? "ENTER SHIP MODE?" : "RESTART ESP32-P4?",
                                      &lv_font_montserrat_20,
                                      ship_mode ? UI_WARN : UI_PRIMARY);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *message = ui_label_create(s_set_ui.power_confirm_dialog,
                                        ship_mode ? "The device will enter battery shelf mode." :
                                                     "The main processor will restart.",
                                        &lv_font_montserrat_14,
                                        UI_MUTED);
    lv_obj_set_width(message, LV_PCT(100));
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *buttons = ui_flex_container_create(s_set_ui.power_confirm_dialog,
                                                 LV_PCT(100), 48,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(buttons, 14, 0);

    lv_obj_t *no_btn = lv_button_create(buttons);
    lv_obj_set_size(no_btn, 150, 42);
    lv_obj_set_style_bg_color(no_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(no_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(no_btn, 21, 0);
    lv_obj_set_style_border_width(no_btn, 1, 0);
    lv_obj_set_style_border_color(no_btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(no_btn, LV_OPA_30, 0);
    lv_obj_add_event_cb(no_btn, set_page_power_confirm_no_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *no_label = ui_label_create(no_btn, "NO", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(no_label);

    lv_obj_t *yes_btn = lv_button_create(buttons);
    lv_obj_set_size(yes_btn, 150, 42);
    lv_obj_set_style_bg_color(yes_btn, lv_color_hex(ship_mode ? UI_WARN : UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(yes_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yes_btn, 21, 0);
    lv_obj_add_event_cb(yes_btn, set_page_power_confirm_yes_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)action);
    lv_obj_t *yes_label = ui_label_create(yes_btn, "YES", &lv_font_montserrat_14, UI_BG);
    lv_obj_center(yes_label);

    lv_obj_clear_flag(s_set_ui.power_confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_ui.power_confirm_dialog);
}

static void set_page_power_action_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    set_power_action_t action = (set_power_action_t)(intptr_t)lv_event_get_user_data(e);
    if (s_set_power_action_busy)
    {
        set_page_power_status_set("BUSY", UI_WARN);
        return;
    }

    switch (action)
    {
    case SET_POWER_ACTION_SHIP_MODE:
    case SET_POWER_ACTION_RESTART:
        set_page_power_confirm_show(action);
        break;
    default:
        return;
    }
}

static lv_obj_t *set_page_panel_create(lv_obj_t *parent, int32_t w, int32_t h, const char *title, uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, (lv_obj_get_width(parent) - w) / 2, 450);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, SET_PAGE_PANEL_PAD, 0);
    lv_obj_set_style_pad_row(panel, SET_PAGE_PANEL_ROW_GAP, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(panel,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START);

    lv_obj_t *head = ui_flex_container_create(panel,
                                              LV_PCT(100),
                                              SET_PAGE_PANEL_HEAD_H,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);

    lv_obj_t *mark = lv_obj_create(head);
    lv_obj_set_size(mark, 4, 18);
    lv_obj_set_style_bg_color(mark, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_radius(mark, 2, 0);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_label_create(head, title, &lv_font_montserrat_16, color);
    lv_obj_set_width(label, LV_PCT(80));

    return panel;
}

static void set_page_slider_style(lv_obj_t *slider, uint32_t color)
{
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(color), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
}

static void set_page_switch_style(lv_obj_t *sw, uint32_t color)
{
    lv_obj_set_size(sw, 50, 22);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 11, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_LINE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 11, LV_PART_KNOB);
}

static lv_obj_t *set_page_switch_row_create(lv_obj_t *parent, const char *icon, const char *name,
                                            const char *desc, uint32_t color, bool checked)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             SET_PAGE_SWITCH_ROW_H,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
    lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(row, 10, 0);
    lv_obj_set_style_shadow_color(row, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_20, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_column(row, 10, 0);

    lv_obj_t *icon_label = ui_label_create(row, icon, &lv_font_montserrat_16, color);
    lv_obj_set_width(icon_label, 28);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *text_cont = ui_flex_container_create(row,
                                                   160,
                                                   40,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_cont, 2, 0);

    lv_obj_t *name_label = ui_label_create(text_cont, name, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = ui_label_create(text_cont, desc, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(row);

    lv_obj_t *sw = lv_switch_create(row);
    set_page_switch_style(sw, color);
    if (checked)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *set_page_action_create(lv_obj_t *parent, const char *icon, const char *name,
                                        const char *desc, uint32_t color, bool danger)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), SET_PAGE_ACTION_ROW_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(danger ? UI_ERROR : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, danger ? LV_OPA_70 : LV_OPA_60, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(danger ? UI_ERROR : UI_TEXT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(danger ? UI_ERROR : color), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    lv_obj_set_style_pad_column(btn, 10, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(btn,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon_label = ui_label_create(btn, icon, &lv_font_montserrat_16, danger ? UI_BG : color);
    lv_obj_set_width(icon_label, 28);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *text_cont = ui_flex_container_create(btn,
                                                   190,
                                                   40,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_cont, 2, 0);

    lv_obj_t *name_label = ui_label_create(text_cont, name, &lv_font_montserrat_14, danger ? UI_BG : UI_TEXT);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = ui_label_create(text_cont, desc, &lv_font_montserrat_14, danger ? UI_PANEL : UI_MUTED);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(btn);

    lv_obj_t *arrow = ui_label_create(btn, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, danger ? UI_BG : UI_MUTED);
    lv_obj_set_width(arrow, 18);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);

    return btn;
}

static lv_obj_t *set_page_info_line_create(lv_obj_t *parent, const char *name, const char *value, uint32_t color)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             SET_PAGE_INFO_LINE_H,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(row, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 200);

    ui_flex_spacer_create(row);

    lv_obj_t *value_label = ui_label_create(row, value, &lv_font_montserrat_14, color);
    lv_obj_set_width(value_label, 200);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
    return value_label;
}

static void set_page_dropdown_style(lv_obj_t *dropdown, uint32_t color)
{
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_60, 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(dropdown, LV_OPA_50, 0);
    lv_obj_set_style_radius(dropdown, 16, 0);
    lv_obj_set_style_shadow_width(dropdown, 8, 0);
    lv_obj_set_style_shadow_color(dropdown, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(dropdown, LV_OPA_20, 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_pad_left(dropdown, 12, 0);
    lv_obj_set_style_pad_right(dropdown, 10, 0);
}

static void set_page_wifi_list_clear(void)
{
    if (s_set_ui.wifi_list_cont)
    {
        lv_obj_clean(s_set_ui.wifi_list_cont);
    }
}

static void set_page_wifi_password_dialog_show(const esp32c5_wifi_ap_info_t *ap);

static void set_page_wifi_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const esp32c5_wifi_ap_info_t *ap = (const esp32c5_wifi_ap_info_t *)lv_event_get_user_data(e);
    if (ap == NULL)
    {
        return;
    }
    set_page_wifi_password_dialog_show(ap);
}

static void set_page_wifi_row_create(lv_obj_t *parent, const char *ssid, int rssi, const char *type, bool header, const esp32c5_wifi_ap_info_t *ap)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             header ? 30 : 38,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(header ? UI_BG : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, header ? LV_OPA_TRANSP : LV_OPA_70, 0);
    lv_obj_set_style_radius(row, 7, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    if (!header)
    {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, set_page_wifi_row_event_cb, LV_EVENT_CLICKED, (void *)ap);
    }

    lv_obj_t *ssid_label = ui_label_create(row, ssid, header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_TEXT);
    lv_obj_set_width(ssid_label, SET_WIFI_ROW_SSID_W);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *rssi_label = ui_label_create(row, "", header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_SECONDARY);
    lv_obj_set_width(rssi_label, SET_WIFI_ROW_RSSI_W);
    if (header)
    {
        lv_label_set_text(rssi_label, rssi == 0 ? "RSSI" : "");
    }
    else
    {
        lv_label_set_text_fmt(rssi_label, "%d dBm", rssi);
    }
    lv_obj_set_style_text_align(rssi_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *type_label = ui_label_create(row, type, header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_OK);
    lv_obj_set_width(type_label, SET_WIFI_ROW_TYPE_W);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_MODE_DOTS);
}

static void set_page_wifi_scan_ui_update(void)
{
    if (s_set_ui.wifi_dialog == NULL || s_set_ui.wifi_list_cont == NULL)
    {
        return;
    }

    if (s_set_ui.wifi_status_label)
    {
        lv_label_set_text(s_set_ui.wifi_status_label, s_set_wifi_scan_status);
        lv_obj_set_style_text_color(s_set_ui.wifi_status_label,
                                    lv_color_hex(s_set_wifi_scan_busy ? UI_PRIMARY : (s_set_wifi_ap_count > 0 ? UI_OK : UI_WARN)),
                                    0);
    }

    set_page_wifi_list_clear();
    set_page_wifi_row_create(s_set_ui.wifi_list_cont, "SSID", 0, "TYPE", true, NULL);
    if (s_set_wifi_scan_busy)
    {
        lv_obj_t *scan_label = ui_label_create(s_set_ui.wifi_list_cont, "Scanning WiFi networks...", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(scan_label, LV_PCT(100));
        lv_obj_set_style_text_align(scan_label, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    if (s_set_wifi_ap_count <= 0)
    {
        lv_obj_t *empty_label = ui_label_create(s_set_ui.wifi_list_cont, "No AP found", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_set_wifi_ap_count; i++)
    {
        set_page_wifi_row_create(s_set_ui.wifi_list_cont,
                                 s_set_wifi_aps[i].ssid,
                                 s_set_wifi_aps[i].rssi,
                                 s_set_wifi_aps[i].type,
                                 false,
                                 &s_set_wifi_aps[i]);
    }
}

static void set_page_wifi_scan_task(void *arg)
{
    (void)arg;
    int ap_count = 0;
    char err[32] = {0};
    bool ok = esp32c5_sdio_slave_wifi_scan(s_set_wifi_aps, SET_WIFI_SCAN_MAX_APS, &ap_count, err, sizeof(err));

    if (ok)
    {
        s_set_wifi_ap_count = ap_count;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "Scan done: %d AP", ap_count);
    }
    else
    {
        s_set_wifi_ap_count = 0;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "Scan failed: %.32s", err[0] ? err : "unknown");
    }
    s_set_wifi_scan_busy = false;
    s_set_wifi_scan_task_handle = NULL;

    ui_lock();
    Page *current = ui_page_get_current();
    if (current && current->id == PAGE_SET)
    {
        set_page_wifi_scan_ui_update();
    }
    ui_unlock();

    vTaskDelete(NULL);
}

static void set_page_wifi_scan_start(void)
{
    if (s_set_wifi_scan_busy)
    {
        set_page_wifi_scan_ui_update();
        return;
    }

    s_set_wifi_ap_count = 0;
    s_set_wifi_scan_busy = true;
    snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "%s", "Scanning...");
    set_page_wifi_scan_ui_update();

    BaseType_t ok = xTaskCreate(set_page_wifi_scan_task,
                                "wifi_scan_ui",
                                8192,
                                NULL,
                                4,
                                &s_set_wifi_scan_task_handle);
    if (ok != pdPASS)
    {
        s_set_wifi_scan_busy = false;
        s_set_wifi_scan_task_handle = NULL;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "%s", "Scan task failed");
        set_page_wifi_scan_ui_update();
    }
}

static void set_page_wifi_password_dialog_hide(void)
{
    if (s_set_ui.wifi_password_dialog)
    {
        lv_obj_add_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_set_ui.wifi_keyboard)
    {
        lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, NULL);
    }
}

static void set_page_wifi_password_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        set_page_wifi_password_dialog_hide();
    }
}

static void set_page_wifi_connect_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const char *password = "";
    if (s_set_ui.wifi_password_ta)
    {
        password = lv_textarea_get_text(s_set_ui.wifi_password_ta);
    }

    if (s_set_wifi_selected_auth != WIFI_AUTH_OPEN && strlen(password) == 0)
    {
        if (s_set_ui.wifi_status_label)
        {
            lv_label_set_text(s_set_ui.wifi_status_label, "Password required");
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    char err[32] = {0};
    bool ok = esp32c5_sdio_slave_wifi_connect(s_set_wifi_selected_ssid,
                                              password,
                                              s_set_wifi_selected_auth,
                                              err,
                                              sizeof(err));

    if (s_set_ui.wifi_status_label)
    {
        if (ok)
        {
            lv_label_set_text_fmt(s_set_ui.wifi_status_label, "Connecting: %.24s", s_set_wifi_selected_ssid);
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_PRIMARY), 0);
        }
        else
        {
            lv_label_set_text_fmt(s_set_ui.wifi_status_label, "Connect failed: %.24s", err[0] ? err : "unknown");
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_ERROR), 0);
        }
    }

    if (ok)
    {
        snprintf(s_set_wifi_last_connected_ssid, sizeof(s_set_wifi_last_connected_ssid), "%.32s", s_set_wifi_selected_ssid);
        set_page_wifi_password_dialog_hide();
    }
}

static void set_page_wifi_password_dialog_create(void)
{
    if (s_set_ui.wifi_password_dialog || s_set_ui.root == NULL)
    {
        return;
    }

    s_set_ui.wifi_password_dialog = lv_obj_create(s_set_ui.root);
    lv_obj_set_size(s_set_ui.wifi_password_dialog, SET_WIFI_PASSWORD_W, SET_WIFI_PASSWORD_H);
    lv_obj_align(s_set_ui.wifi_password_dialog, LV_ALIGN_BOTTOM_MID, 0, SET_WIFI_PASSWORD_Y);
    lv_obj_set_style_bg_color(s_set_ui.wifi_password_dialog, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_password_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_password_dialog, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_password_dialog, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_set_ui.wifi_password_dialog, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_set_ui.wifi_password_dialog, 22, 0);
    lv_obj_set_style_shadow_width(s_set_ui.wifi_password_dialog, 20, 0);
    lv_obj_set_style_shadow_color(s_set_ui.wifi_password_dialog, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_set_ui.wifi_password_dialog, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_password_dialog, 12, 0);
    lv_obj_remove_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(s_set_ui.wifi_password_dialog,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(s_set_ui.wifi_password_dialog,
                                              LV_PCT(100),
                                              34,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_t *icon = ui_label_create(head, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_PRIMARY);
    lv_obj_set_width(icon, 28);
    s_set_ui.wifi_ssid_list_cont_label = ui_label_create(head, "SSID", &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_width(s_set_ui.wifi_ssid_list_cont_label, SET_WIFI_PASSWORD_SSID_W);
    lv_label_set_long_mode(s_set_ui.wifi_ssid_list_cont_label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 34, 30);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, set_page_wifi_password_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(close_label);

    lv_obj_t *input_row = ui_flex_container_create(s_set_ui.wifi_password_dialog,
                                                   LV_PCT(100),
                                                   48,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 10, 0);
    s_set_ui.wifi_password_ta = lv_textarea_create(input_row);
    lv_obj_set_size(s_set_ui.wifi_password_ta, SET_WIFI_PASSWORD_TA_W, 42);
    lv_textarea_set_one_line(s_set_ui.wifi_password_ta, true);
    lv_textarea_set_password_mode(s_set_ui.wifi_password_ta, true);
    lv_textarea_set_placeholder_text(s_set_ui.wifi_password_ta, "WiFi password");
    lv_obj_set_style_bg_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_password_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_password_ta, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_set_ui.wifi_password_ta, 8, 0);
    lv_obj_set_style_text_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_set_ui.wifi_password_ta, &lv_font_montserrat_14, 0);

    lv_obj_t *connect_btn = lv_button_create(input_row);
    lv_obj_set_size(connect_btn, 110, 42);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(connect_btn, 0, 0);
    lv_obj_set_style_radius(connect_btn, 8, 0);
    lv_obj_set_style_shadow_width(connect_btn, 0, 0);
    lv_obj_add_event_cb(connect_btn, set_page_wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = ui_label_create(connect_btn, "CONNECT", &lv_font_montserrat_14, UI_BG);
    lv_obj_center(connect_label);

    s_set_ui.wifi_keyboard = lv_keyboard_create(s_set_ui.wifi_password_dialog);
    lv_obj_set_size(s_set_ui.wifi_keyboard, LV_PCT(100), SET_WIFI_KEYBOARD_H);
    lv_keyboard_set_mode(s_set_ui.wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, s_set_ui.wifi_password_ta);
    lv_obj_set_style_bg_color(s_set_ui.wifi_keyboard, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_keyboard, LV_OPA_COVER, 0);

    lv_obj_add_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void set_page_wifi_password_dialog_show(const esp32c5_wifi_ap_info_t *ap)
{
    if (ap == NULL)
    {
        return;
    }

    snprintf(s_set_wifi_selected_ssid, sizeof(s_set_wifi_selected_ssid), "%.32s", ap->ssid);
    s_set_wifi_selected_auth = ap->authmode;
    set_page_wifi_password_dialog_create();

    if (s_set_ui.wifi_ssid_list_cont_label)
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_list_cont_label, "%.28s  %s", s_set_wifi_selected_ssid, ap->type);
    }
    if (s_set_ui.wifi_password_ta)
    {
        lv_textarea_set_text(s_set_ui.wifi_password_ta, "");
        lv_textarea_set_password_mode(s_set_ui.wifi_password_ta, ap->authmode != WIFI_AUTH_OPEN);
        lv_textarea_set_placeholder_text(s_set_ui.wifi_password_ta,
                                         ap->authmode == WIFI_AUTH_OPEN ? "Open network, password optional" : "WiFi password");
    }
    if (s_set_ui.wifi_keyboard && s_set_ui.wifi_password_ta)
    {
        lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, s_set_ui.wifi_password_ta);
    }
    if (s_set_ui.wifi_password_dialog)
    {
        lv_obj_clear_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_set_ui.wifi_password_dialog);
    }
}

static void set_page_wifi_dialog_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (s_set_ui.wifi_dialog)
    {
        lv_obj_add_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    set_page_wifi_password_dialog_hide();
}

static void set_page_wifi_dialog_create(void)
{
    if (s_set_ui.wifi_dialog || s_set_ui.main_cont == NULL)
    {
        return;
    }

    s_set_ui.wifi_dialog = lv_obj_create(s_set_ui.main_cont);
    lv_obj_set_size(s_set_ui.wifi_dialog, SET_WIFI_DIALOG_W, SET_WIFI_DIALOG_H);
    lv_obj_align(s_set_ui.wifi_dialog, LV_ALIGN_TOP_MID, 0, SET_WIFI_DIALOG_Y);
    lv_obj_set_style_bg_color(s_set_ui.wifi_dialog, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_dialog, LV_OPA_100, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_dialog, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_dialog, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_set_ui.wifi_dialog, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_set_ui.wifi_dialog, 22, 0);
    lv_obj_set_style_shadow_width(s_set_ui.wifi_dialog, 20, 0);
    lv_obj_set_style_shadow_color(s_set_ui.wifi_dialog, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_set_ui.wifi_dialog, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_dialog, 14, 0);
    lv_obj_remove_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(s_set_ui.wifi_dialog,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(s_set_ui.wifi_dialog,
                                              LV_PCT(100),
                                              38,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_t *icon = ui_label_create(head, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_PRIMARY);
    lv_obj_set_width(icon, 28);
    lv_obj_t *title = ui_label_create(head, "WIFI REMOTE", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_set_width(title, SET_WIFI_DIALOG_TITLE_W);
    s_set_ui.wifi_status_label = ui_label_create(head, s_set_wifi_scan_status, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_set_ui.wifi_status_label, SET_WIFI_STATUS_W);
    lv_obj_set_style_text_align(s_set_ui.wifi_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 34, 30);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, set_page_wifi_dialog_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(close_label);

    s_set_ui.wifi_list_cont = ui_flex_container_create(s_set_ui.wifi_dialog,
                                                       LV_PCT(100),
                                                       SET_WIFI_LIST_H,
                                                       LV_FLEX_FLOW_COLUMN,
                                                       LV_FLEX_ALIGN_START,
                                                       LV_FLEX_ALIGN_CENTER,
                                                       LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_set_ui.wifi_list_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_list_cont, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_list_cont, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_list_cont, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_set_ui.wifi_list_cont, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_set_ui.wifi_list_cont, 14, 0);
    lv_obj_set_style_shadow_width(s_set_ui.wifi_list_cont, 8, 0);
    lv_obj_set_style_shadow_color(s_set_ui.wifi_list_cont, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_set_ui.wifi_list_cont, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_list_cont, 10, 0);
    lv_obj_set_style_pad_row(s_set_ui.wifi_list_cont, 6, 0);
    lv_obj_set_scrollbar_mode(s_set_ui.wifi_list_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_set_ui.wifi_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_set_ui.wifi_list_cont,
                       LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

    set_page_wifi_scan_ui_update();
}

static void set_page_wifi_remote_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (!esp32c5_sdio_slave_wifi_is_enabled())
    {
        if (s_set_ui.wifi_state_label)
        {
            lv_label_set_text(s_set_ui.wifi_state_label, "TURN ON WIFI");
            lv_obj_set_style_text_color(s_set_ui.wifi_state_label, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    set_page_wifi_dialog_create();
    if (s_set_ui.wifi_dialog)
    {
        lv_obj_clear_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_set_ui.wifi_dialog);
    }
    set_page_wifi_scan_start();
}

static void set_page_wifi_refresh(void)
{
    if (s_set_ui.wifi_ssid_label == NULL || s_set_ui.wifi_ip_label == NULL)
    {
        return;
    }

    bool wifi_enabled = esp32c5_sdio_slave_wifi_is_enabled();
    if (s_set_ui.wifi_state_label)
    {
        lv_label_set_text(s_set_ui.wifi_state_label, wifi_enabled ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_set_ui.wifi_state_label,
                                    lv_color_hex(wifi_enabled ? UI_OK : UI_MUTED), 0);
    }
    if (!wifi_enabled)
    {
        lv_label_set_text(s_set_ui.wifi_ssid_label, "Wi-Fi disabled");
        lv_label_set_text(s_set_ui.wifi_ip_label, "IP --");
        return;
    }

    status_info_t status = {0};
    if (!status_bar_info_get(&status) || !status.wifi_connected)
    {
        lv_label_set_text(s_set_ui.wifi_ssid_label, "----");
        lv_label_set_text(s_set_ui.wifi_ip_label, "IP --");
        return;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        const char *ssid = ap.ssid[0] ? (const char *)ap.ssid : s_set_wifi_last_connected_ssid;
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_label, "%s", ssid && ssid[0] ? ssid : "WiFi connected");
    }
    else
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_label, "%s", s_set_wifi_last_connected_ssid[0] ? s_set_wifi_last_connected_ssid : "WiFi connected");
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ip_label, IPSTR, IP2STR(&ip_info.ip));
    }
    else
    {
        lv_label_set_text(s_set_ui.wifi_ip_label, "IP --");
    }
}

static void set_page_detail_clear_refs(void)
{
    s_set_ui.brightness_slider = NULL;
    s_set_ui.brightness_label = NULL;
    s_set_ui.auto_dim_switch = NULL;
    s_set_ui.screen_timeout_dropdown = NULL;
    s_set_ui.power_status_label = NULL;
    s_set_ui.storage_label = NULL;
    s_set_ui.storage_type_label = NULL;
    s_set_ui.storage_size_label = NULL;
    s_set_ui.storage_bus_label = NULL;
    s_set_ui.wifi_ssid_label = NULL;
    s_set_ui.wifi_ip_label = NULL;
    s_set_ui.wifi_enable_switch = NULL;
    s_set_ui.wifi_state_label = NULL;
    s_set_ui.c5_sleep_switch = NULL;
    s_set_ui.c5_state_label = NULL;
    s_set_ui.usb_otg_switch = NULL;
    s_set_ui.pmu_charger_switch = NULL;
    s_set_ui.pmu_charge_status_label = NULL;
    s_set_ui.pmu_charge_voltage_label = NULL;
    s_set_ui.pmu_ibus_label = NULL;
    s_set_ui.pmu_ichg_label = NULL;
    s_set_ui.pmu_charge_current_dropdown = NULL;
    s_set_ui.pmu_battery_soh_label = NULL;
    s_set_ui.pmu_battery_detect_label = NULL;
    s_set_ui.pmu_vbus_label = NULL;
    s_set_ui.pmu_vsys_label = NULL;
    s_set_ui.pmu_vbat_label = NULL;
    s_set_ui.pmu_input_limit_label = NULL;
}

static void set_page_detail_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_set_ui.settings_detail_dialog)
    {
        lv_obj_add_flag(s_set_ui.settings_detail_dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *set_page_detail_begin(const char *icon, const char *title, uint32_t color)
{
    set_page_detail_clear_refs();
    if (s_set_ui.settings_detail_dialog == NULL)
    {
        s_set_ui.settings_detail_dialog = lv_obj_create(s_set_ui.main_cont);
        lv_obj_set_style_bg_color(s_set_ui.settings_detail_dialog, lv_color_hex(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(s_set_ui.settings_detail_dialog, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_set_ui.settings_detail_dialog, 1, 0);
        lv_obj_set_style_border_color(s_set_ui.settings_detail_dialog, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_border_opa(s_set_ui.settings_detail_dialog, LV_OPA_30, 0);
        lv_obj_set_style_radius(s_set_ui.settings_detail_dialog, 22, 0);
        lv_obj_set_style_shadow_width(s_set_ui.settings_detail_dialog, 20, 0);
        lv_obj_set_style_shadow_color(s_set_ui.settings_detail_dialog, lv_color_hex(UI_PRIMARY), 0);
        lv_obj_set_style_shadow_opa(s_set_ui.settings_detail_dialog, LV_OPA_30, 0);
        lv_obj_set_style_pad_all(s_set_ui.settings_detail_dialog, 14, 0);
        lv_obj_remove_flag(s_set_ui.settings_detail_dialog, LV_OBJ_FLAG_SCROLLABLE);
        ui_obj_set_flex(s_set_ui.settings_detail_dialog,
                        LV_FLEX_FLOW_COLUMN,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
    }
    else
    {
        lv_obj_clean(s_set_ui.settings_detail_dialog);
    }
    lv_obj_set_size(s_set_ui.settings_detail_dialog, SET_PAGE_DETAIL_W, SET_PAGE_DETAIL_H);
    lv_obj_set_pos(s_set_ui.settings_detail_dialog, SET_PAGE_DETAIL_X, SET_PAGE_DETAIL_Y);

    lv_obj_t *head = ui_flex_container_create(s_set_ui.settings_detail_dialog,
                                              LV_PCT(100),
                                              42,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);

    lv_obj_t *icon_label = ui_label_create(head, icon, &lv_font_montserrat_20, color);
    lv_obj_set_width(icon_label, 36);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *title_label = ui_label_create(head, title, &lv_font_montserrat_20, UI_TEXT);
    lv_obj_set_width(title_label, SET_PAGE_DETAIL_TITLE_W);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);

    ui_flex_spacer_create(head);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 18, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, set_page_detail_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(close_label);

    s_set_ui.settings_detail_body = ui_flex_container_create(s_set_ui.settings_detail_dialog,
                                                             LV_PCT(100),
                                                             SET_PAGE_DETAIL_BODY_H,
                                                             LV_FLEX_FLOW_COLUMN,
                                                             LV_FLEX_ALIGN_START,
                                                             LV_FLEX_ALIGN_CENTER,
                                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_set_ui.settings_detail_body, 10, 0);
    lv_obj_set_scroll_dir(s_set_ui.settings_detail_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_set_ui.settings_detail_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_set_ui.settings_detail_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_set_ui.settings_detail_body,
                       LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

    lv_obj_remove_flag(s_set_ui.settings_detail_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_ui.settings_detail_dialog);
    return s_set_ui.settings_detail_body;
}

static void set_page_wifi_detail_show(void)
{
    lv_obj_t *body = set_page_detail_begin(LV_SYMBOL_WIFI, "Wi-Fi", UI_PRIMARY);
    lv_obj_t *panel = set_page_panel_create(body, LV_PCT(100), 218, "WIFI", UI_PRIMARY);
    s_set_ui.wifi_enable_switch = set_page_switch_row_create(panel,
                                                             LV_SYMBOL_WIFI,
                                                             "Wi-Fi",
                                                             "ESP32-C5 wireless radio",
                                                             UI_PRIMARY,
                                                             esp32c5_sdio_slave_wifi_is_enabled());
    lv_obj_add_event_cb(s_set_ui.wifi_enable_switch, set_page_wifi_enable_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi_remote_btn = set_page_action_create(panel,
                                                       LV_SYMBOL_REFRESH,
                                                       "Wi-Fi Networks",
                                                       "Scan and connect",
                                                       UI_SECONDARY,
                                                       false);
    lv_obj_add_event_cb(wifi_remote_btn, set_page_wifi_remote_event_cb, LV_EVENT_CLICKED, NULL);
    s_set_ui.wifi_state_label = set_page_info_line_create(panel,
                                                          "Wi-Fi Status",
                                                          esp32c5_sdio_slave_wifi_is_enabled() ? "ON" : "OFF",
                                                          esp32c5_sdio_slave_wifi_is_enabled() ? UI_OK : UI_MUTED);

    lv_obj_t *wifi_info = ui_flex_container_create(panel, LV_PCT(100), 52,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(wifi_info, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(wifi_info, LV_OPA_80, 0);
    lv_obj_set_style_radius(wifi_info, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(wifi_info, 12, 0);
    lv_obj_set_style_pad_right(wifi_info, 12, 0);
    lv_obj_set_style_pad_column(wifi_info, 12, 0);
    lv_obj_t *wifi_info_icon = ui_label_create(wifi_info, LV_SYMBOL_OK, &lv_font_montserrat_16, UI_OK);
    lv_obj_set_width(wifi_info_icon, 28);
    lv_obj_t *wifi_text = ui_flex_container_create(wifi_info, SET_PAGE_DETAIL_TEXT_W, 42,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    s_set_ui.wifi_ssid_label = ui_label_create(wifi_text, "----", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(s_set_ui.wifi_ssid_label, LV_PCT(100));
    lv_label_set_long_mode(s_set_ui.wifi_ssid_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_set_ui.wifi_ip_label = ui_label_create(wifi_text, "IP --", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_set_ui.wifi_ip_label, LV_PCT(100));
    set_page_wifi_refresh();
}

static void set_page_display_detail_show(void)
{
    lv_obj_t *body = set_page_detail_begin(LV_SYMBOL_EYE_OPEN, "Screen Display", UI_SECONDARY);
    lv_obj_t *display_panel = set_page_panel_create(body, LV_PCT(100), 218, "DISPLAY", UI_SECONDARY);
    lv_obj_t *brightness_head = ui_flex_container_create(display_panel, LV_PCT(100), 30,
                                                         LV_FLEX_FLOW_ROW,
                                                         LV_FLEX_ALIGN_START,
                                                         LV_FLEX_ALIGN_CENTER,
                                                         LV_FLEX_ALIGN_CENTER);
    lv_obj_t *brightness_icon = ui_label_create(brightness_head, LV_SYMBOL_EYE_OPEN,
                                                &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_width(brightness_icon, 36);
    lv_obj_t *brightness_name = ui_label_create(brightness_head, "Brightness",
                                                &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_width(brightness_name, 180);
    ui_flex_spacer_create(brightness_head);
    s_set_ui.brightness_label = ui_label_create(brightness_head, "", &lv_font_montserrat_20, UI_SECONDARY);
    lv_label_set_text_fmt(s_set_ui.brightness_label, "%d%%", s_set_brightness_percent);
    lv_obj_set_width(s_set_ui.brightness_label, 70);
    lv_obj_set_style_text_align(s_set_ui.brightness_label, LV_TEXT_ALIGN_RIGHT, 0);
    s_set_ui.brightness_slider = lv_slider_create(display_panel);
    lv_obj_set_size(s_set_ui.brightness_slider, LV_PCT(100), 14);
    lv_slider_set_range(s_set_ui.brightness_slider, SET_BRIGHTNESS_MIN, SET_BRIGHTNESS_MAX);
    lv_slider_set_value(s_set_ui.brightness_slider, s_set_brightness_percent, LV_ANIM_OFF);
    set_page_slider_style(s_set_ui.brightness_slider, UI_SECONDARY);
    lv_obj_add_event_cb(s_set_ui.brightness_slider, set_page_brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_set_ui.auto_dim_switch = set_page_switch_row_create(display_panel,
                                                          LV_SYMBOL_EYE_OPEN,
                                                          "Auto Dim",
                                                          "Dim after inactivity",
                                                          UI_WARN,
                                                          s_set_auto_dim_enabled);
    lv_obj_add_event_cb(s_set_ui.auto_dim_switch, set_page_auto_dim_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *timeout_row = ui_flex_container_create(display_panel, LV_PCT(100), 48,
                                                     LV_FLEX_FLOW_ROW,
                                                     LV_FLEX_ALIGN_START,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(timeout_row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(timeout_row, LV_OPA_80, 0);
    lv_obj_set_style_radius(timeout_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(timeout_row, 12, 0);
    lv_obj_set_style_pad_right(timeout_row, 12, 0);
    lv_obj_set_style_pad_column(timeout_row, 12, 0);
    lv_obj_t *timeout_icon = ui_label_create(timeout_row, LV_SYMBOL_EYE_CLOSE,
                                             &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_width(timeout_icon, 28);
    lv_obj_t *timeout_name = ui_label_create(timeout_row, "Screen Timeout",
                                             &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(timeout_name, 240);
    ui_flex_spacer_create(timeout_row);
    s_set_ui.screen_timeout_dropdown = lv_dropdown_create(timeout_row);
    lv_obj_set_size(s_set_ui.screen_timeout_dropdown, 118, 34);
    lv_dropdown_set_options(s_set_ui.screen_timeout_dropdown, "OFF\n30 sec\n1 min\n5 min\n10 min");
    lv_dropdown_set_selected(s_set_ui.screen_timeout_dropdown,
                             set_page_timeout_index_from_ms(s_set_screen_timeout_ms));
    set_page_dropdown_style(s_set_ui.screen_timeout_dropdown, UI_SECONDARY);
    lv_obj_add_event_cb(s_set_ui.screen_timeout_dropdown,
                        set_page_screen_timeout_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
}

static const char *set_page_pmu_charge_status_text(int status)
{
    static const char *const names[] = {"TRICKLE", "PRE", "CC", "CV", "DONE", "IDLE"};
    return status >= 0 && status < (int)(sizeof(names) / sizeof(names[0])) ? names[status] : "--";
}

static const int s_set_pmu_charge_current_values[] = {256, 512, 768, 960};

static int set_page_pmu_charge_current_index(int ma)
{
    int best = 0;
    int best_diff = INT32_MAX;
    for (int i = 0; i < (int)(sizeof(s_set_pmu_charge_current_values) /
                              sizeof(s_set_pmu_charge_current_values[0]));
         i++)
    {
        int diff = LV_ABS(ma - s_set_pmu_charge_current_values[i]);
        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }
    }
    return best;
}

static void set_page_pmu_value_set(lv_obj_t *label, int value, const char *unit)
{
    if (label == NULL)
    {
        return;
    }
    if (value < 0)
    {
        lv_label_set_text(label, "--");
    }
    else
    {
        lv_label_set_text_fmt(label, "%d%s", value, unit);
    }
}

static void set_page_pmu_refresh(void)
{
    if (s_set_ui.pmu_charger_switch == NULL)
    {
        return;
    }

    bmu_info_t info = {0};
    bool ready = bmu_is_ready();
    bool data_ready = bmu_status_info_get(&info) && info.ready;
    s_set_pmu_ui_updating = true;
    if (data_ready && info.charger_enabled)
    {
        lv_obj_add_state(s_set_ui.pmu_charger_switch, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(s_set_ui.pmu_charger_switch, LV_STATE_CHECKED);
    }
    if (ready)
    {
        lv_obj_remove_state(s_set_ui.pmu_charger_switch, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(s_set_ui.pmu_charger_switch, LV_STATE_DISABLED);
    }
    if (data_ready && s_set_ui.pmu_charge_current_dropdown)
    {
        lv_dropdown_set_selected(s_set_ui.pmu_charge_current_dropdown,
                                 set_page_pmu_charge_current_index(info.charge_current_limit_ma));
    }
    s_set_pmu_ui_updating = false;

    if (s_set_ui.pmu_charge_status_label)
    {
        lv_label_set_text(s_set_ui.pmu_charge_status_label,
                          data_ready ? set_page_pmu_charge_status_text(info.charge_status) : "OFFLINE");
        lv_obj_set_style_text_color(s_set_ui.pmu_charge_status_label,
                                    lv_color_hex(data_ready ? UI_OK : UI_ERROR), 0);
    }
    set_page_pmu_value_set(s_set_ui.pmu_charge_voltage_label, data_ready ? info.charge_voltage_mv : -1, " mV");
    set_page_pmu_value_set(s_set_ui.pmu_ibus_label, data_ready ? info.ibus_ma : -1, " mA");
    set_page_pmu_value_set(s_set_ui.pmu_ichg_label, data_ready ? info.ichg_ma : -1, " mA");
    set_page_pmu_value_set(s_set_ui.pmu_battery_soh_label, data_ready ? info.battery_soh : -1, "%");
    if (s_set_ui.pmu_battery_detect_label)
    {
        lv_label_set_text(s_set_ui.pmu_battery_detect_label,
                          data_ready ? (info.battery_detection_enabled ? (info.bat_present ? "DETECTED" : "NO BATTERY") : "DISABLED") : "--");
    }
    set_page_pmu_value_set(s_set_ui.pmu_vbus_label, data_ready ? info.vbus_mv : -1, " mV");
    set_page_pmu_value_set(s_set_ui.pmu_vsys_label, data_ready ? info.vsys_mv : -1, " mV");
    set_page_pmu_value_set(s_set_ui.pmu_vbat_label, data_ready ? info.vbat_mv : -1, " mV");
    set_page_pmu_value_set(s_set_ui.pmu_input_limit_label,
                           data_ready ? info.input_current_limit_ma : -1, " mA");
}

static void set_page_pmu_charger_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_set_pmu_ui_updating)
    {
        return;
    }
    bool enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (!bmu_charger_enable_set(enabled))
    {
        set_page_pmu_refresh();
        return;
    }
    set_page_pmu_refresh();
}

static void set_page_pmu_charger_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        s_set_ui.pmu_charger_switch == NULL ||
        lv_obj_has_state(s_set_ui.pmu_charger_switch, LV_STATE_DISABLED))
    {
        return;
    }

    lv_obj_t *sw = s_set_ui.pmu_charger_switch;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED))
    {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static void set_page_pmu_charge_current_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_set_pmu_ui_updating)
    {
        return;
    }
    uint32_t selected = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (selected >= sizeof(s_set_pmu_charge_current_values) /
                        sizeof(s_set_pmu_charge_current_values[0]) ||
        !bmu_charge_current_set(s_set_pmu_charge_current_values[selected]))
    {
        set_page_pmu_refresh();
    }
}

static void set_page_pmu_charge_current_control_create(lv_obj_t *panel)
{
    lv_obj_t *row = ui_flex_container_create(panel, LV_PCT(100), 42,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = ui_label_create(row, "Charge Current Set",
                                      &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(label, 180);
    ui_flex_spacer_create(row);

    s_set_ui.pmu_charge_current_dropdown = lv_dropdown_create(row);
    lv_obj_set_size(s_set_ui.pmu_charge_current_dropdown, 150, 36);
    lv_dropdown_set_options(s_set_ui.pmu_charge_current_dropdown,
                            "256 mA\n512 mA\n768 mA\n960 mA");
    set_page_dropdown_style(s_set_ui.pmu_charge_current_dropdown, UI_PRIMARY);
    lv_obj_add_event_cb(s_set_ui.pmu_charge_current_dropdown,
                        set_page_pmu_charge_current_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
}

static void set_page_pmu_detail_show(void)
{
    lv_obj_t *body = set_page_detail_begin(LV_SYMBOL_CHARGE, "BMU", UI_PRIMARY);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *panel = set_page_panel_create(body, LV_PCT(100), SET_PAGE_PMU_PANEL_H, "POWER MANAGEMENT", UI_PRIMARY);
    lv_obj_set_style_pad_row(panel, 8, 0);
    s_set_ui.pmu_charger_switch = set_page_switch_row_create(panel,
                                                             LV_SYMBOL_CHARGE,
                                                             "Charger",
                                                             "Battery charging control",
                                                             UI_PRIMARY,
                                                             false);
    lv_obj_set_ext_click_area(s_set_ui.pmu_charger_switch, 14);
    lv_obj_add_event_cb(s_set_ui.pmu_charger_switch, set_page_pmu_charger_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *charger_row = lv_obj_get_parent(s_set_ui.pmu_charger_switch);
    lv_obj_add_flag(charger_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(charger_row, set_page_pmu_charger_row_event_cb, LV_EVENT_CLICKED, NULL);
    set_page_pmu_charge_current_control_create(panel);
    s_set_ui.pmu_charge_status_label = set_page_info_line_create(panel, "Charge Status", "--", UI_OK);
    s_set_ui.pmu_charge_voltage_label = set_page_info_line_create(panel, "Charge Voltage", "--", UI_TEXT);
    s_set_ui.pmu_ibus_label = set_page_info_line_create(panel, "IBUS", "--", UI_PRIMARY);
    s_set_ui.pmu_ichg_label = set_page_info_line_create(panel, "ICHG", "--", UI_OK);
    s_set_ui.pmu_battery_soh_label = set_page_info_line_create(panel, "Battery Health", "--", UI_OK);
    s_set_ui.pmu_battery_detect_label = set_page_info_line_create(panel, "Battery Detect", "--", UI_TEXT);
    s_set_ui.pmu_vbus_label = set_page_info_line_create(panel, "VBUS", "--", UI_TEXT);
    s_set_ui.pmu_vsys_label = set_page_info_line_create(panel, "VSYS", "--", UI_TEXT);
    s_set_ui.pmu_vbat_label = set_page_info_line_create(panel, "VBAT", "--", UI_TEXT);
    s_set_ui.pmu_input_limit_label = set_page_info_line_create(panel, "Input Current Max", "--", UI_SECONDARY);
    set_page_pmu_refresh();
}

static void set_page_storage_detail_show(void)
{
    lv_obj_t *body = set_page_detail_begin(LV_SYMBOL_DRIVE, "SD Storage", UI_OK);
    lv_obj_t *storage_panel = set_page_panel_create(body, LV_PCT(100), 196, "STORAGE", UI_OK);
    s_set_usb_otg_usb_mode = usb_otg_msc_is_usb_mode();
    s_set_ui.usb_otg_switch = set_page_switch_row_create(storage_panel,
                                                         LV_SYMBOL_USB,
                                                         "USB OTG",
                                                         "Expose SD card as USB MSC",
                                                         UI_OK,
                                                         s_set_usb_otg_usb_mode);
    lv_obj_add_event_cb(s_set_ui.usb_otg_switch, set_page_usb_otg_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_set_ui.storage_label = set_page_info_line_create(storage_panel, "Status", "SD READY", UI_OK);
    s_set_ui.storage_type_label = set_page_info_line_create(storage_panel, "Type", "SDHC / SDXC", UI_TEXT);
    s_set_ui.storage_size_label = set_page_info_line_create(storage_panel, "Capacity", "-- GB", UI_SECONDARY);
    s_set_ui.storage_bus_label = set_page_info_line_create(storage_panel, "Bus", "SDMMC 4-bit", UI_MUTED);
    set_page_storage_refresh(true);
}

static void set_page_system_detail_show(void)
{
    lv_obj_t *body = set_page_detail_begin(LV_SYMBOL_SETTINGS, "System", UI_WARN);
    lv_obj_t *power_panel = set_page_panel_create(body, LV_PCT(100), 342, "POWER & SYSTEM", UI_WARN);
    s_set_ui.c5_state_label = set_page_info_line_create(power_panel,
                                                        "ESP32-C5",
                                                        esp32c5_sdio_slave_is_sleeping() ? "SLEEP" : (esp32c5_sdio_slave_is_ready() ? "ACTIVE" : "ERROR"),
                                                        esp32c5_sdio_slave_is_ready() ? UI_OK : UI_ERROR);
    s_set_ui.c5_sleep_switch = set_page_switch_row_create(power_panel,
                                                          LV_SYMBOL_PAUSE,
                                                          "C5 Sleep",
                                                          "Power down wireless coprocessor",
                                                          UI_WARN,
                                                          esp32c5_sdio_slave_is_sleeping());
    lv_obj_add_event_cb(s_set_ui.c5_sleep_switch, set_page_c5_sleep_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *c5_restart_btn = set_page_action_create(power_panel,
                                                      LV_SYMBOL_REFRESH,
                                                      "Restart ESP32-C5",
                                                      "Restart Hosted coprocessor",
                                                      UI_SECONDARY,
                                                      false);
    lv_obj_add_event_cb(c5_restart_btn, set_page_c5_restart_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_btn = set_page_action_create(power_panel,
                                                   LV_SYMBOL_REFRESH,
                                                   "Restart ESP32-P4",
                                                   "Restart the main processor",
                                                   UI_PRIMARY,
                                                   false);
    lv_obj_add_event_cb(restart_btn, set_page_power_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SET_POWER_ACTION_RESTART);
    lv_obj_t *ship_mode_btn = set_page_action_create(power_panel,
                                                     LV_SYMBOL_POWER,
                                                     "Ship Mode",
                                                     "Battery shelf mode",
                                                     UI_WARN,
                                                     false);
    lv_obj_add_event_cb(ship_mode_btn, set_page_power_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SET_POWER_ACTION_SHIP_MODE);

    set_page_c5_ui_apply();
}

static void set_page_detail_option_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    set_detail_page_t detail = (set_detail_page_t)(intptr_t)lv_event_get_user_data(e);
    switch (detail)
    {
    case SET_DETAIL_WIFI:
        set_page_wifi_detail_show();
        break;
    case SET_DETAIL_DISPLAY:
        set_page_display_detail_show();
        break;
    case SET_DETAIL_STORAGE:
        set_page_storage_detail_show();
        break;
    case SET_DETAIL_PMU:
        set_page_pmu_detail_show();
        break;
    case SET_DETAIL_SYSTEM:
        set_page_system_detail_show();
        break;
    default:
        break;
    }
}

static lv_obj_t *set_page_option_create(lv_obj_t *parent, const char *icon, const char *name,
                                        const char *desc, uint32_t color, set_detail_page_t detail)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, SET_PAGE_OPTION_FAR_W, SET_PAGE_OPTION_H);
    lv_obj_set_pos(btn, 20,
                   SET_PAGE_OPTIONS_PAD_Y +
                       (SET_PAGE_OPTION_H + SET_PAGE_OPTION_ROW_GAP) * detail);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 14, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(btn, 14, 0);
    lv_obj_set_style_pad_right(btn, 14, 0);
    lv_obj_set_style_pad_column(btn, 12, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(btn,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, set_page_detail_option_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)detail);
    lv_obj_t *icon_label = ui_label_create(btn, icon, &lv_font_montserrat_20, color);
    lv_obj_set_width(icon_label, 36);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *text_cont = ui_flex_container_create(btn,
                                                   SET_PAGE_OPTION_TEXT_W,
                                                   48,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_cont, 2, 0);

    lv_obj_t *name_label = ui_label_create(text_cont, name, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = ui_label_create(text_cont, desc, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(btn);

    lv_obj_t *arrow = ui_label_create(btn, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, UI_MUTED);
    lv_obj_set_width(arrow, 20);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);
    return btn;
}

static void set_page_timer(Page *page)
{
    (void)page;
    set_page_wifi_refresh();
    set_page_storage_refresh(false);
    set_page_pmu_refresh();
    if (!s_set_c5_action_busy && !s_set_wifi_switch_busy)
    {
        set_page_c5_ui_apply();
    }
}

static void set_page_create(Page *page)
{
    s_set_ui.root = page->root;
    lv_obj_set_style_bg_color(s_set_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_set_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_set_ui.root, 0, 0);
    lv_obj_add_flag(s_set_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_set_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(s_set_ui.root);

    s_set_ui.main_cont = lv_obj_create(s_set_ui.root);
    ui_main_cont_style1_init(s_set_ui.main_cont);
    lv_obj_set_style_bg_opa(s_set_ui.main_cont, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_set_ui.main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_set_ui.main_cont, LV_LAYOUT_NONE);

    lv_obj_t *options_panel = lv_obj_create(s_set_ui.main_cont);
    lv_obj_set_size(options_panel, 640, 640);
    lv_obj_set_pos(options_panel, 40, 40);
    ui_obj_set_transparent(options_panel);
    lv_obj_set_layout(options_panel, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(options_panel, LV_SCROLLBAR_MODE_OFF);
    set_page_option_create(options_panel,
                           LV_SYMBOL_WIFI,
                           "Wi-Fi",
                           esp32c5_sdio_slave_wifi_is_enabled() ? "Wireless radio and networks" : "Wireless radio is off",
                           UI_PRIMARY,
                           SET_DETAIL_WIFI);
    set_page_option_create(options_panel,
                           LV_SYMBOL_EYE_OPEN,
                           "Screen Display",
                           "Brightness, dim and timeout",
                           UI_SECONDARY,
                           SET_DETAIL_DISPLAY);
    set_page_option_create(options_panel,
                           LV_SYMBOL_DRIVE,
                           "SD Storage",
                           "SD card and USB MSC mode",
                           UI_OK,
                           SET_DETAIL_STORAGE);
    set_page_option_create(options_panel,
                           LV_SYMBOL_CHARGE,
                           "PMU",
                           "Charging, battery and power rails",
                           UI_PRIMARY,
                           SET_DETAIL_PMU);
    set_page_option_create(options_panel,
                           LV_SYMBOL_SETTINGS,
                           "System",
                           "Power, sleep and restart",
                           UI_SECONDARY,
                           SET_DETAIL_SYSTEM);
}

static void set_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void set_page_destroy(Page *page)
{
    (void)page;
    memset(&s_set_ui, 0, sizeof(s_set_ui));
}

int settings_page_get_brightness_percent(void)
{
    return s_set_brightness_percent;
}

void settings_page_register(void)
{
    Page page = {
        .id = PAGE_SET,
        .name = "set",
        .on_create = set_page_create,
        .on_leave = set_page_leave,
        .on_destroy = set_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = set_page_timer,
        .timer_interval_ms = 1000,
    };
    ui_page_register(&page);
}
