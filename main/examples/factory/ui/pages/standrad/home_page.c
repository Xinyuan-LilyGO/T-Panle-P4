#include "ui_internal.h"
#include "home_page.h"
#include "image/app_icons.h"

#define HOME_LOGO_ANIM_DURATION_MS   700
#define HOME_STATUS_ANIM_DURATION_MS 1200
#define HOME_STATUS_ANIM_DELAY_MS    250
#define HOME_STATUS_START_Y          (-40)
#define HOME_APP_ICON_SIZE           96
#define HOME_APP_GRID_X              10
#define HOME_APP_GRID_Y              110
#define HOME_APP_GRID_W              700
#define HOME_APP_GRID_H              500

static const char *TAG = "[UI][home_page]";
static home_page_ui_t s_home_ui;
static lv_obj_t *s_home_status_bar;
static bool s_home_enter_animated;

typedef struct
{
    const char *name;
    const lv_image_dsc_t *icon;
    PageType page;
    uint32_t accent;
} home_app_t;

static const home_app_t s_home_apps[HOME_APP_MENU_COUNT] = {
    {.name = "Music",    .icon = &_music_RGB565A8_68x68, .page = PAGE_MUSIC,     .accent = 0x4CC9F0},
    {.name = "Record",   .icon = &_record_RGB565A8_68x68, .page = PAGE_RECORD,    .accent =0xFF7896},
    {.name = "Camera",   .icon = &_camera_RGB565A8_68x68, .page = PAGE_CAMERA,    .accent = 0xFFC857},
    {.name = "Lora",     .icon = &_lora_RGB565A8_68x68, .page = PAGE_LORA,      .accent = 0x62D6C1},
    {.name = "File",     .icon = &_files_RGB565A8_68x68, .page = PAGE_FILE,      .accent = 0x90CAF9},
    {.name = "Set",      .icon = &_set_RGB565A8_68x68, .page = PAGE_SET,       .accent = 0xB8A6E8},
    {.name = "SelfTest", .icon = &_list_RGB565A8_68x68, .page = PAGE_SELF_TEST, .accent = 0x7EE787},
};

static const int32_t s_home_grid_cols[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST,
};

static const int32_t s_home_grid_rows[] = {
    200, 200, LV_GRID_TEMPLATE_LAST,
};

void home_page_set_mic_levels(int mic0_db, int mic1_db)
{
    (void)mic0_db;
    (void)mic1_db;
}

void home_music_status_update_apply(void)
{
}

void home_camera_status_update_apply(void)
{
}

void home_lora_status_update_apply(void)
{
}

int home_clamp_percent(int percent)
{
    if (percent < 0)
    {
        return -1;
    }
    if (percent > 100)
    {
        return 100;
    }
    return percent;
}

const char *home_battery_symbol(int percent)
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

static void home_app_clicked_cb(lv_event_t *event)
{
    const home_app_t *app = (const home_app_t *)lv_event_get_user_data(event);
    if (app == NULL || app->page >= PAGE_SIZE)
    {
        return;
    }

    ESP_LOGI(TAG, "%s menu clicked", app->name);
    ui_page_switch_async(app->page);
}

static void home_app_press_cb(lv_event_t *event)
{
    lv_obj_t *icon_box = (lv_obj_t *)lv_event_get_user_data(event);
    if (icon_box == NULL)
    {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED)
    {
        lv_obj_add_state(icon_box, LV_STATE_PRESSED);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        lv_obj_remove_state(icon_box, LV_STATE_PRESSED);
    }
}

