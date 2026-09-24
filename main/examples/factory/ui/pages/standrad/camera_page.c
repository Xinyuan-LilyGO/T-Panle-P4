#include "ui_internal.h"
#include "camera_page.h"

static const char *TAG = "[UI][camera_page]";

static camera_page_ui_t s_camera_ui;
static uint8_t *s_camera_last_photo_thumb_buf;
static lv_image_dsc_t s_camera_last_photo_dsc;
static lv_timer_t *s_camera_state_timer;
static uint8_t s_camera_thumb_poll_buf[CAMERA_UI_LAST_PHOTO_WIDTH * CAMERA_UI_LAST_PHOTO_HEIGHT * 3U];
static uint32_t s_camera_thumb_generation;
static char s_camera_home_mode[24] = "720P";
static int s_camera_home_fps_x10 = 250;

/*********camera Page************/
static void camera_shutter_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    camera_page_set_status("SAVING", true);
    esp_err_t ret = camera_capture_photo_async();
    if (ret == ESP_OK)
    {
        return;
    }

    if (ret == ESP_ERR_NO_MEM)
    {
        camera_page_set_status("NO MEM", false);
    }
    else if (ret == ESP_ERR_TIMEOUT)
    {
        camera_page_set_status("BUSY", false);
    }
    else if (ret == ESP_ERR_INVALID_STATE)
    {
        camera_page_set_status("WAIT", false);
    }
    else
    {
        camera_page_set_status("SAVE ERR", false);
    }
}

static void camera_state_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    camera_state_t state;
    if (camera_state_get(&state) != ESP_OK)
    {
        return;
    }
    if (state.width > 0 && state.height > 0)
    {
        camera_page_set_preview_info(state.width, state.height, state.fps_x10);
    }
    camera_page_set_shot_count(state.photo_count);
    if (state.photo_saving)
    {
        camera_page_set_status("SAVING", true);
    }
    else if (state.photo_count > 0 && state.last_photo_result != ESP_OK)
    {
        camera_page_set_status("SAVE ERR", false);
    }
    else if (state.photo_count > 0)
    {
        camera_page_set_status("SAVED", true);
    }
    if (state.photo_generation != s_camera_thumb_generation &&
        camera_last_photo_copy(s_camera_thumb_poll_buf,
                               sizeof(s_camera_thumb_poll_buf), NULL, NULL,
                               &s_camera_thumb_generation) == ESP_OK)
    {
        camera_page_set_last_photo(s_camera_thumb_poll_buf,
                                   CAMERA_UI_LAST_PHOTO_WIDTH,
                                   CAMERA_UI_LAST_PHOTO_HEIGHT);
    }
}

