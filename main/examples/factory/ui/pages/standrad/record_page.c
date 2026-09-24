#include "ui_internal.h"
#include "audio.h"
#include "record_page.h"

#define RECORD_MAIN_X 0
#define RECORD_MAIN_Y 40
#define RECORD_MAIN_W 720
#define RECORD_MAIN_H 680
#define RECORD_PANEL_X 40
#define RECORD_PANEL_W 640
#define RECORD_INPUT_PANEL_Y 20
#define RECORD_INPUT_PANEL_H 220
#define RECORD_WAVE_X 180
#define RECORD_WAVE_Y 70
#define RECORD_WAVE_W 440
#define RECORD_WAVE_H 160
#define RECORD_WAVE_MAX_BAR_H 108
#define RECORD_CONTROL_Y 278
#define RECORD_CONTROL_SIZE 80
#define RECORD_CONTROL_RECORD_X 40
#define RECORD_CONTROL_STOP_X 220
#define RECORD_CONTROL_PLAY_X 400
#define RECORD_CONTROL_SAVE_X 580
#define RECORD_FILE_LIST_Y 410
#define RECORD_FILE_LIST_H 240
#define RECORD_FILE_ROW_H 28
#define RECORD_FILE_ROW_GAP 3
#define RECORD_FILE_PAGE_SIZE 6
#define RECORD_FILE_SCAN_MAX 64
#define RECORD_FILE_NAME_MAX 256
#define RECORD_FILE_NAV_H 32

#define RECORD_WAVE_BAR_COUNT 48
#define RECORD_WAVE_MIN_BAR_H 4

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;
    lv_obj_t *mic_db_label[HOME_MIC_METER_COUNT];
    lv_obj_t *status_label;
    lv_obj_t *time_label;
    lv_obj_t *record_btn;
    lv_obj_t *stop_btn;
    lv_obj_t *play_btn;
    lv_obj_t *save_btn;
    lv_obj_t *record_icon;
    lv_obj_t *wave_view;
    uint8_t wave_level[RECORD_WAVE_BAR_COUNT];
    int displayed_mic_db[HOME_MIC_METER_COUNT];
    uint32_t displayed_time_tenth;
    int displayed_time_mode;
    lv_obj_t *file_list;
    lv_obj_t *file_rows[RECORD_FILE_PAGE_SIZE];
    lv_obj_t *file_page_label;
    uint16_t file_count;
    uint16_t file_page;
    bool recording_ui;
    bool playback_ui;
    bool controls_initialized;
    bool controls_recording;
    bool controls_playing;
    bool controls_paused;
    bool controls_available;
} record_page_ui_t;

typedef struct
{
    char name[RECORD_FILE_NAME_MAX];
    time_t modified;
} record_file_entry_t;

static const char *TAG = "[UI][record_page]";
static record_page_ui_t s_record_ui;
static record_file_entry_t s_record_files[RECORD_FILE_SCAN_MAX];
static void record_file_list_refresh(void);

static uint8_t record_db_to_level(int db)
{
    db = LV_CLAMP(MIC_DB_MIN, db, MIC_DB_MAX);
    return (uint8_t)(((db - MIC_DB_MIN) * 100) / (MIC_DB_MAX - MIC_DB_MIN));
}

static void record_wave_draw_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN)
    {
        return;
    }

    lv_obj_t *wave_view = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(wave_view, &coords);

    lv_draw_rect_dsc_t line_dsc;
    lv_draw_rect_dsc_init(&line_dsc);
    line_dsc.bg_color = lv_color_hex(UI_LINE);
    line_dsc.bg_opa = LV_OPA_50;
    lv_area_t line_area = {
        .x1 = coords.x1,
        .y1 = coords.y1 + lv_area_get_height(&coords) / 2,
        .x2 = coords.x2,
        .y2 = coords.y1 + lv_area_get_height(&coords) / 2,
    };
    lv_draw_rect(layer, &line_dsc, &line_area);

    lv_draw_rect_dsc_t bar_dsc;
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_opa = LV_OPA_80;
    bar_dsc.radius = LV_RADIUS_CIRCLE;
    const int32_t width = lv_area_get_width(&coords);
    const int32_t center_y = coords.y1 + lv_area_get_height(&coords) / 2;
    for (int i = 0; i < RECORD_WAVE_BAR_COUNT; i++)
    {
        uint32_t level = s_record_ui.wave_level[i];
        int32_t height = RECORD_WAVE_MIN_BAR_H +
                         (level * (RECORD_WAVE_MAX_BAR_H - RECORD_WAVE_MIN_BAR_H)) / 100;
        int32_t center_x = coords.x1 + (i * (width - 1) / (RECORD_WAVE_BAR_COUNT - 1));
        bar_dsc.bg_color = lv_color_hex(level >= 75 ? UI_ERROR
                                                    : (level >= 40 ? UI_SECONDARY : UI_PRIMARY));
        lv_area_t bar_area = {
            .x1 = center_x - 1,
            .y1 = center_y - height / 2,
            .x2 = center_x + 1,
            .y2 = center_y + (height - 1) / 2,
        };
        lv_draw_rect(layer, &bar_dsc, &bar_area);
    }
}

