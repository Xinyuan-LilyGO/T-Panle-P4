#include "ui_internal.h"
#include "camera_page.h"

static const char *TAG = "[UI][camera_round]";

#define ROUND_CAMERA_PREVIEW_W 480
#define ROUND_CAMERA_PREVIEW_H 270
#define ROUND_CAMERA_PREVIEW_X 15
#define ROUND_CAMERA_PREVIEW_Y 110
#define ROUND_CAMERA_SHUTTER_SIZE 80
#define ROUND_CAMERA_SHUTTER_X 630
#define ROUND_CAMERA_SHUTTER_Y 310
#define ROUND_CAMERA_INFO_Y 388
#define ROUND_CAMERA_THUMB_Y 426

static camera_page_ui_t s_camera_ui;
static uint8_t *s_camera_last_photo_thumb_buf;
static lv_image_dsc_t s_camera_last_photo_dsc;
static lv_image_dsc_t s_camera_preview_dsc;
static lv_timer_t *s_camera_state_timer;
static uint8_t s_camera_thumb_poll_buf[CAMERA_UI_LAST_PHOTO_WIDTH * CAMERA_UI_LAST_PHOTO_HEIGHT * 3U];
static uint32_t s_camera_thumb_generation;
static char s_camera_home_mode[24] = "720P";
static int s_camera_home_fps_x10 = 250;

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

    const char *status = "SAVE ERR";
    if (ret == ESP_ERR_NO_MEM)
    {
        status = "NO MEM";
    }
    else if (ret == ESP_ERR_TIMEOUT)
    {
        status = "BUSY";
    }
    else if (ret == ESP_ERR_INVALID_STATE)
    {
        status = "WAIT";
    }
    camera_page_set_status(status, false);
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

