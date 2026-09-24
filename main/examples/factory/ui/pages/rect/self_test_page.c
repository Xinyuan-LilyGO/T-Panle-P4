#include "ui_internal.h"
#include "self_test_page.h"
#include "lcd.h"

#define SELF_TEST_ITEM_COUNT 8
#define SELF_TEST_PREVIEW_WIDTH 640
#define SELF_TEST_PREVIEW_HEIGHT 320
#define SELF_TEST_DISPLAY_COLOR_COUNT 6
#define SELF_TEST_STORAGE_FILE FILE_SCAN_DIR "/.t_panel_self_test.tmp"
#define SELF_TEST_STORAGE_TEXT "T-Panel-P4 SD read/write test"

#define SELF_TEST_ITEM_HEIGHT     112
#define SELF_TEST_ITEM_ICON_WIDTH 48
#define SELF_TEST_ITEM_BUTTON_W   104
#define SELF_TEST_ITEM_BUTTON_H   52
#define SELF_TEST_ITEM_LIST_GAP   16
#define SELF_TEST_ITEM_ICON_FONT  (&lv_font_montserrat_28)
#define SELF_TEST_ITEM_TEXT_FONT  (&lv_font_SourceHanSansCN_Bold_2_24)
#define SELF_TEST_PAIR_ROW_HEIGHT 44
#define SELF_TEST_PAIR_ROW_FONT   (&lv_font_SourceHanSansCN_Bold_2_24)

