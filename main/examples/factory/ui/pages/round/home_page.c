#include <time.h>

#include "home_page.h"
#include "lvgl_page_manager.h"
#include "status_bar.h"
#include "ui.h"
#include "ui_widgets.h"
#include "image/app_icons.h"

#define ROUND_CARD_COUNT 5
#define ROUND_CARD_CENTER_SLOT 2
#define ROUND_CARD_STEP_FP 256
#define ROUND_CARD_SPACING 176
#define ROUND_CARD_CENTER_W 232
#define ROUND_CARD_CENTER_H 212
#define ROUND_CARD_SIDE_W 174
#define ROUND_CARD_SIDE_H 158
#define ROUND_CARD_OUTER_W 130
#define ROUND_CARD_OUTER_H 118
#define ROUND_SWIPE_THRESHOLD_FP 96
#define ROUND_APP_COUNT 7
#define ROUND_PAGE_DOTS_Y 369
#define ROUND_DESCRIPTION_Y 393
#define HOME_LOGO_ANIM_DURATION_MS 700
#define HOME_LOGO_START_Y 28
#define HOME_STATUS_ANIM_DURATION_MS 1200
#define HOME_STATUS_ANIM_DELAY_MS 250
#define HOME_STATUS_START_Y (-40)

void home_page_set_mic_levels(int mic0_db, int mic1_db)
{
    (void)mic0_db;
    (void)mic1_db;
}
#define ROUND_SWIPE_VELOCITY_THRESHOLD 12
#define ROUND_MULTI_SWIPE_VELOCITY 32
#define ROUND_MULTI_SWIPE_FAST_VELOCITY 64
#define ROUND_NO_CARD_SLOT ROUND_CARD_COUNT

typedef struct
{
    lv_obj_t *obj;
    lv_obj_t *icon;
    lv_obj_t *title;
    int relative_slot;
} round_app_card_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;
    lv_obj_t *date_label;
    lv_obj_t *section_title;
    lv_obj_t *description;
    lv_obj_t *carousel;
    lv_obj_t *logo_label;
    round_app_card_t cards[ROUND_CARD_COUNT];
    lv_obj_t *page_dots[ROUND_APP_COUNT];
    int selected;
    int32_t position_fp;
    int32_t press_x;
    int32_t press_y;
    int32_t drag_delta_x;
    int32_t last_vect_x;
    int snap_direction;
    int pending_steps;
    int pressed_slot;
    int foreground_card;
} home_round_ui_t;

static home_round_ui_t s_round_ui;
static lv_obj_t *s_home_status_bar;
static bool s_home_enter_animated;

static const lv_image_dsc_t *const s_app_menu_icons[ROUND_APP_COUNT] = {
    &_music_RGB565A8_68x68,
    &_record_RGB565A8_68x68,
    &_camera_RGB565A8_68x68,
    &_lora_RGB565A8_68x68,
    &_files_RGB565A8_68x68,
    &_set_RGB565A8_68x68,
    &_list_RGB565A8_68x68,
};

static const char *const s_app_menu_titles[ROUND_APP_COUNT] = {
    "Music",
    "Record",
    "Camera",
    "Lora",
    "File",
    "Set",
    "Self Test",
};

const char *const s_app_menu_descs[HOME_APP_MENU_COUNT] = {
    "Codec: ES8389\n\nFormat: MP3/WAV/AAC/LRC\n\nOutput: Speaker/I2S",
    "Input: Dual Microphone\n\nFormat: Stereo WAV\n\nStorage: SD / RECORD",
    "Sensor: OV2710\n\nResolution: 1080P/720P\n\nFPS: 25fps\n\nSave: SD Card",
    "Module: Auto Detect\n\nBand: Sub-GHz/2.4GHz\n\nMode: TX/RX",
    "Storage: SD Card\n\nFormat: TXT/BMP/JPEG/PNG\n\nBrowser: Local Files",
    "Panel: Brightness/Timeout\n\nWiFi: Scan/Connect\n\nUSB: MSC/App",
    "Display: RGB/Touch\n\nHardware: Audio/Camera\n\nSystem: Power/Radio",
};

static int round_wrap_index(int index)
{
    while (index < 0)
    {
        index += ROUND_APP_COUNT;
    }
    while (index >= ROUND_APP_COUNT)
    {
        index -= ROUND_APP_COUNT;
    }
    return index;
}