static void record_wave_render(void)
{
    if (s_record_ui.wave_view)
    {
        lv_obj_invalidate(s_record_ui.wave_view);
    }
}

static void record_wave_push(int mic0_db, int mic1_db)
{
    memmove(&s_record_ui.wave_level[0], &s_record_ui.wave_level[1],
            sizeof(s_record_ui.wave_level) - sizeof(s_record_ui.wave_level[0]));
    int peak_db = mic0_db > mic1_db ? mic0_db : mic1_db;
    s_record_ui.wave_level[RECORD_WAVE_BAR_COUNT - 1] = record_db_to_level(peak_db);
    record_wave_render();
}

static void record_button_set_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled)
    {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_COVER, 0);
    }
    else
    {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, 0);
    }
}

static void record_controls_update(void)
{
    bool recording = audio_record_is_active();
    bool playing = audio_record_playback_is_active();
    bool paused = recording ? audio_record_is_paused()
                            : audio_record_playback_is_paused();
    bool available = audio_record_available();
    bool busy = recording || playing;

    if (s_record_ui.controls_initialized &&
        s_record_ui.controls_recording == recording &&
        s_record_ui.controls_playing == playing &&
        s_record_ui.controls_paused == paused &&
        s_record_ui.controls_available == available)
    {
        return;
    }

    s_record_ui.controls_initialized = true;
    s_record_ui.controls_recording = recording;
    s_record_ui.controls_playing = playing;
    s_record_ui.controls_paused = paused;
    s_record_ui.controls_available = available;

    record_button_set_enabled(s_record_ui.record_btn, !playing);
    record_button_set_enabled(s_record_ui.stop_btn, busy);
    record_button_set_enabled(s_record_ui.play_btn, !recording &&
                                                        (playing || audio_record_available()));
    record_button_set_enabled(s_record_ui.save_btn, !busy && audio_record_available());
    if (s_record_ui.record_icon)
    {
        lv_label_set_text(s_record_ui.record_icon,
                          recording ? (paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE)
                                    : LV_SYMBOL_AUDIO);
    }
}

static lv_obj_t *record_control_button_create(lv_obj_t *parent, int32_t x,
                                              const char *symbol,
                                              uint32_t color,
                                              lv_event_cb_t event_cb,
                                              lv_obj_t **icon_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, RECORD_CONTROL_Y);
    lv_obj_set_size(button, RECORD_CONTROL_SIZE, RECORD_CONTROL_SIZE);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(button, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(button, 20, 0);
    lv_obj_set_style_shadow_spread(button, 3, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = ui_label_create(button, symbol, &lv_font_montserrat_28, color);
    lv_obj_center(icon);
    if (icon_out)
    {
        *icon_out = icon;
    }
    return button;
}

static void record_control_caption_create(lv_obj_t *parent, int32_t x,
                                          const char *text, uint32_t color)
{
    lv_obj_t *caption = ui_label_create(parent, text, &lv_font_montserrat_14, color);
    lv_obj_set_pos(caption, x - 10, RECORD_CONTROL_Y + RECORD_CONTROL_SIZE + 8);
    lv_obj_set_size(caption, RECORD_CONTROL_SIZE + 20, 22);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
}

static void record_toggle_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        audio_record_playback_is_active())
    {
        return;
    }

    if (audio_record_is_active())
    {
        bool paused = audio_record_is_paused();
        esp_err_t ret = paused ? audio_record_resume() : audio_record_pause();
        if (ret == ESP_OK)
        {
            lv_label_set_text(s_record_ui.status_label, paused ? "RECORDING" : "PAUSED");
        }
        record_controls_update();
        return;
    }

    char path[64] = {0};
    esp_err_t ret = audio_record_start(path, sizeof(path));
    if (ret == ESP_OK)
    {
        memset(s_record_ui.wave_level, 0, sizeof(s_record_ui.wave_level));
        record_wave_render();
        s_record_ui.recording_ui = true;
        s_record_ui.playback_ui = false;
        lv_label_set_text(s_record_ui.status_label, "RECORDING");
    }
    else
    {
        ESP_LOGE(TAG, "Start recording failed: %s", esp_err_to_name(ret));
        lv_label_set_text(s_record_ui.status_label, "RECORD FAILED");
    }
    record_controls_update();
}