typedef enum
{
    SELF_TEST_DISPLAY = 0,
    SELF_TEST_TOUCH,
    SELF_TEST_STORAGE,
    SELF_TEST_AUDIO,
    SELF_TEST_CAMERA,
    SELF_TEST_POWER,
    SELF_TEST_SENSOR,
    SELF_TEST_MOTOR,
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
    lv_obj_t *sensor_values[7];
    uint8_t display_color_index;
    uint32_t refresh_ticks;
    bool camera_started;
    bool motor_running;
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
    {.name = "Sensor", .symbol = LV_SYMBOL_LOOP, .kind = SELF_TEST_SENSOR},
    {.name = "Motor", .symbol = LV_SYMBOL_PLAY, .kind = SELF_TEST_MOTOR},
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
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_left(row, 24, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_pad_column(row, 20, 0);
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

    lv_obj_t *run_button = lv_button_create(row);
    lv_obj_set_size(run_button,
                    SELF_TEST_ITEM_BUTTON_W, SELF_TEST_ITEM_BUTTON_H);
    lv_obj_set_style_bg_color(run_button, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(run_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(run_button, 0, 0);
    lv_obj_set_style_radius(run_button, 8, 0);
    lv_obj_set_style_pad_all(run_button, 0, 0);
    lv_obj_add_event_cb(run_button, self_test_run_clicked_cb,
                        LV_EVENT_CLICKED, (void *)item);

    lv_obj_t *run_label = lv_label_create(run_button);
    lv_label_set_text(run_label, "Run");
    lv_obj_set_style_text_color(run_label, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_font(run_label, SELF_TEST_ITEM_TEXT_FONT, 0);
    lv_obj_center(run_label);
}

static void self_test_page_create(Page *page)
{
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);

    self_test_header_create(page->root, "Self Test", self_test_back_to_home_cb);

    lv_obj_t *list = lv_obj_create(page->root);
    lv_obj_set_size(list, 656, 1120);
    lv_obj_set_pos(list, 32, 160);
    ui_obj_set_transparent(list);
    lv_obj_set_style_pad_row(list, SELF_TEST_ITEM_LIST_GAP, 0);
    ui_obj_set_flex(list, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

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
    lv_obj_set_size(row, 656, 76);
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
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);

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

    self_test_result_row_create(84, "Write", result.write_ok);
    self_test_result_row_create(176, "Read", result.read_ok);
    self_test_result_row_create(268, "Compare", result.compare_ok);

    char total_text[32];
    char free_text[32];
    snprintf(total_text, sizeof(total_text), "%" PRIu64 " MB",
             info.total_bytes / (1024U * 1024U));
    snprintf(free_text, sizeof(free_text), "%" PRIu64 " MB",
             info.free_bytes / (1024U * 1024U));
    const int32_t info_y = 390;
    const int32_t info_step = 52;
    const int32_t written_y = 670;
    const int32_t readback_y = 722;
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
        int32_t y = 140 + i * 180;
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
    lv_obj_set_size(speaker_row, 656, 68);
    lv_obj_set_pos(speaker_row, 0, 520);
    ui_obj_set_transparent(speaker_row);
    ui_obj_set_flex(speaker_row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *speaker_label = lv_label_create(speaker_row);
    lv_label_set_text(speaker_label, "Speaker");
    lv_obj_set_flex_grow(speaker_label, 1);
    lv_obj_set_style_text_color(speaker_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(speaker_label, SELF_TEST_PAIR_ROW_FONT, 0);

    lv_obj_t *play_button = lv_button_create(speaker_row);
    lv_obj_set_size(play_button, 280, 64);
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
        const int32_t y = 100 + i * 58;
        s_run_ui.power_values[i] = self_test_pair_row_create(
            s_run_ui.body, 0, y, 656,
            names[i], "--", UI_TEXT);
    }
    self_test_power_refresh();
}

static void self_test_sensor_refresh(void)
{
    qmi8658c_data_t data = {0};
    esp_err_t ret = motion_sensor_read(&data);
    bool ok = ret == ESP_OK;
    if (s_run_ui.status != NULL)
    {
        lv_label_set_text(s_run_ui.status,
                          ok ? "Sensor running" : "Sensor read failed");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
    }
    if (!ok || s_run_ui.sensor_values[0] == NULL)
    {
        return;
    }

    lv_label_set_text_fmt(s_run_ui.sensor_values[0], "%.3f g", data.acc.x);
    lv_label_set_text_fmt(s_run_ui.sensor_values[1], "%.3f g", data.acc.y);
    lv_label_set_text_fmt(s_run_ui.sensor_values[2], "%.3f g", data.acc.z);
    lv_label_set_text_fmt(s_run_ui.sensor_values[3], "%.2f dps", data.gyro.x);
    lv_label_set_text_fmt(s_run_ui.sensor_values[4], "%.2f dps", data.gyro.y);
    lv_label_set_text_fmt(s_run_ui.sensor_values[5], "%.2f dps", data.gyro.z);
    lv_label_set_text_fmt(s_run_ui.sensor_values[6], "%.2f C", data.temperature);
}

static void self_test_sensor_build(void)
{
    bool ready = motion_sensor_is_ready();
    self_test_status_create(ready ? "Sensor starting" : "Sensor unavailable",
                            ready ? UI_WARN : UI_ERROR);

    uint8_t address = 0;
    uint8_t revision = 0;
    motion_sensor_device_info_get(&address, &revision);
    char address_text[16];
    char revision_text[16];
    snprintf(address_text, sizeof(address_text), "0x%02X", address);
    snprintf(revision_text, sizeof(revision_text), "0x%02X", revision);
    self_test_pair_row_create(s_run_ui.body, 0, 80, 656,
                              "Device", "QMI8658C", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, 132, 656,
                              "Address", ready ? address_text : "--", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, 184, 656,
                              "Revision", ready ? revision_text : "--", UI_TEXT);

    static const char *names[7] = {
        "Accel X",
        "Accel Y",
        "Accel Z",
        "Gyro X",
        "Gyro Y",
        "Gyro Z",
        "Temperature",
    };
    for (int i = 0; i < 7; i++)
    {
        s_run_ui.sensor_values[i] = self_test_pair_row_create(
            s_run_ui.body, 0, 260 + i * 58, 656,
            names[i], "--", UI_TEXT);
    }
    self_test_sensor_refresh();
}

static void self_test_motor_start(void)
{
    esp_err_t ret = haptic_motor_play_test();
    s_run_ui.motor_running = ret == ESP_OK;
    if (s_run_ui.status != NULL)
    {
        lv_label_set_text(s_run_ui.status,
                          ret == ESP_OK ? "Motor vibrating" : "Motor start failed");
        lv_obj_set_style_text_color(s_run_ui.status,
                                    lv_color_hex(ret == ESP_OK ? UI_WARN : UI_ERROR), 0);
    }
}

static void self_test_motor_clicked_cb(lv_event_t *event)
{
    (void)event;
    self_test_motor_start();
}

static void self_test_motor_build(void)
{
    bool ready = haptic_motor_is_ready();
    self_test_status_create(ready ? "Motor ready" : "Motor unavailable",
                            ready ? UI_OK : UI_ERROR);
    self_test_pair_row_create(s_run_ui.body, 0, 100, 656,
                              "Driver", "DRV2605", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, 158, 656,
                              "Address", "0x5A", UI_TEXT);
    self_test_pair_row_create(s_run_ui.body, 0, 216, 656,
                              "Effect", "Buzz 1", UI_TEXT);

    lv_obj_t *button = lv_button_create(s_run_ui.body);
    lv_obj_set_size(button, 220, 68);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(button, ready ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_add_event_cb(button, self_test_motor_clicked_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, "Vibrate");
    lv_obj_set_style_text_color(label, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_text_font(label,
                               &lv_font_SourceHanSansCN_Bold_2_24, 0);
    lv_obj_center(label);

    if (ready)
    {
        self_test_motor_start();
    }
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
    for (int i = 0; i < 7; i++)
    {
        s_run_ui.sensor_values[i] = NULL;
    }
    s_run_ui.refresh_ticks = 0;
    s_run_ui.camera_started = false;
    s_run_ui.motor_running = false;
    s_run_ui.audio_playing = false;
}

static void self_test_run_page_create(Page *page)
{
    s_run_ui.root = page->root;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);
    s_run_ui.body = lv_obj_create(page->root);
    lv_obj_set_size(s_run_ui.body, 656, 1120);
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
    lv_obj_set_size(s_run_ui.body, 656, 1120);
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
    case SELF_TEST_SENSOR:
        self_test_sensor_build();
        break;
    case SELF_TEST_MOTOR:
        self_test_motor_build();
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
    if (s_active_test == SELF_TEST_MOTOR && haptic_motor_is_ready())
    {
        haptic_motor_stop();
        s_run_ui.motor_running = false;
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
    else if (s_active_test == SELF_TEST_SENSOR && ++s_run_ui.refresh_ticks >= 3)
    {
        s_run_ui.refresh_ticks = 0;
        self_test_sensor_refresh();
    }
    else if (s_active_test == SELF_TEST_MOTOR && s_run_ui.motor_running)
    {
        bool playing = false;
        esp_err_t ret = haptic_motor_is_playing(&playing);
        if (ret != ESP_OK || !playing)
        {
            s_run_ui.motor_running = false;
            lv_label_set_text(s_run_ui.status,
                              ret == ESP_OK ? "Motor complete" : "Motor read failed");
            lv_obj_set_style_text_color(s_run_ui.status,
                                        lv_color_hex(ret == ESP_OK ? UI_OK : UI_ERROR), 0);
        }
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