static void home_app_create(lv_obj_t *parent, const home_app_t *app, int index)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_grid_cell(tile,
                         LV_GRID_ALIGN_STRETCH, index % 4, 1,
                         LV_GRID_ALIGN_STRETCH, index / 4, 1);
    ui_obj_set_transparent(tile);
    lv_obj_set_style_pad_all(tile, 8, 0);
    lv_obj_set_style_pad_row(tile, 12, 0);
    lv_obj_set_style_opa(tile, LV_OPA_70, LV_STATE_PRESSED);
    ui_obj_set_flex(tile,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, home_app_clicked_cb, LV_EVENT_CLICKED, (void *)app);

    lv_obj_t *icon_box = lv_obj_create(tile);
    lv_obj_set_size(icon_box, HOME_APP_ICON_SIZE, HOME_APP_ICON_SIZE);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(app->accent), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_border_color(icon_box, lv_color_hex(app->accent), 0);
    lv_obj_set_style_border_opa(icon_box, LV_OPA_50, 0);
    lv_obj_set_style_border_opa(icon_box, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(icon_box, 20, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_set_style_shadow_color(icon_box, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_shadow_width(icon_box, 26, 0);
    lv_obj_set_style_shadow_spread(icon_box, 3, 0);
    lv_obj_set_style_shadow_opa(icon_box, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(icon_box, 32, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(icon_box, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, home_app_press_cb, LV_EVENT_PRESSED, icon_box);
    lv_obj_add_event_cb(tile, home_app_press_cb, LV_EVENT_RELEASED, icon_box);
    lv_obj_add_event_cb(tile, home_app_press_cb, LV_EVENT_PRESS_LOST, icon_box);

    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, app->icon);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_text(name, app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_SourceHanSansCN_Bold_2_20, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
}

static void home_apps_create(lv_obj_t *parent)
{
    lv_obj_set_grid_dsc_array(parent, s_home_grid_cols, s_home_grid_rows);
    lv_obj_set_grid_align(parent, LV_GRID_ALIGN_SPACE_EVENLY,
                          LV_GRID_ALIGN_CENTER);
    lv_obj_set_style_pad_top(parent, 18, 0);
    lv_obj_set_style_pad_bottom(parent, 18, 0);
    lv_obj_set_style_pad_left(parent, 18, 0);
    lv_obj_set_style_pad_right(parent, 18, 0);
    lv_obj_set_style_pad_row(parent, 20, 0);
    lv_obj_set_style_pad_column(parent, 10, 0);

    for (int i = 0; i < HOME_APP_MENU_COUNT; i++)
    {
        home_app_create(parent, &s_home_apps[i], i);
    }
}

static void home_status_translate_y_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void home_logo_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void home_enter_anim_start(lv_obj_t *obj,
                                  lv_anim_exec_xcb_t exec_cb,
                                  int32_t start,
                                  int32_t end,
                                  uint32_t duration,
                                  uint32_t delay)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, start, end);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_start(&anim);
}

static void home_page_create(Page *page)
{
    s_home_ui.root = page->root;
    lv_obj_set_style_bg_color(s_home_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_home_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_home_ui.root, 0, 0);

    status_bar_create();
    s_home_status_bar = status_bar_get_container();
    if (s_home_status_bar != NULL)
    {
        lv_obj_set_style_translate_y(s_home_status_bar, HOME_STATUS_START_Y, 0);
    }

    ui_background_create(s_home_ui.root);

    s_home_ui.main_cont = lv_obj_create(s_home_ui.root);
    lv_obj_set_size(s_home_ui.main_cont, HOME_APP_GRID_W, HOME_APP_GRID_H);
    lv_obj_set_pos(s_home_ui.main_cont, HOME_APP_GRID_X, HOME_APP_GRID_Y);
    ui_obj_set_transparent(s_home_ui.main_cont);
    lv_obj_set_scrollbar_mode(s_home_ui.main_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_home_ui.main_cont, LV_OBJ_FLAG_SCROLLABLE);
    home_apps_create(s_home_ui.main_cont);

    s_home_ui.logo_label = ui_label_create(s_home_ui.root, "LILYGO",
                                            &lv_font_SourceHanSansCN_Bold_2_24,
                                            UI_MUTED);
    lv_obj_set_width(s_home_ui.logo_label, 160);
    lv_obj_set_style_opa(s_home_ui.logo_label, LV_OPA_TRANSP, 0);
    lv_obj_align(s_home_ui.logo_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_align(s_home_ui.logo_label, LV_TEXT_ALIGN_CENTER, 0);

    s_home_enter_animated = false;
}

static void home_page_enter(Page *page)
{
    (void)page;

    if (s_home_enter_animated)
    {
        return;
    }
    s_home_enter_animated = true;

    if (s_home_status_bar != NULL)
    {
        home_enter_anim_start(s_home_status_bar,
                              home_status_translate_y_cb,
                              HOME_STATUS_START_Y,
                              0,
                              HOME_STATUS_ANIM_DURATION_MS,
                              HOME_STATUS_ANIM_DELAY_MS);
    }
    if (s_home_ui.logo_label != NULL)
    {
        home_enter_anim_start(s_home_ui.logo_label,
                              home_logo_opa_cb,
                              LV_OPA_TRANSP,
                              LV_OPA_COVER,
                              HOME_LOGO_ANIM_DURATION_MS,
                              0);
    }
}

void home_page_register(void)
{
    Page page = {
        .id = PAGE_HOME,
        .name = "home",
        .on_create = home_page_create,
        .on_enter = home_page_enter,
    };
    ui_page_register(&page);
}
