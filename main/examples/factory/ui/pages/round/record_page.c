#include "ui_internal.h"
#include "audio.h"
#include "record_page.h"

#define ROUND_RECORD_FILE_PAGE_SIZE 5
#define ROUND_RECORD_FILE_SCAN_MAX 64
#define ROUND_RECORD_FILE_NAME_MAX 256
#define ROUND_RECORD_FILE_ROW_H 24
#define ROUND_RECORD_FILE_ROW_W 330
#define ROUND_RECORD_FILE_ROW_GAP 3
#define ROUND_RECORD_PAGE_TIMER_MS 100
#define ROUND_RECORD_FILE_RETRY_MS 500
#define ROUND_RECORD_STATUS_MESSAGE_MS 2000

typedef struct
{
    char name[ROUND_RECORD_FILE_NAME_MAX];
    time_t modified;
    uint32_t duration_seconds;
} round_record_file_entry_t;

static record_page_ui_t s_record_ui;
static round_record_file_entry_t s_record_files[ROUND_RECORD_FILE_SCAN_MAX];

static bool record_file_list_refresh(void);
static uint32_t s_record_status_message_tick;
static bool s_record_status_message_active;
static bool s_record_file_scan_pending;
static uint32_t s_record_file_scan_retry_ms;

static void record_status_message_set(const char *text)
{
    lv_label_set_text(s_record_ui.record_status_lable, text);
    s_record_status_message_tick = lv_tick_get();
    s_record_status_message_active = true;
}

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

    const int32_t center_x = coords.x1 + lv_area_get_width(&coords) / 2;
    const int32_t center_y = coords.y1 + lv_area_get_height(&coords) / 2;

    lv_draw_rect_dsc_t glow_dsc;
    lv_draw_rect_dsc_init(&glow_dsc);
    glow_dsc.bg_color = lv_color_hex(UI_ERROR);
    glow_dsc.bg_opa = LV_OPA_10;
    glow_dsc.radius = 12;
    lv_area_t glow_area = {
        .x1 = center_x - 12,
        .y1 = coords.y1 + 4,
        .x2 = center_x + 12,
        .y2 = coords.y2 - 4,
    };
    lv_draw_rect(layer, &glow_dsc, &glow_area);

    lv_draw_rect_dsc_t baseline_dsc;
    lv_draw_rect_dsc_init(&baseline_dsc);
    baseline_dsc.bg_color = lv_color_hex(UI_ERROR);
    baseline_dsc.bg_opa = LV_OPA_20;
    lv_area_t baseline_area = {
        .x1 = coords.x1 + 14,
        .y1 = center_y,
        .x2 = center_x - 7,
        .y2 = center_y,
    };
    lv_draw_rect(layer, &baseline_dsc, &baseline_area);

    lv_draw_rect_dsc_t bar_dsc;
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_color = lv_color_hex(UI_ERROR);
    bar_dsc.radius = LV_RADIUS_CIRCLE;

    for (int32_t age = 0; age < ROUND_RECORD_WAVE_SAMPLE_COUNT; age++)
    {
        int32_t sample_index = ROUND_RECORD_WAVE_SAMPLE_COUNT - 1 - age;
        uint32_t level = s_record_ui.record_wave_level[sample_index];
        int32_t height = 2 + (int32_t)(level * 180U / 100U);
        int32_t x = center_x - 8 - age * 4;

        bar_dsc.bg_opa = (lv_opa_t)(240 - age * 6);
        lv_area_t bar_area = {
            .x1 = x,
            .y1 = center_y - height / 2,
            .x2 = x + 1,
            .y2 = center_y + (height - 1) / 2,
        };
        lv_draw_rect(layer, &bar_dsc, &bar_area);
    }
}

static void record_wave_render(void)
{
    if (s_record_ui.record_wave_view)
    {
        lv_obj_invalidate(s_record_ui.record_wave_view);
    }
}

