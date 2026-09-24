#include "ui_internal.h"
#include "music_cover.h"
#include "music_page.h"

static const char *TAG = "[UI][music_page]";

#define MUSIC_PLAYLIST_PANEL_W 640
#define MUSIC_PLAYLIST_PANEL_H 440
#define MUSIC_PLAYLIST_HEADER_H 42
#define MUSIC_PLAYLIST_LIST_H 320
#define MUSIC_PLAYLIST_ROW_H 48
#define MUSIC_PLAYLIST_ROW_GAP 6
#define MUSIC_PLAYLIST_ROW_W 576
#define MUSIC_PLAYLIST_NAME_W 470
#define MUSIC_PLAYLIST_PAD_Y 6
#define MUSIC_PLAYLIST_PANEL_X 40
#define MUSIC_PLAYLIST_PANEL_Y 60

#define MUSIC_COVER_W 300
#define MUSIC_COVER_H 300

#define MUSIC_LYRIC_LINE_COUNT 3
#define MUSIC_LYRIC_LINE_W 280
#define MUSIC_LYRIC_LINE_H 34
#define MUSIC_LYRIC_ANIM_Y 18
#define MUSIC_LYRIC_ANIM_MS 180
#define MUSIC_PROGRESS_H 6

static music_page_ui_t s_music_ui;
static char s_music_track_names[MUSIC_SCAN_MAX_TRACKS][MUSIC_TRACK_NAME_MAX_LEN];
static const char *s_music_track_ptrs[MUSIC_SCAN_MAX_TRACKS];
static int s_music_track_count = 0;
static int s_music_current_index = -1;
static int s_music_playlist_start = 0;
static int s_music_playlist_row_track_index[MUSIC_PLAYLIST_VISIBLE_COUNT] = {-1, -1, -1, -1};
static bool s_music_playing = false;
static int s_music_play_mode = 0;
static int s_music_volume_percent = AUDIO_DEFAULT_OUTPUT_VOLUME;
static bool s_music_volume_dragging;
static volatile uint8_t s_music_spectrum_target[MUSIC_SPECTRUM_BAR_COUNT];
static uint16_t s_music_spectrum_display_x16[MUSIC_SPECTRUM_BAR_COUNT];
static volatile bool s_music_spectrum_has_data = false;
static lv_obj_t *s_music_playlist_panel = NULL;
static lv_obj_t *s_music_playlist_list_cont = NULL;
static lv_obj_t *s_music_playlist_item[MUSIC_SCAN_MAX_TRACKS];
static lv_obj_t *s_music_playlist_item_index[MUSIC_SCAN_MAX_TRACKS];
static lv_obj_t *s_music_playlist_item_name[MUSIC_SCAN_MAX_TRACKS];
static lv_obj_t *s_music_playlist_item_play[MUSIC_SCAN_MAX_TRACKS];
static int s_music_playlist_item_count = 0;
static bool s_music_playlist_selecting_from_row = false;
static lv_obj_t *s_music_cover_layer = NULL;
static lv_obj_t *s_music_lyric_layer = NULL;
static bool s_music_show_lyrics = false;
static char s_music_lyric_cache[MUSIC_LYRIC_LINE_COUNT][MUSIC_TRACK_NAME_MAX_LEN];
static music_cover_image_t s_music_cover_image;
static char s_music_cover_path[384];
static audio_music_state_t s_music_last_service_state;
static bool s_music_last_service_state_valid = false;

/*********music Page************/
static void music_lyrics_apply_cached(bool animate);
static void music_playlist_panel_hide(void);

static lv_style_t s_music_circle_grad_style[MUSIC_CIRCLE_GRAD_COUNT];
static lv_grad_dsc_t s_music_circle_grad[MUSIC_CIRCLE_GRAD_COUNT];
static bool s_music_circle_grad_style_inited = false;

static void music_circle_grad_style_init_one(music_circle_grad_t type, const lv_color_t *colors, uint8_t color_count,
                                             int32_t start_angle, int32_t end_angle)
{
    lv_style_init(&s_music_circle_grad_style[type]);
    lv_style_set_radius(&s_music_circle_grad_style[type], 500);
    lv_style_set_bg_opa(&s_music_circle_grad_style[type], LV_OPA_COVER);
    lv_style_set_border_width(&s_music_circle_grad_style[type], 0);
    lv_grad_init_stops(&s_music_circle_grad[type], colors, NULL, NULL, color_count);
    lv_grad_conical_init(&s_music_circle_grad[type],
                         LV_GRAD_CENTER,
                         LV_GRAD_CENTER,
                         start_angle,
                         end_angle,
                         LV_GRAD_EXTEND_REFLECT);
    lv_style_set_bg_grad(&s_music_circle_grad_style[type], &s_music_circle_grad[type]);
}

static void music_circle_grad_styles_init(void)
{
    if (s_music_circle_grad_style_inited)
    {
        return;
    }

    static const lv_color_t disc_colors[] = {
        LV_COLOR_MAKE(0x00, 0xf5, 0xff),
        LV_COLOR_MAKE(0xff, 0x2b, 0xd6),
    };
    static const lv_color_t panel_colors[] = {
        LV_COLOR_MAKE(0x10, 0x10, 0x1f),
        LV_COLOR_MAKE(0x22, 0x30, 0x4a),
    };
    static const lv_color_t accent_colors[] = {
        LV_COLOR_MAKE(0xff, 0x2b, 0xd6),
        LV_COLOR_MAKE(0x39, 0xff, 0x88),
    };

    music_circle_grad_style_init_one(MUSIC_CIRCLE_GRAD_DISC,
                                     disc_colors,
                                     sizeof(disc_colors) / sizeof(disc_colors[0]),
                                     0,
                                     130);
    music_circle_grad_style_init_one(MUSIC_CIRCLE_GRAD_PANEL,
                                     panel_colors,
                                     sizeof(panel_colors) / sizeof(panel_colors[0]),
                                     35,
                                     150);
    music_circle_grad_style_init_one(MUSIC_CIRCLE_GRAD_ACCENT,
                                     accent_colors,
                                     sizeof(accent_colors) / sizeof(accent_colors[0]),
                                     60,
                                     170);
    s_music_circle_grad_style_inited = true;
}

