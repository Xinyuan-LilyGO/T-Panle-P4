#pragma once

#include "ui.h"

#define UI_ROUND_MAIN_CONT_X 105
#define UI_ROUND_MAIN_CONT_Y 105
#define UI_ROUND_MAIN_CONT_W 510
#define UI_ROUND_MAIN_CONT_H 510
#define UI_ROUND_TITLE_W 260
#define UI_ROUND_TITLE_X ((SCREEN_WIDTH - UI_ROUND_TITLE_W) / 2)
#define UI_ROUND_TITLE_Y 50
#define UI_ROUND_POPUP_SIZE 500
#define UI_ROUND_POPUP_X ((SCREEN_WIDTH - UI_ROUND_POPUP_SIZE) / 2)
#define UI_ROUND_POPUP_Y ((SCREEN_HEIGHT - UI_ROUND_POPUP_SIZE) / 2)

#define UI_STANDARD_TITLE_W 200
#define UI_STANDARD_TITLE_H 40
#define UI_STANDARD_TITLE_X ((SCREEN_WIDTH - UI_STANDARD_TITLE_W) / 2)
#define UI_STANDARD_TITLE_Y 50
#define UI_STANDARD_MAIN_CONT_X 0
#define UI_STANDARD_MAIN_CONT_Y 40
#define UI_STANDARD_MAIN_CONT_W 720
#define UI_STANDARD_MAIN_CONT_H 680

void ui_obj_set_transparent(lv_obj_t *obj);
void ui_obj_set_flex(lv_obj_t *obj, lv_flex_flow_t flow, lv_flex_align_t main_place,
                     lv_flex_align_t cross_place, lv_flex_align_t track_cross_place);
lv_obj_t *ui_flex_container_create(lv_obj_t *parent, int32_t w, int32_t h,
                                   lv_flex_flow_t flow, lv_flex_align_t main_place,
                                   lv_flex_align_t cross_place, lv_flex_align_t track_cross_place);
lv_obj_t *ui_flex_spacer_create(lv_obj_t *parent);
void ui_main_cont_style_init(lv_obj_t *obj);
void ui_main_cont_style1_init(lv_obj_t *obj);
lv_obj_t *ui_background_create(lv_obj_t *parent);
lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color);
lv_obj_t *ui_standard_page_title_create(lv_obj_t *root, const char *text);
lv_obj_t *ui_light_button_create(lv_obj_t *parent, const char *text,
                                 int32_t width, int32_t height,
                                 const lv_font_t *font,
                                 lv_event_cb_t event_cb, void *user_data);