static void record_stop_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (audio_record_is_active() || s_record_ui.recording_ui)
    {
        esp_err_t ret = audio_record_stop();
        s_record_ui.recording_ui = false;
        lv_label_set_text(s_record_ui.status_label,
                          ret == ESP_OK ? "RECORDING READY" : "STOP FAILED");
    }
    else if (audio_record_playback_is_active())
    {
        audio_record_stop_playback();
        s_record_ui.playback_ui = false;
        lv_label_set_text(s_record_ui.status_label, "PLAYBACK STOPPED");
    }
    record_controls_update();
}

static void record_play_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (audio_record_playback_is_active())
    {
        audio_record_stop_playback();
        s_record_ui.playback_ui = false;
        lv_label_set_text(s_record_ui.status_label, "PLAYBACK STOPPED");
    }
    else if (audio_record_available())
    {
        esp_err_t ret = audio_record_play_file(AUDIO_RECORD_MEMORY_PATH);
        if (ret == ESP_OK)
        {
            s_record_ui.playback_ui = true;
            lv_label_set_text(s_record_ui.status_label, "PLAYING");
        }
        else
        {
            ESP_LOGE(TAG, "Play recording failed: %s", esp_err_to_name(ret));
            lv_label_set_text(s_record_ui.status_label, "PLAYBACK FAILED");
        }
    }
    record_controls_update();
}

static void record_save_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        audio_record_is_active() || audio_record_playback_is_active())
    {
        return;
    }

    char path[160] = {0};
    esp_err_t ret = audio_record_save_to_sd(path, sizeof(path));
    if (ret == ESP_OK)
    {
        lv_label_set_text(s_record_ui.status_label, "SAVED TO SD");
        s_record_ui.file_page = 0;
        record_file_list_refresh();
    }
    else
    {
        lv_label_set_text(s_record_ui.status_label,
                          ret == ESP_ERR_NOT_FOUND ? "NO RECORDING" : "SAVE FAILED");
    }
    record_controls_update();
}