static void camera_page_create(Page *page)
{
    s_camera_ui.root = page->root;
    lv_obj_set_style_bg_color(s_camera_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_camera_ui.root, 0, 0);
    lv_obj_remove_flag(s_camera_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(s_camera_ui.root);
    status_bar_create();

    s_camera_ui.main_cont = lv_obj_create(s_camera_ui.root);
    lv_obj_set_pos(s_camera_ui.main_cont, 0, 40);
    lv_obj_set_size(s_camera_ui.main_cont, 720, 680);
    ui_obj_set_transparent(s_camera_ui.main_cont);
    lv_obj_remove_flag(s_camera_ui.main_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_camera_ui.preview_cont = lv_obj_create(s_camera_ui.main_cont);
    lv_obj_set_pos(s_camera_ui.preview_cont, 0, 0);
    lv_obj_set_size(s_camera_ui.preview_cont, CAMERA_UI_PREVIEW_WIDTH,
                    CAMERA_UI_PREVIEW_HEIGHT);
    lv_obj_set_style_bg_color(s_camera_ui.preview_cont, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.preview_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.preview_cont, 0, 0);
    lv_obj_set_style_border_color(s_camera_ui.preview_cont, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_radius(s_camera_ui.preview_cont, 0, 0);
    lv_obj_set_style_pad_all(s_camera_ui.preview_cont, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.preview_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_camera_ui.preview_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.preview_img = lv_image_create(s_camera_ui.preview_cont);
    lv_obj_set_size(s_camera_ui.preview_img, CAMERA_UI_PREVIEW_WIDTH, CAMERA_UI_PREVIEW_HEIGHT);
    lv_obj_set_pos(s_camera_ui.preview_img, 0, 0);
    lv_obj_set_style_bg_color(s_camera_ui.preview_img, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.preview_img, 0, 0);
    lv_obj_set_style_radius(s_camera_ui.preview_img, 0, 0);
    lv_obj_set_style_pad_all(s_camera_ui.preview_img, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.preview_img, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_camera_ui.preview_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preview_icon = ui_label_create(s_camera_ui.preview_img, LV_SYMBOL_IMAGE, &lv_font_montserrat_28, UI_MUTED);
    lv_obj_center(preview_icon);

    lv_obj_t *preview_info = ui_flex_container_create(s_camera_ui.main_cont,
                                                      CAMERA_UI_PREVIEW_WIDTH,
                                                      48,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(preview_info, 0, CAMERA_UI_PREVIEW_HEIGHT);
    lv_obj_set_style_pad_column(preview_info, 10, 0);
    lv_obj_set_style_pad_left(preview_info, 14, 0);
    lv_obj_set_style_pad_right(preview_info, 14, 0);

    s_camera_ui.resolution_label = ui_label_create(preview_info, "--P", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(s_camera_ui.resolution_label, 110);
    lv_obj_t *format_label = ui_label_create(preview_info, "RGB 888", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(format_label, 150);
    s_camera_ui.fps_label = ui_label_create(preview_info, "--.- FPS", &lv_font_montserrat_14, UI_OK);
    lv_obj_set_width(s_camera_ui.fps_label, 100);
    ui_flex_spacer_create(preview_info);
    s_camera_ui.shot_count_label = ui_label_create(preview_info, "SHOT 000", &lv_font_montserrat_14, UI_SECONDARY);
    lv_obj_set_width(s_camera_ui.shot_count_label, 110);
    lv_obj_set_style_text_align(s_camera_ui.shot_count_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_camera_ui.last_photo_btn = lv_button_create(s_camera_ui.main_cont);
    lv_obj_set_pos(s_camera_ui.last_photo_btn, 48, 562);
    lv_obj_set_size(s_camera_ui.last_photo_btn, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_set_style_radius(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_set_style_bg_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.last_photo_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_set_style_border_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_pad_all(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_remove_flag(s_camera_ui.last_photo_btn, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.last_photo_img = lv_image_create(s_camera_ui.last_photo_btn);
    lv_obj_set_size(s_camera_ui.last_photo_img, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_add_flag(s_camera_ui.last_photo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_camera_ui.last_photo_img);

    s_camera_ui.last_photo_icon = ui_label_create(s_camera_ui.last_photo_btn, LV_SYMBOL_IMAGE, &lv_font_montserrat_24, UI_SECONDARY);
    lv_obj_center(s_camera_ui.last_photo_icon);

    s_camera_ui.shutter_btn = lv_button_create(s_camera_ui.main_cont);
    lv_obj_set_pos(s_camera_ui.shutter_btn, 310, 548);
    lv_obj_set_size(s_camera_ui.shutter_btn, 100, 100);
    lv_obj_set_style_radius(s_camera_ui.shutter_btn, 50, 0);
    lv_obj_set_style_bg_color(s_camera_ui.shutter_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.shutter_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_camera_ui.shutter_btn, 2, 0);
    lv_obj_set_style_border_color(s_camera_ui.shutter_btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_camera_ui.shutter_btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(s_camera_ui.shutter_btn, 20, 0);
    lv_obj_set_style_shadow_spread(s_camera_ui.shutter_btn, 3, 0);
    lv_obj_set_style_shadow_color(s_camera_ui.shutter_btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_shadow_opa(s_camera_ui.shutter_btn, LV_OPA_70, 0);
    lv_obj_remove_flag(s_camera_ui.shutter_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_camera_ui.shutter_btn, camera_shutter_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *shutter_icon = ui_label_create(s_camera_ui.shutter_btn, LV_SYMBOL_IMAGE, &lv_font_montserrat_28, UI_TEXT);
    lv_obj_center(shutter_icon);

    // s_camera_ui.status_label = ui_label_create(s_camera_ui.main_cont, "READY",
    //                                            &lv_font_montserrat_16, UI_MUTED);
    // lv_obj_set_pos(s_camera_ui.status_label, 500, 500);
    // lv_obj_set_size(s_camera_ui.status_label, 190, 28);
    // lv_obj_set_style_text_align(s_camera_ui.status_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_camera_ui.storage_label = ui_label_create(s_camera_ui.main_cont, "SD --",
                                                &lv_font_montserrat_20, UI_MUTED);
    lv_obj_set_pos(s_camera_ui.storage_label, 500, 582);
    lv_obj_set_size(s_camera_ui.storage_label, 190, 32);
    lv_obj_set_style_text_align(s_camera_ui.storage_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void camera_page_enter(Page *page)
{
    (void)page;
    if (s_camera_ui.preview_img)
    {
        lv_area_t area;
        lv_obj_update_layout(lv_screen_active());
        lv_obj_get_coords(s_camera_ui.preview_img, &area);
        camera_preview_set_area(area.x1,
                                area.y1,
                                CAMERA_UI_PREVIEW_WIDTH,
                                CAMERA_UI_PREVIEW_HEIGHT);
        lv_obj_clean(s_camera_ui.preview_img);
    }
    bool sd_ready = camera_storage_is_ready();
    if (s_camera_ui.storage_label)
    {
        lv_label_set_text(s_camera_ui.storage_label, sd_ready ? "SD READY" : "NO SD");
        lv_obj_set_style_text_color(s_camera_ui.storage_label, lv_color_hex(sd_ready ? UI_OK : UI_ERROR), 0);
    }
    camera_preview_set_direct_crop(false);
    camera_preview_start(1280, 720);
    if (s_camera_ui.resolution_label)
    {
        lv_label_set_text(s_camera_ui.resolution_label, "720P");
    }
    if (s_camera_ui.fps_label)
    {
        lv_label_set_text(s_camera_ui.fps_label, "--.- FPS");
    }
    s_camera_thumb_generation = 0;
    if (s_camera_state_timer == NULL)
    {
        s_camera_state_timer = lv_timer_create(camera_state_timer_cb, 250, NULL);
    }
}

static void camera_page_leave(Page *page)
{
    if (s_camera_state_timer)
    {
        lv_timer_del(s_camera_state_timer);
        s_camera_state_timer = NULL;
    }
    camera_preview_stop();
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void camera_page_destroy(Page *page)
{
    (void)page;
    camera_preview_stop();
    if (s_camera_state_timer)
    {
        lv_timer_del(s_camera_state_timer);
        s_camera_state_timer = NULL;
    }
    if (s_camera_last_photo_thumb_buf)
    {
        free(s_camera_last_photo_thumb_buf);
        s_camera_last_photo_thumb_buf = NULL;
        memset(&s_camera_last_photo_dsc, 0, sizeof(s_camera_last_photo_dsc));
    }
    memset(&s_camera_ui, 0, sizeof(s_camera_ui));
}

static void camera_page_gesture(Page *page, GestureDirection direction)
{
    ESP_LOGI(TAG, "camera_page_gesture page:%d direction:%d", page->id, direction);
    if (direction == GESTURE_RIGHT)
    {
        camera_preview_stop();
        ui_page_switch_async(PAGE_HOME);
    }
}

void camera_page_set_status(const char *text, bool ok)
{
    ui_lock();
    if (s_camera_ui.status_label)
    {
        lv_label_set_text(s_camera_ui.status_label, (text && text[0]) ? text : "--");
        lv_obj_set_style_text_color(s_camera_ui.status_label, lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
    }
    ui_unlock();
}

void camera_page_set_shot_count(uint32_t count)
{
    ui_lock();
    if (s_camera_ui.shot_count_label)
    {
        lv_label_set_text_fmt(s_camera_ui.shot_count_label, "SHOT %03" PRIu32, count % 1000);
    }
    ui_unlock();
}

void camera_page_set_preview_info(uint32_t width, uint32_t height, uint32_t fps_x10)
{
    if (width == 1920 && height == 1080)
    {
        snprintf(s_camera_home_mode, sizeof(s_camera_home_mode), "1080P");
    }
    else if (width == 1280 && height == 720)
    {
        snprintf(s_camera_home_mode, sizeof(s_camera_home_mode), "720P");
    }
    else if (width > 0 && height > 0)
    {
        snprintf(s_camera_home_mode, sizeof(s_camera_home_mode), "%" PRIu32 "x%" PRIu32, width, height);
    }
    s_camera_home_fps_x10 = (int)fps_x10;

    ui_lock();
    if (s_camera_ui.resolution_label)
    {
        if (width == 1920 && height == 1080)
        {
            lv_label_set_text(s_camera_ui.resolution_label, "1080P");
        }
        else if (width == 1280 && height == 720)
        {
            lv_label_set_text(s_camera_ui.resolution_label, "720P");
        }
        else
        {
            lv_label_set_text_fmt(s_camera_ui.resolution_label, "%" PRIu32 "x%" PRIu32, width, height);
        }
    }
    if (s_camera_ui.fps_label)
    {
        if (fps_x10 > 0)
        {
            lv_label_set_text_fmt(s_camera_ui.fps_label,
                                  "%" PRIu32 ".%" PRIu32 " FPS",
                                  fps_x10 / 10,
                                  fps_x10 % 10);
        }
        else
        {
            lv_label_set_text(s_camera_ui.fps_label, "--.- FPS");
        }
    }
    home_camera_status_update_apply();
    ui_unlock();
}

void camera_page_set_last_photo(const uint8_t *rgb888, uint32_t width, uint32_t height)
{
    if (rgb888 == NULL || width != CAMERA_UI_LAST_PHOTO_WIDTH || height != CAMERA_UI_LAST_PHOTO_HEIGHT)
    {
        return;
    }

    ui_lock();
    if (s_camera_ui.last_photo_btn == NULL || s_camera_ui.last_photo_img == NULL)
    {
        ui_unlock();
        return;
    }

    const size_t data_size = (size_t)CAMERA_UI_LAST_PHOTO_WIDTH * CAMERA_UI_LAST_PHOTO_HEIGHT * 3U;
    if (s_camera_last_photo_thumb_buf == NULL)
    {
        s_camera_last_photo_thumb_buf = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_camera_last_photo_thumb_buf == NULL)
        {
            s_camera_last_photo_thumb_buf = heap_caps_malloc(data_size, MALLOC_CAP_DEFAULT);
        }
    }
    if (s_camera_last_photo_thumb_buf == NULL)
    {
        ui_unlock();
        return;
    }

    memcpy(s_camera_last_photo_thumb_buf, rgb888, data_size);

    s_camera_last_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_camera_last_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_camera_last_photo_dsc.header.flags = 0;
    s_camera_last_photo_dsc.header.w = CAMERA_UI_LAST_PHOTO_WIDTH;
    s_camera_last_photo_dsc.header.h = CAMERA_UI_LAST_PHOTO_HEIGHT;
    s_camera_last_photo_dsc.header.stride = CAMERA_UI_LAST_PHOTO_WIDTH * 3U;
    s_camera_last_photo_dsc.data_size = data_size;
    s_camera_last_photo_dsc.data = s_camera_last_photo_thumb_buf;

    lv_image_cache_drop(&s_camera_last_photo_dsc);
    lv_image_set_src(s_camera_ui.last_photo_img, &s_camera_last_photo_dsc);
    lv_obj_set_size(s_camera_ui.last_photo_img, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_center(s_camera_ui.last_photo_img);
    lv_obj_clear_flag(s_camera_ui.last_photo_img, LV_OBJ_FLAG_HIDDEN);
    if (s_camera_ui.last_photo_icon)
    {
        lv_obj_add_flag(s_camera_ui.last_photo_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_border_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_OK), 0);
    ui_unlock();
}

void camera_page_set_storage_ready(bool ready)
{
    ui_lock();
    if (s_camera_ui.storage_label)
    {
        lv_label_set_text(s_camera_ui.storage_label, ready ? "SD READY" : "NO SD");
        lv_obj_set_style_text_color(s_camera_ui.storage_label, lv_color_hex(ready ? UI_OK : UI_ERROR), 0);
    }
    ui_unlock();
}

void camera_page_set_preview_frame(const uint8_t *rgb888, uint32_t width, uint32_t height)
{
    static lv_image_dsc_t preview_dsc;

    if (rgb888 == NULL || width == 0 || height == 0)
    {
        return;
    }

    ui_lock();
    if (s_camera_ui.preview_img)
    {
        lv_obj_t *preview = s_camera_ui.preview_img;
        if (!lv_obj_check_type(preview, &lv_image_class))
        {
            ui_unlock();
            return;
        }

        preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        preview_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        preview_dsc.header.flags = 0;
        preview_dsc.header.w = width;
        preview_dsc.header.h = height;
        preview_dsc.header.stride = width * 3;
        preview_dsc.data_size = width * height * 3;
        preview_dsc.data = rgb888;

        lv_image_cache_drop(&preview_dsc);
        lv_image_set_src(preview, &preview_dsc);
        lv_obj_set_size(preview, width, height);
        lv_obj_set_pos(preview, 0, 0);
    }
    ui_unlock();
}

const char *camera_page_get_home_mode(void)
{
    return s_camera_home_mode;
}

int camera_page_get_home_fps_x10(void)
{
    return s_camera_home_fps_x10;
}

void camera_page_register(void)
{
    Page page = {
        .id = PAGE_CAMERA,
        .name = "camera",
        .on_create = camera_page_create,
        .on_enter = camera_page_enter,
        .on_leave = camera_page_leave,
        .on_destroy = camera_page_destroy,
        .on_gesture = camera_page_gesture,
    };
    ui_page_register(&page);
}