static void record_wave_clear(void)
{
    memset(s_record_ui.record_wave_level, 0,
           sizeof(s_record_ui.record_wave_level));
    record_wave_render();
}

static void record_wave_push(int mic0_db, int mic1_db)
{
    memmove(&s_record_ui.record_wave_level[0],
            &s_record_ui.record_wave_level[1],
            sizeof(s_record_ui.record_wave_level) -
                sizeof(s_record_ui.record_wave_level[0]));
    int peak_db = mic0_db > mic1_db ? mic0_db : mic1_db;
    s_record_ui.record_wave_level[ROUND_RECORD_WAVE_SAMPLE_COUNT - 1] =
        record_db_to_level(peak_db);
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
    bool paused = recording && audio_record_is_paused();
    bool playing = audio_record_playback_is_active();

    if (s_record_status_message_active)
    {
        if ((uint32_t)(lv_tick_get() - s_record_status_message_tick) >=
            ROUND_RECORD_STATUS_MESSAGE_MS)
        {
            s_record_status_message_active = false;
        }
    }

    if (recording)
    {
        lv_obj_set_size(s_record_ui.record_play_btn, 125, 60);
        lv_obj_align(s_record_ui.record_play_btn, LV_ALIGN_TOP_LEFT, 22, 132);
        lv_label_set_text(s_record_ui.record_play_btn_lable, paused ? "RESUME" : "PAUSE");
        lv_obj_remove_flag(s_record_ui.record_finish_btn, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_set_size(s_record_ui.record_play_btn, 240, 60);
        lv_obj_align(s_record_ui.record_play_btn, LV_ALIGN_TOP_MID, 0, 132);
        lv_label_set_text(s_record_ui.record_play_btn_lable, LV_SYMBOL_PLAY);
        lv_obj_add_flag(s_record_ui.record_finish_btn, LV_OBJ_FLAG_HIDDEN);
    }

    record_button_set_enabled(s_record_ui.record_play_btn, !playing);
    record_button_set_enabled(s_record_ui.record_preview_btn,
                              !recording && (playing || audio_record_available()));
    record_button_set_enabled(s_record_ui.record_save_btn,
                              !recording && !playing && audio_record_available());
    lv_label_set_text(s_record_ui.record_preview_btn_lable, playing ? "STOP" : "Preview");

    if (s_record_status_message_active)
    {
        return;
    }

    if (recording)
    {
        lv_label_set_text(s_record_ui.record_status_lable, paused ? "Paused" : "Recording");
    }
    else if (playing)
    {
        lv_label_set_text(s_record_ui.record_status_lable, "Preview");
    }
    else
    {
        lv_label_set_text(s_record_ui.record_status_lable,
                          audio_record_available() ? "Ready" : "Ready to record");
    }
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
        if (audio_record_is_paused())
        {
            (void)audio_record_resume();
        }
        else
        {
            (void)audio_record_pause();
        }
    }
    else
    {
        char path[64] = {0};
        if (audio_record_start(path, sizeof(path)) == ESP_OK)
        {
            record_wave_clear();
        }
    }

    record_controls_update();
}

static void record_finish_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !audio_record_is_active())
    {
        return;
    }

    (void)audio_record_stop();
    record_controls_update();
}

static void record_preview_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || audio_record_is_active())
    {
        return;
    }

    if (audio_record_playback_is_active())
    {
        audio_record_stop_playback();
    }
    else if (audio_record_available())
    {
        (void)audio_record_play_file(AUDIO_RECORD_MEMORY_PATH);
    }

    record_controls_update();
}

static void record_save_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        audio_record_is_active() || audio_record_playback_is_active() ||
        !audio_record_available())
    {
        return;
    }

    char path[160] = {0};
    esp_err_t ret = audio_record_save_to_sd(path, sizeof(path));
    record_status_message_set(ret == ESP_OK ? "Saved to SD" : "Save failed");
    if (ret == ESP_OK)
    {
        s_record_ui.record_file_page = 0;
        s_record_file_scan_retry_ms = 0;
        s_record_file_scan_pending = !record_file_list_refresh();
    }
}