static lv_obj_t *camera_info_label_create(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *label = ui_label_create(parent, text, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void camera_page_create(Page *page)
{
    s_camera_ui.root = page->root;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(page->root);

    s_camera_ui.main_cont = lv_obj_create(page->root);
    ui_main_cont_style_init(s_camera_ui.main_cont);
    lv_obj_set_layout(s_camera_ui.main_cont, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(s_camera_ui.main_cont, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.main_cont, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_camera_ui.main_cont, 1, 0);
    lv_obj_set_style_border_color(s_camera_ui.main_cont, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_camera_ui.main_cont, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_camera_ui.main_cont, 20, 0);

    s_camera_ui.status_label = ui_label_create(s_camera_ui.main_cont, "READY", &lv_font_montserrat_16, UI_OK);
    lv_obj_set_width(s_camera_ui.status_label, 160);
    lv_obj_set_pos(s_camera_ui.status_label, 175, 36);
    lv_obj_set_style_text_align(s_camera_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);

    s_camera_ui.preview_cont = lv_obj_create(s_camera_ui.main_cont);
    lv_obj_set_pos(s_camera_ui.preview_cont, ROUND_CAMERA_PREVIEW_X,
                   ROUND_CAMERA_PREVIEW_Y);
    lv_obj_set_size(s_camera_ui.preview_cont, ROUND_CAMERA_PREVIEW_W,
                    ROUND_CAMERA_PREVIEW_H);
    lv_obj_set_style_bg_color(s_camera_ui.preview_cont, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.preview_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.preview_cont, 3, 0);
    lv_obj_set_style_border_color(s_camera_ui.preview_cont, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_radius(s_camera_ui.preview_cont, 10, 0);
    lv_obj_set_style_pad_all(s_camera_ui.preview_cont, 3, 0);
    lv_obj_remove_flag(s_camera_ui.preview_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.preview_img = lv_image_create(s_camera_ui.preview_cont);
    lv_obj_set_size(s_camera_ui.preview_img, ROUND_CAMERA_PREVIEW_W,
                    ROUND_CAMERA_PREVIEW_H);
    lv_obj_center(s_camera_ui.preview_img);

    s_camera_ui.preview_icon = ui_label_create(s_camera_ui.preview_cont, LV_SYMBOL_IMAGE,
                                               &lv_font_montserrat_28, UI_MUTED);
    lv_obj_center(s_camera_ui.preview_icon);

    lv_obj_t *info = ui_flex_container_create(s_camera_ui.main_cont, 460, 30, LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(info, 25, ROUND_CAMERA_INFO_Y);
    lv_obj_set_style_pad_column(info, 8, 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_40, 0);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_border_color(info, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(info, LV_OPA_20, 0);
    lv_obj_set_style_radius(info, 15, 0);
    lv_obj_set_style_shadow_width(info, 12, 0);
    lv_obj_set_style_shadow_color(info, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(info, LV_OPA_20, 0);

    s_camera_ui.resolution_label = camera_info_label_create(info, "720P", 90);
    s_camera_ui.fps_label = camera_info_label_create(info, "--.- FPS", 100);
    s_camera_ui.shot_count_label = camera_info_label_create(info, "SHOT 000", 100);
    s_camera_ui.storage_label = camera_info_label_create(info, "SD --", 90);

    s_camera_ui.last_photo_btn = lv_button_create(s_camera_ui.main_cont);
    lv_obj_set_pos(s_camera_ui.last_photo_btn, 191, ROUND_CAMERA_THUMB_Y);
    lv_obj_set_size(s_camera_ui.last_photo_btn, CAMERA_UI_LAST_PHOTO_WIDTH,
                    CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_set_style_radius(s_camera_ui.last_photo_btn, 8, 0);
    lv_obj_set_style_bg_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.last_photo_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_camera_ui.last_photo_btn, 2, 0);
    lv_obj_set_style_border_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_border_opa(s_camera_ui.last_photo_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_camera_ui.last_photo_btn, 12, 0);
    lv_obj_set_style_shadow_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(s_camera_ui.last_photo_btn, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(s_camera_ui.last_photo_btn, 0, 0);

    s_camera_ui.last_photo_img = lv_image_create(s_camera_ui.last_photo_btn);
    lv_obj_add_flag(s_camera_ui.last_photo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_camera_ui.last_photo_img);
    s_camera_ui.last_photo_icon = ui_label_create(s_camera_ui.last_photo_btn, LV_SYMBOL_IMAGE,
                                                  &lv_font_montserrat_24, UI_SECONDARY);
    lv_obj_center(s_camera_ui.last_photo_icon);

    s_camera_ui.shutter_btn = lv_button_create(page->root);
    lv_obj_set_size(s_camera_ui.shutter_btn, ROUND_CAMERA_SHUTTER_SIZE,
                    ROUND_CAMERA_SHUTTER_SIZE);
    lv_obj_set_pos(s_camera_ui.shutter_btn, ROUND_CAMERA_SHUTTER_X,
                   ROUND_CAMERA_SHUTTER_Y);
    lv_obj_set_style_radius(s_camera_ui.shutter_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_camera_ui.shutter_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_border_width(s_camera_ui.shutter_btn, 6, 0);
    lv_obj_set_style_border_color(s_camera_ui.shutter_btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_shadow_width(s_camera_ui.shutter_btn, 18, 0);
    lv_obj_set_style_shadow_color(s_camera_ui.shutter_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_add_event_cb(s_camera_ui.shutter_btn, camera_shutter_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *shutter_icon = ui_label_create(s_camera_ui.shutter_btn, LV_SYMBOL_IMAGE,
                                             &lv_font_montserrat_28, UI_BG);
    lv_obj_center(shutter_icon);
}

static void camera_page_enter(Page *page)
{
    (void)page;
    if (s_camera_ui.preview_img)
    {
        lv_area_t area;
        lv_obj_update_layout(lv_screen_active());
        lv_obj_get_coords(s_camera_ui.preview_img, &area);
        camera_preview_set_area(area.x1, area.y1, ROUND_CAMERA_PREVIEW_W,
                                ROUND_CAMERA_PREVIEW_H);
    }
    camera_page_set_storage_ready(camera_storage_is_ready());
    camera_preview_set_direct_crop(false);
    camera_preview_set_frame_callback(camera_page_set_preview_frame);
    camera_preview_start(1280, 720);
    s_camera_thumb_generation = 0;
    if (s_camera_state_timer == NULL)
    {
        s_camera_state_timer = lv_timer_create(camera_state_timer_cb, 250, NULL);
    }
}

static void camera_page_leave(Page *page)
{
    camera_preview_set_frame_callback(NULL);
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
    camera_preview_set_frame_callback(NULL);
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
    }
    memset(&s_camera_last_photo_dsc, 0, sizeof(s_camera_last_photo_dsc));
    memset(&s_camera_preview_dsc, 0, sizeof(s_camera_preview_dsc));
    memset(&s_camera_ui, 0, sizeof(s_camera_ui));
}

static void camera_page_gesture(Page *page, GestureDirection direction)
{
    ESP_LOGI(TAG, "camera page gesture: page=%d direction=%d", page->id, direction);
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
        lv_obj_set_style_text_color(s_camera_ui.status_label,
                                    lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
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
        lv_label_set_text(s_camera_ui.resolution_label, s_camera_home_mode);
    }
    if (s_camera_ui.fps_label)
    {
        if (fps_x10 > 0)
        {
            lv_label_set_text_fmt(s_camera_ui.fps_label, "%" PRIu32 ".%" PRIu32 " FPS",
                                  fps_x10 / 10, fps_x10 % 10);
        }
        else
        {
            lv_label_set_text(s_camera_ui.fps_label, "--.- FPS");
        }
    }
    ui_unlock();
}

void camera_page_set_last_photo(const uint8_t *rgb888, uint32_t width, uint32_t height)
{
    if (!rgb888 || width != CAMERA_UI_LAST_PHOTO_WIDTH ||
        height != CAMERA_UI_LAST_PHOTO_HEIGHT)
    {
        return;
    }

    ui_lock();
    if (!s_camera_ui.last_photo_img)
    {
        ui_unlock();
        return;
    }

    const size_t data_size = (size_t)width * height * 3U;
    if (!s_camera_last_photo_thumb_buf)
    {
        s_camera_last_photo_thumb_buf = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_camera_last_photo_thumb_buf)
        {
            s_camera_last_photo_thumb_buf = heap_caps_malloc(data_size, MALLOC_CAP_DEFAULT);
        }
    }
    if (!s_camera_last_photo_thumb_buf)
    {
        ui_unlock();
        return;
    }

    memcpy(s_camera_last_photo_thumb_buf, rgb888, data_size);
    s_camera_last_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_camera_last_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_camera_last_photo_dsc.header.w = width;
    s_camera_last_photo_dsc.header.h = height;
    s_camera_last_photo_dsc.header.stride = width * 3U;
    s_camera_last_photo_dsc.data_size = data_size;
    s_camera_last_photo_dsc.data = s_camera_last_photo_thumb_buf;
    lv_image_cache_drop(&s_camera_last_photo_dsc);
    lv_image_set_src(s_camera_ui.last_photo_img, &s_camera_last_photo_dsc);
    lv_obj_set_size(s_camera_ui.last_photo_img, width, height);
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
        lv_obj_set_style_text_color(s_camera_ui.storage_label,
                                    lv_color_hex(ready ? UI_OK : UI_ERROR), 0);
    }
    ui_unlock();
}

void camera_page_set_preview_frame(const uint8_t *pixels, uint32_t width, uint32_t height)
{
    if (!pixels || width == 0 || height == 0)
    {
        return;
    }

    ui_lock();
    if (s_camera_ui.preview_img)
    {
#if CONFIG_DISPLAY_PANEL_RGB565
        const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565;
        const uint32_t bytes_per_pixel = 2U;
#else
        const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB888;
        const uint32_t bytes_per_pixel = 3U;
#endif
        const uint32_t stride = width * bytes_per_pixel;
        const bool descriptor_changed = s_camera_preview_dsc.data == NULL ||
                                        s_camera_preview_dsc.header.cf != color_format ||
                                        s_camera_preview_dsc.header.w != width ||
                                        s_camera_preview_dsc.header.h != height ||
                                        s_camera_preview_dsc.header.stride != stride;

        s_camera_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_camera_preview_dsc.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        s_camera_preview_dsc.header.cf = color_format;
        s_camera_preview_dsc.header.w = width;
        s_camera_preview_dsc.header.h = height;
        s_camera_preview_dsc.header.stride = stride;
        s_camera_preview_dsc.data_size = (size_t)width * height * bytes_per_pixel;
        s_camera_preview_dsc.data = pixels;
        if (descriptor_changed)
        {
            lv_image_set_src(s_camera_ui.preview_img, &s_camera_preview_dsc);
        }
        else
        {
            lv_obj_invalidate(s_camera_ui.preview_img);
        }
        if (s_camera_ui.preview_icon)
        {
            lv_obj_add_flag(s_camera_ui.preview_icon, LV_OBJ_FLAG_HIDDEN);
        }
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
        .name = "camera_round",
        .on_create = camera_page_create,
        .on_enter = camera_page_enter,
        .on_leave = camera_page_leave,
        .on_destroy = camera_page_destroy,
        .on_gesture = camera_page_gesture,
    };
    ui_page_register(&page);
}