static void circle_create(lv_obj_t *parent, int x, int y, int r, uint32_t color, music_circle_fill_t fill)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_size(circle, r * 2, r * 2);
    lv_obj_set_style_bg_color(circle, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(circle, r, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (fill != MUSIC_CIRCLE_FILL_SOLID)
    {
        music_circle_grad_styles_init();
        music_circle_grad_t grad_type = MUSIC_CIRCLE_GRAD_DISC;
        if (fill == MUSIC_CIRCLE_FILL_PANEL)
        {
            grad_type = MUSIC_CIRCLE_GRAD_PANEL;
        }
        else if (fill == MUSIC_CIRCLE_FILL_ACCENT)
        {
            grad_type = MUSIC_CIRCLE_GRAD_ACCENT;
        }
        lv_obj_add_style(circle, &s_music_circle_grad_style[grad_type], 0);
    }

    lv_obj_align(circle, LV_ALIGN_CENTER, x, y);
}

static void music_cover_toggle_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    s_music_show_lyrics = !s_music_show_lyrics;
    if (s_music_show_lyrics)
    {
        if (s_music_cover_layer)
        {
            lv_obj_add_flag(s_music_cover_layer, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_music_lyric_layer)
        {
            lv_obj_clear_flag(s_music_lyric_layer, LV_OBJ_FLAG_HIDDEN);
        }
        music_lyrics_apply_cached(true);
    }
    else
    {
        if (s_music_lyric_layer)
        {
            lv_obj_add_flag(s_music_lyric_layer, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_music_cover_layer)
        {
            lv_obj_clear_flag(s_music_cover_layer, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void music_spectrum_static_create(lv_obj_t *parent)
{
    static const uint32_t color_stops[] = {
        UI_SECONDARY,
        UI_PRIMARY,
        UI_OK,
        UI_WARN,
        UI_ERROR,
    };
    const int bar_w = 8;
    const int bar_gap = 5;
    const int seg_h = 4;
    const int seg_gap = 1;
    const int spectrum_w = (MUSIC_SPECTRUM_BAR_COUNT * bar_w) +
                           ((MUSIC_SPECTRUM_BAR_COUNT - 1) * bar_gap);
    const int spectrum_h = (MUSIC_SPECTRUM_SEGMENT_COUNT * seg_h) +
                           ((MUSIC_SPECTRUM_SEGMENT_COUNT - 1) * seg_gap);

    lv_obj_t *spectrum = lv_obj_create(parent);
    lv_obj_set_size(spectrum, spectrum_w, spectrum_h);
    lv_obj_align(spectrum, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(spectrum, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(spectrum, LV_OPA_20, 0);
    lv_obj_set_style_border_width(spectrum, 0, 0);
    lv_obj_set_style_radius(spectrum, 6, 0);
    lv_obj_set_style_pad_all(spectrum, 0, 0);
    lv_obj_set_scrollbar_mode(spectrum, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(spectrum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spectrum, LV_OBJ_FLAG_CLICKABLE);

    for (int bar = 0; bar < MUSIC_SPECTRUM_BAR_COUNT; bar++)
    {
        const int stop_count = sizeof(color_stops) / sizeof(color_stops[0]);
        const int color_pos = bar * (stop_count - 1) * 255 /
                              (MUSIC_SPECTRUM_BAR_COUNT - 1);
        const int stop_index = color_pos / 255;
        const uint8_t mix = (uint8_t)(color_pos % 255);
        lv_color_t bar_color;
        if (stop_index >= stop_count - 1)
        {
            bar_color = lv_color_hex(color_stops[stop_count - 1]);
        }
        else
        {
            bar_color = lv_color_mix(lv_color_hex(color_stops[stop_index + 1]),
                                     lv_color_hex(color_stops[stop_index]), mix);
        }

        for (int seg = 0; seg < MUSIC_SPECTRUM_SEGMENT_COUNT; seg++)
        {
            int y = spectrum_h - ((seg + 1) * seg_h) - (seg * seg_gap);

            lv_obj_t *block = lv_obj_create(spectrum);
            lv_obj_set_size(block, bar_w, seg_h);
            lv_obj_set_pos(block, bar * (bar_w + bar_gap), y);
            lv_obj_set_style_bg_color(block, bar_color, 0);
            lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(block, 0, 0);
            lv_obj_set_style_radius(block, 1, 0);
            lv_obj_set_style_pad_all(block, 0, 0);
            lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE);
            s_music_ui.spectrum_segment[bar][seg] = block;
        }
        s_music_ui.spectrum_level[bar] = MUSIC_SPECTRUM_SEGMENT_COUNT;
        s_music_spectrum_display_x16[bar] = 0;
    }
}

static void music_spectrum_set_bar_level(int bar, uint8_t level)
{
    if (bar < 0 || bar >= MUSIC_SPECTRUM_BAR_COUNT)
    {
        return;
    }
    if (level < 1)
    {
        level = 1;
    }
    else if (level > MUSIC_SPECTRUM_SEGMENT_COUNT)
    {
        level = MUSIC_SPECTRUM_SEGMENT_COUNT;
    }
    if (s_music_ui.spectrum_level[bar] == level)
    {
        return;
    }

    uint8_t old_level = s_music_ui.spectrum_level[bar];
    if (old_level < level)
    {
        for (uint8_t seg = old_level; seg < level; seg++)
        {
            if (s_music_ui.spectrum_segment[bar][seg])
            {
                lv_obj_clear_flag(s_music_ui.spectrum_segment[bar][seg], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_opa(s_music_ui.spectrum_segment[bar][seg], LV_OPA_COVER, 0);
            }
        }
    }
    else
    {
        for (uint8_t seg = level; seg < old_level; seg++)
        {
            if (s_music_ui.spectrum_segment[bar][seg])
            {
                lv_obj_clear_flag(s_music_ui.spectrum_segment[bar][seg], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_opa(s_music_ui.spectrum_segment[bar][seg], LV_OPA_30, 0);
            }
        }
    }
    s_music_ui.spectrum_level[bar] = level;
}

static void music_spectrum_update_timer(void)
{
    static const uint8_t idle_levels[MUSIC_SPECTRUM_BAR_COUNT] = {
        2,
        3,
        2,
        1,
        3,
        4,
        5,
        3,
        2,
        4,
        3,
        2,
        3,
        3,
        2,
        1,
        2,
        4,
        6,
        5,
        3,
        2,
        4,
        3,
        5,
        4,
        2,
        3,
        4,
        2,
        3,
        1,
    };

    for (int bar = 0; bar < MUSIC_SPECTRUM_BAR_COUNT; bar++)
    {
        uint8_t level = (s_music_playing && s_music_spectrum_has_data) ? s_music_spectrum_target[bar] : idle_levels[bar];
        uint16_t target_x16 = (uint16_t)level * 16;
        uint16_t display_x16 = s_music_spectrum_display_x16[bar];

        if (display_x16 == 0)
        {
            display_x16 = target_x16;
        }

        if (target_x16 > display_x16)
        {
            uint16_t delta = target_x16 - display_x16;
            display_x16 += (delta > 24) ? (delta / 2) : 8;
            if (display_x16 > target_x16)
            {
                display_x16 = target_x16;
            }
        }
        else if (target_x16 < display_x16)
        {
            uint16_t delta = display_x16 - target_x16;
            display_x16 -= (delta > 16) ? (delta / 4) : 4;
            if (display_x16 < target_x16)
            {
                display_x16 = target_x16;
            }
        }

        s_music_spectrum_display_x16[bar] = display_x16;
        music_spectrum_set_bar_level(bar, (uint8_t)((display_x16 + 8) / 16));
    }
}

void music_page_set_spectrum_levels(const uint8_t *levels, int count)
{
    if (levels == NULL || count <= 0)
    {
        s_music_spectrum_has_data = false;
        for (int i = 0; i < MUSIC_SPECTRUM_BAR_COUNT; i++)
        {
            s_music_spectrum_target[i] = 1;
        }
        return;
    }

    if (count > MUSIC_SPECTRUM_BAR_COUNT)
    {
        count = MUSIC_SPECTRUM_BAR_COUNT;
    }

    for (int i = 0; i < count; i++)
    {
        uint8_t level = levels[i];
        if (level < 1)
        {
            level = 1;
        }
        else if (level > MUSIC_SPECTRUM_SEGMENT_COUNT)
        {
            level = MUSIC_SPECTRUM_SEGMENT_COUNT;
        }
        s_music_spectrum_target[i] = level;
    }
    for (int i = count; i < MUSIC_SPECTRUM_BAR_COUNT; i++)
    {
        s_music_spectrum_target[i] = 1;
    }
    s_music_spectrum_has_data = true;
}

static int32_t music_lyric_line_y_get(int index)
{
    return (index - 1) * MUSIC_LYRIC_LINE_H;
}

static lv_opa_t music_lyric_line_opa_get(int index)
{
    return index == 1 ? LV_OPA_COVER : LV_OPA_60;
}

static void music_lyric_opa_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void music_lyric_translate_y_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

static void music_lyric_anim_start(lv_obj_t *label, int32_t y_from, int32_t y_to,
                                   lv_opa_t opa_from, lv_opa_t opa_to,
                                   lv_anim_completed_cb_t completed_cb)
{
    if (label == NULL)
    {
        return;
    }

    lv_anim_del(label, NULL);
    lv_obj_set_style_translate_y(label, y_from, 0);
    lv_obj_set_style_opa(label, opa_from, 0);

    lv_anim_t y_anim;
    lv_anim_init(&y_anim);
    lv_anim_set_var(&y_anim, label);
    lv_anim_set_exec_cb(&y_anim, music_lyric_translate_y_anim_cb);
    lv_anim_set_values(&y_anim, y_from, y_to);
    lv_anim_set_duration(&y_anim, MUSIC_LYRIC_ANIM_MS);
    lv_anim_set_path_cb(&y_anim, lv_anim_path_ease_out);
    if (completed_cb)
    {
        lv_anim_set_completed_cb(&y_anim, completed_cb);
    }
    lv_anim_start(&y_anim);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, label);
    lv_anim_set_exec_cb(&opa_anim, music_lyric_opa_anim_cb);
    lv_anim_set_values(&opa_anim, opa_from, opa_to);
    lv_anim_set_duration(&opa_anim, MUSIC_LYRIC_ANIM_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_start(&opa_anim);
}

static void music_lyrics_apply_cached(bool animate)
{
    for (int i = 0; i < MUSIC_LYRIC_LINE_COUNT; i++)
    {
        lv_obj_t *label = s_music_ui.paly_lyric_text[i];
        if (label == NULL)
        {
            continue;
        }

        lv_label_set_text(label, s_music_lyric_cache[i]);
        if (animate)
        {
            music_lyric_anim_start(label,
                                   music_lyric_line_y_get(i) + MUSIC_LYRIC_ANIM_Y,
                                   music_lyric_line_y_get(i),
                                   LV_OPA_TRANSP,
                                   music_lyric_line_opa_get(i),
                                   NULL);
        }
        else
        {
            lv_anim_del(label, NULL);
            lv_obj_set_style_translate_y(label, music_lyric_line_y_get(i), 0);
            lv_obj_set_style_opa(label, music_lyric_line_opa_get(i), 0);
        }
    }
}

static void music_lyrics_out_completed_cb(lv_anim_t *anim)
{
    (void)anim;
    music_lyrics_apply_cached(true);
}

void music_page_set_lyrics(const char *prev, const char *current, const char *next)
{
    static const char *default_prev = "SD card local music";
    static const char *default_current = "No lyrics for this track";
    static const char *default_next = "Add .lrc with the same file name";

    const char *lyrics[3] = {
        (prev && prev[0]) ? prev : default_prev,
        (current && current[0]) ? current : default_current,
        (next && next[0]) ? next : default_next,
    };

    ui_lock();
    for (int i = 0; i < MUSIC_LYRIC_LINE_COUNT; i++)
    {
        snprintf(s_music_lyric_cache[i], sizeof(s_music_lyric_cache[i]), "%s", lyrics[i]);
    }

    if (s_music_show_lyrics && s_music_ui.paly_lyric_text[0])
    {
        for (int i = 0; i < MUSIC_LYRIC_LINE_COUNT; i++)
        {
            music_lyric_anim_start(s_music_ui.paly_lyric_text[i],
                                   music_lyric_line_y_get(i),
                                   music_lyric_line_y_get(i) - MUSIC_LYRIC_ANIM_Y,
                                   music_lyric_line_opa_get(i),
                                   LV_OPA_TRANSP,
                                   i == 1 ? music_lyrics_out_completed_cb : NULL);
        }
    }
    else
    {
        music_lyrics_apply_cached(false);
    }
    ui_unlock();
}

static bool music_filename_has_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return false;
    }

    dot++;
    return strcasecmp(dot, "mp3") == 0 ||
           strcasecmp(dot, "aac") == 0 ||
           strcasecmp(dot, "wav") == 0 ||
           strcasecmp(dot, "flac") == 0 ||
           strcasecmp(dot, "m4a") == 0 ||
           strcasecmp(dot, "ts") == 0 ||
           strcasecmp(dot, "ogg") == 0 ||
           strcasecmp(dot, "amrnb") == 0 ||
           strcasecmp(dot, "amrwb") == 0 ||
           strcasecmp(dot, "amr") == 0 ||
           strcasecmp(dot, "awb") == 0;
}

static const char *music_display_name(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return path;
    }

    const char *slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static void music_track_add_file(const char *relative_path)
{
    if (s_music_track_count >= MUSIC_SCAN_MAX_TRACKS || relative_path == NULL || relative_path[0] == '\0')
    {
        return;
    }

    snprintf(s_music_track_names[s_music_track_count],
             sizeof(s_music_track_names[s_music_track_count]), "%s", relative_path);
    s_music_track_ptrs[s_music_track_count] = s_music_track_names[s_music_track_count];
    s_music_track_count++;
}

static void music_scan_dir_recursive(const char *dir_path, const char *relative_prefix)
{
    if (s_music_track_count >= MUSIC_SCAN_MAX_TRACKS)
    {
        return;
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL)
    {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_music_track_count < MUSIC_SCAN_MAX_TRACKS)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        char child_path[384];
        char relative_path[MUSIC_TRACK_NAME_MAX_LEN];
        int path_len = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);
        int relative_len = relative_prefix && relative_prefix[0]
                               ? snprintf(relative_path, sizeof(relative_path), "%s/%s",
                                          relative_prefix, entry->d_name)
                               : snprintf(relative_path, sizeof(relative_path), "%s", entry->d_name);
        if (path_len < 0 || path_len >= (int)sizeof(child_path) ||
            relative_len < 0 || relative_len >= (int)sizeof(relative_path))
        {
            continue;
        }

        struct stat st;
        if (stat(child_path, &st) != 0)
        {
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            music_scan_dir_recursive(child_path, relative_path);
        }
        else if (S_ISREG(st.st_mode) && music_filename_has_audio_ext(entry->d_name))
        {
            music_track_add_file(relative_path);
        }
    }
    closedir(dir);
}

static void music_track_list_sort(void)
{
    for (int i = 0; i < s_music_track_count; i++)
    {
        for (int j = i + 1; j < s_music_track_count; j++)
        {
            if (strcasecmp(s_music_track_names[i], s_music_track_names[j]) > 0)
            {
                char temp[MUSIC_TRACK_NAME_MAX_LEN];
                memcpy(temp, s_music_track_names[i], sizeof(temp));
                memcpy(s_music_track_names[i], s_music_track_names[j], sizeof(temp));
                memcpy(s_music_track_names[j], temp, sizeof(temp));
            }
        }
    }
    for (int i = 0; i < s_music_track_count; i++)
    {
        s_music_track_ptrs[i] = s_music_track_names[i];
    }
}

static void music_page_update_artist_album_locked(int index)
{
    if (s_music_ui.artist_album_label == NULL)
    {
        return;
    }

    if (index < 0 || index >= s_music_track_count || s_music_track_ptrs[index] == NULL)
    {
        lv_label_set_text(s_music_ui.artist_album_label, "No artist / No album");
        return;
    }

    const char *name = music_display_name(s_music_track_ptrs[index]);
    const char *separator = strstr(name, " - ");
    if (separator && separator > name)
    {
        char artist[64];
        size_t artist_len = (size_t)(separator - name);
        while (artist_len > 0 && name[artist_len - 1] == ' ')
        {
            artist_len--;
        }
        if (artist_len >= sizeof(artist))
        {
            artist_len = sizeof(artist) - 1;
        }
        memcpy(artist, name, artist_len);
        artist[artist_len] = '\0';
        lv_label_set_text_fmt(s_music_ui.artist_album_label, "%s / Local album", artist);
        return;
    }

    lv_label_set_text(s_music_ui.artist_album_label, "SD card / Local music");
}

void music_page_set_artist_album(const char *artist, const char *album)
{
    const char *artist_text = (artist && artist[0]) ? artist : "Unknown artist";
    const char *album_text = (album && album[0]) ? album : "Unknown album";

    ui_lock();
    if (s_music_ui.artist_album_label)
    {
        lv_label_set_text_fmt(s_music_ui.artist_album_label, "%s / %s", artist_text, album_text);
    }
    ui_unlock();
}

static void music_page_select_track(int index)
{
    if (index < 0 || index >= s_music_track_count)
    {
        return;
    }

    s_music_current_index = index;
    if (s_music_current_index < s_music_playlist_start)
    {
        s_music_playlist_start = s_music_current_index;
    }
    else if (s_music_current_index >= s_music_playlist_start + MUSIC_PLAYLIST_VISIBLE_COUNT)
    {
        s_music_playlist_start = s_music_current_index - MUSIC_PLAYLIST_VISIBLE_COUNT + 1;
    }
    music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);

    if (s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label,
                          music_display_name(s_music_track_ptrs[s_music_current_index]));
    }
    music_page_update_artist_album_locked(s_music_current_index);

    ESP_LOGI(TAG, "Music track selected: %d %s", s_music_current_index, s_music_track_ptrs[s_music_current_index]);
    if (!audio_music_control(MUSIC_CONTROL_TRACK_SELECT, s_music_current_index))
    {
        ESP_LOGW(TAG, "Music track select command failed: %d", s_music_current_index);
        music_page_set_play_state(false);
    }
}

static const char *music_play_mode_symbol(int mode)
{
    switch (mode)
    {
    case 1:
        return LV_SYMBOL_SHUFFLE;
    case 2:
        return LV_SYMBOL_LOOP;
    case 0:
    default:
        return LV_SYMBOL_BARS;
    }
}

static void music_page_set_play_mode(int mode)
{
    if (mode < 0 || mode > 2)
    {
        mode = 0;
    }
    s_music_play_mode = mode;
    if (s_music_ui.play_mode_btn_label)
    {
        lv_label_set_text(s_music_ui.play_mode_btn_label, music_play_mode_symbol(mode));
    }
}

void music_page_set_current_track(int index)
{
    if (index < 0 || index >= s_music_track_count)
    {
        return;
    }

    s_music_current_index = index;
    music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);

    ui_lock();
    if (s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label,
                          music_display_name(s_music_track_ptrs[s_music_current_index]));
    }
    music_page_update_artist_album_locked(s_music_current_index);
    home_music_status_update_apply();
    ui_unlock();
}

static int music_page_random_track_index(int current_index)
{
    if (s_music_track_count <= 0)
    {
        return -1;
    }
    if (s_music_track_count == 1)
    {
        return 0;
    }

    uint32_t seed = lv_tick_get() ^ (uint32_t)(uintptr_t)s_music_ui.root;
    int next = (int)(seed % (uint32_t)s_music_track_count);
    if (next == current_index)
    {
        next = (next + 1) % s_music_track_count;
    }
    return next;
}

int music_page_scan_sd_music(void)
{
    char current_name[MUSIC_TRACK_NAME_MAX_LEN] = {0};
    if (s_music_current_index >= 0 && s_music_current_index < s_music_track_count)
    {
        snprintf(current_name, sizeof(current_name), "%s", s_music_track_names[s_music_current_index]);
    }

    DIR *root_dir = opendir(MUSIC_SCAN_DIR);
    if (root_dir == NULL)
    {
        ESP_LOGW(TAG, "Failed to open music dir: %s", MUSIC_SCAN_DIR);
        s_music_track_count = 0;
        s_music_current_index = -1;
        s_music_playlist_start = 0;
        (void)audio_music_playlist_set(NULL, 0, -1);
        music_page_set_playlist(NULL, 0, -1);
        if (s_music_ui.song_name_label)
        {
            lv_label_set_text(s_music_ui.song_name_label, "No music files");
        }
        music_page_update_artist_album_locked(-1);
        home_music_status_update_apply();
        return -1;
    }
    closedir(root_dir);

    s_music_track_count = 0;
    s_music_playlist_start = 0;
    memset(s_music_track_names, 0, sizeof(s_music_track_names));
    music_scan_dir_recursive(MUSIC_SCAN_DIR, "");
    music_track_list_sort();

    int selected_index = -1;
    if (current_name[0] != '\0')
    {
        for (int i = 0; i < s_music_track_count; i++)
        {
            if (strcmp(current_name, s_music_track_names[i]) == 0)
            {
                selected_index = i;
                break;
            }
        }
    }
    if (selected_index < 0)
    {
        selected_index = s_music_track_count > 0 ? 0 : -1;
    }

    s_music_current_index = selected_index;
    if (s_music_current_index < 0)
    {
        s_music_playlist_start = 0;
    }
    else if (s_music_current_index < s_music_playlist_start)
    {
        s_music_playlist_start = s_music_current_index;
    }
    else if (s_music_current_index >= s_music_playlist_start + MUSIC_PLAYLIST_VISIBLE_COUNT)
    {
        s_music_playlist_start = s_music_current_index - MUSIC_PLAYLIST_VISIBLE_COUNT + 1;
    }
    ESP_LOGI(TAG, "Scanned %d music file(s) in %s", s_music_track_count, MUSIC_SCAN_DIR);
    (void)audio_music_playlist_set(s_music_track_ptrs, s_music_track_count, s_music_current_index);
    music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);

    if (s_music_current_index >= 0 && s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label,
                          music_display_name(s_music_track_ptrs[s_music_current_index]));
    }
    else if (s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label, "No music files");
    }
    music_page_update_artist_album_locked(s_music_current_index);

    home_music_status_update_apply();
    return s_music_track_count;
}