static void record_time_update(void)
{
    uint32_t elapsed_ms = 0;
    if (audio_record_playback_is_active())
    {
        elapsed_ms = audio_record_playback_elapsed_ms();
    }
    else if (audio_record_is_active() || audio_record_available())
    {
        elapsed_ms = audio_record_duration_ms();
    }

    uint32_t elapsed_seconds = elapsed_ms / 1000U;
    lv_label_set_text_fmt(s_record_ui.record_time_lable,
                          "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                          elapsed_seconds / 3600U,
                          (elapsed_seconds / 60U) % 60U,
                          elapsed_seconds % 60U);
}

static uint32_t record_wav_duration_seconds(const char *path)
{
    uint8_t header[44];
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return 0;
    }

    size_t read_size = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (read_size != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0)
    {
        return 0;
    }

    uint32_t byte_rate = (uint32_t)header[28] |
                         ((uint32_t)header[29] << 8) |
                         ((uint32_t)header[30] << 16) |
                         ((uint32_t)header[31] << 24);
    uint32_t data_size = (uint32_t)header[40] |
                         ((uint32_t)header[41] << 8) |
                         ((uint32_t)header[42] << 16) |
                         ((uint32_t)header[43] << 24);
    return byte_rate > 0 ? (data_size + byte_rate - 1U) / byte_rate : 0;
}

static int record_file_compare(const void *lhs, const void *rhs)
{
    const round_record_file_entry_t *a = lhs;
    const round_record_file_entry_t *b = rhs;
    if (a->modified != b->modified)
    {
        return a->modified < b->modified ? 1 : -1;
    }
    return strcasecmp(b->name, a->name);
}

static bool record_file_path_build(uint16_t index, char *path, size_t path_size)
{
    if (index >= s_record_ui.record_file_count || path == NULL || path_size == 0)
    {
        return false;
    }
    int written = snprintf(path, path_size, AUDIO_RECORD_SAVE_DIR "/%s",
                           s_record_files[index].name);
    return written > 0 && (size_t)written < path_size;
}

static void record_file_play_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || audio_record_is_active())
    {
        return;
    }

    if (audio_record_playback_is_active())
    {
        audio_record_stop_playback();
        record_controls_update();
        return;
    }

    uint16_t index = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
    char path[AUDIO_MUSIC_PATH_MAX_LEN] = {0};
    if (record_file_path_build(index, path, sizeof(path)) &&
        audio_record_play_file(path) == ESP_OK)
    {
        record_controls_update();
    }
}

static void record_file_refresh_async(void *user_data)
{
    (void)user_data;
    s_record_file_scan_retry_ms = 0;
    s_record_file_scan_pending = !record_file_list_refresh();
}

static void record_file_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (audio_record_is_active() || audio_record_playback_is_active())
    {
        record_status_message_set("Stop before delete");
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
        record_status_message_set("Recording deleted");
        lv_async_call(record_file_refresh_async, NULL);
    }
    else
    {
        record_status_message_set("Delete failed");
    }
}

static void record_file_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }

    int32_t next_page = (int32_t)s_record_ui.record_file_page +
                        (int32_t)(intptr_t)lv_event_get_user_data(event);
    if (next_page < 0)
    {
        next_page = 0;
    }
    s_record_ui.record_file_page = (uint16_t)next_page;
    record_file_list_refresh();
}

