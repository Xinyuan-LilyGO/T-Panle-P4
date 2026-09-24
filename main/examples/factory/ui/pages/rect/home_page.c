#include "ui_internal.h"
#include "home_page.h"

#define HOME_LOGO_ANIM_DURATION_MS       700
#define HOME_STATUS_ANIM_DURATION_MS     1200
#define HOME_STATUS_ANIM_DELAY_MS        250
#define HOME_STATUS_START_Y              (-40)
#define HOME_APP_COUNT                   7
#define HOME_APP_ICON_SIZE               108

extern const uint8_t galaxy_jpg_start[] asm("_binary_galaxy_jpg_start");
extern const uint8_t galaxy_jpg_end[] asm("_binary_galaxy_jpg_end");

static const char *TAG = "[UI][home_page]";
static home_page_ui_t s_home_ui;
static lv_obj_t *s_home_status_bar;
static lv_obj_t *s_home_background_img;
static uint8_t *s_home_background_pixels;
static lv_image_dsc_t s_home_background_dsc;
static bool s_home_enter_animated;

typedef struct
{
    const char *name;
    const char *symbol;
    PageType page;
} home_app_t;

static const home_app_t s_home_apps[HOME_APP_COUNT] = {
    {.name = "Music",     .symbol = LV_SYMBOL_AUDIO,     .page = PAGE_MUSIC},
    {.name = "Record",    .symbol = LV_SYMBOL_EDIT,      .page = PAGE_RECORD},
    {.name = "Camera",    .symbol = LV_SYMBOL_IMAGE,     .page = PAGE_CAMERA},
    {.name = "Files",     .symbol = LV_SYMBOL_DIRECTORY, .page = PAGE_FILE},
    {.name = "Sensor",    .symbol = LV_SYMBOL_EYE_OPEN,  .page = PAGE_SENSOR},
    {.name = "Settings",  .symbol = LV_SYMBOL_SETTINGS,  .page = PAGE_SET},
    {.name = "Self Test", .symbol = LV_SYMBOL_REFRESH,   .page = PAGE_SELF_TEST},
};

static const int32_t s_home_grid_cols[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST,
};

static const int32_t s_home_grid_rows[] = {
    200, 200, 200, LV_GRID_TEMPLATE_LAST,
};

static bool home_background_decode(void)
{
    if (s_home_background_pixels != NULL)
    {
        return true;
    }

    const uint8_t *jpg_data = galaxy_jpg_start;
    const size_t jpg_size = (size_t)(galaxy_jpg_end - galaxy_jpg_start);
    if (jpg_size == 0)
    {
        ESP_LOGE(TAG, "Embedded galaxy JPEG is empty");
        return false;
    }

    jpeg_decode_picture_info_t picture_info = {0};
    esp_err_t ret = jpeg_decoder_get_info(jpg_data, (uint32_t)jpg_size,
                                          &picture_info);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Read galaxy JPEG info failed: %s", esp_err_to_name(ret));
        return false;
    }
    if (picture_info.width != DISPLAY_PANEL_H_RES ||
        picture_info.height != DISPLAY_PANEL_V_RES)
    {
        ESP_LOGE(TAG, "Galaxy JPEG must be %ux%u, got %" PRIu32 "x%" PRIu32,
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
        ESP_LOGE(TAG, "Allocate galaxy JPEG input failed");
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
        ESP_LOGE(TAG, "Allocate galaxy RGB888 output failed");
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
        ESP_LOGE(TAG, "Create galaxy JPEG decoder failed: %s",
                 esp_err_to_name(ret));
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
        ESP_LOGE(TAG, "Decode galaxy JPEG failed: decode=%s delete=%s size=%" PRIu32,
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

static void home_background_create(lv_obj_t *parent)
{
    if (!home_background_decode())
    {
        return;
    }

    s_home_background_img = lv_image_create(parent);
    lv_image_set_src(s_home_background_img, &s_home_background_dsc);
    lv_obj_set_pos(s_home_background_img, 0, 0);
    lv_obj_remove_flag(s_home_background_img,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(s_home_background_img);
}

static void home_app_clicked_cb(lv_event_t *event)
{
    const home_app_t *app = (const home_app_t *)lv_event_get_user_data(event);
    if (app == NULL || app->page >= PAGE_SIZE)
    {
        return;
    }

    ui_page_switch_async(app->page);
}

static void home_app_create(lv_obj_t *parent, const home_app_t *app, int index)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_grid_cell(tile,
                         LV_GRID_ALIGN_STRETCH, index % 3, 1,
                         LV_GRID_ALIGN_STRETCH, index / 3, 1);
    ui_obj_set_transparent(tile);
    lv_obj_set_style_pad_all(tile, 8, 0);
    lv_obj_set_style_pad_row(tile, 14, 0);
    lv_obj_set_style_opa(tile, LV_OPA_70, LV_STATE_PRESSED);
    ui_obj_set_flex(tile,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, home_app_clicked_cb, LV_EVENT_CLICKED, (void *)app);

    lv_obj_t *icon_box = lv_obj_create(tile);
    lv_obj_set_size(icon_box, HOME_APP_ICON_SIZE, HOME_APP_ICON_SIZE);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_radius(icon_box, 24, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, app->symbol);
    lv_obj_center(icon);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, app->name);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_SourceHanSansCN_Bold_2_24, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
}

static void home_apps_create(lv_obj_t *parent)
{
    lv_obj_set_grid_dsc_array(parent, s_home_grid_cols, s_home_grid_rows);
    lv_obj_set_grid_align(parent, LV_GRID_ALIGN_SPACE_EVENLY, LV_GRID_ALIGN_START);
    lv_obj_set_style_pad_top(parent, 40, 0);
    lv_obj_set_style_pad_bottom(parent, 0, 0);
    lv_obj_set_style_pad_left(parent, 36, 0);
    lv_obj_set_style_pad_right(parent, 36, 0);
    lv_obj_set_style_pad_row(parent, 28, 0);
    lv_obj_set_style_pad_column(parent, 24, 0);

    for (int i = 0; i < HOME_APP_COUNT; i++)
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
                                  uint32_t delay,
                                  lv_anim_completed_cb_t completed_cb)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, start, end);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, exec_cb);
    if (completed_cb != NULL)
    {
        lv_anim_set_completed_cb(&anim, completed_cb);
    }
    lv_anim_start(&anim);
}

