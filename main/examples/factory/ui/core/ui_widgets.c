#include "ui_widgets.h"
#include "lvgl_page_manager.h"
#include "display_panel.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_widgets";

#if CONFIG_T_PANEL_P4_BOARD_RECT
extern const uint8_t galaxy_jpg_start[] asm("_binary_galaxy_jpg_start");
extern const uint8_t galaxy_jpg_end[] asm("_binary_galaxy_jpg_end");
#define UI_HOME_BACKGROUND_NAME "galaxy"
#define UI_HOME_BACKGROUND_START galaxy_jpg_start
#define UI_HOME_BACKGROUND_END galaxy_jpg_end
#elif CONFIG_T_PANEL_P4_BOARD_ROUND
extern const uint8_t round_jpg_start[] asm("_binary_black_gray_gradient_jpg_start");
extern const uint8_t round_jpg_end[] asm("_binary_black_gray_gradient_jpg_end");
#define UI_HOME_BACKGROUND_NAME "round"
#define UI_HOME_BACKGROUND_START round_jpg_start
#define UI_HOME_BACKGROUND_END round_jpg_end
#else
extern const uint8_t black_gray_gradient_jpg_start[] asm("_binary_black_gray_gradient_jpg_start");
extern const uint8_t black_gray_gradient_jpg_end[] asm("_binary_black_gray_gradient_jpg_end");
#define UI_HOME_BACKGROUND_NAME "black_gray_gradient"
#define UI_HOME_BACKGROUND_START black_gray_gradient_jpg_start
#define UI_HOME_BACKGROUND_END black_gray_gradient_jpg_end
#endif

static uint8_t *s_home_background_pixels;
static lv_image_dsc_t s_home_background_dsc;

void ui_obj_set_transparent(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void ui_obj_set_flex(lv_obj_t *obj, lv_flex_flow_t flow, lv_flex_align_t main_place,
                     lv_flex_align_t cross_place, lv_flex_align_t track_cross_place)
{
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_flex_align(obj, main_place, cross_place, track_cross_place);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_flex_container_create(lv_obj_t *parent, int32_t w, int32_t h,
                                   lv_flex_flow_t flow, lv_flex_align_t main_place,
                                   lv_flex_align_t cross_place, lv_flex_align_t track_cross_place)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    ui_obj_set_transparent(obj);
    ui_obj_set_flex(obj, flow, main_place, cross_place, track_cross_place);
    return obj;
}

lv_obj_t *ui_flex_spacer_create(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);
    ui_obj_set_transparent(spacer);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    return spacer;
}

void ui_main_cont_style_init(lv_obj_t *obj)
{
#if CONFIG_T_PANEL_P4_BOARD_ROUND
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_30, 0);
    lv_obj_set_pos(obj, UI_ROUND_MAIN_CONT_X, UI_ROUND_MAIN_CONT_Y);
    lv_obj_set_size(obj, UI_ROUND_MAIN_CONT_W, UI_ROUND_MAIN_CONT_H);
    lv_obj_set_style_border_width(obj, 0, 0);
    // lv_obj_set_style_border_color(obj, lv_color_hex(UI_TEXT), 0);
    // lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_radius(obj, 24, 0);
    // lv_obj_set_style_shadow_width(obj, 24, 0);
    // lv_obj_set_style_shadow_spread(obj, 2, 0);
    // lv_obj_set_style_shadow_color(obj, lv_color_hex(UI_PRIMARY), 0);
    // lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
#else
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_20, 0);
    lv_obj_set_pos(obj, UI_STANDARD_MAIN_CONT_X, UI_STANDARD_MAIN_CONT_Y);
    lv_obj_set_size(obj, UI_STANDARD_MAIN_CONT_W, UI_STANDARD_MAIN_CONT_H);
    lv_obj_set_style_border_width(obj, 0, 0);
#endif
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 20, 0);
    ui_obj_set_flex(obj, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void ui_main_cont_style1_init(lv_obj_t *obj)
{
#if CONFIG_T_PANEL_P4_BOARD_ROUND
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_30, 0);
    lv_obj_set_pos(obj, UI_ROUND_MAIN_CONT_X, UI_ROUND_MAIN_CONT_Y);
    lv_obj_set_size(obj, UI_ROUND_MAIN_CONT_W, UI_ROUND_MAIN_CONT_H);
    lv_obj_set_style_border_width(obj, 0, 0);
    // lv_obj_set_style_border_color(obj, lv_color_hex(UI_TEXT), 0);
    // lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_radius(obj, 24, 0);
    // lv_obj_set_style_shadow_width(obj, 24, 0);
    // lv_obj_set_style_shadow_spread(obj, 2, 0);
    // lv_obj_set_style_shadow_color(obj, lv_color_hex(UI_PRIMARY), 0);
    // lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
#else
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_20, 0);
    lv_obj_set_pos(obj, UI_STANDARD_MAIN_CONT_X, UI_STANDARD_MAIN_CONT_Y);
    lv_obj_set_size(obj, UI_STANDARD_MAIN_CONT_W, UI_STANDARD_MAIN_CONT_H);
    lv_obj_set_style_border_width(obj, 0, 0);
#endif
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 0, 0);
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    return label;
}

