#include "status_bar.h"

static int status_bar_clamp_percent(int percent)
{
    if (percent < 0)
    {
        return -1;
    }
    return percent > 100 ? 100 : percent;
}

static const char *status_bar_battery_symbol(int percent)
{
    if (percent < 0)
    {
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (percent >= 90)
    {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 65)
    {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 35)
    {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 10)
    {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

#include "sdkconfig.h"
#include "lvgl_page_manager.h"
#include "system.h"
#include "ui_widgets.h"
#include "display_dim.h"

static status_bar_ui_t s_status_ui;
static lv_timer_t *s_status_bar_timer;
struct tm g_ui_timeinfo = {0};

static void status_bar_refresh(void)
{
    status_info_t status_info = {0};
    status_bar_update(status_bar_info_get(&status_info) ? &status_info : NULL);
}

static void status_bar_timer_cb(lv_timer_t *timer)
{
    status_bar_refresh();
}

static void status_bar_timer_start(void)
{
    if (s_status_bar_timer == NULL)
    {
        s_status_bar_timer = lv_timer_create(status_bar_timer_cb, STATUS_TIMER_MS, NULL);
    }
}

static void status_bar_base_create(void)
{
    s_status_ui.status_cont = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_status_ui.status_cont, SCREEN_WIDTH, 56);
    lv_obj_align(s_status_ui.status_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_status_ui.status_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_status_ui.status_cont, 0, 0);
    lv_obj_set_style_pad_all(s_status_ui.status_cont, 0, 0);
    lv_obj_remove_flag(s_status_ui.status_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_ui.status_cont, LV_OBJ_FLAG_CLICKABLE);
}

#if CONFIG_T_PANEL_P4_BOARD_ROUND
static void status_bar_round_create(void)
{
    status_bar_base_create();
    lv_obj_set_height(s_status_ui.status_cont, 120);

    s_status_ui.wifi_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_WIFI,
                                            &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_pos(s_status_ui.wifi_icon, 190, 33);
    lv_obj_set_width(s_status_ui.wifi_icon, 60);
    lv_obj_set_style_text_align(s_status_ui.wifi_icon, LV_TEXT_ALIGN_CENTER, 0);

    s_status_ui.bluetooth_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_BLUETOOTH,
                                                 &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_pos(s_status_ui.bluetooth_icon, 240, 16);
    lv_obj_set_width(s_status_ui.bluetooth_icon, 60);
    lv_obj_set_style_text_align(s_status_ui.bluetooth_icon, LV_TEXT_ALIGN_CENTER, 0);

    s_status_ui.time_label = ui_label_create(s_status_ui.status_cont, "--:--",
                                             &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_width(s_status_ui.time_label, 100);
    lv_obj_set_pos(s_status_ui.time_label, 310, 5);
    lv_obj_set_style_text_align(s_status_ui.time_label, LV_TEXT_ALIGN_CENTER, 0);

    s_status_ui.battery_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_BATTERY_FULL,
                                               &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_pos(s_status_ui.battery_icon, 420, 16);
    lv_obj_set_width(s_status_ui.battery_icon, 60);
    lv_obj_set_style_text_align(s_status_ui.battery_icon, LV_TEXT_ALIGN_CENTER, 0);

    s_status_ui.battery_label = ui_label_create(s_status_ui.status_cont, "--%",
                                                &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_width(s_status_ui.battery_label, 90);
    lv_obj_set_pos(s_status_ui.battery_label, 455, 33);
    lv_obj_set_style_text_align(s_status_ui.battery_label, LV_TEXT_ALIGN_CENTER, 0);

    s_status_ui.date_label = ui_label_create(s_status_ui.status_cont, "----/--/--",
                                                &lv_font_SourceHanSansSC_Regular_2_20, UI_MUTED);
    lv_obj_set_width(s_status_ui.date_label, 120);
    lv_obj_align(s_status_ui.date_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_align(s_status_ui.date_label, LV_TEXT_ALIGN_CENTER, 0);
}
#else
static void status_bar_linear_create(void)
{
    status_bar_base_create();
    lv_obj_set_height(s_status_ui.status_cont, 40);
    lv_obj_set_style_bg_color(s_status_ui.status_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_status_ui.status_cont, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(s_status_ui.status_cont, 20, 0);
    lv_obj_set_style_pad_right(s_status_ui.status_cont, 20, 0);
    lv_obj_set_style_pad_column(s_status_ui.status_cont, 10, 0);
    ui_obj_set_flex(s_status_ui.status_cont, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_status_ui.date_label = ui_label_create(s_status_ui.status_cont, "----/--/--", &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_style_text_align(s_status_ui.date_label, LV_TEXT_ALIGN_CENTER, 0);
    s_status_ui.time_label = ui_label_create(s_status_ui.status_cont, "--:--",
                                             &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_set_style_text_align(s_status_ui.time_label, LV_TEXT_ALIGN_CENTER, 0);
    ui_flex_spacer_create(s_status_ui.status_cont);
    s_status_ui.wifi_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_WIFI,
                                            &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    s_status_ui.bluetooth_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_BLUETOOTH,
                                                 &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    s_status_ui.battery_icon = ui_label_create(s_status_ui.status_cont, LV_SYMBOL_BATTERY_FULL,
                                               &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    s_status_ui.battery_label = ui_label_create(s_status_ui.status_cont, "100%",
                                                &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
}
#endif

void status_bar_create(void)
{
    if (s_status_ui.status_cont)
    {
        lv_obj_move_foreground(s_status_ui.status_cont);
        status_bar_timer_start();
        display_dim_timer_start();
        return;
    }

#if CONFIG_T_PANEL_P4_BOARD_ROUND
    status_bar_round_create();
#else
    status_bar_linear_create();
#endif

    status_bar_timer_start();
    display_dim_timer_start();
}

lv_obj_t *status_bar_get_container(void)
{
    return s_status_ui.status_cont;
}

void status_bar_update(const status_info_t *status)
{
    ui_lock();

    char time_str[6] = "--:--";
    char date_str[11] = "----/--/--";
    time_t now = time(NULL);
    if (now > 1609459200 && localtime_r(&now, &g_ui_timeinfo) != NULL)
    {
        strftime(time_str, sizeof(time_str), "%H:%M", &g_ui_timeinfo);
        strftime(date_str, sizeof(date_str), "%Y/%m/%d", &g_ui_timeinfo);
    }

    if (s_status_ui.time_label)
    {
        lv_label_set_text(s_status_ui.time_label, time_str);
    }
    if (s_status_ui.date_label)
    {
        lv_label_set_text(s_status_ui.date_label, date_str);
    }

    if (status)
    {
        if (s_status_ui.wifi_icon)
        {
            lv_obj_set_style_text_color(s_status_ui.wifi_icon,
                                        lv_color_hex(status->wifi_connected ? UI_OK : UI_MUTED), 0);
        }

        if (s_status_ui.bluetooth_icon)
        {
            uint32_t bt_color = UI_MUTED;
            if (status->bluetooth_connected)
            {
                bt_color = UI_PRIMARY;
            }
            else if (status->bluetooth_enabled)
            {
                bt_color = UI_WARN;
            }
            lv_obj_set_style_text_color(s_status_ui.bluetooth_icon, lv_color_hex(bt_color), 0);
        }

        int battery_percent = status_bar_clamp_percent(status->battery_percent);
        uint32_t battery_color = battery_percent >= 0 && battery_percent < 15 ? UI_ERROR : UI_TEXT;
        if (s_status_ui.battery_icon)
        {
            lv_label_set_text(s_status_ui.battery_icon,
                              status->battery_charging ? LV_SYMBOL_CHARGE : status_bar_battery_symbol(battery_percent));
            lv_obj_set_style_text_color(s_status_ui.battery_icon, lv_color_hex(battery_color), 0);
        }
        if (s_status_ui.battery_label)
        {
            if (battery_percent >= 0)
            {
                lv_label_set_text_fmt(s_status_ui.battery_label, "%d%%", battery_percent);
            }
            else
            {
                lv_label_set_text(s_status_ui.battery_label, "--%");
            }
            lv_obj_set_style_text_color(s_status_ui.battery_label, lv_color_hex(battery_color), 0);
        }
    }

    ui_unlock();
}
