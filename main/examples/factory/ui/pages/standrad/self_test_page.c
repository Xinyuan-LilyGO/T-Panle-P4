#include "ui_internal.h"
#include "self_test_page.h"
#include "lcd.h"

#define SELF_TEST_ITEM_COUNT 8
#define SELF_TEST_PREVIEW_WIDTH 640
#define SELF_TEST_PREVIEW_HEIGHT 320
#define SELF_TEST_DISPLAY_COLOR_COUNT 6
#define SELF_TEST_STORAGE_FILE FILE_SCAN_DIR "/.t_panel_self_test.tmp"
#define SELF_TEST_STORAGE_TEXT "T-Panel-P4 SD read/write test"

#define SELF_TEST_ITEM_HEIGHT     60
#define SELF_TEST_ITEM_ICON_WIDTH 36
#define SELF_TEST_ITEM_BUTTON_W   80
#define SELF_TEST_ITEM_BUTTON_H   40
#define SELF_TEST_ITEM_LIST_GAP   6
#define SELF_TEST_ITEM_ICON_FONT  (&lv_font_montserrat_20)
#define SELF_TEST_ITEM_TEXT_FONT  (&lv_font_SourceHanSansCN_Bold_2_20)
#define SELF_TEST_PAIR_ROW_HEIGHT 36
#define SELF_TEST_PAIR_ROW_FONT   (&lv_font_SourceHanSansCN_Bold_2_20)

typedef enum
{
    SELF_TEST_DISPLAY = 0,
    SELF_TEST_TOUCH,
    SELF_TEST_STORAGE,
    SELF_TEST_AUDIO,
    SELF_TEST_CAMERA,
    SELF_TEST_POWER,
    SELF_TEST_LORA,
    SELF_TEST_ESP32C5,
} self_test_kind_t;

typedef struct
{
    const char *name;
    const char *symbol;
    self_test_kind_t kind;
} self_test_item_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *status;
    lv_obj_t *display_screen;
    lv_obj_t *display_color_label;
    lv_obj_t *display_step_label;
    lv_obj_t *display_next_button;
    lv_obj_t *touch_dots[TOUCH_PANEL_MAX_POINTS];
    lv_obj_t *finger_count_label;
    lv_obj_t *touch_position_label;
    lv_obj_t *audio_bars[2];
    lv_obj_t *audio_labels[2];
    lv_obj_t *audio_play_button_label;
    lv_obj_t *power_values[6];
    lv_obj_t *c5_values[5];
    uint8_t display_color_index;
    uint32_t refresh_ticks;
    bool camera_started;
    bool audio_playing;
} self_test_run_ui_t;

typedef struct
{
    bool access_ok;
    bool write_ok;
    bool read_ok;
    bool compare_ok;
    char readback[64];
} self_test_storage_result_t;

static const self_test_item_t s_self_test_items[SELF_TEST_ITEM_COUNT] = {
    {.name = "Display", .symbol = LV_SYMBOL_IMAGE, .kind = SELF_TEST_DISPLAY},
    {.name = "Touch", .symbol = LV_SYMBOL_EDIT, .kind = SELF_TEST_TOUCH},
    {.name = "Storage", .symbol = LV_SYMBOL_DIRECTORY, .kind = SELF_TEST_STORAGE},
    {.name = "Audio", .symbol = LV_SYMBOL_AUDIO, .kind = SELF_TEST_AUDIO},
    {.name = "Camera", .symbol = LV_SYMBOL_EYE_OPEN, .kind = SELF_TEST_CAMERA},
    {.name = "Power", .symbol = LV_SYMBOL_CHARGE, .kind = SELF_TEST_POWER},
    {.name = "LoRa", .symbol = LV_SYMBOL_WIFI, .kind = SELF_TEST_LORA},
    {.name = "ESP32-C5", .symbol = LV_SYMBOL_SETTINGS, .kind = SELF_TEST_ESP32C5},
};

static const uint32_t s_display_test_colors[SELF_TEST_DISPLAY_COLOR_COUNT] = {
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0xFFFFFF,
    0x808080,
    0x000000,
};

static const char *s_display_test_names[SELF_TEST_DISPLAY_COLOR_COUNT] = {
    "Red",
    "Green",
    "Blue",
    "White",
    "Gray",
    "Black",
};

static self_test_kind_t s_active_test = SELF_TEST_DISPLAY;
static self_test_run_ui_t s_run_ui;
static portMUX_TYPE s_c5_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_c5_probe_running;
static bool s_c5_probe_done;
static bool s_c5_probe_ok;
static bool s_c5_probe_attempted;
static uint32_t s_c5_probe_chip_id;
static char s_c5_probe_target[16];

static void self_test_back_to_home_cb(lv_event_t *event)
{
    (void)event;
    ui_page_switch_async(PAGE_HOME);
}

static void self_test_back_to_list_cb(lv_event_t *event)
{
    (void)event;
    ui_page_switch_async(PAGE_SELF_TEST);
}