lv_obj_t *ui_standard_page_title_create(lv_obj_t *root, const char *text)
{
    lv_obj_t *title = ui_label_create(root, text,
                                      &lv_font_SourceHanSansCN_Bold_2_28,
                                      UI_TEXT);
    lv_obj_set_pos(title, UI_STANDARD_TITLE_X, UI_STANDARD_TITLE_Y);
    lv_obj_set_size(title, UI_STANDARD_TITLE_W, UI_STANDARD_TITLE_H);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    return title;
}

lv_obj_t *ui_light_button_create(lv_obj_t *parent, const char *text,
                                 int32_t width, int32_t height,
                                 const lv_font_t *font,
                                 lv_event_cb_t event_cb, void *user_data)
{
    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, height / 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(button, LV_OPA_20, 0);
    lv_obj_set_style_border_opa(button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(button, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_shadow_width(button, 10, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(button, 63, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(button, 3, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(button, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (event_cb != NULL)
    {
        lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = ui_label_create(button, text, font, UI_TEXT);
    lv_obj_center(label);

    return button;
}

static bool ui_background_decode(void)
{
    if (s_home_background_pixels != NULL)
    {
        return true;
    }

    const uint8_t *jpg_data = UI_HOME_BACKGROUND_START;
    const size_t jpg_size = (size_t)(UI_HOME_BACKGROUND_END - UI_HOME_BACKGROUND_START);
    if (jpg_size == 0)
    {
        ESP_LOGE(TAG, "Embedded %s JPEG is empty", UI_HOME_BACKGROUND_NAME);
        return false;
    }

    jpeg_decode_picture_info_t picture_info = {0};
    esp_err_t ret = jpeg_decoder_get_info(jpg_data, (uint32_t)jpg_size,
                                          &picture_info);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Read %s JPEG info failed: %s",
                 UI_HOME_BACKGROUND_NAME, esp_err_to_name(ret));
        return false;
    }
    if (picture_info.width != DISPLAY_PANEL_H_RES ||
        picture_info.height != DISPLAY_PANEL_V_RES)
    {
        ESP_LOGE(TAG, "%s JPEG must be %ux%u, got %" PRIu32 "x%" PRIu32,
                 UI_HOME_BACKGROUND_NAME,
                 (unsigned)DISPLAY_PANEL_H_RES,
                 (unsigned)DISPLAY_PANEL_V_RES,
                 picture_info.width, picture_info.height);
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t input_config = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t input_size = 0;
    uint8_t *input = jpeg_alloc_decoder_mem(jpg_size, &input_config, &input_size);
    if (input == NULL || input_size < jpg_size)
    {
        free(input);
        ESP_LOGE(TAG, "Allocate %s JPEG input failed", UI_HOME_BACKGROUND_NAME);
        return false;
    }
    memcpy(input, jpg_data, jpg_size);

    const size_t output_bytes = (size_t)DISPLAY_PANEL_H_RES *
                                DISPLAY_PANEL_V_RES * 3U;
    jpeg_decode_memory_alloc_cfg_t output_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_size = 0;
    uint8_t *output = jpeg_alloc_decoder_mem(output_bytes,
                                              &output_config,
                                              &output_size);
    if (output == NULL || output_size < output_bytes)
    {
        free(input);
        free(output);
        ESP_LOGE(TAG, "Allocate %s RGB888 output failed", UI_HOME_BACKGROUND_NAME);
        return false;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    ret = jpeg_new_decoder_engine(&engine_config, &decoder);
    if (ret != ESP_OK)
    {
        free(input);
        free(output);
        ESP_LOGE(TAG, "Create %s JPEG decoder failed: %s",
                 UI_HOME_BACKGROUND_NAME, esp_err_to_name(ret));
        return false;
    }

    jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(decoder,
                               &decode_config,
                               input,
                               (uint32_t)jpg_size,
                               output,
                               (uint32_t)output_size,
                               &decoded_size);
    esp_err_t delete_ret = jpeg_del_decoder_engine(decoder);
    free(input);
    if (ret != ESP_OK || delete_ret != ESP_OK || decoded_size < output_bytes)
    {
        free(output);
        ESP_LOGE(TAG, "Decode %s JPEG failed: decode=%s delete=%s size=%" PRIu32,
                 UI_HOME_BACKGROUND_NAME,
                 esp_err_to_name(ret), esp_err_to_name(delete_ret), decoded_size);
        return false;
    }

    s_home_background_pixels = output;
    s_home_background_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_home_background_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_home_background_dsc.header.flags = 0;
    s_home_background_dsc.header.w = DISPLAY_PANEL_H_RES;
    s_home_background_dsc.header.h = DISPLAY_PANEL_V_RES;
    s_home_background_dsc.header.stride = DISPLAY_PANEL_H_RES * 3U;
    s_home_background_dsc.data_size = output_bytes;
    s_home_background_dsc.data = s_home_background_pixels;
    return true;
}

lv_obj_t *ui_background_create(lv_obj_t *parent)
{
    if (parent == NULL || !ui_background_decode())
    {
        return NULL;
    }

    lv_obj_t *background = lv_image_create(parent);
    if (background == NULL)
    {
        return NULL;
    }
    lv_image_set_src(background, &s_home_background_dsc);
    lv_obj_set_pos(background, 0, 0);
    lv_obj_remove_flag(background,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(background);
    return background;
}