int music_page_get_track_count(void)
{
    return s_music_track_count;
}

const char *music_page_get_track_name(int index)
{
    if (index < 0 || index >= s_music_track_count)
    {
        return NULL;
    }

    return s_music_track_names[index];
}

static void music_control_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED)
    {
        return;
    }

    audio_music_control_action_t action = (audio_music_control_action_t)(intptr_t)lv_event_get_user_data(e);
    bool handled = false;

    switch (action)
    {
    case MUSIC_CONTROL_PLAY_PAUSE:
        handled = audio_music_control(action, s_music_playing ? 0 : 1);
        if (!handled)
        {
            ESP_LOGW(TAG, "Music play/pause command failed");
            music_page_set_play_state(false);
        }
        break;
    case MUSIC_CONTROL_PREV:
        if (s_music_track_count > 0)
        {
            int next_index = s_music_current_index > 0 ? s_music_current_index - 1 : s_music_track_count - 1;
            music_page_select_track(next_index);
        }
        break;
    case MUSIC_CONTROL_NEXT:
        if (s_music_track_count > 0)
        {
            int next_index = s_music_play_mode == 1
                                 ? music_page_random_track_index(s_music_current_index)
                                 : (s_music_current_index + 1) % s_music_track_count;
            music_page_select_track(next_index);
        }
        break;
    case MUSIC_CONTROL_PLAY_MODE:
        music_page_set_play_mode((s_music_play_mode + 1) % 3);
        audio_music_control(action, s_music_play_mode);
        break;
    default:
        audio_music_control(action, 0);
        break;
    }
}