static lv_obj_t *self_test_header_create(lv_obj_t *root,
                                         const char *title_text,
                                         lv_event_cb_t back_cb)
{
    lv_obj_t *back_button = lv_button_create(root);
    lv_obj_set_size(back_button, 56, 56);
    lv_obj_set_pos(back_button, 24, 64);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(back_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back_button, 0, 0);
    lv_obj_set_style_radius(back_button, 8, 0);
    lv_obj_set_style_pad_all(back_button, 0, 0);
    lv_obj_add_event_cb(back_button, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_button);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, title_text);
    lv_obj_set_size(title, 360, 56);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_SourceHanSansCN_Bold_2_28, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    return title;
}

static void self_test_run_clicked_cb(lv_event_t *event)
{
    const self_test_item_t *item =
        (const self_test_item_t *)lv_event_get_user_data(event);
    if (item == NULL)
    {
        return;
    }

    s_active_test = item->kind;
    ui_page_switch_async(PAGE_SELF_TEST_RUN);
}

static void self_test_item_create(lv_obj_t *parent,
                                  const self_test_item_t *item)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 656, SELF_TEST_ITEM_HEIGHT);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(row, 18, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, item->symbol);
    lv_obj_set_width(icon, SELF_TEST_ITEM_ICON_WIDTH);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(icon, SELF_TEST_ITEM_ICON_FONT, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, item->name);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(name, SELF_TEST_ITEM_TEXT_FONT, 0);

    ui_light_button_create(row, "Run",
                           SELF_TEST_ITEM_BUTTON_W,
                           SELF_TEST_ITEM_BUTTON_H,
                           &lv_font_SourceHanSansSC_Regular_2_16,
                           self_test_run_clicked_cb,
                           (void *)item);
}

static void self_test_page_create(Page *page)
{
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);

    self_test_header_create(page->root, "Self Test", self_test_back_to_home_cb);

    lv_obj_t *list = lv_obj_create(page->root);
    lv_obj_set_size(list, 656, DISPLAY_PANEL_V_RES - 180);
    lv_obj_set_pos(list, 32, 160);
    ui_obj_set_transparent(list);
    lv_obj_set_style_pad_row(list, SELF_TEST_ITEM_LIST_GAP, 0);
    ui_obj_set_flex(list, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < SELF_TEST_ITEM_COUNT; i++)
    {
        self_test_item_create(list, &s_self_test_items[i]);
    }
}

static lv_obj_t *self_test_pair_row_create(lv_obj_t *parent,
                                           int32_t x, int32_t y, int32_t width,
                                           const char *name, const char *value,
                                           uint32_t value_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, width, SELF_TEST_PAIR_ROW_HEIGHT);
    lv_obj_set_pos(row, x, y);
    ui_obj_set_transparent(row);
    ui_obj_set_flex(row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_obj_set_flex_grow(name_label, 1);
    lv_obj_set_style_text_color(name_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(name_label, SELF_TEST_PAIR_ROW_FONT, 0);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value);
    lv_obj_set_width(value_label, width * 2 / 3);
    lv_obj_set_style_text_color(value_label, lv_color_hex(value_color), 0);
    lv_obj_set_style_text_font(value_label, SELF_TEST_PAIR_ROW_FONT, 0);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    return value_label;
}

static lv_obj_t *self_test_status_create(const char *text, uint32_t color)
{
    s_run_ui.status = self_test_pair_row_create(s_run_ui.body,
                                                0, 0, 656,
                                                "Status", text, color);
    return s_run_ui.status;
}