static void record_wave_panel_create(lv_obj_t *parent)
{
    lv_obj_t *input_panel = lv_obj_create(parent);
    lv_obj_set_pos(input_panel, RECORD_PANEL_X, RECORD_INPUT_PANEL_Y);
    lv_obj_set_size(input_panel, RECORD_PANEL_W, RECORD_INPUT_PANEL_H);
    lv_obj_set_style_bg_color(input_panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(input_panel, LV_OPA_50, 0);
    lv_obj_set_style_border_width(input_panel, 0, 0);
    lv_obj_set_style_border_color(input_panel, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(input_panel, LV_OPA_50, 0);
    lv_obj_set_style_radius(input_panel, 8, 0);
    lv_obj_set_style_shadow_color(input_panel, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_shadow_width(input_panel, 18, 0);
    lv_obj_set_style_shadow_opa(input_panel, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(input_panel, 0, 0);
    lv_obj_remove_flag(input_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sd_status_label = ui_label_create(input_panel, "Status:", &lv_font_SourceHanSansSC_Regular_2_24, UI_MUTED);
    lv_obj_set_pos(sd_status_label, 10, 12);
    lv_obj_set_width(sd_status_label, 170);

    s_record_ui.status_label = ui_label_create(input_panel, audio_record_available() ? "RECORDING READY" : "READY TO RECORD", &lv_font_SourceHanSansSC_Regular_2_20, UI_TEXT);
    lv_obj_align(s_record_ui.status_label, LV_ALIGN_TOP_RIGHT, -20, 12);
    lv_obj_set_width(s_record_ui.status_label, 240);

    lv_obj_t *mic1_label = ui_label_create(input_panel, "Mic1 Input", &lv_font_SourceHanSansSC_Regular_2_16, UI_MUTED);
    lv_obj_set_pos(mic1_label, 10, 60);
    lv_obj_set_width(mic1_label, 170);

    s_record_ui.mic_db_label[0] = ui_label_create(input_panel, "-- dB", &lv_font_SourceHanSansSC_Regular_2_24, UI_TEXT);
    lv_obj_set_pos(s_record_ui.mic_db_label[0], 10, 100);
    lv_obj_set_width(s_record_ui.mic_db_label[0], 170);

    lv_obj_t *mic2_label = ui_label_create(input_panel, "Mic2 Input", &lv_font_SourceHanSansSC_Regular_2_16, UI_MUTED);
    lv_obj_set_pos(mic2_label, 10, 140);
    lv_obj_set_width(mic2_label, 170);

    s_record_ui.mic_db_label[1] = ui_label_create(input_panel, "-- dB", &lv_font_SourceHanSansSC_Regular_2_24, UI_TEXT);
    lv_obj_set_pos(s_record_ui.mic_db_label[1], 10, 180);
    lv_obj_set_width(s_record_ui.mic_db_label[1], 170);

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, RECORD_WAVE_X, RECORD_WAVE_Y);
    lv_obj_set_size(panel, RECORD_WAVE_W, RECORD_WAVE_H);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(panel, 12, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_record_ui.time_label = ui_label_create(panel, "00:00.0",
                                             &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_size(s_record_ui.time_label, 130, 24);
    lv_obj_align(s_record_ui.time_label, LV_ALIGN_TOP_RIGHT, -14, 8);
    lv_obj_set_style_text_align(s_record_ui.time_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_record_ui.wave_view = lv_obj_create(panel);
    lv_obj_set_size(s_record_ui.wave_view, RECORD_WAVE_W - 20, RECORD_WAVE_MAX_BAR_H);
    lv_obj_set_pos(s_record_ui.wave_view, 10, 42);
    ui_obj_set_transparent(s_record_ui.wave_view);
    lv_obj_remove_flag(s_record_ui.wave_view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_record_ui.wave_view, record_wave_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
    record_wave_render();
}

static int record_file_compare(const void *lhs, const void *rhs)
{
    const record_file_entry_t *a = (const record_file_entry_t *)lhs;
    const record_file_entry_t *b = (const record_file_entry_t *)rhs;
    if (a->modified != b->modified)
    {
        return a->modified < b->modified ? 1 : -1;
    }
    return strcasecmp(b->name, a->name);
}

static bool record_file_path_build(uint16_t index, char *path, size_t path_size)
{
    if (index >= s_record_ui.file_count || path == NULL || path_size == 0)
    {
        return false;
    }
    int written = snprintf(path, path_size, AUDIO_RECORD_SAVE_DIR "/%s",
                           s_record_files[index].name);
    return written > 0 && (size_t)written < path_size;
}

static void record_file_refresh_async(void *user_data)
{
    (void)user_data;
    record_file_list_refresh();
}

static void record_file_play_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (audio_record_is_active())
    {
        lv_label_set_text(s_record_ui.status_label, "STOP RECORDING FIRST");
        return;
    }
    if (audio_record_playback_is_active())
    {
        audio_record_stop_playback();
        s_record_ui.playback_ui = false;
        lv_label_set_text(s_record_ui.status_label, "PLAYBACK STOPPED");
        return;
    }

    uint16_t index = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
    char path[AUDIO_MUSIC_PATH_MAX_LEN] = {0};
    if (!record_file_path_build(index, path, sizeof(path)))
    {
        return;
    }
    esp_err_t ret = audio_record_play_file(path);
    if (ret == ESP_OK)
    {
        s_record_ui.playback_ui = true;
        lv_label_set_text(s_record_ui.status_label, "PLAYING SAVED FILE");
    }
    else
    {
        lv_label_set_text(s_record_ui.status_label, "PLAYBACK FAILED");
    }
    s_record_ui.controls_initialized = false;
    record_controls_update();
}

static void record_file_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (audio_record_is_active() || audio_record_playback_is_active())
    {
        lv_label_set_text(s_record_ui.status_label, "STOP BEFORE DELETE");
        return;
    }

    uint16_t index = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
    char path[AUDIO_MUSIC_PATH_MAX_LEN] = {0};
    if (!record_file_path_build(index, path, sizeof(path)))
    {
        return;
    }
    if (remove(path) == 0)
    {
        lv_label_set_text(s_record_ui.status_label, "RECORDING DELETED");
        lv_async_call(record_file_refresh_async, NULL);
    }
    else
    {
        lv_label_set_text(s_record_ui.status_label, "DELETE FAILED");
    }
}

static lv_obj_t *record_file_icon_button_create(lv_obj_t *parent, const char *symbol,
                                                uint32_t color, int32_t x,
                                                lv_event_cb_t event_cb, uint16_t index)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 34, 26);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, x, 0);
    lv_obj_set_style_radius(button, 13, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_t *icon = ui_label_create(button, symbol, &lv_font_montserrat_14, color);
    lv_obj_center(icon);
    return button;
}

static void record_file_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    int next_page = (int)s_record_ui.file_page + step;
    if (next_page < 0)
    {
        next_page = 0;
    }
    s_record_ui.file_page = (uint16_t)next_page;
    lv_async_call(record_file_refresh_async, NULL);
}

static lv_obj_t *record_file_page_button_create(lv_obj_t *parent, const char *symbol,
                                                int step, bool enabled)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 80, 30);
    lv_obj_set_style_radius(button, 15, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, record_file_page_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)step);
    if (!enabled)
    {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_30, 0);
    }
    lv_obj_t *label = ui_label_create(button, symbol, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(label);
    return button;
}