static lv_obj_t *record_list_cont_create(lv_obj_t *parent, const char *name,
                                         const char *duration, bool selected,
                                         uint16_t file_index)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, ROUND_RECORD_FILE_ROW_W, ROUND_RECORD_FILE_ROW_H);
    lv_obj_set_style_bg_color(cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_grad_color(cont, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_grad_dir(cont, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(cont, selected ? LV_OPA_90 : LV_OPA_50, 0);
    lv_obj_set_style_radius(cont, 4, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_shadow_width(cont, 0, 0);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cont, record_file_play_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)file_index);

    lv_obj_t *selected_line = lv_obj_create(cont);
    lv_obj_set_size(selected_line, 2, 14);
    lv_obj_align(selected_line, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(selected_line, lv_color_hex(UI_ERROR), 0);
    lv_obj_set_style_bg_opa(selected_line, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(selected_line, 0, 0);
    lv_obj_set_style_radius(selected_line, 1, 0);
    lv_obj_set_style_pad_all(selected_line, 0, 0);
    lv_obj_remove_flag(selected_line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_label = lv_label_create(cont);
    lv_obj_set_pos(name_label, 20, 3);
    lv_obj_set_size(name_label, 216, 18);
    lv_label_set_text(name_label, name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name_label, &lv_font_SourceHanSansSC_Regular_2_16, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *duration_label = lv_label_create(cont);
    lv_obj_set_size(duration_label, 55, 18);
    lv_obj_align(duration_label, LV_ALIGN_RIGHT_MID, -31, 0);
    lv_label_set_text(duration_label, duration);
    lv_obj_set_style_text_font(duration_label, &lv_font_SourceHanSansSC_Regular_2_16, 0);
    lv_obj_set_style_text_color(duration_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(duration_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *delete_button = lv_button_create(cont);
    lv_obj_set_size(delete_button, 22, 22);
    lv_obj_align(delete_button, LV_ALIGN_RIGHT_MID, -1, 0);
    lv_obj_set_style_bg_color(delete_button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(delete_button, LV_OPA_50, 0);
    lv_obj_set_style_border_width(delete_button, 0, 0);
    lv_obj_set_style_radius(delete_button, 11, 0);
    lv_obj_set_style_pad_all(delete_button, 0, 0);
    lv_obj_add_event_cb(delete_button, record_file_delete_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)file_index);

    lv_obj_t *delete_icon = lv_label_create(delete_button);
    lv_label_set_text(delete_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_font(delete_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(delete_icon, lv_color_hex(UI_ERROR), 0);
    lv_obj_center(delete_icon);

    return cont;
}

static lv_obj_t *record_file_page_button_create(lv_obj_t *parent, int32_t x,
                                                const char *symbol, int32_t step)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, 8);
    lv_obj_set_size(button, 24, 24);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, record_file_page_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)step);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(label);
    return button;
}

static bool record_file_list_refresh(void)
{
    if (s_record_ui.record_list_body == NULL)
    {
        return false;
    }

    DIR *dir = opendir(AUDIO_RECORD_SAVE_DIR);
    if (dir == NULL)
    {
        return false;
    }

    s_record_ui.record_file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL &&
           s_record_ui.record_file_count < ROUND_RECORD_FILE_SCAN_MAX)
    {
        const char *dot = strrchr(entry->d_name, '.');
        if (entry->d_name[0] == '.' || dot == NULL ||
            strcasecmp(dot, ".wav") != 0)
        {
            continue;
        }

        char path[AUDIO_MUSIC_PATH_MAX_LEN] = {0};
        int written = snprintf(path, sizeof(path), AUDIO_RECORD_SAVE_DIR "/%s",
                               entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path))
        {
            continue;
        }

        struct stat st = {0};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }

        round_record_file_entry_t *file =
            &s_record_files[s_record_ui.record_file_count++];
        snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
        file->modified = st.st_mtime;
        file->duration_seconds = record_wav_duration_seconds(path);
    }
    closedir(dir);

    qsort(s_record_files, s_record_ui.record_file_count,
          sizeof(s_record_files[0]), record_file_compare);

    uint16_t page_count = s_record_ui.record_file_count == 0
                              ? 1
                              : (uint16_t)((s_record_ui.record_file_count +
                                            ROUND_RECORD_FILE_PAGE_SIZE - 1) /
                                           ROUND_RECORD_FILE_PAGE_SIZE);
    if (s_record_ui.record_file_page >= page_count)
    {
        s_record_ui.record_file_page = page_count - 1;
    }

    lv_obj_clean(s_record_ui.record_list_body);
    uint16_t first = s_record_ui.record_file_page * ROUND_RECORD_FILE_PAGE_SIZE;
    uint16_t visible = s_record_ui.record_file_count > first
                           ? s_record_ui.record_file_count - first
                           : 0;
    if (visible > ROUND_RECORD_FILE_PAGE_SIZE)
    {
        visible = ROUND_RECORD_FILE_PAGE_SIZE;
    }

    for (uint16_t row = 0; row < visible; row++)
    {
        uint16_t file_index = first + row;
        uint32_t duration = s_record_files[file_index].duration_seconds;
        char duration_text[16];
        snprintf(duration_text, sizeof(duration_text), "%" PRIu32 ":%02" PRIu32,
                 duration / 60U, duration % 60U);
        record_list_cont_create(s_record_ui.record_list_body,
                                s_record_files[file_index].name,
                                duration_text, false, file_index);
    }

    if (s_record_ui.record_file_count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_record_ui.record_list_body);
        lv_obj_set_size(empty, ROUND_RECORD_FILE_ROW_W, 24);
        lv_label_set_text(empty, "No saved recordings");
        lv_obj_set_style_text_font(empty, &lv_font_SourceHanSansSC_Regular_2_16, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_label_set_text_fmt(s_record_ui.record_list_page_lable, "%u/%u",
                          (unsigned)(s_record_ui.record_file_page + 1),
                          (unsigned)page_count);
    record_button_set_enabled(s_record_ui.record_list_prev_btn,
                              s_record_ui.record_file_page > 0);
    record_button_set_enabled(s_record_ui.record_list_next_btn,
                              s_record_ui.record_file_page + 1 < page_count);
    return true;
}

static void record_page_create(Page *page)
{
    s_record_ui.root = page->root;
    lv_obj_set_style_bg_color(s_record_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_record_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_record_ui.root, 0, 0);
    lv_obj_add_flag(s_record_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_record_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(page->root);

    // Write codes screen_round_record_cont_5
    s_record_ui.record_wave_cont = lv_obj_create(page->root);
    lv_obj_set_pos(s_record_ui.record_wave_cont, 40, 100);
    lv_obj_set_size(s_record_ui.record_wave_cont, 350, 350);
    lv_obj_set_scrollbar_mode(s_record_ui.record_wave_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(s_record_ui.record_wave_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_border_color(s_record_ui.record_wave_cont, lv_color_hex(UI_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_border_opa(s_record_ui.record_wave_cont, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_record_ui.record_wave_cont, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_record_ui.record_wave_cont, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_record_ui.record_wave_cont, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_record_ui.record_wave_cont, LV_GRAD_DIR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_record_ui.record_wave_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_width(s_record_ui.record_wave_cont, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_color(s_record_ui.record_wave_cont, lv_color_hex(UI_ERROR), LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_opa(s_record_ui.record_wave_cont, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    s_record_ui.record_wave_view = s_record_ui.record_wave_cont;
    lv_obj_add_event_cb(s_record_ui.record_wave_view, record_wave_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
    record_wave_clear();

    lv_obj_t *red_line = lv_line_create(s_record_ui.record_wave_cont);
    lv_obj_align(red_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(red_line, 6, 350);
    static lv_point_precise_t screen_round_record_line_1[] = {{0, 0},{0, 350}};
    lv_line_set_points(red_line, screen_round_record_line_1, 2);

    //Write style for screen_round_record_line_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_line_width(red_line, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(red_line, lv_color_hex(UI_ERROR), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(red_line, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(red_line, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    lv_obj_t *record_control = lv_obj_create(s_record_ui.root);
    lv_obj_set_pos(record_control, 400, 150);
    lv_obj_set_size(record_control, 250, 270);
    lv_obj_set_scrollbar_mode(record_control, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(record_control, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_border_color(record_control, lv_color_hex(UI_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_border_opa(record_control, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(record_control, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(record_control, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(record_control, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_width(record_control, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_color(record_control, lv_color_hex(UI_PRIMARY), LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_shadow_opa(record_control, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(record_control, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_record_ui.record_status_lable = lv_label_create(record_control);
    lv_obj_align(s_record_ui.record_status_lable, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_size(s_record_ui.record_status_lable, 250, 32);
    lv_label_set_text(s_record_ui.record_status_lable, "Ready to record");
    lv_obj_set_style_text_font(s_record_ui.record_status_lable, &lv_font_SourceHanSansSC_Regular_2_24, 0);
    lv_obj_set_style_text_color(s_record_ui.record_status_lable, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_letter_space(s_record_ui.record_status_lable, 0, 0);
    lv_obj_set_style_text_line_space(s_record_ui.record_status_lable, 0, 0);
    lv_obj_set_style_text_align(s_record_ui.record_status_lable, LV_TEXT_ALIGN_CENTER, 0);

    s_record_ui.record_time_lable = lv_label_create(record_control);
    lv_obj_align(s_record_ui.record_time_lable, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_size(s_record_ui.record_time_lable, 250, 32);
    lv_label_set_text(s_record_ui.record_time_lable, "00:00:00");
    lv_obj_set_style_text_font(s_record_ui.record_time_lable, &lv_font_SourceHanSansCN_Bold_2_24, 0);
    lv_obj_set_style_text_color(s_record_ui.record_time_lable, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_record_ui.record_time_lable, 0, 0);
    lv_obj_set_style_text_line_space(s_record_ui.record_time_lable, 0, 0);
    lv_obj_set_style_text_align(s_record_ui.record_time_lable, LV_TEXT_ALIGN_CENTER, 0);

    s_record_ui.record_play_btn = lv_button_create(record_control);
    lv_obj_align(s_record_ui.record_play_btn, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_size(s_record_ui.record_play_btn, 240, 60);
    s_record_ui.record_play_btn_lable = lv_label_create(s_record_ui.record_play_btn);
    lv_label_set_text(s_record_ui.record_play_btn_lable, LV_SYMBOL_PLAY);
    lv_label_set_long_mode(s_record_ui.record_play_btn_lable, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_record_ui.record_play_btn_lable, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(s_record_ui.record_play_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_width(s_record_ui.record_play_btn_lable, LV_PCT(100));

    //Write style for screen_round_record_btn_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(s_record_ui.record_play_btn, 1, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_record_ui.record_play_btn, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_record_ui.record_play_btn, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_record_ui.record_play_btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_record_ui.record_play_btn, 50, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_record_ui.record_play_btn, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_record_ui.record_play_btn, 16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(s_record_ui.record_play_btn, lv_color_hex(0x00143e), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(s_record_ui.record_play_btn, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(s_record_ui.record_play_btn, 2, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(s_record_ui.record_play_btn, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(s_record_ui.record_play_btn, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_record_ui.record_play_btn, lv_color_hex(UI_ERROR), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_record_ui.record_play_btn, &lv_font_SourceHanSansCN_Bold_2_28, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s_record_ui.record_play_btn, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_record_ui.record_play_btn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_record_ui.record_play_btn, record_toggle_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_record_ui.record_finish_btn = lv_button_create(record_control);
    lv_obj_set_size(s_record_ui.record_finish_btn, 60, 60);
    lv_obj_align(s_record_ui.record_finish_btn, LV_ALIGN_TOP_LEFT, 162, 132);
    lv_obj_set_style_bg_color(s_record_ui.record_finish_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s_record_ui.record_finish_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_record_ui.record_finish_btn, 0, 0);
    lv_obj_set_style_radius(s_record_ui.record_finish_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(s_record_ui.record_finish_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(s_record_ui.record_finish_btn, 12, 0);
    lv_obj_set_style_shadow_opa(s_record_ui.record_finish_btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_record_ui.record_finish_btn, 0, 0);
    lv_obj_remove_flag(s_record_ui.record_finish_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_record_ui.record_finish_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_record_ui.record_finish_btn, record_finish_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_record_ui.record_finish_btn_lable = lv_label_create(s_record_ui.record_finish_btn);
    lv_label_set_text(s_record_ui.record_finish_btn_lable, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(s_record_ui.record_finish_btn_lable, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_record_ui.record_finish_btn_lable, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(s_record_ui.record_finish_btn_lable);

    s_record_ui.record_preview_btn = lv_button_create(record_control);
    lv_obj_set_size(s_record_ui.record_preview_btn, 125, 60);
    lv_obj_align(s_record_ui.record_preview_btn, LV_ALIGN_TOP_LEFT, 22, 202);
    lv_obj_set_style_bg_opa(s_record_ui.record_preview_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_record_ui.record_preview_btn, 1, 0);
    lv_obj_set_style_border_color(s_record_ui.record_preview_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_opa(s_record_ui.record_preview_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_record_ui.record_preview_btn, 30, 0);
    lv_obj_set_style_pad_all(s_record_ui.record_preview_btn, 0, 0);
    lv_obj_remove_flag(s_record_ui.record_preview_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_record_ui.record_preview_btn, record_preview_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_record_ui.record_preview_btn_lable = lv_label_create(s_record_ui.record_preview_btn);
    lv_label_set_text(s_record_ui.record_preview_btn_lable, "Preview");
    lv_obj_set_style_text_font(s_record_ui.record_preview_btn_lable, &lv_font_SourceHanSansSC_Regular_2_24, 0);
    lv_obj_set_style_text_color(s_record_ui.record_preview_btn_lable, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(s_record_ui.record_preview_btn_lable);

    s_record_ui.record_save_btn = lv_button_create(record_control);
    lv_obj_set_size(s_record_ui.record_save_btn, 60, 60);
    lv_obj_align(s_record_ui.record_save_btn, LV_ALIGN_TOP_LEFT, 162, 202);
    lv_obj_set_style_bg_color(s_record_ui.record_save_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_bg_opa(s_record_ui.record_save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_record_ui.record_save_btn, 0, 0);
    lv_obj_set_style_radius(s_record_ui.record_save_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(s_record_ui.record_save_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_width(s_record_ui.record_save_btn, 12, 0);
    lv_obj_set_style_shadow_opa(s_record_ui.record_save_btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_record_ui.record_save_btn, 0, 0);
    lv_obj_remove_flag(s_record_ui.record_save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_record_ui.record_save_btn, record_save_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_record_ui.record_save_btn_lable = lv_label_create(s_record_ui.record_save_btn);
    lv_label_set_text(s_record_ui.record_save_btn_lable, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_font(s_record_ui.record_save_btn_lable, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_record_ui.record_save_btn_lable, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(s_record_ui.record_save_btn_lable);

    record_controls_update();

    lv_obj_t *record_list = lv_obj_create(s_record_ui.root);
    lv_obj_set_size(record_list, 380, 200);
    lv_obj_align(record_list, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(record_list, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(record_list, LV_OPA_30, 0);
    lv_obj_set_style_border_width(record_list, 1, 0);
    lv_obj_set_style_border_color(record_list, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(record_list, LV_OPA_30, 0);
    lv_obj_set_style_radius(record_list, 22, 0);
    lv_obj_set_style_pad_all(record_list, 0, 0);
    lv_obj_set_style_shadow_color(record_list, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(record_list, 20, 0);
    lv_obj_set_style_shadow_spread(record_list, 2, 0);
    lv_obj_set_style_shadow_opa(record_list, LV_OPA_30, 0);
    lv_obj_set_scrollbar_mode(record_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(record_list, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *record_list_title = lv_label_create(record_list);
    lv_obj_set_pos(record_list_title, 20, 12);
    lv_obj_set_size(record_list_title, 245, 24);
    lv_label_set_text(record_list_title, "Recent Recordings");
    lv_obj_set_style_text_font(record_list_title, &lv_font_SourceHanSansSC_Regular_2_20, 0);
    lv_obj_set_style_text_color(record_list_title, lv_color_hex(UI_MUTED), 0);

    s_record_ui.record_list_prev_btn =
        record_file_page_button_create(record_list, 276, LV_SYMBOL_LEFT, -1);
    s_record_ui.record_list_page_lable = lv_label_create(record_list);
    lv_obj_set_pos(s_record_ui.record_list_page_lable, 302, 10);
    lv_obj_set_size(s_record_ui.record_list_page_lable, 40, 20);
    lv_obj_set_style_text_font(s_record_ui.record_list_page_lable, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_record_ui.record_list_page_lable, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_record_ui.record_list_page_lable, LV_TEXT_ALIGN_CENTER, 0);
    s_record_ui.record_list_next_btn =
        record_file_page_button_create(record_list, 342, LV_SYMBOL_RIGHT, 1);

    s_record_ui.record_list_body = lv_obj_create(record_list);
    lv_obj_set_pos(s_record_ui.record_list_body, 15, 44);
    lv_obj_set_size(s_record_ui.record_list_body, 350, 140);
    lv_obj_set_style_bg_opa(s_record_ui.record_list_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_record_ui.record_list_body, 0, 0);
    lv_obj_set_style_radius(s_record_ui.record_list_body, 0, 0);
    lv_obj_set_style_pad_all(s_record_ui.record_list_body, 0, 0);
    lv_obj_set_style_pad_row(s_record_ui.record_list_body, ROUND_RECORD_FILE_ROW_GAP, 0);
    lv_obj_set_flex_flow(s_record_ui.record_list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_record_ui.record_list_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_record_ui.record_list_body, LV_SCROLLBAR_MODE_OFF);

}

static void record_page_enter(Page *page)
{
    (void)page;
    s_record_ui.record_file_page = 0;
    s_record_file_scan_retry_ms = 0;
    s_record_file_scan_pending = !record_file_list_refresh();
}

static void record_page_leave(Page *page)
{
    s_record_file_scan_pending = false;
    s_record_status_message_active = false;
    if (audio_record_is_active())
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

static void record_page_timer(Page *page)
{
    (void)page;
    if (s_record_file_scan_pending)
    {
        s_record_file_scan_retry_ms += ROUND_RECORD_PAGE_TIMER_MS;
        if (s_record_file_scan_retry_ms >= ROUND_RECORD_FILE_RETRY_MS)
        {
            s_record_file_scan_retry_ms = 0;
            s_record_file_scan_pending = !record_file_list_refresh();
        }
    }
    if (audio_record_is_active() && !audio_record_is_paused())
    {
        int mic0_db = MIC_DB_MIN;
        int mic1_db = MIC_DB_MIN;
        audio_get_mic_levels(&mic0_db, &mic1_db);
        record_wave_push(mic0_db, mic1_db);
    }
    record_time_update();
    record_controls_update();
}

void record_page_register(void)
{
    Page page = {
        .id = PAGE_RECORD,
        .name = "record",
        .on_create = record_page_create,
        .on_enter = record_page_enter,
        .on_leave = record_page_leave,
        .on_destroy = record_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = record_page_timer,
        .timer_interval_ms = ROUND_RECORD_PAGE_TIMER_MS,
    };
    ui_page_register(&page);
}