static void home_page_create(Page *page)
{
    s_home_ui.root = page->root;
    lv_obj_set_style_bg_color(s_home_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_home_ui.root, LV_SCROLLBAR_MODE_OFF);

    home_background_create(s_home_ui.root);

    status_bar_create();
    s_home_status_bar = status_bar_get_container();
    if (s_home_status_bar != NULL)
    {
        lv_obj_set_style_translate_y(s_home_status_bar, HOME_STATUS_START_Y, 0);
    }
    
    s_home_ui.logo_label = lv_label_create(s_home_ui.root);
    lv_obj_align(s_home_ui.logo_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(s_home_ui.logo_label, "LILYGO");
    lv_obj_set_style_text_color(s_home_ui.logo_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(s_home_ui.logo_label, &lv_font_SourceHanSansCN_Bold_2_24, 0);
    lv_obj_set_style_opa(s_home_ui.logo_label, LV_OPA_TRANSP, 0);

    s_home_enter_animated = false;

    s_home_ui.main_cont = lv_obj_create(s_home_ui.root);
    lv_obj_set_size(s_home_ui.main_cont, 720, 1280);
    lv_obj_set_pos(s_home_ui.main_cont, 0, 60);
    ui_obj_set_transparent(s_home_ui.main_cont);
    lv_obj_set_style_radius(s_home_ui.main_cont, 20, 0);
    lv_obj_set_scrollbar_mode(s_home_ui.main_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_home_ui.main_cont, LV_OBJ_FLAG_SCROLLABLE);

    home_apps_create(s_home_ui.main_cont);
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
                              HOME_STATUS_ANIM_DELAY_MS,
                              NULL);
    }
    if (s_home_ui.logo_label != NULL)
    {
        home_enter_anim_start(s_home_ui.logo_label,
                              home_logo_opa_cb,
                              LV_OPA_TRANSP,
                              LV_OPA_COVER,
                              HOME_LOGO_ANIM_DURATION_MS,
                              0,
                              NULL);
    }
}

static void home_page_timer(Page *page)
{

}

void home_page_register(void)
{
    Page page = {
        .id = PAGE_HOME,
        .name = "home",
        .on_create = home_page_create,
        .on_enter = home_page_enter,
        .page_timer = home_page_timer,
        .timer_interval_ms = STATUS_TIMER_MS,
    };
    ui_page_register(&page);
}