static void record_file_list_refresh(void)
{
    if (s_record_ui.file_list == NULL)
    {
        return;
    }

    s_record_ui.file_count = 0;
    DIR *dir = opendir(AUDIO_RECORD_SAVE_DIR);
    if (dir != NULL)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL &&
               s_record_ui.file_count < RECORD_FILE_SCAN_MAX)
        {
            const char *dot = strrchr(entry->d_name, '.');
            if (entry->d_name[0] == '.' || dot == NULL ||
                strcasecmp(dot, ".wav") != 0)
            {
                continue;
            }

            record_file_entry_t *file = &s_record_files[s_record_ui.file_count];
            char path[AUDIO_MUSIC_PATH_MAX_LEN] = {0};
            snprintf(path, sizeof(path), AUDIO_RECORD_SAVE_DIR "/%s", entry->d_name);
            struct stat st = {0};
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            {
                continue;
            }
            snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
            file->modified = st.st_mtime;
            s_record_ui.file_count++;
        }
        closedir(dir);
    }

    qsort(s_record_files, s_record_ui.file_count, sizeof(s_record_files[0]),
          record_file_compare);
    uint16_t page_count = s_record_ui.file_count == 0 ? 1 :
                          (uint16_t)((s_record_ui.file_count + RECORD_FILE_PAGE_SIZE - 1) /
                                     RECORD_FILE_PAGE_SIZE);
    if (s_record_ui.file_page >= page_count)
    {
        s_record_ui.file_page = page_count - 1;
    }

    lv_obj_clean(s_record_ui.file_list);
    memset(s_record_ui.file_rows, 0, sizeof(s_record_ui.file_rows));
    s_record_ui.file_page_label = NULL;

    uint16_t first = s_record_ui.file_page * RECORD_FILE_PAGE_SIZE;
    uint16_t visible = s_record_ui.file_count > first ? s_record_ui.file_count - first : 0;
    if (visible > RECORD_FILE_PAGE_SIZE)
    {
        visible = RECORD_FILE_PAGE_SIZE;
    }
    for (uint16_t row_index = 0; row_index < visible; row_index++)
    {
        uint16_t file_index = first + row_index;
        lv_obj_t *row = lv_obj_create(s_record_ui.file_list);
        s_record_ui.file_rows[row_index] = row;
        lv_obj_set_size(row, 590, RECORD_FILE_ROW_H);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_PANEL_HL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = ui_label_create(row, s_record_files[file_index].name,
                                         &lv_font_montserrat_12, UI_TEXT);
        lv_obj_set_width(name, 470);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        record_file_icon_button_create(row, LV_SYMBOL_PLAY, UI_PRIMARY, -38,
                                       record_file_play_event_cb, file_index);
        record_file_icon_button_create(row, LV_SYMBOL_TRASH, UI_ERROR, 0,
                                       record_file_delete_event_cb, file_index);
    }

    if (s_record_ui.file_count == 0)
    {
        lv_obj_t *empty = ui_label_create(s_record_ui.file_list,
                                          "NO SAVED RECORDINGS",
                                          &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    lv_obj_t *navigation = ui_flex_container_create(s_record_ui.file_list, 590, RECORD_FILE_NAV_H,
                                                     LV_FLEX_FLOW_ROW,
                                                     LV_FLEX_ALIGN_SPACE_BETWEEN,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(navigation, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(navigation, 8, RECORD_FILE_LIST_H - 8 - RECORD_FILE_NAV_H);
    record_file_page_button_create(navigation, LV_SYMBOL_LEFT, -1,
                                   s_record_ui.file_page > 0);
    s_record_ui.file_page_label = ui_label_create(navigation, "", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(s_record_ui.file_page_label, 150);
    lv_obj_set_style_text_align(s_record_ui.file_page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(s_record_ui.file_page_label, "%u / %u   %u FILES",
                          (unsigned)(s_record_ui.file_page + 1),
                          (unsigned)page_count,
                          (unsigned)s_record_ui.file_count);
    record_file_page_button_create(navigation, LV_SYMBOL_RIGHT, 1,
                                   s_record_ui.file_page + 1 < page_count);
}

static void record_page_create(Page *page)
{
    memset(&s_record_ui, 0, sizeof(s_record_ui));
    for (int i = 0; i < HOME_MIC_METER_COUNT; i++)
    {
        s_record_ui.displayed_mic_db[i] = INT32_MIN;
    }
    s_record_ui.displayed_time_tenth = UINT32_MAX;
    s_record_ui.displayed_time_mode = -1;
    s_record_ui.root = page->root;

    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(page->root);
    status_bar_create();

    s_record_ui.main_cont = lv_obj_create(page->root);
    lv_obj_set_pos(s_record_ui.main_cont, RECORD_MAIN_X, RECORD_MAIN_Y);
    lv_obj_set_size(s_record_ui.main_cont, RECORD_MAIN_W, RECORD_MAIN_H);
    ui_obj_set_transparent(s_record_ui.main_cont);
    lv_obj_remove_flag(s_record_ui.main_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    record_wave_panel_create(s_record_ui.main_cont);

    s_record_ui.record_btn = record_control_button_create(
        s_record_ui.main_cont, RECORD_CONTROL_RECORD_X, ".", UI_TEXT,
        record_toggle_event_cb, &s_record_ui.record_icon);
    record_control_caption_create(s_record_ui.main_cont, RECORD_CONTROL_RECORD_X,
                                  "RECORD", UI_MUTED);

    s_record_ui.stop_btn = record_control_button_create(
        s_record_ui.main_cont, RECORD_CONTROL_STOP_X, LV_SYMBOL_STOP, UI_TEXT,
        record_stop_event_cb, NULL);
    record_control_caption_create(s_record_ui.main_cont, RECORD_CONTROL_STOP_X,
                                  "STOP", UI_MUTED);

    s_record_ui.play_btn = record_control_button_create(
        s_record_ui.main_cont, RECORD_CONTROL_PLAY_X, LV_SYMBOL_PLAY, UI_TEXT,
        record_play_event_cb, NULL);
    record_control_caption_create(s_record_ui.main_cont, RECORD_CONTROL_PLAY_X,
                                  "PLAY", UI_MUTED);

    s_record_ui.save_btn = record_control_button_create(
        s_record_ui.main_cont, RECORD_CONTROL_SAVE_X, LV_SYMBOL_ENVELOPE, UI_TEXT,
        record_save_event_cb, NULL);
    record_control_caption_create(s_record_ui.main_cont, RECORD_CONTROL_SAVE_X,
                                  "SAVE", UI_MUTED);

    s_record_ui.file_list = lv_obj_create(s_record_ui.main_cont);
    lv_obj_set_pos(s_record_ui.file_list, RECORD_PANEL_X, RECORD_FILE_LIST_Y);
    lv_obj_set_size(s_record_ui.file_list, RECORD_PANEL_W, RECORD_FILE_LIST_H);
    lv_obj_set_style_bg_color(s_record_ui.file_list, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_record_ui.file_list, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_record_ui.file_list, 2, 0);
    lv_obj_set_style_border_color(s_record_ui.file_list, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_record_ui.file_list, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_record_ui.file_list, 40, 0);
    lv_obj_set_style_shadow_color(s_record_ui.file_list, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_shadow_width(s_record_ui.file_list, 20, 0);
    lv_obj_set_style_shadow_spread(s_record_ui.file_list, 3, 0);
    lv_obj_set_style_shadow_opa(s_record_ui.file_list, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_record_ui.file_list, 8, 0);
    lv_obj_set_style_pad_row(s_record_ui.file_list, RECORD_FILE_ROW_GAP, 0);
    ui_obj_set_flex(s_record_ui.file_list, LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_record_ui.file_list, LV_OBJ_FLAG_SCROLLABLE);
    record_file_list_refresh();

    record_controls_update();
}

static void record_page_timer(Page *page)
{
    (void)page;
    int mic0 = MIC_DB_MIN;
    int mic1 = MIC_DB_MIN;
    audio_get_mic_levels(&mic0, &mic1);
    if (s_record_ui.displayed_mic_db[0] != mic0)
    {
        s_record_ui.displayed_mic_db[0] = mic0;
        lv_label_set_text_fmt(s_record_ui.mic_db_label[0], "%d dB", mic0);
    }
    if (s_record_ui.displayed_mic_db[1] != mic1)
    {
        s_record_ui.displayed_mic_db[1] = mic1;
        lv_label_set_text_fmt(s_record_ui.mic_db_label[1], "%d dB", mic1);
    }

    if (audio_record_is_active() && !audio_record_is_paused())
    {
        record_wave_push(mic0, mic1);
    }

    if (s_record_ui.recording_ui && !audio_record_is_active())
    {
        (void)audio_record_stop();
        s_record_ui.recording_ui = false;
        lv_label_set_text(s_record_ui.status_label, "10 SEC RECORDING SAVED");
    }
    if (s_record_ui.playback_ui && !audio_record_playback_is_active())
    {
        s_record_ui.playback_ui = false;
        lv_label_set_text(s_record_ui.status_label, "PLAYBACK FINISHED");
    }

    bool recording = audio_record_is_active();
    bool playing = audio_record_playback_is_active();
    int time_mode = playing ? 2 : (recording ? 1 : 0);
    if (time_mode == 0)
    {
        if (s_record_ui.displayed_time_mode != 0)
        {
            s_record_ui.displayed_time_mode = 0;
            s_record_ui.displayed_time_tenth = UINT32_MAX;
            lv_label_set_text(s_record_ui.time_label, "00:00.0");
        }
    }
    else
    {
        uint32_t elapsed_ms = playing ? audio_record_playback_elapsed_ms()
                                      : audio_record_duration_ms();
        if (elapsed_ms > AUDIO_RECORD_MAX_DURATION_MS)
        {
            elapsed_ms = AUDIO_RECORD_MAX_DURATION_MS;
        }
        uint32_t elapsed_tenth = elapsed_ms / 100U;
        if (s_record_ui.displayed_time_mode != time_mode ||
            s_record_ui.displayed_time_tenth != elapsed_tenth)
        {
            s_record_ui.displayed_time_mode = time_mode;
            s_record_ui.displayed_time_tenth = elapsed_tenth;
            lv_label_set_text_fmt(s_record_ui.time_label, "%02" PRIu32 ":%02" PRIu32 ".%01" PRIu32,
                                  elapsed_tenth / 600U,
                                  (elapsed_tenth / 10U) % 60U,
                                  elapsed_tenth % 10U);
        }
    }
    record_controls_update();
}

static void record_page_leave(Page *page)
{
    if (audio_record_is_active() || s_record_ui.recording_ui)
    {
        (void)audio_record_stop();
    }
    audio_record_stop_playback();
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void record_page_destroy(Page *page)
{
    (void)page;
    memset(&s_record_ui, 0, sizeof(s_record_ui));
}

void record_page_register(void)
{
    Page page = {
        .id = PAGE_RECORD,
        .name = "record",
        .on_create = record_page_create,
        .on_leave = record_page_leave,
        .on_destroy = record_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = record_page_timer,
        .timer_interval_ms = 30,
    };
    ui_page_register(&page);
}