static void music_volume_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE)
    {
        lv_event_stop_bubbling(e);
        return;
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
    {
        s_music_volume_dragging = true;
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        s_music_volume_dragging = false;
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *slider = lv_event_get_target_obj(e);
    int value = lv_slider_get_value(slider);
    music_page_set_volume(value);
    audio_music_control(MUSIC_CONTROL_VOLUME, value);
}

static void music_page_gesture(Page *page, GestureDirection direction)
{
    if (s_music_volume_dragging)
    {
        return;
    }
    app_page_back_gesture(page, direction);
}

static lv_obj_t *music_control_button_create(lv_obj_t *parent,
                                             const char *symbol,
                                             int32_t size,
                                             bool primary,
                                             audio_music_control_action_t action)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    // lv_obj_set_style_border_color(btn, lv_color_hex(primary ? UI_TEXT : UI_LINE), 0);
    lv_obj_set_style_border_opa(btn, primary ? LV_OPA_80 : LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(btn, size >= 80 ? 16 : 12, 0);
    lv_obj_set_style_shadow_spread(btn, 2, 0);
    lv_obj_set_style_shadow_opa(btn, primary ? LV_OPA_60 : LV_OPA_40, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, music_control_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(label,
                               size >= 80 ? &lv_font_montserrat_28
                                          : &lv_font_montserrat_20,
                               0);
    return label;
}

static void music_playlist_popup_row_visual_set(lv_obj_t *row, bool current)
{
    if (row == NULL)
    {
        return;
    }

    uint32_t bg_color = current ? UI_PRIMARY : UI_PANEL_HL;
    uint32_t index_color = current ? UI_PRIMARY : UI_MUTED;
    uint32_t name_color = UI_TEXT;
    uint32_t play_color = current ? UI_PRIMARY : UI_MUTED;

    lv_obj_set_style_bg_color(row, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(row, current ? LV_OPA_30 : LV_OPA_40, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(current ? UI_PRIMARY : UI_LINE), 0);
    lv_obj_set_style_border_opa(row, current ? LV_OPA_90 : LV_OPA_50, 0);
    lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(bg_color), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, current ? LV_OPA_40 : LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(current ? UI_PRIMARY : UI_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(row, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(row, current ? 14 : 0, 0);
    lv_obj_set_style_shadow_spread(row, current ? 2 : 0, 0);
    lv_obj_set_style_shadow_opa(row, current ? LV_OPA_60 : LV_OPA_TRANSP, 0);

    lv_obj_t *index_label = lv_obj_get_child(row, 0);
    lv_obj_t *name_label = lv_obj_get_child(row, 1);
    lv_obj_t *play_label = lv_obj_get_child(row, 3);
    if (index_label)
    {
        lv_obj_set_style_text_color(index_label, lv_color_hex(index_color), 0);
    }
    if (name_label)
    {
        lv_obj_set_style_text_color(name_label, lv_color_hex(name_color), 0);
    }
    if (play_label)
    {
        lv_obj_set_style_text_color(play_label, lv_color_hex(play_color), 0);
        lv_obj_set_style_opa(play_label, LV_OPA_COVER, 0);
    }
}

static void music_playlist_popup_update_visual(void)
{
    for (int i = 0; i < s_music_playlist_item_count; i++)
    {
        lv_obj_t *row = s_music_playlist_item[i];
        if (row == NULL)
        {
            continue;
        }

        bool current = (i == s_music_current_index);
        music_playlist_popup_row_visual_set(row, current);
    }
}

static void music_playlist_popup_refresh_current_mark(void)
{
    for (int i = 0; i < s_music_playlist_item_count; i++)
    {
        lv_obj_t *row = s_music_playlist_item[i];
        if (row == NULL)
        {
            continue;
        }

        bool current = (i == s_music_current_index);
        lv_obj_set_style_border_color(row, lv_color_hex(current ? UI_PRIMARY : UI_LINE), 0);
        if (s_music_playlist_item_index[i])
        {
            lv_obj_set_style_text_color(s_music_playlist_item_index[i],
                                        lv_color_hex(current ? UI_PRIMARY : UI_MUTED),
                                        0);
        }

        lv_obj_t *play_label = s_music_playlist_item_play[i];
        if (play_label)
        {
            lv_label_set_text(play_label, current ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(play_label, lv_color_hex(current ? UI_PRIMARY : UI_MUTED), 0);
        }
    }
    music_playlist_popup_update_visual();
}

static void music_playlist_popup_scroll_to_current(lv_anim_enable_t anim_en)
{
    if (s_music_playlist_list_cont == NULL)
    {
        return;
    }

    if (s_music_current_index < 0 || s_music_current_index >= s_music_playlist_item_count)
    {
        lv_obj_scroll_to_y(s_music_playlist_list_cont, 0, LV_ANIM_OFF);
        return;
    }

    lv_obj_update_layout(s_music_playlist_list_cont);
    lv_obj_t *row = s_music_playlist_item[s_music_current_index];
    if (row == NULL)
    {
        return;
    }

    int32_t target_y = lv_obj_get_y(row) - MUSIC_PLAYLIST_PAD_Y;
    if (target_y < 0)
    {
        target_y = 0;
    }
    lv_obj_scroll_to_y(s_music_playlist_list_cont, target_y, anim_en);
}

static void music_playlist_popup_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED && lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED)
    {
        return;
    }

    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index >= 0 && index < s_music_track_count)
    {
        s_music_playlist_selecting_from_row = true;
        music_page_select_track(index);
        s_music_playlist_selecting_from_row = false;
        music_playlist_popup_refresh_current_mark();
    }
}

static void music_playlist_popup_rows_rebuild(const char *const *tracks, int track_count, int current_index)
{
    if (s_music_playlist_list_cont == NULL)
    {
        return;
    }

    lv_obj_clean(s_music_playlist_list_cont);
    memset(s_music_playlist_item, 0, sizeof(s_music_playlist_item));
    memset(s_music_playlist_item_index, 0, sizeof(s_music_playlist_item_index));
    memset(s_music_playlist_item_name, 0, sizeof(s_music_playlist_item_name));
    memset(s_music_playlist_item_play, 0, sizeof(s_music_playlist_item_play));

    if (track_count < 0)
    {
        track_count = 0;
    }
    if (track_count > MUSIC_SCAN_MAX_TRACKS)
    {
        track_count = MUSIC_SCAN_MAX_TRACKS;
    }
    s_music_playlist_item_count = track_count;

    for (int i = 0; i < track_count; i++)
    {
        lv_obj_t *row = lv_button_create(s_music_playlist_list_cont);
        s_music_playlist_item[i] = row;
        lv_obj_set_size(row, MUSIC_PLAYLIST_ROW_W, MUSIC_PLAYLIST_ROW_H);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_LINE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(i == current_index ? UI_PRIMARY : UI_LINE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, lv_color_hex(i == current_index ? UI_PRIMARY : UI_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 14, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        ui_obj_set_flex(row,
                        LV_FLEX_FLOW_ROW,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, music_playlist_popup_row_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_music_playlist_item_index[i] = ui_label_create(row, "", &lv_font_montserrat_14, UI_MUTED);
        lv_label_set_text_fmt(s_music_playlist_item_index[i], "%02d", i + 1);
        lv_obj_set_width(s_music_playlist_item_index[i], 36);
        lv_obj_set_style_text_align(s_music_playlist_item_index[i], LV_TEXT_ALIGN_CENTER, 0);

        s_music_playlist_item_name[i] = ui_label_create(row,
                                                        tracks && tracks[i]
                                                            ? music_display_name(tracks[i])
                                                            : "--",
                                                        MUSIC_NAME_FONT,
                                                        UI_MUTED);
        lv_obj_set_width(s_music_playlist_item_name[i], MUSIC_PLAYLIST_NAME_W);
        lv_label_set_long_mode(s_music_playlist_item_name[i], LV_LABEL_LONG_MODE_DOTS);

        ui_flex_spacer_create(row);

        s_music_playlist_item_play[i] = ui_label_create(row,
                                                        i == current_index ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO,
                                                        &lv_font_montserrat_14,
                                                        i == current_index ? UI_PRIMARY : UI_MUTED);
        lv_obj_set_width(s_music_playlist_item_play[i], 22);
        lv_obj_set_style_text_align(s_music_playlist_item_play[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    if (track_count <= 0)
    {
        lv_obj_t *empty = ui_label_create(s_music_playlist_list_cont,
                                          "No music files",
                                          &lv_font_montserrat_16,
                                          UI_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_update_layout(s_music_playlist_list_cont);
    music_playlist_popup_scroll_to_current(LV_ANIM_OFF);
    music_playlist_popup_update_visual();
}

static void music_playlist_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        music_playlist_panel_hide();
    }
}

static void music_playlist_panel_create(void)
{
    if (s_music_playlist_panel || s_music_ui.root == NULL)
    {
        return;
    }

    s_music_playlist_panel = lv_obj_create(s_music_ui.root);
    lv_obj_set_size(s_music_playlist_panel, MUSIC_PLAYLIST_PANEL_W, MUSIC_PLAYLIST_PANEL_H);
    lv_obj_set_pos(s_music_playlist_panel, MUSIC_PLAYLIST_PANEL_X,
                   MUSIC_PLAYLIST_PANEL_Y);
    lv_obj_set_style_bg_color(s_music_playlist_panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_music_playlist_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_music_playlist_panel, 1, 0);
    lv_obj_set_style_border_color(s_music_playlist_panel, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_border_opa(s_music_playlist_panel, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_music_playlist_panel, 18, 0);
    lv_obj_set_style_pad_all(s_music_playlist_panel, 12, 0);
    lv_obj_set_style_pad_row(s_music_playlist_panel, 8, 0);
    lv_obj_set_style_shadow_width(s_music_playlist_panel, 24, 0);
    lv_obj_set_style_shadow_spread(s_music_playlist_panel, 4, 0);
    lv_obj_set_style_shadow_opa(s_music_playlist_panel, LV_OPA_70, 0);
    lv_obj_set_style_shadow_color(s_music_playlist_panel, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_remove_flag(s_music_playlist_panel, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(s_music_playlist_panel,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(s_music_playlist_panel,
                                              LV_PCT(100),
                                              MUSIC_PLAYLIST_HEADER_H,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);

    lv_obj_t *icon = ui_label_create(head, LV_SYMBOL_LIST, &lv_font_montserrat_20, UI_SECONDARY);
    lv_obj_set_width(icon, 28);
    lv_obj_t *title = ui_label_create(head, "PLAYLIST", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_set_width(title, 150);
    ui_flex_spacer_create(head);
    s_music_ui.playlist_count_label = ui_label_create(head, "0 TRACKS", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_music_ui.playlist_count_label, 90);
    lv_obj_set_style_text_align(s_music_ui.playlist_count_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 34, 30);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_remove_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close_btn, music_playlist_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(close_label);

    s_music_playlist_list_cont = ui_flex_container_create(s_music_playlist_panel,
                                                          LV_PCT(100),
                                                          MUSIC_PLAYLIST_LIST_H,
                                                          LV_FLEX_FLOW_COLUMN,
                                                          LV_FLEX_ALIGN_START,
                                                          LV_FLEX_ALIGN_CENTER,
                                                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_music_playlist_list_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_music_playlist_list_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_music_playlist_list_cont, 0, 0);
    lv_obj_set_style_pad_top(s_music_playlist_list_cont, MUSIC_PLAYLIST_PAD_Y, 0);
    lv_obj_set_style_pad_bottom(s_music_playlist_list_cont, MUSIC_PLAYLIST_PAD_Y, 0);
    lv_obj_set_style_pad_row(s_music_playlist_list_cont, MUSIC_PLAYLIST_ROW_GAP, 0);
    lv_obj_set_scroll_dir(s_music_playlist_list_cont, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_music_playlist_list_cont, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(s_music_playlist_list_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_music_playlist_list_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(s_music_playlist_panel, LV_OBJ_FLAG_HIDDEN);
}

static void music_playlist_panel_show(void)
{
    music_playlist_panel_create();
    if (s_music_playlist_panel == NULL)
    {
        return;
    }

    lv_obj_set_x(s_music_playlist_panel, MUSIC_PLAYLIST_PANEL_X);
    lv_obj_set_y(s_music_playlist_panel, MUSIC_PLAYLIST_PANEL_Y);
    lv_obj_clear_flag(s_music_playlist_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_music_playlist_panel);
    music_playlist_popup_refresh_current_mark();
    music_playlist_popup_scroll_to_current(LV_ANIM_OFF);
    music_playlist_popup_update_visual();
}

static void music_playlist_panel_hide(void)
{
    if (s_music_playlist_panel == NULL || lv_obj_has_flag(s_music_playlist_panel, LV_OBJ_FLAG_HIDDEN))
    {
        return;
    }

    lv_obj_add_flag(s_music_playlist_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_music_playlist_panel, MUSIC_PLAYLIST_PANEL_X);
}

static void music_playlist_toggle_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED && lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED)
    {
        return;
    }

    if (s_music_playlist_panel == NULL || lv_obj_has_flag(s_music_playlist_panel, LV_OBJ_FLAG_HIDDEN))
    {
        music_playlist_panel_show();
    }
    else
    {
        music_playlist_panel_hide();
    }
}

static lv_obj_t *music_list_button_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 80, 80);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_width(btn, 16, 0);
    lv_obj_set_style_shadow_spread(btn, 2, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, music_playlist_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_LIST);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    return label;
}

static void music_playlist_row_set(int row, int track_index, const char *name, bool current)
{
    if (row < 0 || row >= MUSIC_PLAYLIST_VISIBLE_COUNT)
    {
        return;
    }

    if (s_music_ui.playlist_row[row])
    {
        s_music_playlist_row_track_index[row] = track_index;
        lv_obj_set_style_bg_color(s_music_ui.playlist_row[row],
                                  lv_color_hex(current ? UI_PANEL_HL : UI_BG),
                                  0);
        lv_obj_set_style_bg_opa(s_music_ui.playlist_row[row],
                                current ? LV_OPA_COVER : LV_OPA_TRANSP,
                                0);
    }

    if (s_music_ui.playlist_index_label[row])
    {
        if (track_index >= 0)
        {
            lv_label_set_text_fmt(s_music_ui.playlist_index_label[row], "%02d", track_index + 1);
        }
        else
        {
            lv_label_set_text(s_music_ui.playlist_index_label[row], "--");
        }
        lv_obj_set_style_text_color(s_music_ui.playlist_index_label[row],
                                    lv_color_hex(current ? UI_PRIMARY : UI_MUTED),
                                    0);
    }

    if (s_music_ui.playlist_file_label[row])
    {
        lv_label_set_text(s_music_ui.playlist_file_label[row], (name && name[0]) ? name : "--");
        lv_obj_set_style_text_color(s_music_ui.playlist_file_label[row],
                                    lv_color_hex(current ? UI_TEXT : UI_MUTED),
                                    0);
    }
}

void music_page_set_play_state(bool playing)
{
    s_music_playing = playing;
    if (!playing)
    {
        s_music_spectrum_has_data = false;
    }

    ui_lock();
    if (s_music_ui.play_btn_label)
    {
        lv_label_set_text(s_music_ui.play_btn_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    ui_unlock();
}

void music_page_set_volume(int volume_percent)
{
    volume_percent = home_clamp_percent(volume_percent);
    s_music_volume_percent = volume_percent;

    ui_lock();
    if (s_music_ui.volume_slider)
    {
        lv_slider_set_value(s_music_ui.volume_slider, volume_percent, LV_ANIM_OFF);
    }

    if (s_music_ui.volume_value_label)
    {
        lv_label_set_text_fmt(s_music_ui.volume_value_label, "%d%%", volume_percent);
    }
    home_music_status_update_apply();
    ui_unlock();
}

static void music_page_time_format(char *buf, size_t buf_size, int sec)
{
    if (sec < 0)
    {
        snprintf(buf, buf_size, "--:--");
        return;
    }
    snprintf(buf, buf_size, "%02d:%02d", sec / 60, sec % 60);
}

void music_page_set_progress(int current_sec, int total_sec)
{
    char current_text[16] = {0};
    char total_text[16] = {0};
    int progress = 0;

    if (total_sec > 0)
    {
        progress = (current_sec * 100) / total_sec;
        if (progress < 0)
        {
            progress = 0;
        }
        else if (progress > 100)
        {
            progress = 100;
        }
    }

    music_page_time_format(current_text, sizeof(current_text), current_sec);
    music_page_time_format(total_text, sizeof(total_text), total_sec > 0 ? total_sec : -1);

    ui_lock();
    if (s_music_ui.paly_progress_bar)
    {
        lv_bar_set_value(s_music_ui.paly_progress_bar, progress, LV_ANIM_OFF);
    }
    if (s_music_ui.paly_progress_current_time_label)
    {
        lv_label_set_text(s_music_ui.paly_progress_current_time_label, current_text);
    }
    if (s_music_ui.paly_progress_total_time_label)
    {
        lv_label_set_text(s_music_ui.paly_progress_total_time_label, total_text);
    }
    ui_unlock();
}

void music_page_set_playlist(const char *const *tracks, int track_count, int current_index)
{
    if (track_count < 0)
    {
        track_count = 0;
    }

    if (current_index < 0 || current_index >= track_count)
    {
        current_index = track_count > 0 ? 0 : -1;
    }

    ui_lock();
    if (s_music_ui.playlist_count_label)
    {
        lv_label_set_text_fmt(s_music_ui.playlist_count_label, "%d TRACKS", track_count);
    }

    int max_start = track_count > MUSIC_PLAYLIST_VISIBLE_COUNT ? track_count - MUSIC_PLAYLIST_VISIBLE_COUNT : 0;
    if (s_music_playlist_start < 0)
    {
        s_music_playlist_start = 0;
    }
    else if (s_music_playlist_start > max_start)
    {
        s_music_playlist_start = max_start;
    }

    for (int row = 0; row < MUSIC_PLAYLIST_VISIBLE_COUNT; row++)
    {
        int idx = s_music_playlist_start + row;
        const char *name = (tracks && idx < track_count) ? tracks[idx] : NULL;
        music_playlist_row_set(row, idx < track_count ? idx : -1,
                               music_display_name(name), idx == current_index);
    }
    if (s_music_playlist_selecting_from_row)
    {
        music_playlist_popup_refresh_current_mark();
    }
    else if (s_music_playlist_item_count > 0)
    {
        for (int i = 0; i < s_music_playlist_item_count; i++)
        {
            if (s_music_playlist_item_name[i])
            {
                lv_label_set_text(s_music_playlist_item_name[i],
                                  tracks && i < track_count && tracks[i]
                                      ? music_display_name(tracks[i])
                                      : "--");
            }
        }
        music_playlist_popup_refresh_current_mark();
    }
    else
    {
        music_playlist_popup_rows_rebuild(tracks, track_count, current_index);
    }
    ui_unlock();
}

void music_page_set_track_params(const char *format, const char *sample_rate, const char *bitrate, const char *channels)
{
    const char *values[MUSIC_TRACK_PARAM_COUNT] = {
        (format && format[0]) ? format : "--",
        (sample_rate && sample_rate[0]) ? sample_rate : "--",
        (bitrate && bitrate[0]) ? bitrate : "--",
        (channels && channels[0]) ? channels : "--",
    };

    ui_lock();
    for (int i = 0; i < MUSIC_TRACK_PARAM_COUNT; i++)
    {
        if (s_music_ui.track_param_value[i])
        {
            lv_label_set_text(s_music_ui.track_param_value[i], values[i]);
        }
    }
    ui_unlock();
}

static void music_cover_apply_locked(void)
{
    if (s_music_ui.music_cover_img == NULL)
    {
        return;
    }
    if (s_music_cover_image.pixels)
    {
        lv_image_set_src(s_music_ui.music_cover_img, &s_music_cover_image.dsc);
        lv_obj_clear_flag(s_music_ui.music_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_image_set_src(s_music_ui.music_cover_img, NULL);
        lv_obj_add_flag(s_music_ui.music_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void music_page_set_cover_file(const char *path)
{
    if (path && s_music_cover_image.pixels && strcmp(path, s_music_cover_path) == 0)
    {
        ui_lock();
        music_cover_apply_locked();
        ui_unlock();
        return;
    }

    music_cover_image_t next = {0};
    bool loaded = path && music_cover_image_load_mp3(path, MUSIC_COVER_W, MUSIC_COVER_H, &next);

    ui_lock();
    if (s_music_ui.music_cover_img)
    {
        lv_obj_add_flag(s_music_ui.music_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_music_ui.music_cover_img, NULL);
    }
    if (s_music_cover_image.pixels)
    {
        lv_image_cache_drop(&s_music_cover_image.dsc);
    }
    music_cover_image_release(&s_music_cover_image);
    s_music_cover_path[0] = '\0';
    if (loaded)
    {
        s_music_cover_image = next;
        snprintf(s_music_cover_path, sizeof(s_music_cover_path), "%s", path);
    }
    music_cover_apply_locked();
    ui_unlock();
}

static void music_page_sync_audio_state(void)
{
    audio_music_state_t state = {0};
    if (!audio_music_state_get(&state) ||
        (s_music_last_service_state_valid && state.revision == s_music_last_service_state.revision))
    {
        return;
    }

    const audio_music_state_t *last = &s_music_last_service_state;
    if (!s_music_last_service_state_valid || state.playing != last->playing)
    {
        music_page_set_play_state(state.playing);
    }
    if (!s_music_last_service_state_valid || state.volume_percent != last->volume_percent)
    {
        music_page_set_volume(state.volume_percent);
    }
    if (!s_music_last_service_state_valid || state.play_mode != last->play_mode)
    {
        music_page_set_play_mode(state.play_mode);
    }
    if (!s_music_last_service_state_valid || state.current_track != last->current_track)
    {
        music_page_set_current_track(state.current_track);
    }
    if (!s_music_last_service_state_valid || state.current_sec != last->current_sec ||
        state.total_sec != last->total_sec)
    {
        music_page_set_progress(state.current_sec, state.total_sec);
    }
    if (!s_music_last_service_state_valid || strcmp(state.format, last->format) != 0 ||
        strcmp(state.sample_rate, last->sample_rate) != 0 ||
        strcmp(state.bitrate, last->bitrate) != 0 ||
        strcmp(state.channels, last->channels) != 0)
    {
        music_page_set_track_params(state.format, state.sample_rate, state.bitrate, state.channels);
    }
    if (!s_music_last_service_state_valid || state.lyrics_valid != last->lyrics_valid ||
        strcmp(state.lyrics_prev, last->lyrics_prev) != 0 ||
        strcmp(state.lyrics_current, last->lyrics_current) != 0 ||
        strcmp(state.lyrics_next, last->lyrics_next) != 0)
    {
        music_page_set_lyrics(state.lyrics_valid ? state.lyrics_prev : NULL,
                              state.lyrics_valid ? state.lyrics_current : NULL,
                              state.lyrics_valid ? state.lyrics_next : NULL);
    }
    if (!s_music_last_service_state_valid || state.spectrum_valid != last->spectrum_valid ||
        memcmp(state.spectrum, last->spectrum, sizeof(state.spectrum)) != 0)
    {
        music_page_set_spectrum_levels(state.spectrum_valid ? state.spectrum : NULL,
                                       state.spectrum_valid ? AUDIO_MUSIC_SPECTRUM_BAR_COUNT : 0);
    }
    if (!s_music_last_service_state_valid || strcmp(state.cover_path, last->cover_path) != 0)
    {
        music_page_set_cover_file(state.cover_path[0] ? state.cover_path : NULL);
    }

    s_music_last_service_state = state;
    s_music_last_service_state_valid = true;
}

static void music_page_timer(Page *page)
{
    (void)page;
    music_page_sync_audio_state();
    music_spectrum_update_timer();
}

static void music_page_create(Page *page)
{
    s_music_ui.root = page->root;
    lv_obj_set_style_bg_color(s_music_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_music_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_music_ui.root, 0, 0);
    lv_obj_add_flag(s_music_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_music_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    ui_background_create(s_music_ui.root);
    lv_obj_t *main_cont = lv_obj_create(page->root);
    ui_main_cont_style1_init(main_cont);
    // lv_obj_set_style_pad_row(main_cont, 8, 0);
    lv_obj_t *music_img_cont = lv_obj_create(main_cont);
    lv_obj_set_size(music_img_cont, 300, 300);
    lv_obj_align(music_img_cont, LV_ALIGN_TOP_LEFT, 40, 60);
    lv_obj_set_style_radius(music_img_cont, 0, 0);
    lv_obj_set_style_bg_color(music_img_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(music_img_cont, LV_OPA_30, 0);
    lv_obj_set_style_border_width(music_img_cont, 0, 0);
    lv_obj_set_style_border_color(music_img_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_pad_all(music_img_cont, 0, 0);
    lv_obj_set_scrollbar_mode(music_img_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(music_img_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_img_cont, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_add_event_cb(music_img_cont, music_cover_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *music_icon = lv_label_create(music_img_cont);
    lv_label_set_text(music_icon, LV_SYMBOL_AUDIO);
    lv_obj_align(music_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(music_icon, &lv_font_SourceHanSansCN_Bold_2_50, 0);
    lv_obj_set_style_text_color(music_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(music_icon, LV_TEXT_ALIGN_CENTER, 0);

    s_music_ui.music_cover_img = lv_image_create(music_img_cont);
    lv_obj_set_size(s_music_ui.music_cover_img, MUSIC_COVER_W, MUSIC_COVER_H);
    lv_obj_center(s_music_ui.music_cover_img);
    lv_obj_set_style_radius(s_music_ui.music_cover_img, 0, 0);
    lv_obj_remove_flag(s_music_ui.music_cover_img, LV_OBJ_FLAG_CLICKABLE);
    music_cover_apply_locked();

    s_music_lyric_layer = lv_obj_create(main_cont);
    lv_obj_set_size(s_music_lyric_layer, 300, 200);
    lv_obj_align(s_music_lyric_layer, LV_ALIGN_TOP_LEFT, 380, 110);
    lv_obj_set_style_bg_opa(s_music_lyric_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_music_lyric_layer, 0, 0);
    lv_obj_set_style_pad_all(s_music_lyric_layer, 0, 0);
    lv_obj_set_scrollbar_mode(s_music_lyric_layer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_music_lyric_layer, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < MUSIC_LYRIC_LINE_COUNT; i++)
    {
        s_music_ui.paly_lyric_text[i] = lv_label_create(s_music_lyric_layer);
        lv_obj_set_size(s_music_ui.paly_lyric_text[i], MUSIC_LYRIC_LINE_W, MUSIC_LYRIC_LINE_H);
        lv_obj_align(s_music_ui.paly_lyric_text[i], LV_ALIGN_CENTER, 0, music_lyric_line_y_get(i));
        lv_label_set_long_mode(s_music_ui.paly_lyric_text[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(s_music_ui.paly_lyric_text[i],
                                   i == 1 ? MUSIC_LYRIC_FONT_CURRENT : MUSIC_LYRIC_FONT_SMALL,
                                   0);
        lv_obj_set_style_text_color(s_music_ui.paly_lyric_text[i], lv_color_hex(i == 1 ? UI_TEXT : UI_MUTED), 0);
        lv_obj_set_style_text_align(s_music_ui.paly_lyric_text[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_opa(s_music_ui.paly_lyric_text[i], music_lyric_line_opa_get(i), 0);
        lv_obj_remove_flag(s_music_ui.paly_lyric_text[i], LV_OBJ_FLAG_CLICKABLE);
    }
    snprintf(s_music_lyric_cache[0], sizeof(s_music_lyric_cache[0]), "%s", "SD card local music");
    snprintf(s_music_lyric_cache[1], sizeof(s_music_lyric_cache[1]), "%s", "No lyrics for this track");
    snprintf(s_music_lyric_cache[2], sizeof(s_music_lyric_cache[2]), "%s", "Add .lrc with the same file name");
    music_lyrics_apply_cached(false);
    // lv_obj_add_flag(s_music_lyric_layer, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *song_info_cont = ui_flex_container_create(main_cont, 640, 80, LV_FLEX_FLOW_COLUMN,
                                                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(song_info_cont, LV_ALIGN_TOP_LEFT, 40, 380);
    lv_obj_set_style_pad_top(song_info_cont, 6, 0);
    lv_obj_set_style_pad_bottom(song_info_cont, 6, 0);
    lv_obj_set_style_pad_row(song_info_cont, 4, 0);
    lv_obj_set_style_bg_color(song_info_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(song_info_cont, LV_OPA_0, 0);

    s_music_ui.song_name_label = lv_label_create(song_info_cont);
    lv_obj_set_width(s_music_ui.song_name_label, 500);
    lv_label_set_text(s_music_ui.song_name_label, "music_file.mp3");
    lv_label_set_long_mode(s_music_ui.song_name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_music_ui.song_name_label, MUSIC_NAME_FONT, 0);
    lv_obj_set_style_text_color(s_music_ui.song_name_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(s_music_ui.song_name_label, LV_TEXT_ALIGN_CENTER, 0);

    s_music_ui.paly_progress_bar = lv_bar_create(song_info_cont);
    lv_obj_set_size(s_music_ui.paly_progress_bar, LV_PCT(80), MUSIC_PROGRESS_H);
    lv_bar_set_range(s_music_ui.paly_progress_bar, 0, 100);
    lv_bar_set_value(s_music_ui.paly_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_music_ui.paly_progress_bar, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_ui.paly_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_ui.paly_progress_bar, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.paly_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_ui.paly_progress_bar, MUSIC_PROGRESS_H / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_ui.paly_progress_bar, MUSIC_PROGRESS_H / 2, LV_PART_INDICATOR);

    lv_obj_t *time_row = ui_flex_container_create(song_info_cont,
                                                  LV_PCT(80),
                                                  14,
                                                  LV_FLEX_FLOW_ROW,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    s_music_ui.paly_progress_current_time_label = ui_label_create(time_row, "00:00", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(s_music_ui.paly_progress_current_time_label, 80);
    lv_obj_set_style_text_align(s_music_ui.paly_progress_current_time_label, LV_TEXT_ALIGN_LEFT, 0);
    ui_flex_spacer_create(time_row);
    s_music_ui.paly_progress_total_time_label = ui_label_create(time_row, "--:--", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(s_music_ui.paly_progress_total_time_label, 80);
    lv_obj_set_style_text_align(s_music_ui.paly_progress_total_time_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_music_ui.artist_album_label = lv_label_create(song_info_cont);
    lv_obj_set_width(s_music_ui.artist_album_label, 500);
    lv_label_set_text(s_music_ui.artist_album_label, "Unknown artist / Unknown album");
    lv_label_set_long_mode(s_music_ui.artist_album_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_music_ui.artist_album_label, MUSIC_NAME_FONT, 0);
    lv_obj_set_style_text_color(s_music_ui.artist_album_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.artist_album_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *music_cont = ui_flex_container_create(main_cont, 640, 160, LV_FLEX_FLOW_COLUMN,
                                                    LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(music_cont, LV_ALIGN_TOP_LEFT, 40, 500);
    lv_obj_set_style_pad_all(music_cont, 8, 0);
    lv_obj_set_style_pad_row(music_cont, 4, 0);
    lv_obj_set_style_bg_color(music_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(music_cont, LV_OPA_20, 0);
    lv_obj_set_style_border_color(music_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(music_cont, 1, 0);
    lv_obj_set_style_radius(music_cont, 80, 0);
    lv_obj_set_style_shadow_color(music_cont, lv_color_hex(0xafafaf), 0);
    lv_obj_set_style_shadow_opa(music_cont, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(music_cont, 30, 0);
    lv_obj_set_style_shadow_spread(music_cont, 3, 0);
    lv_obj_set_style_shadow_offset_y(music_cont, 0, 0);
    lv_obj_set_style_shadow_offset_x(music_cont, 0, 0);

    lv_obj_t *music_crtl_cont = ui_flex_container_create(music_cont, LV_PCT(100), 88, LV_FLEX_FLOW_ROW,
                                                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(music_crtl_cont, 14, 0);
    music_list_button_create(music_crtl_cont);
    s_music_ui.play_mode_btn_label = music_control_button_create(music_crtl_cont, music_play_mode_symbol(s_music_play_mode), 80, false, MUSIC_CONTROL_PLAY_MODE);
    music_control_button_create(music_crtl_cont, LV_SYMBOL_PREV, 60, false, MUSIC_CONTROL_PREV);
    s_music_ui.play_btn_label = music_control_button_create(music_crtl_cont, LV_SYMBOL_PAUSE, 80, true, MUSIC_CONTROL_PLAY_PAUSE);
    music_control_button_create(music_crtl_cont, LV_SYMBOL_NEXT, 60, false, MUSIC_CONTROL_NEXT);
    music_playlist_panel_create();

    lv_obj_t *music_volume_cont = ui_flex_container_create(music_cont, LV_PCT(100), 44, LV_FLEX_FLOW_ROW,
                                                           LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(music_volume_cont, 28, 0);
    lv_obj_set_style_pad_right(music_volume_cont, 28, 0);
    lv_obj_set_style_pad_column(music_volume_cont, 12, 0);

    lv_obj_t *volume_icon = lv_label_create(music_volume_cont);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_width(volume_icon, 32);
    lv_obj_set_style_text_align(volume_icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(volume_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(volume_icon, &lv_font_montserrat_24, 0);

    s_music_ui.volume_slider = lv_slider_create(music_volume_cont);
    lv_obj_set_width(s_music_ui.volume_slider, 0);
    lv_obj_set_height(s_music_ui.volume_slider, 10);
    lv_obj_set_flex_grow(s_music_ui.volume_slider, 1);
    lv_slider_set_range(s_music_ui.volume_slider, 0, 100);
    lv_slider_set_value(s_music_ui.volume_slider, s_music_volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_ui.volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_ui.volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(s_music_ui.volume_slider, 24, LV_PART_KNOB);
    lv_obj_set_style_height(s_music_ui.volume_slider, 24, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_music_ui.volume_slider, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_music_ui.volume_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_music_ui.volume_slider, LV_OPA_50, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_music_ui.volume_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_music_ui.volume_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb,
                        LV_EVENT_GESTURE, NULL);

    music_page_set_volume(s_music_volume_percent);
}

static void music_page_enter(Page *page)
{
    (void)page;
    music_page_scan_sd_music();
}

static void music_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void music_page_destroy(Page *page)
{
    (void)page;
    if (s_music_cover_image.pixels)
    {
        lv_image_cache_drop(&s_music_cover_image.dsc);
    }
    music_cover_image_release(&s_music_cover_image);
    s_music_cover_path[0] = '\0';
    memset(&s_music_ui, 0, sizeof(s_music_ui));
    s_music_playlist_panel = NULL;
    s_music_playlist_list_cont = NULL;
    s_music_cover_layer = NULL;
    s_music_lyric_layer = NULL;
    s_music_show_lyrics = false;
    s_music_volume_dragging = false;
    s_music_last_service_state_valid = false;
    memset(s_music_playlist_item, 0, sizeof(s_music_playlist_item));
    memset(s_music_playlist_item_index, 0, sizeof(s_music_playlist_item_index));
    memset(s_music_playlist_item_name, 0, sizeof(s_music_playlist_item_name));
    memset(s_music_playlist_item_play, 0, sizeof(s_music_playlist_item_play));
    s_music_playlist_item_count = 0;
    for (int i = 0; i < MUSIC_PLAYLIST_VISIBLE_COUNT; i++)
    {
        s_music_playlist_row_track_index[i] = -1;
    }
}

int music_page_get_current_track(void)
{
    return s_music_current_index;
}

int music_page_get_volume(void)
{
    return s_music_volume_percent;
}

void music_page_register(void)
{
    Page page = {
        .id = PAGE_MUSIC,
        .name = "music",
        .on_create = music_page_create,
        .on_enter = music_page_enter,
        .on_leave = music_page_leave,
        .on_destroy = music_page_destroy,
        .on_gesture = music_page_gesture,
        .page_timer = music_page_timer,
        .timer_interval_ms = 50,
    };
    ui_page_register(&page);
}