static int32_t round_lerp(int32_t from, int32_t to, int32_t amount)
{
    return from + ((to - from) * amount) / ROUND_CARD_STEP_FP;
}

static void round_page_dots_update(void);

static void round_card_content_update(round_app_card_t *card)
{
    int app_index = round_wrap_index(s_round_ui.selected + card->relative_slot);
    if (card->icon != NULL)
    {
        lv_image_set_src(card->icon, s_app_menu_icons[app_index]);
    }
    if (card->title != NULL)
    {
        lv_label_set_text(card->title, s_app_menu_titles[app_index]);
    }
}

static round_app_card_t round_card_create(lv_obj_t *parent, int relative_slot)
{
    round_app_card_t card = {
        .relative_slot = relative_slot,
    };

    card.obj = lv_obj_create(parent);
    lv_obj_set_style_radius(card.obj, 20, 0);
    lv_obj_set_style_bg_color(card.obj, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_color(card.obj, lv_color_hex(UI_PANEL_HL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card.obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card.obj, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_color(card.obj, lv_color_hex(UI_PRIMARY), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(card.obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card.obj, 2, 0);
    lv_obj_set_style_shadow_color(card.obj, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_shadow_color(card.obj, lv_color_hex(UI_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(card.obj, 34, 0);
    lv_obj_set_style_shadow_spread(card.obj, 5, 0);
    lv_obj_set_style_pad_all(card.obj, 0, 0);
    lv_obj_remove_flag(card.obj, LV_OBJ_FLAG_SCROLLABLE);

    card.icon = lv_image_create(card.obj);
    lv_image_set_src(card.icon, s_app_menu_icons[round_wrap_index(relative_slot)]);
    lv_obj_align(card.icon, LV_ALIGN_CENTER, 0, -18);
    
    return card;
}

static void round_card_layout_apply(round_app_card_t *card, int32_t relative_fp)
{
    int32_t distance = LV_ABS(relative_fp);
    int32_t width;
    int32_t height;
    int32_t y;
    int32_t opacity;

    if (distance <= ROUND_CARD_STEP_FP)
    {
        width = round_lerp(ROUND_CARD_CENTER_W, ROUND_CARD_SIDE_W, distance);
        height = round_lerp(ROUND_CARD_CENTER_H, ROUND_CARD_SIDE_H, distance);
        y = round_lerp(23, 50, distance);
        opacity = round_lerp(LV_OPA_COVER, 90, distance);
    }
    else
    {
        int32_t outer = LV_MIN(distance - ROUND_CARD_STEP_FP, ROUND_CARD_STEP_FP);
        width = round_lerp(ROUND_CARD_SIDE_W, ROUND_CARD_OUTER_W, outer);
        height = round_lerp(ROUND_CARD_SIDE_H, ROUND_CARD_OUTER_H, outer);
        y = round_lerp(50, 70, outer);
        opacity = round_lerp(90, 18, outer);
    }

    int32_t center_x = 300 + (relative_fp * ROUND_CARD_SPACING) / ROUND_CARD_STEP_FP;
    lv_obj_set_pos(card->obj, center_x - width / 2, y);
    lv_obj_set_size(card->obj, width, height);
    lv_obj_set_style_opa(card->obj, opacity, 0);
    lv_opa_t border_opa = (lv_opa_t)round_lerp(LV_OPA_COVER, LV_OPA_20,
                                               LV_MIN(distance, ROUND_CARD_STEP_FP));
    lv_obj_set_style_border_opa(card->obj, border_opa, 0);
    lv_opa_t shadow_opa = (lv_opa_t)round_lerp(LV_OPA_30, LV_OPA_TRANSP,
                                               LV_MIN(distance, ROUND_CARD_STEP_FP));
    lv_obj_set_style_shadow_opa(card->obj, shadow_opa, 0);
}

static void round_carousel_layout_update(void)
{
    int32_t distances[ROUND_CARD_COUNT];
    int closest = 0;

    for (int i = 0; i < ROUND_CARD_COUNT; i++)
    {
        int32_t relative_fp = s_round_ui.cards[i].relative_slot * ROUND_CARD_STEP_FP +
                              s_round_ui.position_fp;
        distances[i] = LV_ABS(relative_fp);
        round_card_layout_apply(&s_round_ui.cards[i], relative_fp);
        if (distances[i] < distances[closest])
        {
            closest = i;
        }
    }

    if (closest != s_round_ui.foreground_card)
    {
        lv_obj_move_foreground(s_round_ui.cards[closest].obj);
        s_round_ui.foreground_card = closest;
    }

    int32_t transition = LV_MIN(LV_ABS(s_round_ui.position_fp), ROUND_CARD_STEP_FP);
    lv_opa_t metadata_opa = (lv_opa_t)round_lerp(LV_OPA_COVER, LV_OPA_30, transition);
    lv_obj_set_style_text_opa(s_round_ui.section_title, metadata_opa, 0);
    lv_obj_set_style_text_opa(s_round_ui.description, metadata_opa, 0);
    round_page_dots_update();
}

static void round_page_dots_update(void)
{
    const int inactive_width = 10;
    const int active_width = 28;
    const int gap = 8;
    const int total_width = (ROUND_APP_COUNT - 1) * inactive_width + active_width +
                            (ROUND_APP_COUNT - 1) * gap;
    int x = (UI_ROUND_MAIN_CONT_W - total_width) / 2;
    int32_t progress = LV_MIN(LV_ABS(s_round_ui.position_fp), ROUND_CARD_STEP_FP);
    int target = s_round_ui.selected;
    if (s_round_ui.position_fp < 0)
    {
        target = round_wrap_index(s_round_ui.selected + 1);
    }
    else if (s_round_ui.position_fp > 0)
    {
        target = round_wrap_index(s_round_ui.selected - 1);
    }

    for (int i = 0; i < ROUND_APP_COUNT; i++)
    {
        int32_t active_amount = 0;
        if (i == s_round_ui.selected)
        {
            active_amount = ROUND_CARD_STEP_FP - progress;
        }
        if (progress > 0 && i == target)
        {
            active_amount = progress;
        }

        int width = inactive_width +
                    ((active_width - inactive_width) * active_amount) / ROUND_CARD_STEP_FP;
        uint8_t color_mix = (uint8_t)((active_amount * LV_OPA_COVER) / ROUND_CARD_STEP_FP);
        lv_opa_t opacity = (lv_opa_t)round_lerp(LV_OPA_50, LV_OPA_COVER, active_amount);
        lv_obj_set_pos(s_round_ui.page_dots[i], x, ROUND_PAGE_DOTS_Y);
        lv_obj_set_size(s_round_ui.page_dots[i], width, 4);
        lv_obj_set_style_bg_color(s_round_ui.page_dots[i],
                                  lv_color_mix(lv_color_hex(UI_WARN),
                                               lv_color_hex(UI_LINE), color_mix), 0);
        lv_obj_set_style_bg_opa(s_round_ui.page_dots[i], opacity, 0);
        x += width + gap;
    }
}

static void round_selected_content_update(void)
{
    lv_label_set_text(s_round_ui.section_title, s_app_menu_titles[s_round_ui.selected]);
    lv_label_set_text(s_round_ui.description, s_app_menu_descs[s_round_ui.selected]);
    for (int i = 0; i < ROUND_CARD_COUNT; i++)
    {
        round_card_content_update(&s_round_ui.cards[i]);
    }
    round_page_dots_update();
}

static void round_carousel_drag_normalize(void)
{
    bool selection_changed = false;
    while (s_round_ui.position_fp <= -ROUND_CARD_STEP_FP)
    {
        s_round_ui.position_fp += ROUND_CARD_STEP_FP;
        s_round_ui.selected = round_wrap_index(s_round_ui.selected + 1);
        selection_changed = true;
    }
    while (s_round_ui.position_fp >= ROUND_CARD_STEP_FP)
    {
        s_round_ui.position_fp -= ROUND_CARD_STEP_FP;
        s_round_ui.selected = round_wrap_index(s_round_ui.selected - 1);
        selection_changed = true;
    }

    if (selection_changed)
    {
        round_selected_content_update();
    }
}

static void round_carousel_anim_cb(void *var, int32_t value)
{
    (void)var;
    s_round_ui.position_fp = value;
    round_carousel_layout_update();
}

static void round_carousel_snap_step(int direction);

static void round_carousel_settle(void)
{
    int completed_direction = s_round_ui.snap_direction;
    if (completed_direction < 0)
    {
        s_round_ui.selected = round_wrap_index(s_round_ui.selected + 1);
    }
    else if (completed_direction > 0)
    {
        s_round_ui.selected = round_wrap_index(s_round_ui.selected - 1);
    }

    s_round_ui.position_fp = 0;
    s_round_ui.snap_direction = 0;
    if (completed_direction != 0 && s_round_ui.pending_steps > 0)
    {
        s_round_ui.pending_steps--;
    }
    round_selected_content_update();
    round_carousel_layout_update();

    if (s_round_ui.pending_steps > 0)
    {
        round_carousel_snap_step(completed_direction);
    }
}

static void round_carousel_anim_completed(lv_anim_t *anim)
{
    (void)anim;
    round_carousel_settle();
}

static void round_carousel_snap_step(int direction)
{
    int32_t target = direction < 0 ? -ROUND_CARD_STEP_FP :
                     direction > 0 ? ROUND_CARD_STEP_FP : 0;
    int32_t remaining = LV_ABS(target - s_round_ui.position_fp);
    uint32_t duration;
    lv_anim_path_cb_t path;
    if (direction == 0)
    {
        duration = 90 + (remaining * 70U) / ROUND_CARD_STEP_FP;
        path = lv_anim_path_ease_out;
    }
    else if (s_round_ui.pending_steps > 1)
    {
        /* Pass intermediate cards quickly; only the destination card settles. */
        duration = 80;
        path = lv_anim_path_linear;
    }
    else
    {
        duration = 80 + (remaining * 80U) / ROUND_CARD_STEP_FP;
        path = lv_anim_path_ease_out;
    }

    s_round_ui.snap_direction = direction;
    if (remaining <= 2)
    {
        round_carousel_settle();
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_round_ui.carousel);
    lv_anim_set_values(&anim, s_round_ui.position_fp, target);
    lv_anim_set_time(&anim, duration);
    lv_anim_set_path_cb(&anim, path);
    lv_anim_set_exec_cb(&anim, round_carousel_anim_cb);
    lv_anim_set_completed_cb(&anim, round_carousel_anim_completed);
    lv_anim_start(&anim);
}

static void round_carousel_snap(int direction, int steps)
{
    s_round_ui.pending_steps = direction == 0 ? 0 : LV_CLAMP(1, steps, 3);
    round_carousel_snap_step(direction);
}

static void round_app_page_open(int app_index)
{
    static const PageType app_pages[ROUND_APP_COUNT] = {
        PAGE_MUSIC,
        PAGE_RECORD,
        PAGE_CAMERA,
        PAGE_LORA,
        PAGE_FILE,
        PAGE_SET,
        PAGE_SELF_TEST,
    };

    if (app_index >= 0 && app_index < ROUND_APP_COUNT)
    {
        ui_page_switch_async(app_pages[app_index]);
    }
}

static void round_carousel_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL)
    {
        return;
    }

    if (code == LV_EVENT_PRESSED)
    {
        lv_obj_t *target = lv_event_get_current_target(event);
        if (target != NULL)
        {
            lv_obj_remove_state(target, LV_STATE_PRESSED);
        }
        lv_anim_delete(s_round_ui.carousel, round_carousel_anim_cb);
        s_round_ui.pending_steps = 0;
        s_round_ui.snap_direction = 0;
        lv_point_t point = {0};
        lv_indev_get_point(indev, &point);
        s_round_ui.press_x = point.x;
        s_round_ui.press_y = point.y;
        s_round_ui.drag_delta_x = 0;
        s_round_ui.last_vect_x = 0;
        round_app_card_t *pressed_card = lv_event_get_user_data(event);
        s_round_ui.pressed_slot = pressed_card ? pressed_card->relative_slot : ROUND_NO_CARD_SLOT;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        lv_point_t point = {0};
        lv_point_t vect = {0};
        lv_indev_get_point(indev, &point);
        lv_indev_get_vect(indev, &vect);
        int32_t delta_x = point.x - s_round_ui.press_x;
        int32_t delta_y = point.y - s_round_ui.press_y;
        s_round_ui.drag_delta_x = delta_x;
        s_round_ui.last_vect_x = vect.x;
        if (LV_ABS(delta_x) < 3 && LV_ABS(delta_y) < 3)
        {
            lv_event_stop_bubbling(event);
            return;
        }
        if (LV_ABS(delta_x) >= LV_ABS(delta_y) ||
            LV_ABS(vect.x) >= LV_ABS(vect.y))
        {
            if (vect.x != 0)
            {
                s_round_ui.position_fp +=
                    (vect.x * ROUND_CARD_STEP_FP) / ROUND_CARD_SPACING;
                round_carousel_drag_normalize();
                round_carousel_layout_update();
            }
            lv_event_stop_bubbling(event);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        int direction = 0;
        int steps = 1;
        int32_t distance = LV_ABS(s_round_ui.drag_delta_x);
        int32_t speed = LV_ABS(s_round_ui.last_vect_x);
        bool is_click = code == LV_EVENT_RELEASED && distance < 8;

        if (is_click && s_round_ui.pressed_slot == 0)
        {
            round_app_page_open(s_round_ui.selected);
        }
        else if (is_click && s_round_ui.pressed_slot != ROUND_NO_CARD_SLOT)
        {
            direction = s_round_ui.pressed_slot < 0 ? 1 : -1;
            steps = LV_ABS(s_round_ui.pressed_slot);
        }
        else if (s_round_ui.position_fp <= -ROUND_SWIPE_THRESHOLD_FP ||
                 s_round_ui.last_vect_x < -ROUND_SWIPE_VELOCITY_THRESHOLD)
        {
            direction = -1;
        }
        else if (s_round_ui.position_fp >= ROUND_SWIPE_THRESHOLD_FP ||
                 s_round_ui.last_vect_x > ROUND_SWIPE_VELOCITY_THRESHOLD)
        {
            direction = 1;
        }

        if (!is_click && direction != 0)
        {
            /* Dragged-over cards are already selected in real time. */
            steps = 1;
            if (speed > ROUND_MULTI_SWIPE_VELOCITY)
            {
                steps++;
            }
            if (speed > ROUND_MULTI_SWIPE_FAST_VELOCITY)
            {
                steps++;
            }
            steps = LV_CLAMP(1, steps, 3);
        }

        if (!is_click || s_round_ui.pressed_slot != 0)
        {
            round_carousel_snap(direction, steps);
        }
        s_round_ui.drag_delta_x = 0;
        s_round_ui.last_vect_x = 0;
        s_round_ui.pressed_slot = ROUND_NO_CARD_SLOT;
        lv_event_stop_bubbling(event);
    }
}

static void round_carousel_event_bind(lv_obj_t *obj, round_app_card_t *card)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, round_carousel_event_cb, LV_EVENT_PRESSED, card);
    lv_obj_add_event_cb(obj, round_carousel_event_cb, LV_EVENT_PRESSING, card);
    lv_obj_add_event_cb(obj, round_carousel_event_cb, LV_EVENT_RELEASED, card);
    lv_obj_add_event_cb(obj, round_carousel_event_cb, LV_EVENT_PRESS_LOST, card);
}

static void home_enter_translate_y_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void home_enter_opa_cb(void *var, int32_t value)
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
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_start(&anim);
}

static void home_round_page_create(Page *page)
{
    s_round_ui.root = page->root;
    s_round_ui.selected = 0;
    s_round_ui.position_fp = 0;
    s_round_ui.foreground_card = -1;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(page->root);
    
    status_bar_create();
    s_home_status_bar = status_bar_get_container();
    if (s_home_status_bar != NULL)
    {
        lv_obj_set_style_translate_y(s_home_status_bar, HOME_STATUS_START_Y, 0);
        lv_obj_set_style_opa(s_home_status_bar, LV_OPA_TRANSP, 0);
    }

    s_round_ui.main_cont = lv_obj_create(page->root);
    ui_main_cont_style_init(s_round_ui.main_cont);
    lv_obj_set_style_bg_opa(s_round_ui.main_cont, LV_OPA_0, 0);
    lv_obj_set_layout(s_round_ui.main_cont, LV_LAYOUT_NONE);
    lv_obj_add_flag(s_round_ui.main_cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_round_ui.section_title = ui_label_create(s_round_ui.main_cont, "",
                                               &lv_font_SourceHanSansCN_Bold_2_30, UI_TEXT);
    lv_obj_set_width(s_round_ui.section_title, 200);
    lv_obj_set_pos(s_round_ui.section_title, 155, 69);
    lv_obj_set_style_text_align(s_round_ui.section_title, LV_TEXT_ALIGN_CENTER, 0);

    s_round_ui.carousel = lv_obj_create(s_round_ui.main_cont);
    lv_obj_set_pos(s_round_ui.carousel, -45, 107);
    lv_obj_set_size(s_round_ui.carousel, 600, 260);
    ui_obj_set_transparent(s_round_ui.carousel);
    lv_obj_set_style_bg_opa(s_round_ui.carousel, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(s_round_ui.carousel, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(s_round_ui.carousel, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(s_round_ui.carousel, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_add_flag(s_round_ui.carousel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    round_carousel_event_bind(s_round_ui.carousel, NULL);

    for (int i = 0; i < ROUND_CARD_COUNT; i++)
    {
        s_round_ui.cards[i] = round_card_create(s_round_ui.carousel,
                                                i - ROUND_CARD_CENTER_SLOT);
        round_carousel_event_bind(s_round_ui.cards[i].obj, &s_round_ui.cards[i]);
    }

    for (int i = 0; i < ROUND_APP_COUNT; i++)
    {
        s_round_ui.page_dots[i] = lv_obj_create(s_round_ui.main_cont);
        lv_obj_set_style_radius(s_round_ui.page_dots[i], 2, 0);
        lv_obj_set_style_border_width(s_round_ui.page_dots[i], 0, 0);
        lv_obj_remove_flag(s_round_ui.page_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    s_round_ui.description = ui_label_create(s_round_ui.main_cont,
                                             s_app_menu_descs[s_round_ui.selected],
                                             &lv_font_SourceHanSansSC_Regular_2_20, UI_MUTED);
    lv_obj_set_width(s_round_ui.description, 420);
    lv_obj_set_pos(s_round_ui.description, 45, ROUND_DESCRIPTION_Y);
    lv_obj_set_style_text_align(s_round_ui.description, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_round_ui.description, 0, 0);

    s_round_ui.logo_label = ui_label_create(page->root, "LILYGO",
                                            &lv_font_SourceHanSansCN_Bold_2_20, UI_TEXT);
    lv_obj_set_width(s_round_ui.logo_label, 160);
    lv_obj_set_style_text_opa(s_round_ui.logo_label, LV_OPA_50, 0);
    lv_obj_set_style_opa(s_round_ui.logo_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_round_ui.logo_label, HOME_LOGO_START_Y, 0);
    lv_obj_align(s_round_ui.logo_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_align(s_round_ui.logo_label, LV_TEXT_ALIGN_CENTER, 0);

    round_selected_content_update();
    round_carousel_layout_update();
    s_home_enter_animated = false;
}

static void home_round_page_enter(Page *page)
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
                              home_enter_translate_y_cb,
                              HOME_STATUS_START_Y,
                              0,
                              HOME_STATUS_ANIM_DURATION_MS,
                              HOME_STATUS_ANIM_DELAY_MS);
        home_enter_anim_start(s_home_status_bar,
                              home_enter_opa_cb,
                              LV_OPA_TRANSP,
                              LV_OPA_COVER,
                              HOME_STATUS_ANIM_DURATION_MS,
                              HOME_STATUS_ANIM_DELAY_MS);
    }

    if (s_round_ui.logo_label != NULL)
    {
        home_enter_anim_start(s_round_ui.logo_label,
                              home_enter_translate_y_cb,
                              HOME_LOGO_START_Y,
                              0,
                              HOME_LOGO_ANIM_DURATION_MS,
                              0);
        home_enter_anim_start(s_round_ui.logo_label,
                              home_enter_opa_cb,
                              LV_OPA_TRANSP,
                              LV_OPA_COVER,
                              HOME_LOGO_ANIM_DURATION_MS,
                              0);
    }
}

static void home_round_page_timer(Page *page)
{
    (void)page;
}

void home_round_page_register(void)
{
    Page page = {
        .id = PAGE_HOME,
        .name = "home_round",
        .on_create = home_round_page_create,
        .on_enter = home_round_page_enter,
        .page_timer = home_round_page_timer,
        .timer_interval_ms = STATUS_TIMER_MS,
    };
    ui_page_register(&page);
}