static void self_test_display_update(void)
{
    uint8_t index = s_run_ui.display_color_index;
    if (index >= SELF_TEST_DISPLAY_COLOR_COUNT)
    {
        return;
    }

    lv_obj_set_style_bg_color(s_run_ui.display_screen,
                              lv_color_hex(s_display_test_colors[index]), 0);
    bool dark_text = index == 3 || index == 4;
    lv_obj_set_style_text_color(s_run_ui.title,
                                lv_color_hex(dark_text ? UI_BG : UI_TEXT), 0);
    lv_label_set_text(s_run_ui.display_color_label,
                      s_display_test_names[index]);
    lv_obj_set_style_text_color(s_run_ui.display_color_label,
                                lv_color_hex(dark_text ? UI_BG : UI_TEXT), 0);

    if (index == SELF_TEST_DISPLAY_COLOR_COUNT - 1)
    {
        lv_label_set_text(s_run_ui.display_step_label, "Complete");
        lv_obj_set_style_text_color(s_run_ui.display_step_label,
                                    lv_color_hex(UI_OK), 0);
        lv_obj_add_flag(s_run_ui.display_next_button, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text_fmt(s_run_ui.display_step_label, "%u / %u",
                              (unsigned)index + 1U,
                              (unsigned)SELF_TEST_DISPLAY_COLOR_COUNT);
        lv_obj_set_style_text_color(s_run_ui.display_step_label,
                                    lv_color_hex(dark_text ? UI_BG : UI_TEXT), 0);
        lv_obj_remove_flag(s_run_ui.display_next_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void self_test_display_next_cb(lv_event_t *event)
{
    (void)event;
    if (s_run_ui.display_color_index < SELF_TEST_DISPLAY_COLOR_COUNT - 1)
    {
        s_run_ui.display_color_index++;
        self_test_display_update();
    }
}

static void self_test_display_build(void)
{
    lv_obj_set_size(s_run_ui.body, DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    lv_obj_set_pos(s_run_ui.body, 0, 0);
    lv_obj_remove_flag(s_run_ui.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *status_bar = status_bar_get_container();
    if (status_bar != NULL)
    {
        lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
    }

    s_run_ui.display_screen = lv_obj_create(s_run_ui.body);
    lv_obj_set_size(s_run_ui.display_screen,
                    DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    lv_obj_set_pos(s_run_ui.display_screen, 0, 0);
    lv_obj_set_style_bg_opa(s_run_ui.display_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_run_ui.display_screen, 0, 0);
    lv_obj_set_style_radius(s_run_ui.display_screen, 0, 0);
    lv_obj_set_style_pad_all(s_run_ui.display_screen, 0, 0);
    lv_obj_remove_flag(s_run_ui.display_screen,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_run_ui.display_color_label = self_test_pair_row_create(
        s_run_ui.body, 32, 126, 656, "Color", "Red", UI_TEXT);
    s_run_ui.display_step_label = self_test_pair_row_create(
        s_run_ui.body, 32, 178, 656, "Step", "1 / 6", UI_TEXT);

    s_run_ui.display_next_button = lv_button_create(s_run_ui.body);
    lv_obj_set_size(s_run_ui.display_next_button, 180, 64);
    lv_obj_align(s_run_ui.display_next_button, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_color(s_run_ui.display_next_button,
                              lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_run_ui.display_next_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_run_ui.display_next_button, 1, 0);
    lv_obj_set_style_border_color(s_run_ui.display_next_button,
                                  lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_radius(s_run_ui.display_next_button, 8, 0);
    lv_obj_add_event_cb(s_run_ui.display_next_button,
                        self_test_display_next_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_label = lv_label_create(s_run_ui.display_next_button);
    lv_label_set_text(next_label, "Next");
    lv_obj_set_style_text_color(next_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(next_label,
                               &lv_font_SourceHanSansCN_Bold_2_24, 0);
    lv_obj_center(next_label);

    s_run_ui.display_color_index = 0;
    self_test_display_update();
}

static void self_test_touch_refresh(void)
{
    touch_panel_data_t data = {0};
    lcd_touch_data_get(&data);
    uint8_t finger_count = data.finger_count;
    if (finger_count > TOUCH_PANEL_MAX_POINTS)
    {
        finger_count = TOUCH_PANEL_MAX_POINTS;
    }

    for (uint8_t i = 0; i < TOUCH_PANEL_MAX_POINTS; i++)
    {
        lv_obj_t *dot = s_run_ui.touch_dots[i];
        if (dot == NULL)
        {
            continue;
        }
        if (i >= finger_count)
        {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        int32_t x = data.points[i].x;
        int32_t y = data.points[i].y;
        if (x < 18)
            x = 18;
        if (y < 18)
            y = 18;
        if (x > DISPLAY_PANEL_H_RES - 18)
            x = DISPLAY_PANEL_H_RES - 18;
        if (y > DISPLAY_PANEL_V_RES - 18)
            y = DISPLAY_PANEL_V_RES - 18;
        lv_obj_set_pos(dot, x - 18, y - 18);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_run_ui.finger_count_label != NULL)
    {
        lv_label_set_text_fmt(s_run_ui.finger_count_label, "%u",
                              (unsigned)finger_count);
    }
    if (s_run_ui.touch_position_label != NULL)
    {
        if (finger_count > 0)
        {
            lv_label_set_text_fmt(s_run_ui.touch_position_label,
                                  "%u, %u",
                                  (unsigned)data.points[0].x,
                                  (unsigned)data.points[0].y);
        }
        else
        {
            lv_label_set_text(s_run_ui.touch_position_label, "--");
        }
    }
}

static void self_test_touch_build(void)
{
    static const uint32_t dot_colors[TOUCH_PANEL_MAX_POINTS] = {
        UI_PRIMARY,
        UI_SECONDARY,
        UI_OK,
        UI_WARN,
        UI_ERROR,
    };

    lv_obj_set_size(s_run_ui.body, DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    lv_obj_set_pos(s_run_ui.body, 0, 0);
    lv_obj_remove_flag(s_run_ui.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *status_bar = status_bar_get_container();
    if (status_bar != NULL)
    {
        lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *pad = lv_obj_create(s_run_ui.body);
    lv_obj_set_size(pad, DISPLAY_PANEL_H_RES, DISPLAY_PANEL_V_RES);
    lv_obj_set_pos(pad, 0, 0);
    lv_obj_set_style_bg_color(pad, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pad, 2, 0);
    lv_obj_set_style_border_color(pad, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_radius(pad, 0, 0);
    lv_obj_set_style_pad_all(pad, 0, 0);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (uint8_t i = 0; i < TOUCH_PANEL_MAX_POINTS; i++)
    {
        lv_obj_t *dot = lv_obj_create(s_run_ui.body);
        lv_obj_set_size(dot, 36, 36);
        lv_obj_set_style_bg_color(dot, lv_color_hex(dot_colors[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *number = lv_label_create(dot);
        lv_label_set_text_fmt(number, "%u", (unsigned)i + 1U);
        lv_obj_set_style_text_color(number, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_text_font(number, &lv_font_montserrat_14, 0);
        lv_obj_center(number);
        s_run_ui.touch_dots[i] = dot;
    }

    s_run_ui.finger_count_label = self_test_pair_row_create(
        s_run_ui.body, 32, 126, 656, "Fingers", "0", UI_TEXT);
    s_run_ui.touch_position_label = self_test_pair_row_create(
        s_run_ui.body, 32, 178, 656, "Position", "--", UI_TEXT);
    self_test_touch_refresh();
}

static void self_test_result_row_create(int32_t y, const char *name, bool passed)
{
    uint32_t color = passed ? UI_OK : UI_ERROR;
    lv_obj_t *row = lv_obj_create(s_run_ui.body);
    lv_obj_set_size(row, 656, 50);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(color), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_left(row, 22, 0);
    lv_obj_set_style_pad_right(row, 22, 0);
    lv_obj_set_style_pad_column(row, 18, 0);
    ui_obj_set_flex(row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, passed ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
    lv_obj_set_width(icon, 40);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_obj_set_flex_grow(name_label, 1);
    lv_obj_set_style_text_color(name_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(name_label, SELF_TEST_PAIR_ROW_FONT, 0);

    lv_obj_t *result_label = lv_label_create(row);
    lv_label_set_text(result_label, passed ? "Passed" : "Failed");
    lv_obj_set_width(result_label, 110);
    lv_obj_set_style_text_color(result_label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(result_label, SELF_TEST_PAIR_ROW_FONT, 0);
    lv_obj_set_style_text_align(result_label, LV_TEXT_ALIGN_RIGHT, 0);
}

static void self_test_storage_build(void)
{
    factory_storage_info_t info = {0};
    bool info_ok = storage_info_get(&info);
    self_test_storage_result_t result = {0};

    if (info_ok && info.app_mounted && storage_app_access_begin())
    {
        result.access_ok = true;
        const size_t expected_size = strlen(SELF_TEST_STORAGE_TEXT);
        FILE *file = fopen(SELF_TEST_STORAGE_FILE, "wb");
        if (file != NULL)
        {
            size_t written = fwrite(SELF_TEST_STORAGE_TEXT, 1,
                                    expected_size, file);
            int flush_result = fflush(file);
            int close_result = fclose(file);
            result.write_ok = written == expected_size &&
                              flush_result == 0 && close_result == 0;
        }

        if (result.write_ok)
        {
            file = fopen(SELF_TEST_STORAGE_FILE, "rb");
            if (file != NULL)
            {
                size_t read_size = fread(result.readback, 1,
                                         sizeof(result.readback) - 1U, file);
                bool read_error = ferror(file) != 0;
                int close_result = fclose(file);
                result.readback[read_size] = '\0';
                result.read_ok = !read_error && close_result == 0 &&
                                 read_size == expected_size;
                result.compare_ok = result.read_ok &&
                                    memcmp(result.readback,
                                           SELF_TEST_STORAGE_TEXT,
                                           expected_size) == 0;
            }
        }

        remove(SELF_TEST_STORAGE_FILE);
        storage_app_access_end();
    }

    bool passed = result.access_ok && result.write_ok &&
                  result.read_ok && result.compare_ok;
    self_test_status_create(passed ? "SD card passed" : "SD card failed",
                            passed ? UI_OK : UI_ERROR);

    self_test_result_row_create(48, "Write", result.write_ok);
    self_test_result_row_create(104, "Read", result.read_ok);
    self_test_result_row_create(160, "Compare", result.compare_ok);

    char total_text[32];
    char free_text[32];
    snprintf(total_text, sizeof(total_text), "%" PRIu64 " MB",
             info.total_bytes / (1024U * 1024U));
    snprintf(free_text, sizeof(free_text), "%" PRIu64 " MB",
             info.free_bytes / (1024U * 1024U));
    const int32_t info_y = 226;
    const int32_t info_step = 38;
    const int32_t written_y = info_y + info_step * 5;
    const int32_t readback_y = info_y + info_step * 6;
    self_test_pair_row_create(s_run_ui.body, 0, info_y, 656,
                              "Type", info.type[0] ? info.type : "--", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, info_y + info_step, 656,
                              "Bus", info.bus[0] ? info.bus : "--", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, info_y + info_step * 2, 656,
                              "Mounted", info.app_mounted ? "Yes" : "No", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, info_y + info_step * 3, 656,
                              "Total", total_text, UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, info_y + info_step * 4, 656,
                              "Free", free_text, UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, written_y, 656,
                              "Written", SELF_TEST_STORAGE_TEXT, UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, readback_y, 656,
                              "Read back",
                              result.readback[0] ? result.readback : "--",
                              result.compare_ok ? UI_OK : UI_ERROR);
}

static void self_test_audio_refresh(void)
{
    int levels[2] = {MIC_DB_MIN, MIC_DB_MIN};
    bool playing = audio_self_test_playback_is_active();
    audio_get_mic_levels(&levels[0], &levels[1]);
    for (int i = 0; i < 2; i++)
    {
        if (s_run_ui.audio_bars[i] != NULL)
        {
            lv_bar_set_value(s_run_ui.audio_bars[i], levels[i], LV_ANIM_OFF);
        }
        if (s_run_ui.audio_labels[i] != NULL)
        {
            lv_label_set_text_fmt(s_run_ui.audio_labels[i], "%d dB", levels[i]);
        }
    }

    if (s_run_ui.audio_play_button_label != NULL)
    {
        lv_label_set_text(s_run_ui.audio_play_button_label,
                          playing ? "Playing" : "Play lilygo.MP3");
    }
    if (playing != s_run_ui.audio_playing && s_run_ui.status != NULL)
    {
        lv_label_set_text(s_run_ui.status,
                          playing ? "Playing lilygo.MP3" : "Playback complete");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(playing ? UI_WARN : UI_OK), 0);
        s_run_ui.audio_playing = playing;
    }
}

static void self_test_audio_play_clicked_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t ret = audio_self_test_play_lilygo();
    if (s_run_ui.status != NULL)
    {
        lv_label_set_text(s_run_ui.status,
                          ret == ESP_OK ? "Starting lilygo.MP3" : "Playback failed");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(ret == ESP_OK ? UI_WARN : UI_ERROR), 0);
    }
}

static void self_test_audio_build(void)
{
    bool ready = (s_init_error & INIT_AUDIO_ERROR) == 0;
    if (ready)
    {
        audio_stop_music();
    }
    self_test_status_create(ready ? "Microphone monitoring" : "Audio unavailable",
                            ready ? UI_OK : UI_ERROR);

    for (int i = 0; i < 2; i++)
    {
        int32_t y = 64 + i * 120;
        lv_obj_t *name = lv_label_create(s_run_ui.body);
        lv_label_set_text_fmt(name, "Mic %d", i + 1);
        lv_obj_set_pos(name, 0, y);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);

        lv_obj_t *bar = lv_bar_create(s_run_ui.body);
        lv_obj_set_size(bar, 500, 32);
        lv_obj_set_pos(bar, 0, y + 48);
        lv_bar_set_range(bar, MIC_DB_MIN, MIC_DB_MAX);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 8, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);
        s_run_ui.audio_bars[i] = bar;

        lv_obj_t *value = lv_label_create(s_run_ui.body);
        lv_label_set_text(value, "-- dB");
        lv_obj_set_size(value, 136, 40);
        lv_obj_set_pos(value, 520, y + 42);
        lv_obj_set_style_text_color(value, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        s_run_ui.audio_labels[i] = value;
    }

    lv_obj_t *speaker_row = lv_obj_create(s_run_ui.body);
    lv_obj_set_size(speaker_row, 656, 60);
    lv_obj_set_pos(speaker_row, 0, 304);
    ui_obj_set_transparent(speaker_row);
    ui_obj_set_flex(speaker_row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *speaker_label = lv_label_create(speaker_row);
    lv_label_set_text(speaker_label, "Speaker");
    lv_obj_set_flex_grow(speaker_label, 1);
    lv_obj_set_style_text_color(speaker_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(speaker_label, SELF_TEST_PAIR_ROW_FONT, 0);

    lv_obj_t *play_button = lv_button_create(speaker_row);
    lv_obj_set_size(play_button, 280, 56);
    lv_obj_set_style_bg_color(play_button, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(play_button, ready ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_border_width(play_button, 0, 0);
    lv_obj_set_style_radius(play_button, 8, 0);
    lv_obj_add_event_cb(play_button, self_test_audio_play_clicked_cb,
                        LV_EVENT_CLICKED, NULL);
    if (!ready)
    {
        lv_obj_add_state(play_button, LV_STATE_DISABLED);
    }

    s_run_ui.audio_play_button_label = lv_label_create(play_button);
    lv_label_set_text(s_run_ui.audio_play_button_label, "Play lilygo.MP3");
    lv_obj_set_style_text_color(s_run_ui.audio_play_button_label,
                                lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_font(s_run_ui.audio_play_button_label,
                               SELF_TEST_PAIR_ROW_FONT, 0);
    lv_obj_center(s_run_ui.audio_play_button_label);
    self_test_audio_refresh();
}

static void self_test_camera_build(void)
{
    bool ready = (s_init_error & INIT_CAMERA_ERROR) == 0;
    self_test_status_create(ready ? "Camera starting" : "Camera unavailable",
                            ready ? UI_WARN : UI_ERROR);

    lv_obj_t *preview = lv_obj_create(s_run_ui.body);
    lv_obj_set_size(preview, SELF_TEST_PREVIEW_WIDTH, SELF_TEST_PREVIEW_HEIGHT);
    lv_obj_set_pos(preview, 8, 100);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 0, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (!ready)
    {
        return;
    }

    lv_obj_update_layout(s_run_ui.root);
    lv_area_t area;
    lv_obj_get_coords(preview, &area);
    camera_preview_set_area(area.x1, area.y1,
                            SELF_TEST_PREVIEW_WIDTH,
                            SELF_TEST_PREVIEW_HEIGHT);
    camera_preview_set_direct_crop(true);
    s_run_ui.camera_started = camera_preview_start(1920, 1080);
    lv_label_set_text(s_run_ui.status,
                      s_run_ui.camera_started ? "Camera running" : "Camera start failed");
    lv_obj_set_style_text_color(s_run_ui.status,
                                lv_color_hex(s_run_ui.camera_started ? UI_OK : UI_ERROR), 0);
}

static void self_test_power_refresh(void)
{
    bmu_info_t info = {0};
    bool ok = bmu_status_info_get(&info);
    if (s_run_ui.status != NULL)
    {
        lv_label_set_text(s_run_ui.status, ok ? "Power ready" : "Power unavailable");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
    }
    if (s_run_ui.power_values[0] != NULL)
    {
        lv_label_set_text_fmt(s_run_ui.power_values[0], "%d%%", info.battery_percent);
        lv_label_set_text_fmt(s_run_ui.power_values[1], "%d mV", info.vbat_mv);
        lv_label_set_text_fmt(s_run_ui.power_values[2], "%d mV", info.vbus_mv);
        lv_label_set_text_fmt(s_run_ui.power_values[3], "%d mV", info.vsys_mv);
        lv_label_set_text_fmt(s_run_ui.power_values[4], "%d mA", info.ibus_ma);
        lv_label_set_text_fmt(s_run_ui.power_values[5], "%d.%d C",
                              info.die_temp_c_x10 / 10,
                              abs(info.die_temp_c_x10 % 10));
    }
}

static void self_test_power_build(void)
{
    self_test_status_create("Reading power", UI_WARN);
    static const char *names[6] = {
        "Battery",
        "VBAT",
        "VBUS",
        "VSYS",
        "IBUS",
        "Temperature",
    };
    for (int i = 0; i < 6; i++)
    {
        const int32_t y = 60 + i * 44;
        s_run_ui.power_values[i] = self_test_pair_row_create(
            s_run_ui.body, 0, y, 656,
            names[i], "--", UI_TEXT);
    }
    self_test_power_refresh();
}

static void self_test_lora_build(void)
{
    bool initialized = (s_init_error & INIT_LORA_ERROR) == 0 &&
                       lora_app_is_started();
    int state = initialized ? lora_app_standby() : ESP_ERR_INVALID_STATE;
    bool communication_ok = state == 0;

    self_test_status_create(communication_ok ? "LoRa communication passed" :
                            initialized ? "LoRa communication failed" :
                                          "LoRa initialization failed",
                            communication_ok ? UI_OK : UI_ERROR);
    self_test_pair_row_create(s_run_ui.body, 0, 60, 656,
                              "Initialization",
                              initialized ? "Passed" : "Failed",
                              initialized ? UI_OK : UI_ERROR);
    self_test_pair_row_create(s_run_ui.body, 0, 104, 656,
                              "SPI communication",
                              communication_ok ? "Passed" : "Failed",
                              communication_ok ? UI_OK : UI_ERROR);
    self_test_pair_row_create(s_run_ui.body, 0, 148, 656,
                              "Chip",
                              initialized ? lora_page_chip_name(lora_app_get_chip()) : "--",
                              initialized ? UI_TEXT : UI_MUTED);
    self_test_pair_row_create(s_run_ui.body, 0, 192, 656,
                              "Mode",
                              communication_ok ? "Standby" : "--",
                              communication_ok ? UI_TEXT : UI_MUTED);
}

static void self_test_c5_probe_task(void *arg)
{
    (void)arg;
    uint32_t chip_id = 0;
    char target[sizeof(s_c5_probe_target)] = {0};
    bool ok = esp32c5_sdio_slave_probe(&chip_id, target, sizeof(target));

    portENTER_CRITICAL(&s_c5_probe_lock);
    s_c5_probe_chip_id = chip_id;
    memcpy(s_c5_probe_target, target, sizeof(s_c5_probe_target));
    s_c5_probe_ok = ok;
    s_c5_probe_done = true;
    s_c5_probe_running = false;
    portEXIT_CRITICAL(&s_c5_probe_lock);
    vTaskDelete(NULL);
}

static void self_test_c5_refresh(void)
{
    bool init_failed = (s_init_error & INIT_SLAVE_ERROR) != 0;
    bool enabled = esp32c5_sdio_slave_is_enabled();
    bool ready = esp32c5_sdio_slave_is_ready() &&
                 !esp32c5_sdio_slave_is_sleeping();
    bool start_probe = false;

    portENTER_CRITICAL(&s_c5_probe_lock);
    if (!init_failed && ready && !s_c5_probe_attempted && !s_c5_probe_running)
    {
        s_c5_probe_attempted = true;
        s_c5_probe_running = true;
        s_c5_probe_done = false;
        start_probe = true;
    }
    portEXIT_CRITICAL(&s_c5_probe_lock);

    if (start_probe &&
        xTaskCreate(self_test_c5_probe_task, "c5_self_test", 4096,
                    NULL, 3, NULL) != pdPASS)
    {
        portENTER_CRITICAL(&s_c5_probe_lock);
        s_c5_probe_running = false;
        s_c5_probe_ok = false;
        s_c5_probe_done = true;
        portEXIT_CRITICAL(&s_c5_probe_lock);
    }

    bool probe_running;
    bool probe_done;
    bool probe_ok;
    uint32_t probe_chip_id;
    char probe_target[sizeof(s_c5_probe_target)];
    portENTER_CRITICAL(&s_c5_probe_lock);
    probe_running = s_c5_probe_running;
    probe_done = s_c5_probe_done;
    probe_ok = s_c5_probe_ok;
    probe_chip_id = s_c5_probe_chip_id;
    memcpy(probe_target, s_c5_probe_target, sizeof(probe_target));
    portEXIT_CRITICAL(&s_c5_probe_lock);

    if (s_run_ui.status == NULL || s_run_ui.c5_values[0] == NULL ||
        s_run_ui.c5_values[1] == NULL || s_run_ui.c5_values[2] == NULL ||
        s_run_ui.c5_values[3] == NULL || s_run_ui.c5_values[4] == NULL)
    {
        return;
    }

    const char *init_text = init_failed ? "Failed" : ready ? "Passed" : "Pending";
    uint32_t init_color = init_failed ? UI_ERROR : ready ? UI_OK : UI_WARN;
    lv_label_set_text(s_run_ui.c5_values[0], init_text);
    lv_obj_set_style_text_color(s_run_ui.c5_values[0], lv_color_hex(init_color), 0);
    lv_label_set_text(s_run_ui.c5_values[1], enabled ? "Yes" : "No");
    lv_obj_set_style_text_color(s_run_ui.c5_values[1],
                                lv_color_hex(enabled ? UI_OK : UI_ERROR), 0);

    if (probe_running)
    {
        lv_label_set_text(s_run_ui.status, "Checking ESP32-C5 communication");
        lv_obj_set_style_text_color(s_run_ui.status, lv_color_hex(UI_WARN), 0);
        return;
    }
    if (!probe_done)
    {
        lv_label_set_text(s_run_ui.c5_values[2],
                          init_failed ? "Not run" : "Pending");
        lv_obj_set_style_text_color(s_run_ui.c5_values[2],
                                    lv_color_hex(init_failed ? UI_MUTED : UI_WARN), 0);
        lv_label_set_text(s_run_ui.status,
                          init_failed ? "ESP32-C5 initialization failed" :
                          ready ? "Waiting for communication test" : "ESP32-C5 initializing");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(init_failed ? UI_ERROR : UI_WARN), 0);
        return;
    }

    lv_label_set_text(s_run_ui.status,
                       probe_ok ? "ESP32-C5 communication passed" :
                                  "ESP32-C5 communication failed");
    lv_obj_set_style_text_color(s_run_ui.status,
                                 lv_color_hex(probe_ok ? UI_OK : UI_ERROR), 0);
    lv_label_set_text(s_run_ui.c5_values[2], probe_ok ? "Passed" : "Failed");
    lv_obj_set_style_text_color(s_run_ui.c5_values[2],
                                 lv_color_hex(probe_ok ? UI_OK : UI_ERROR), 0);
    lv_label_set_text(s_run_ui.c5_values[3],
                       probe_ok && probe_target[0] ? probe_target : "--");
    if (probe_ok)
    {
        lv_label_set_text_fmt(s_run_ui.c5_values[4], "0x%02" PRIX32,
                               probe_chip_id);
    }
}

static void self_test_c5_build(void)
{
    self_test_status_create("ESP32-C5 initializing", UI_WARN);
    s_run_ui.c5_values[0] = self_test_pair_row_create(
        s_run_ui.body, 0, 60, 656, "Initialization", "Pending", UI_WARN);
    s_run_ui.c5_values[1] = self_test_pair_row_create(
        s_run_ui.body, 0, 104, 656, "Enabled", "--", UI_MUTED);
    s_run_ui.c5_values[2] = self_test_pair_row_create(
        s_run_ui.body, 0, 148, 656, "RPC communication", "Pending", UI_WARN);
    s_run_ui.c5_values[3] = self_test_pair_row_create(
        s_run_ui.body, 0, 192, 656, "Target", "--", UI_MUTED);
    s_run_ui.c5_values[4] = self_test_pair_row_create(
        s_run_ui.body, 0, 236, 656, "Chip ID", "--", UI_MUTED);

    portENTER_CRITICAL(&s_c5_probe_lock);
    s_c5_probe_attempted = s_c5_probe_running;
    if (!s_c5_probe_running)
    {
        s_c5_probe_done = false;
        s_c5_probe_ok = false;
        s_c5_probe_chip_id = 0;
        s_c5_probe_target[0] = '\0';
    }
    portEXIT_CRITICAL(&s_c5_probe_lock);
    self_test_c5_refresh();
}

static void self_test_run_dynamic_reset(void)
{
    s_run_ui.status = NULL;
    s_run_ui.display_screen = NULL;
    s_run_ui.display_color_label = NULL;
    s_run_ui.display_step_label = NULL;
    s_run_ui.display_next_button = NULL;
    for (uint8_t i = 0; i < TOUCH_PANEL_MAX_POINTS; i++)
    {
        s_run_ui.touch_dots[i] = NULL;
    }
    s_run_ui.finger_count_label = NULL;
    s_run_ui.touch_position_label = NULL;
    s_run_ui.display_color_index = 0;
    s_run_ui.audio_bars[0] = NULL;
    s_run_ui.audio_bars[1] = NULL;
    s_run_ui.audio_labels[0] = NULL;
    s_run_ui.audio_labels[1] = NULL;
    s_run_ui.audio_play_button_label = NULL;
    for (int i = 0; i < 6; i++)
    {
        s_run_ui.power_values[i] = NULL;
    }
    for (int i = 0; i < 5; i++)
    {
        s_run_ui.c5_values[i] = NULL;
    }
    s_run_ui.refresh_ticks = 0;
    s_run_ui.camera_started = false;
    s_run_ui.audio_playing = false;
}

static void self_test_run_page_create(Page *page)
{
    s_run_ui.root = page->root;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);
    s_run_ui.body = lv_obj_create(page->root);
    lv_obj_set_size(s_run_ui.body, 656, DISPLAY_PANEL_V_RES - 180);
    lv_obj_remove_flag(s_run_ui.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_run_ui.body, 32, 160);
    ui_obj_set_transparent(s_run_ui.body);

    s_run_ui.title = self_test_header_create(page->root, "Test",
                                             self_test_back_to_list_cb);
}

static void self_test_run_page_enter(Page *page)
{
    (void)page;
    const self_test_item_t *item = &s_self_test_items[s_active_test];
    lv_label_set_text_fmt(s_run_ui.title, "%s Test", item->name);
    lv_obj_set_style_text_color(s_run_ui.title, lv_color_hex(UI_TEXT), 0);
    lv_obj_clean(s_run_ui.body);
    lv_obj_set_size(s_run_ui.body, 656, DISPLAY_PANEL_V_RES - 180);
    lv_obj_remove_flag(s_run_ui.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_run_ui.body, 32, 160);
    self_test_run_dynamic_reset();

    switch (s_active_test)
    {
    case SELF_TEST_DISPLAY:
        self_test_display_build();
        break;
    case SELF_TEST_TOUCH:
        self_test_touch_build();
        break;
    case SELF_TEST_STORAGE:
        self_test_storage_build();
        break;
    case SELF_TEST_AUDIO:
        self_test_audio_build();
        break;
    case SELF_TEST_CAMERA:
        self_test_camera_build();
        break;
    case SELF_TEST_POWER:
        self_test_power_build();
        break;
    case SELF_TEST_LORA:
        self_test_lora_build();
        break;
    case SELF_TEST_ESP32C5:
        self_test_c5_build();
        break;
    }
}

static void self_test_run_page_leave(Page *page)
{
    (void)page;
    if (s_active_test == SELF_TEST_DISPLAY || s_active_test == SELF_TEST_TOUCH)
    {
        lv_obj_t *status_bar = status_bar_get_container();
        if (status_bar != NULL)
        {
            lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_active_test == SELF_TEST_CAMERA && s_run_ui.camera_started)
    {
        camera_preview_stop();
        s_run_ui.camera_started = false;
    }
    if (s_active_test == SELF_TEST_AUDIO)
    {
        audio_self_test_stop();
    }
}

static void self_test_run_page_timer(Page *page)
{
    (void)page;
    if (s_active_test == SELF_TEST_TOUCH)
    {
        self_test_touch_refresh();
    }
    else if (s_active_test == SELF_TEST_AUDIO && ++s_run_ui.refresh_ticks >= 3)
    {
        s_run_ui.refresh_ticks = 0;
        self_test_audio_refresh();
    }
    else if (s_active_test == SELF_TEST_POWER && ++s_run_ui.refresh_ticks >= 33)
    {
        s_run_ui.refresh_ticks = 0;
        self_test_power_refresh();
    }
    else if (s_active_test == SELF_TEST_ESP32C5)
    {
        self_test_c5_refresh();
    }
}

static void self_test_run_page_gesture(Page *page, GestureDirection direction)
{
    (void)page;
    if (s_active_test == SELF_TEST_TOUCH)
    {
        return;
    }
    if (direction == GESTURE_RIGHT)
    {
        ui_page_switch_async(PAGE_SELF_TEST);
    }
}

void self_test_page_register(void)
{
    Page list_page = {
        .id = PAGE_SELF_TEST,
        .name = "self_test",
        .on_create = self_test_page_create,
        .on_gesture = app_page_back_gesture,
    };
    ui_page_register(&list_page);

    Page run_page = {
        .id = PAGE_SELF_TEST_RUN,
        .name = "self_test_run",
        .on_create = self_test_run_page_create,
        .on_enter = self_test_run_page_enter,
        .on_leave = self_test_run_page_leave,
        .on_gesture = self_test_run_page_gesture,
        .page_timer = self_test_run_page_timer,
        .timer_interval_ms = 30,
    };
    ui_page_register(&run_page);
}
