#include "ui.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include "esp_err.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "driver/jpeg_decode.h"
#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif
#include "lcd_jd9365_driver.h"
#include "camera.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "lvgl_page_manager.h"
#include "lora_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "[UI]";

static void display_dim_timer_start(void);

#define SET_WIFI_SCAN_MAX_APS 16

static start_page_ui_t s_start_ui;
static status_bar_ui_t s_status_ui;
static lv_timer_t *s_status_bar_timer;
static home_page_ui_t s_home_ui;
static music_page_ui_t s_music_ui;
static camera_page_ui_t s_camera_ui;
static uint8_t *s_camera_last_photo_thumb_buf;
static lv_image_dsc_t s_camera_last_photo_dsc;
static lora_page_ui_t s_lora_ui;
static file_page_ui_t s_file_ui;
static uint8_t *s_file_image_buf;
static lv_image_dsc_t s_file_image_dsc;
static char s_file_open_pending_path[FILE_PATH_MAX_LEN];
static char s_file_open_pending_name[FILE_NAME_MAX_LEN];
static bmu_page_ui_t s_bmu_ui;
static set_page_ui_t s_set_ui;
static char s_file_current_path[FILE_PATH_MAX_LEN] = FILE_SCAN_DIR;
static char s_file_pending_path[FILE_PATH_MAX_LEN] = FILE_SCAN_DIR;

typedef struct
{
    lv_obj_t *item;
    lv_obj_t *icon;
    lv_obj_t *status_cont;
    lv_obj_t *status_label[4];
} home_app_item_t;

typedef struct
{
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *desc_label;
    lv_obj_t *progress_track;
    lv_obj_t *progress_thumb;
    home_app_item_t items[HOME_APP_MENU_COUNT];
    int selected;
    int displayed_selected;
    int32_t position_fp;
    int32_t last_vect_y;
} home_app_menu_t;

struct tm timeinfo = {0};
static home_app_menu_t s_app_menu;
static lv_style_t s_app_menu_selected_style;
static bool s_app_menu_selected_style_inited = false;
static int s_home_mic_db[HOME_MIC_METER_COUNT] = {MIC_DB_MIN, MIC_DB_MIN};
static int s_home_mic_active[HOME_MIC_METER_COUNT] = {-1, -1};
static int s_home_mic_label_db[HOME_MIC_METER_COUNT] = {MIC_DB_MIN - 1, MIC_DB_MIN - 1};
static char s_home_calendar_cells[HOME_CALENDAR_CELL_COUNT][3];
static const char *s_home_calendar_map[HOME_CALENDAR_MAP_COUNT];
static lv_buttonmatrix_ctrl_t s_home_calendar_ctrl[HOME_CALENDAR_BUTTON_COUNT];
static int s_home_calendar_year = -1;
static int s_home_calendar_month = -1;
static int s_home_calendar_day = -1;

static char s_music_track_names[MUSIC_SCAN_MAX_TRACKS][MUSIC_TRACK_NAME_MAX_LEN];
static const char *s_music_track_ptrs[MUSIC_SCAN_MAX_TRACKS];
static int s_music_track_count = 0;
static int s_music_current_index = -1;
static int s_music_playlist_start = 0;
static int s_music_playlist_drag_accum = 0;
static int s_music_playlist_row_track_index[MUSIC_PLAYLIST_VISIBLE_COUNT] = {-1, -1, -1, -1};
static bool s_music_playing = false;
static int s_music_play_mode = 0;
static int s_music_volume_percent = 64;
static volatile uint8_t s_music_spectrum_target[MUSIC_SPECTRUM_BAR_COUNT];
static uint16_t s_music_spectrum_display_x16[MUSIC_SPECTRUM_BAR_COUNT];
static volatile bool s_music_spectrum_has_data = false;

static char s_camera_home_mode[24] = "1080P";
static int s_camera_home_fps_x10 = 250;

static int s_lora_status = LORA_CONTROL_STOP;
static lora_control_action_t s_lora_pending_action = LORA_CONTROL_STOP;
static TaskHandle_t s_lora_tx_task_handle = NULL;
static TaskHandle_t s_lora_rx_task_handle = NULL;
static int s_lora_sf = LORA_SF_DEFAULT;
static int s_lora_cr = LORA_CR_DEFAULT;
static int s_lora_power_dbm = LORA_POWER_DEFAULT_DBM;
static int s_lora_tx_interval_ms = LORA_TX_INTERVAL_DEFAULT_MS;
static float s_lora_bw = 125.0f;
static bool s_lora_rx_enabled = false;
static bool s_lora_periodic_tx_enabled = false;
static bool s_lora_mode_switch_pending = false;
static volatile bool s_lora_page_active = false;
static volatile bool s_lora_tx_busy = false;
static int64_t s_lora_next_periodic_tx_us = 0;
static int64_t s_lora_mode_switch_start_us = 0;
static char s_lora_tx_text[LORA_TEXT_MAX_LEN];
static float s_lora_freq = LORA_FREQ_DEFAULT_X10;
static const float s_lora_freq_choices[] = {
    433.0,
    868.0,
    915.0,
    923.0,
    2400.0,
    2450.0,
};
static const float s_lota_bw_choices[] = {
    62.5,
    125.0,
    250.0,
    406.0,
    500.0,
    812.0,
    1000.0,
};

static int s_bmu_charge_current_ma = 512;
static int s_bmu_input_current_limit_ma = 1500;
static int s_bmu_low_warn_percent = 10;
static int s_set_brightness_percent = SET_BRIGHTNESS_DEFAULT;
static bool s_set_auto_dim_enabled = true;
static uint32_t s_set_screen_timeout_ms = 30000;
static bool s_display_dimmed = false;
static lv_timer_t *s_display_dim_timer = NULL;
static bool s_display_indev_hooked = false;
static uint32_t s_display_last_activity_ms = 0;
static volatile bool s_set_power_action_busy = false;
static volatile bool s_set_usb_otg_switch_busy = false;
static volatile bool s_set_usb_otg_usb_mode = false;
static uint32_t s_set_storage_last_update_ms = 0;
static volatile bool s_set_low_power_enabled = false;
static volatile bool s_set_low_power_active = false;
static volatile bool s_set_low_power_transition = false;
static bool s_bmu_ui_updating = false;

static wifi_scan_ap_info_t s_set_wifi_aps[SET_WIFI_SCAN_MAX_APS];
static int s_set_wifi_ap_count = 0;
static char s_set_wifi_scan_status[64] = "Tap WiFi Remote to scan";
static volatile bool s_set_wifi_scan_busy = false;
static TaskHandle_t s_set_wifi_scan_task_handle = NULL;
static char s_set_wifi_selected_ssid[33];
static wifi_auth_mode_t s_set_wifi_selected_auth = WIFI_AUTH_WPA2_PSK;
static char s_set_wifi_last_connected_ssid[33];

#define LORA_TASK_STACK_SIZE 6144
#define LORA_MODE_SWITCH_WAIT_MS 2000
#define LORA_TASK_STOP_POLL_MS 20
#define VIDEO_PREVIEW_MAX_W 210
#define VIDEO_PREVIEW_MAX_H 210
#define VIDEO_VIEWER_MAX_W 640
#define VIDEO_VIEWER_MAX_H 500
#define VIDEO_IMAGE_MAX_FILE_SIZE (4 * 1024 * 1024)
#define VIDEO_JPEG_MAX_DECODE_BYTES (24 * 1024 * 1024)
#define VIDEO_PNG_MAX_DECODE_BYTES (96 * 1024)
#define FILE_TXT_MAX_BYTES (32 * 1024)
#define FILE_BMP_MAX_DECODE_BYTES (8 * 1024 * 1024)
#define FILE_JPEG_MAX_INPUT_BYTES (8 * 1024 * 1024)

static const char *s_app_menu_icons[HOME_APP_MENU_COUNT] = {
    LV_SYMBOL_AUDIO,
    LV_SYMBOL_VIDEO,
    LV_SYMBOL_WIFI,
    LV_SYMBOL_PASTE,
    LV_SYMBOL_CHARGE,
    LV_SYMBOL_SETTINGS,
};

static const char *s_app_menu_titles[HOME_APP_MENU_COUNT] = {
    "MUSIC",
    "CAMERA",
    "LORA",
    "FILE",
    "BMU",
    "SET",
};

static const char *s_app_menu_descs[HOME_APP_MENU_COUNT] = {
    "Codec: ES8389\n\nFormat: MP3/WAV/AAC/LRC\n\nOutput: Speaker/I2S",
    "Sensor: OV2710\n\nResolution: 1080P/720P\n\nFPS: 25fps\n\nSave: SD Card",
    "Module: Auto Detect\n\nBand: Sub-GHz/2.4GHz\n\nMode: TX/RX",
    "Storage: SD Card\n\nFormat: TXT/BMP/JPEG/PNG\n\nBrowser: Local Files",
    "PMIC: AXP517\n\nBattery: Gauge/Charge\n\nPower: Ship/Low",
    "Panel: Brightness/Timeout\n\nWiFi: Scan/Connect\n\nUSB: MSC/App",
};

static const char *s_start_device_names[START_DEVICE_COUNT] = {
    "LCD",
    "TOUCH",
    "SD CARD",
    "BMU",
    "LORA",
    "AUDIO",
    "ESP32-C5",
    "CAMERA",
};

static void home_calendar_update(const struct tm *timeinfo);
static const char *home_battery_symbol(int percent);
static int home_clamp_percent(int percent);
static void home_music_status_update_apply(void);
static void home_app_status_update(void);
static const char *lora_page_chip_name(lora_app_chip_t chip);

static void ui_obj_set_transparent(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static void ui_obj_set_flex(lv_obj_t *obj, lv_flex_flow_t flow, lv_flex_align_t main_place, lv_flex_align_t cross_place, lv_flex_align_t track_cross_place)
{
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_flex_align(obj, main_place, cross_place, track_cross_place);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *ui_flex_container_create(lv_obj_t *parent, int32_t w, int32_t h, lv_flex_flow_t flow,
                                          lv_flex_align_t main_place, lv_flex_align_t cross_place, lv_flex_align_t track_cross_place)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    ui_obj_set_transparent(obj);
    ui_obj_set_flex(obj, flow, main_place, cross_place, track_cross_place);
    return obj;
}

static lv_obj_t *ui_flex_spacer_create(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);
    ui_obj_set_transparent(spacer);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    return spacer;
}

static void ui_main_cont_style_init(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BG), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_size(obj, SCREEN_WIDTH, 680);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 20, 0);
    ui_obj_set_flex(obj, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    return label;
}

static void app_page_destroy_async_cb(void *user_data)
{
    PageType id = (PageType)(uintptr_t)user_data;
    ui_page_destroy(id);
}

/*********Start Page************/
static void start_device_row_set(start_device_t device, const char *symbol, const char *status, uint32_t color)
{
    if (device < 0 || device >= START_DEVICE_COUNT)
    {
        return;
    }

    if (s_start_ui.device_icon[device])
    {
        lv_label_set_text(s_start_ui.device_icon[device], symbol);
        lv_obj_set_style_text_color(s_start_ui.device_icon[device], lv_color_hex(color), 0);
    }

    if (s_start_ui.device_status[device])
    {
        lv_label_set_text(s_start_ui.device_status[device], status);
        lv_obj_set_style_text_color(s_start_ui.device_status[device], lv_color_hex(color), 0);
    }
}

static void start_device_row_create(lv_obj_t *parent, start_device_t device)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             30,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 8, 0);

    s_start_ui.device_icon[device] = lv_label_create(row);
    lv_obj_set_width(s_start_ui.device_icon[device], 28);
    lv_obj_set_style_text_font(s_start_ui.device_icon[device], &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_start_ui.device_icon[device], LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, s_start_device_names[device]);
    lv_obj_set_width(name, 180);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

    ui_flex_spacer_create(row);

    s_start_ui.device_status[device] = lv_label_create(row);
    lv_obj_set_width(s_start_ui.device_status[device], 96);
    lv_obj_set_style_text_font(s_start_ui.device_status[device], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_start_ui.device_status[device], LV_TEXT_ALIGN_RIGHT, 0);

    start_device_row_set(device, LV_SYMBOL_REFRESH, "WAIT", UI_MUTED);
}

static void start_page_device_list_create(lv_obj_t *parent)
{
    for (int i = 0; i < START_DEVICE_COUNT; i++)
    {
        start_device_row_create(parent, (start_device_t)i);
    }

    start_device_row_set(START_DEVICE_LCD, LV_SYMBOL_OK, "OK", UI_OK);
    start_device_row_set(START_DEVICE_TOUCH, LV_SYMBOL_OK, "OK", UI_OK);
}

static void start_page_create(Page *page)
{
    display_dim_timer_start();

    s_start_ui.root = page->root;
    lv_obj_set_style_bg_color(s_start_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_start_ui.root, LV_SCROLLBAR_MODE_OFF);

    s_start_ui.main_cont = ui_flex_container_create(s_start_ui.root,
                                                    LV_PCT(100),
                                                    LV_PCT(100),
                                                    LV_FLEX_FLOW_COLUMN,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_center(s_start_ui.main_cont);
    lv_obj_set_style_pad_row(s_start_ui.main_cont, 24, 0);

    s_start_ui.device_name_label = lv_label_create(s_start_ui.main_cont);
    lv_label_set_text(s_start_ui.device_name_label, "T-Panle-P4");
    lv_obj_set_style_text_color(s_start_ui.device_name_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_start_ui.device_name_label, &lv_font_montserrat_40, 0);

    s_start_ui.init_label = lv_label_create(s_start_ui.main_cont);
    lv_label_set_text(s_start_ui.init_label, "INITIALIZING");
    lv_obj_set_style_text_color(s_start_ui.init_label, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_text_font(s_start_ui.init_label, &lv_font_montserrat_24, 0);

    s_start_ui.init_device_cont = lv_obj_create(s_start_ui.main_cont);
    lv_obj_set_size(s_start_ui.init_device_cont, 600, 398);
    lv_obj_set_style_bg_color(s_start_ui.init_device_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_start_ui.init_device_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_start_ui.init_device_cont, 1, 0);
    lv_obj_set_style_border_color(s_start_ui.init_device_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_start_ui.init_device_cont, 16, 0);
    lv_obj_set_style_pad_all(s_start_ui.init_device_cont, 18, 0);
    lv_obj_set_style_pad_row(s_start_ui.init_device_cont, 5, 0);
    ui_obj_set_flex(s_start_ui.init_device_cont,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    s_start_ui.status_label = lv_label_create(s_start_ui.init_device_cont);
    lv_label_set_text(s_start_ui.status_label, "System self-check");
    lv_obj_set_width(s_start_ui.status_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_start_ui.status_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_start_ui.status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_start_ui.status_label, LV_TEXT_ALIGN_LEFT, 0);

    s_start_ui.sub_status_label = lv_label_create(s_start_ui.init_device_cont);
    lv_label_set_text(s_start_ui.sub_status_label, "Preparing peripherals...");
    lv_obj_set_width(s_start_ui.sub_status_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_start_ui.sub_status_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(s_start_ui.sub_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_start_ui.sub_status_label, LV_TEXT_ALIGN_LEFT, 0);

    start_page_device_list_create(s_start_ui.init_device_cont);

    s_start_ui.progress_bar = lv_bar_create(s_start_ui.main_cont);
    lv_obj_set_size(s_start_ui.progress_bar, 660, 8);
    lv_bar_set_mode(s_start_ui.progress_bar, LV_BAR_MODE_NORMAL);
    lv_bar_set_range(s_start_ui.progress_bar, 0, 100);
    lv_bar_set_value(s_start_ui.progress_bar, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_opa(s_start_ui.progress_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_start_ui.progress_bar, lv_color_hex(UI_LINE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_start_ui.progress_bar, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_opa(s_start_ui.progress_bar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_start_ui.progress_bar, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_start_ui.progress_bar, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    s_start_ui.logo_label = lv_label_create(s_start_ui.main_cont);
    lv_label_set_text(s_start_ui.logo_label, "LILYGO");
    lv_obj_set_style_text_color(s_start_ui.logo_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(s_start_ui.logo_label, &lv_font_montserrat_28, 0);
}

static void start_page_destroy(Page *page)
{
    memset(&s_start_ui, 0, sizeof(s_start_ui));
}

void start_page_set_status(const char *text, int progress)
{
    ui_lock();

    bool done = progress >= 100;
    bool failed = done && text != NULL && strstr(text, "failed") != NULL;

    if (s_start_ui.init_label)
    {
        lv_label_set_text(s_start_ui.init_label, done ? (failed ? "INIT FAILED" : "READY") : "INITIALIZING");
        lv_obj_set_style_text_color(s_start_ui.init_label,
                                    lv_color_hex(failed ? UI_ERROR : (done ? UI_OK : UI_PRIMARY)),
                                    0);
    }

    if (s_start_ui.sub_status_label)
    {
        lv_label_set_text(s_start_ui.sub_status_label, text);
    }

    if (s_start_ui.progress_bar)
    {
        lv_obj_set_style_bg_color(s_start_ui.progress_bar,
                                  lv_color_hex(failed ? UI_ERROR : UI_PRIMARY),
                                  LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_bar_set_value(s_start_ui.progress_bar, progress, LV_ANIM_ON);
    }

    ui_unlock();
}

void start_page_set_device_status(start_device_t device, bool ok)
{
    ui_lock();
    start_device_row_set(device,
                         ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                         ok ? "OK" : "FAIL",
                         ok ? UI_OK : UI_ERROR);
    ui_unlock();
}

/*********Status Bar************/
static void status_bar_refresh(void)
{
    status_info_t status_info = {0};
    if (status_bar_info_get(&status_info))
    {
        status_bar_update(&status_info);
    }
    else
    {
        status_bar_update(NULL);
    }
}

static void status_bar_timer_cb(lv_timer_t *timer)
{
    status_bar_refresh();
}

static void status_bar_timer_start(void)
{
    if (s_status_bar_timer == NULL)
    {
        s_status_bar_timer = lv_timer_create(status_bar_timer_cb, STATUS_TIMER_MS, NULL);
    }
}

static void status_bar_create(void)
{
    if (s_status_ui.status_cont)
    {
        lv_obj_move_foreground(s_status_ui.status_cont);
        status_bar_timer_start();
        display_dim_timer_start();
        return;
    }

    s_status_ui.status_cont = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_status_ui.status_cont, SCREEN_WIDTH, 40);
    lv_obj_align(s_status_ui.status_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_status_ui.status_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(s_status_ui.status_cont, 0, 0);
    lv_obj_set_style_radius(s_status_ui.status_cont, 0, 0);
    lv_obj_set_style_pad_all(s_status_ui.status_cont, 0, 0);
    lv_obj_set_style_pad_left(s_status_ui.status_cont, 20, 0);
    lv_obj_set_style_pad_right(s_status_ui.status_cont, 20, 0);
    lv_obj_set_style_pad_column(s_status_ui.status_cont, 10, 0);
    lv_obj_remove_flag(s_status_ui.status_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_ui.status_cont, LV_OBJ_FLAG_CLICKABLE);
    ui_obj_set_flex(s_status_ui.status_cont,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    s_status_ui.time_label = lv_label_create(s_status_ui.status_cont);
    lv_label_set_text(s_status_ui.time_label, "12:00");
    lv_obj_set_style_text_color(s_status_ui.time_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_status_ui.time_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_status_ui.time_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *spacer = lv_obj_create(s_status_ui.status_cont);
    lv_obj_set_size(spacer, 0, 1);
    lv_obj_set_flex_grow(spacer, 1); // 占据剩余空间
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);

    s_status_ui.wifi_icon = lv_label_create(s_status_ui.status_cont);
    lv_label_set_text(s_status_ui.wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_status_ui.wifi_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_status_ui.wifi_icon, &lv_font_montserrat_16, 0);

    s_status_ui.bluetooth_icon = lv_label_create(s_status_ui.status_cont);
    lv_label_set_text(s_status_ui.bluetooth_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_status_ui.bluetooth_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_status_ui.bluetooth_icon, &lv_font_montserrat_16, 0);

    s_status_ui.battery_icon = lv_label_create(s_status_ui.status_cont);
    lv_label_set_text(s_status_ui.battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_status_ui.battery_icon, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_status_ui.battery_icon, &lv_font_montserrat_16, 0);

    s_status_ui.battery_label = lv_label_create(s_status_ui.status_cont);
    lv_label_set_text(s_status_ui.battery_label, "100%");
    lv_obj_set_style_text_color(s_status_ui.battery_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_status_ui.battery_label, &lv_font_montserrat_16, 0);

    status_bar_timer_start();
    display_dim_timer_start();
}

void status_bar_update(const status_info_t *status)
{
    ui_lock();

    if (s_status_ui.time_label)
    {
        char time_str[6] = "--:--";
        time_t now = time(NULL);
        if (now > 1609459200 && localtime_r(&now, &timeinfo) != NULL)
        {
            strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
        }
        lv_label_set_text(s_status_ui.time_label, time_str);
    }

    if (status)
    {
        if (s_status_ui.wifi_icon)
        {
            lv_obj_set_style_text_color(s_status_ui.wifi_icon,
                                        lv_color_hex(status->wifi_connected ? UI_OK : UI_MUTED),
                                        0);
        }

        if (s_status_ui.bluetooth_icon)
        {
            uint32_t bt_color = UI_MUTED;
            if (status->bluetooth_connected)
            {
                bt_color = UI_PRIMARY;
            }
            else if (status->bluetooth_enabled)
            {
                bt_color = UI_WARN;
            }
            lv_obj_set_style_text_color(s_status_ui.bluetooth_icon, lv_color_hex(bt_color), 0);
        }

        if (s_status_ui.battery_icon)
        {
            int battery_percent = home_clamp_percent(status->battery_percent);
            lv_label_set_text(s_status_ui.battery_icon,
                              status->battery_charging ? LV_SYMBOL_CHARGE : home_battery_symbol(battery_percent));
            lv_obj_set_style_text_color(s_status_ui.battery_icon,
                                        lv_color_hex(battery_percent >= 0 && battery_percent < 15 ? UI_ERROR : UI_TEXT),
                                        0);
        }

        if (s_status_ui.battery_label)
        {
            int battery_percent = home_clamp_percent(status->battery_percent);
            if (battery_percent >= 0)
            {
                lv_label_set_text_fmt(s_status_ui.battery_label, "%d%%", battery_percent);
            }
            else
            {
                lv_label_set_text(s_status_ui.battery_label, "--%");
            }
            lv_obj_set_style_text_color(s_status_ui.battery_label,
                                        lv_color_hex(battery_percent >= 0 && battery_percent < 15 ? UI_ERROR : UI_TEXT),
                                        0);
        }
    }

    ui_unlock();
}

/*********Home Page************/
static void home_create_metric_row(lv_obj_t *parent, const char *name, const char *value, int32_t percent,
                                   uint32_t color, lv_obj_t **ret_value_label, lv_obj_t **ret_bar)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             24,
                                             LV_FLEX_FLOW_COLUMN,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(row, 2, 0);

    lv_obj_t *header = ui_flex_container_create(row,
                                                LV_PCT(100),
                                                15,
                                                LV_FLEX_FLOW_ROW,
                                                LV_FLEX_ALIGN_START,
                                                LV_FLEX_ALIGN_CENTER,
                                                LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(header, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 90);

    ui_flex_spacer_create(header);

    lv_obj_t *value_label = ui_label_create(header, value, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(value_label, 54);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (ret_value_label)
    {
        *ret_value_label = value_label;
    }

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, LV_PCT(100), 4);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    if (ret_bar)
    {
        *ret_bar = bar;
    }
}

static void home_create_info_row(lv_obj_t *parent, const char *name, const char *value, uint32_t color, lv_obj_t **ret_value_label)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             18,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(row, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 90);

    ui_flex_spacer_create(row);

    lv_obj_t *value_label = ui_label_create(row, value, &lv_font_montserrat_14, color);
    lv_obj_set_width(value_label, 120);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (ret_value_label)
    {
        *ret_value_label = value_label;
    }
}

static uint32_t mic_segment_color(int index)
{
    int level_pct = ((index + 1) * 100) / MIC_METER_SEGMENTS;
    if (level_pct >= 86)
    {
        return UI_SECONDARY;
    }
    if (level_pct >= 70)
    {
        return UI_WARN;
    }
    if (level_pct >= 35)
    {
        return UI_PRIMARY;
    }
    return UI_OK;
}

static int mic_db_to_segments(int db)
{
    if (db < MIC_DB_MIN)
    {
        db = MIC_DB_MIN;
    }
    else if (db > MIC_DB_MAX)
    {
        db = MIC_DB_MAX;
    }

    int range = MIC_DB_MAX - MIC_DB_MIN;
    int active = ((db - MIC_DB_MIN) * MIC_METER_SEGMENTS + range - 1) / range;
    if (active < 1 && db > MIC_DB_MIN)
    {
        active = 1;
    }
    return active;
}

static void home_mic_meter_update(int mic, int db)
{
    if (mic < 0 || mic >= MIC_METER_COUNT)
    {
        return;
    }

    if (db < MIC_DB_MIN)
    {
        db = MIC_DB_MIN;
    }
    else if (db > MIC_DB_MAX)
    {
        db = MIC_DB_MAX;
    }
    s_home_mic_db[mic] = db;

    if (s_home_ui.mic_db_label[mic] && s_home_mic_label_db[mic] != db)
    {
        // char db_text[16];
        // snprintf(db_text, sizeof(db_text), "%d dB", db);
        // lv_label_set_text(s_home_ui.mic_db_label[mic], db_text);
        s_home_mic_label_db[mic] = db;
    }

    int active = mic_db_to_segments(db);
    int old_active = s_home_mic_active[mic];
    if (old_active == active)
    {
        return;
    }

    int start = 0;
    int end = MIC_METER_SEGMENTS;
    if (old_active >= 0)
    {
        start = LV_MIN(old_active, active);
        end = LV_MAX(old_active, active);
    }

    for (int i = start; i < end; i++)
    {
        lv_obj_t *seg = s_home_ui.mic_segments[mic][i];
        if (!seg)
        {
            continue;
        }

        uint32_t color = i < active ? mic_segment_color(i) : UI_LINE;
        lv_opa_t opa = i < active ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_color(seg, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(seg, opa, 0);
    }

    s_home_mic_active[mic] = active;
}

static void home_mic_meter_reset_cache(void)
{
    for (int mic = 0; mic < MIC_METER_COUNT; mic++)
    {
        s_home_mic_active[mic] = -1;
        s_home_mic_label_db[mic] = MIC_DB_MIN - 1;
        for (int i = 0; i < MIC_METER_SEGMENTS; i++)
        {
            s_home_ui.mic_segments[mic][i] = NULL;
        }
        s_home_ui.mic_db_label[mic] = NULL;
    }
}

static int home_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 0 || month > 11)
    {
        return 30;
    }

    if (month == 1)
    {
        bool leap = ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
        return leap ? 29 : 28;
    }

    return days[month];
}

static void home_calendar_map_init(void)
{
    static const char *weekdays[] = {"S", "M", "T", "W", "T", "F", "S"};

    memset(s_home_calendar_ctrl, 0, sizeof(s_home_calendar_ctrl));
    s_home_calendar_year = -1;
    s_home_calendar_month = -1;
    s_home_calendar_day = -1;

    int map_index = 0;
    for (int i = 0; i < 7; i++)
    {
        s_home_calendar_map[map_index++] = weekdays[i];
    }
    s_home_calendar_map[map_index++] = "\n";

    for (int i = 0; i < HOME_CALENDAR_CELL_COUNT; i++)
    {
        s_home_calendar_cells[i][0] = ' ';
        s_home_calendar_cells[i][1] = '\0';
        s_home_calendar_map[map_index++] = s_home_calendar_cells[i];
        if ((i % 7) == 6 && i != HOME_CALENDAR_CELL_COUNT - 1)
        {
            s_home_calendar_map[map_index++] = "\n";
        }
    }

    s_home_calendar_map[map_index++] = "";
}

static void home_calendar_update(const struct tm *timeinfo)
{
    static const char *months[] = {
        "JANUARY",
        "FEBRUARY",
        "MARCH",
        "APRIL",
        "MAY",
        "JUNE",
        "JULY",
        "AUGUST",
        "SEPTEMBER",
        "OCTOBER",
        "NOVEMBER",
        "DECEMBER",
    };

    if (timeinfo == NULL || s_home_ui.calendar_head == NULL || s_home_ui.calendar_matrix == NULL)
    {
        return;
    }

    int year = timeinfo->tm_year + 1900;
    int month = timeinfo->tm_mon;
    int day = timeinfo->tm_mday;
    if (year == s_home_calendar_year &&
        month == s_home_calendar_month &&
        day == s_home_calendar_day)
    {
        return;
    }

    struct tm first_day = {
        .tm_year = timeinfo->tm_year,
        .tm_mon = month,
        .tm_mday = 1,
        .tm_isdst = -1,
    };
    if (mktime(&first_day) == (time_t)-1)
    {
        return;
    }

    for (int i = 0; i < HOME_CALENDAR_CELL_COUNT; i++)
    {
        s_home_calendar_cells[i][0] = ' ';
        s_home_calendar_cells[i][1] = '\0';
    }

    int first_wday = first_day.tm_wday;
    int month_days = home_days_in_month(year, month);
    for (int d = 1; d <= month_days; d++)
    {
        int cell = first_wday + d - 1;
        if (cell >= 0 && cell < HOME_CALENDAR_CELL_COUNT)
        {
            if (d < 10)
            {
                s_home_calendar_cells[cell][0] = '0' + d;
                s_home_calendar_cells[cell][1] = '\0';
            }
            else
            {
                s_home_calendar_cells[cell][0] = '0' + (d / 10);
                s_home_calendar_cells[cell][1] = '0' + (d % 10);
                s_home_calendar_cells[cell][2] = '\0';
            }
        }
    }

    for (int i = 0; i < HOME_CALENDAR_BUTTON_COUNT; i++)
    {
        s_home_calendar_ctrl[i] = 0;
    }

    int today_cell = first_wday + day - 1;
    if (today_cell >= 0 && today_cell < HOME_CALENDAR_CELL_COUNT)
    {
        s_home_calendar_ctrl[7 + today_cell] = LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CHECKED;
    }

    lv_label_set_text_fmt(s_home_ui.calendar_head, "%s %d", months[month], year);
    lv_buttonmatrix_set_map(s_home_ui.calendar_matrix, s_home_calendar_map);
    lv_buttonmatrix_set_ctrl_map(s_home_ui.calendar_matrix, s_home_calendar_ctrl);

    s_home_calendar_year = year;
    s_home_calendar_month = month;
    s_home_calendar_day = day;
}

void home_page_set_mic_levels(int mic0_db, int mic1_db)
{
    ui_lock();
    home_mic_meter_update(0, mic0_db);
    home_mic_meter_update(1, mic1_db);
    ui_unlock();
}

static void home_first_body_cont_create(lv_obj_t *parent)
{
    // info panel
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 700, 160);
    lv_obj_set_style_bg_color(cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_opa(cont, 255, 0);
    lv_obj_set_style_pad_column(cont, 10, 0);
    ui_obj_set_flex(cont,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *text_cont = lv_obj_create(cont);
    lv_obj_set_style_bg_color(text_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(text_cont, 0, 0);
    lv_obj_set_size(text_cont, 270, 140);
    lv_obj_set_style_pad_all(text_cont, 0, 0);
    lv_obj_set_style_pad_row(text_cont, 6, 0);
    ui_obj_set_flex(text_cont,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START);

    lv_obj_t *monitor_header = ui_flex_container_create(text_cont,
                                                        LV_PCT(100),
                                                        20,
                                                        LV_FLEX_FLOW_ROW,
                                                        LV_FLEX_ALIGN_START,
                                                        LV_FLEX_ALIGN_CENTER,
                                                        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *monitor_title = ui_label_create(monitor_header, "SYS MON", &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_width(monitor_title, 100);
    ui_flex_spacer_create(monitor_header);

    home_create_metric_row(text_cont,
                           "SRAM",
                           "--",
                           0,
                           UI_PRIMARY,
                           &s_home_ui.monitor_metric_value[HOME_MONITOR_SRAM],
                           &s_home_ui.monitor_metric_bar[HOME_MONITOR_SRAM]);
    home_create_metric_row(text_cont,
                           "PSRAM",
                           "--",
                           0,
                           UI_SECONDARY,
                           &s_home_ui.monitor_metric_value[HOME_MONITOR_PSRAM],
                           &s_home_ui.monitor_metric_bar[HOME_MONITOR_PSRAM]);
    home_create_info_row(text_cont, "TEMP", "--", UI_WARN, &s_home_ui.temp_value_label);

    lv_obj_t *spacer_1 = lv_obj_create(cont);
    lv_obj_set_size(spacer_1, 0, 1);
    lv_obj_set_flex_grow(spacer_1, 1); // 占据剩余空间
    lv_obj_set_style_bg_opa(spacer_1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer_1, 0, 0);
    lv_obj_set_style_pad_all(spacer_1, 0, 0);

    lv_obj_t *info_cont = lv_obj_create(cont);
    lv_obj_set_size(info_cont, 340, 140);
    lv_obj_set_style_bg_color(info_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_radius(info_cont, 10, 0);
    lv_obj_set_style_border_width(info_cont, 0, 0);
    lv_obj_set_style_border_color(info_cont, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_pad_all(info_cont, 0, 0);
    lv_obj_set_scrollbar_mode(info_cont, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *calendar_tile = lv_tileview_add_tile(info_cont, 0, 0, LV_DIR_RIGHT);
    ui_obj_set_transparent(calendar_tile);
    lv_obj_set_style_pad_all(calendar_tile, 4, 0);
    lv_obj_set_style_pad_row(calendar_tile, 4, 0);
    ui_obj_set_flex(calendar_tile,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START);

    s_home_ui.calendar_head = lv_label_create(calendar_tile);
    lv_obj_set_width(s_home_ui.calendar_head, LV_PCT(100));
    lv_label_set_text(s_home_ui.calendar_head, "---- ----");
    lv_obj_set_style_text_color(s_home_ui.calendar_head, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_text_font(s_home_ui.calendar_head, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_home_ui.calendar_head, LV_TEXT_ALIGN_CENTER, 0);

    home_calendar_map_init();

    s_home_ui.calendar_matrix = lv_buttonmatrix_create(calendar_tile);
    lv_obj_set_size(s_home_ui.calendar_matrix, LV_PCT(100), 112);
    lv_buttonmatrix_set_map(s_home_ui.calendar_matrix, s_home_calendar_map);
    lv_buttonmatrix_set_ctrl_map(s_home_ui.calendar_matrix, s_home_calendar_ctrl);
    lv_obj_remove_flag(s_home_ui.calendar_matrix, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_home_ui.calendar_matrix, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_home_ui.calendar_matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_home_ui.calendar_matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_home_ui.calendar_matrix, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_home_ui.calendar_matrix, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_home_ui.calendar_matrix, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_home_ui.calendar_matrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_home_ui.calendar_matrix, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_home_ui.calendar_matrix, lv_color_hex(UI_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_home_ui.calendar_matrix, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_home_ui.calendar_matrix, lv_color_hex(UI_PRIMARY), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_home_ui.calendar_matrix, LV_OPA_90, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_home_ui.calendar_matrix, lv_color_hex(UI_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(s_home_ui.calendar_matrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_opa(s_home_ui.calendar_matrix, LV_OPA_TRANSP, LV_PART_ITEMS);

    lv_obj_set_style_shadow_width(s_home_ui.calendar_matrix, 0, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(s_home_ui.calendar_matrix, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_set_style_shadow_width(s_home_ui.calendar_matrix, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(s_home_ui.calendar_matrix, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
}

static int app_menu_wrap_index(int index)
{
    while (index < 0)
    {
        index += HOME_APP_MENU_COUNT;
    }

    while (index >= HOME_APP_MENU_COUNT)
    {
        index -= HOME_APP_MENU_COUNT;
    }

    return index;
}

static int32_t app_menu_wrap_position_fp(int32_t position_fp)
{
    const int32_t total = HOME_APP_MENU_COUNT * 256;

    while (position_fp < 0)
    {
        position_fp += total;
    }

    while (position_fp >= total)
    {
        position_fp -= total;
    }

    return position_fp;
}

static int32_t app_menu_relative_offset_fp(int item_index, int32_t position_fp)
{
    int32_t offset = item_index * 256 - position_fp;
    const int32_t half = (HOME_APP_MENU_COUNT * 256) / 2;
    const int32_t total = HOME_APP_MENU_COUNT * 256;

    if (offset > half)
    {
        offset -= total;
    }
    else if (offset < -half)
    {
        offset += total;
    }

    return offset;
}

static int app_menu_nearest_index(int32_t position_fp)
{
    if (position_fp >= 0)
    {
        return (position_fp + 128) / 256;
    }

    return -((-position_fp + 128) / 256);
}

static int32_t app_menu_progress_thumb_x(void)
{
    return APP_MENU_PROGRESS_TRACK_X - ((APP_MENU_PROGRESS_THUMB_W - APP_MENU_PROGRESS_TRACK_W) / 2);
}

static int32_t app_menu_progress_thumb_y(int index)
{
    const int32_t range = APP_MENU_PROGRESS_TRACK_H - APP_MENU_PROGRESS_THUMB_H;
    if (HOME_APP_MENU_COUNT <= 1)
    {
        return APP_MENU_PROGRESS_TRACK_Y;
    }

    return APP_MENU_PROGRESS_TRACK_Y + (app_menu_wrap_index(index) * range) / (HOME_APP_MENU_COUNT - 1);
}

static void app_menu_progress_y_cb(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

static void app_menu_set_progress_thumb(int index, bool anim)
{
    if (s_app_menu.progress_thumb == NULL)
    {
        return;
    }

    const int32_t target_y = app_menu_progress_thumb_y(index);
    lv_obj_set_x(s_app_menu.progress_thumb, app_menu_progress_thumb_x());
    lv_anim_delete(s_app_menu.progress_thumb, app_menu_progress_y_cb);

    if (!anim)
    {
        lv_obj_set_y(s_app_menu.progress_thumb, target_y);
        return;
    }

    const int32_t start_y = lv_obj_get_y(s_app_menu.progress_thumb);
    if (start_y == target_y)
    {
        return;
    }

    lv_anim_t thumb_anim;
    lv_anim_init(&thumb_anim);
    lv_anim_set_var(&thumb_anim, s_app_menu.progress_thumb);
    lv_anim_set_values(&thumb_anim, start_y, target_y);
    lv_anim_set_time(&thumb_anim, 180);
    lv_anim_set_path_cb(&thumb_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&thumb_anim, app_menu_progress_y_cb);
    lv_anim_start(&thumb_anim);
}

static void app_menu_anim_position_cb(void *obj, int32_t value)
{
    (void)obj;
    s_app_menu.position_fp = value;

    for (int i = 0; i < HOME_APP_MENU_COUNT; i++)
    {
        int32_t offset_fp = app_menu_relative_offset_fp(i, s_app_menu.position_fp);
        home_app_item_t *item = &s_app_menu.items[i];

        const int32_t abs_offset = LV_ABS(offset_fp);
        const int32_t sign = offset_fp < 0 ? -1 : 1;
        int32_t y = 0;
        int32_t x = 0;
        int32_t w = 0;
        int32_t h = 0;
        int32_t opa = 0;
        bool is_center = abs_offset <= 128;

        if (abs_offset <= 256)
        {
            const int32_t t = abs_offset;
            w = APP_MENU_CENTER_W - ((APP_MENU_CENTER_W - APP_MENU_SIDE_W) * t) / 256;
            h = APP_MENU_CENTER_H - ((APP_MENU_CENTER_H - APP_MENU_SIDE_H) * t) / 256;
            x = APP_MENU_CENTER_X + ((APP_MENU_SIDE_X - APP_MENU_CENTER_X) * t) / 256;
            if (offset_fp < 0)
            {
                y = APP_MENU_CENTER_Y + ((APP_MENU_TOP_Y - APP_MENU_CENTER_Y) * t) / 256;
            }
            else
            {
                y = APP_MENU_CENTER_Y + ((APP_MENU_BOTTOM_Y - APP_MENU_CENTER_Y) * t) / 256;
            }
            opa = APP_MENU_CENTER_OPA - ((APP_MENU_CENTER_OPA - APP_MENU_SIDE_OPA) * t) / 256;
        }
        else if (abs_offset <= 512)
        {
            const int32_t t = abs_offset - 256;
            w = APP_MENU_SIDE_W - ((APP_MENU_SIDE_W - APP_MENU_HIDE_W) * t) / 256;
            h = APP_MENU_SIDE_H - ((APP_MENU_SIDE_H - APP_MENU_HIDE_H) * t) / 256;
            x = APP_MENU_SIDE_X + ((APP_MENU_HIDE_X - APP_MENU_SIDE_X) * t) / 256;
            if (offset_fp < 0)
            {
                y = APP_MENU_TOP_Y + ((APP_MENU_HIDE_TOP_Y - APP_MENU_TOP_Y) * t) / 256;
            }
            else
            {
                y = APP_MENU_BOTTOM_Y + ((APP_MENU_HIDE_BOTTOM_Y - APP_MENU_BOTTOM_Y) * t) / 256;
            }
            opa = APP_MENU_SIDE_OPA - ((APP_MENU_SIDE_OPA - APP_MENU_HIDE_OPA) * t) / 256;
        }
        else
        {
            w = APP_MENU_HIDE_W;
            h = APP_MENU_HIDE_H;
            x = APP_MENU_HIDE_X;
            y = sign < 0 ? APP_MENU_HIDE_TOP_Y : APP_MENU_HIDE_BOTTOM_Y;
            opa = APP_MENU_HIDE_OPA;
        }

        lv_obj_set_pos(item->item, x, y);
        lv_obj_set_size(item->item, w, h);
        lv_obj_set_style_bg_opa(item->item, (lv_opa_t)opa, 0);
        lv_obj_set_style_border_opa(item->item, (lv_opa_t)opa, 0);
        lv_obj_set_style_text_opa(item->icon, (lv_opa_t)opa, 0);

        if (is_center)
        {
            lv_obj_add_style(item->item, &s_app_menu_selected_style, 0);
            // lv_obj_align(item->status_cont, LV_ALIGN_CENTER, APP_MENU_CENTER_W + 40, 20);
            lv_obj_set_pos(item->status_cont, x + APP_MENU_CENTER_W + 40, y);
            lv_obj_remove_flag(item->status_cont, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_style(item->item, &s_app_menu_selected_style, 0);
            lv_obj_set_style_bg_color(item->item, lv_color_hex(UI_PANEL_HL), 0);
            lv_obj_add_flag(item->status_cont, LV_OBJ_FLAG_HIDDEN);
        }

        if (opa == 0)
        {
            lv_obj_add_flag(item->item, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_flag(item->item, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_app_menu.selected = app_menu_wrap_index(app_menu_nearest_index(s_app_menu.position_fp));
    if (s_app_menu.selected != s_app_menu.displayed_selected)
    {
        if (s_app_menu.title)
        {
            lv_label_set_text(s_app_menu.title, s_app_menu_titles[s_app_menu.selected]);
        }
        if (s_app_menu.desc_label)
        {
            lv_label_set_text(s_app_menu.desc_label, s_app_menu_descs[s_app_menu.selected]);
        }

        app_menu_set_progress_thumb(s_app_menu.selected, s_app_menu.displayed_selected >= 0);
        s_app_menu.displayed_selected = s_app_menu.selected;
    }

    lv_obj_move_to_index(s_app_menu.items[s_app_menu.selected].item, -1);
}

static void app_menu_snap_ready_cb(lv_anim_t *anim)
{
    (void)anim;
    s_app_menu.position_fp = app_menu_wrap_position_fp(s_app_menu.position_fp);
    app_menu_anim_position_cb(s_app_menu.cont, s_app_menu.position_fp);
}

static void app_menu_start_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t start, int32_t end, uint32_t time)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, start, end);
    lv_anim_set_time(&anim, time);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_completed_cb(&anim, app_menu_snap_ready_cb);
    lv_anim_start(&anim);
}

static void app_menu_snap_to_nearest(int32_t throw_vect_y)
{
    int32_t target_index = app_menu_nearest_index(s_app_menu.position_fp);
    int32_t speed = LV_ABS(throw_vect_y);
    int32_t steps = 0;

    if (speed > APP_MENU_ROLL_SPEED)
    {
        steps = 1 + (speed - APP_MENU_ROLL_SPEED) / APP_MENU_ROLL_SPEED;
        if (steps > 3)
        {
            steps = 3;
        }
    }

    if (throw_vect_y > APP_MENU_ROLL_SPEED)
    {
        target_index -= steps;
    }
    else if (throw_vect_y < -APP_MENU_ROLL_SPEED)
    {
        target_index += steps;
    }

    int32_t target = target_index * 256;

    int32_t delta = target - s_app_menu.position_fp;
    const int32_t total = HOME_APP_MENU_COUNT * 256;
    const int32_t half = total / 2;

    if (delta > half)
    {
        target -= total;
    }
    else if (delta < -half)
    {
        target += total;
    }

    uint32_t time = 280;
    if (speed > APP_MENU_ROLL_SPEED)
    {
        time = 260 - LV_MIN(speed * 4, 140);
    }

    app_menu_start_anim(s_app_menu.cont, app_menu_anim_position_cb, s_app_menu.position_fp, target, time);
}

static void app_menu_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL)
    {
        return;
    }

    if (code == LV_EVENT_PRESSING)
    {
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        if (LV_ABS(vect.y) > LV_ABS(vect.x))
        {
            lv_anim_delete(s_app_menu.cont, app_menu_anim_position_cb);
            s_app_menu.position_fp = app_menu_wrap_position_fp(s_app_menu.position_fp - vect.y * 1.2);
            s_app_menu.last_vect_y = vect.y;
            app_menu_anim_position_cb(s_app_menu.cont, s_app_menu.position_fp);
        }
        lv_event_stop_bubbling(e);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        app_menu_snap_to_nearest(s_app_menu.last_vect_y);
        s_app_menu.last_vect_y = 0;
        lv_event_stop_bubbling(e);
    }
}

static void app_menu_bind_press(lv_obj_t *obj, bool bubble_to_parent)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (bubble_to_parent)
    {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    else
    {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    lv_obj_add_event_cb(obj, app_menu_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, app_menu_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, app_menu_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void app_menu_meta_create(lv_obj_t *menu_cont)
{
    s_app_menu.title = lv_label_create(menu_cont);
    lv_obj_set_width(s_app_menu.title, APP_MENU_DESC_W);
    lv_obj_align(s_app_menu.title, LV_ALIGN_TOP_LEFT, 20, 80);
    lv_label_set_text(s_app_menu.title, s_app_menu_titles[0]);
    lv_label_set_long_mode(s_app_menu.title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_app_menu.title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_app_menu.title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_app_menu.title, LV_TEXT_ALIGN_CENTER, 0);

    s_app_menu.desc_label = lv_label_create(menu_cont);
    lv_obj_set_width(s_app_menu.desc_label, 180);
    lv_obj_align(s_app_menu.desc_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(s_app_menu.desc_label, s_app_menu_descs[0]);
    lv_label_set_long_mode(s_app_menu.desc_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_app_menu.desc_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(s_app_menu.desc_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_app_menu.desc_label, LV_TEXT_ALIGN_LEFT, 0);

    s_app_menu.progress_track = lv_obj_create(menu_cont);
    lv_obj_set_size(s_app_menu.progress_track, APP_MENU_PROGRESS_TRACK_W, APP_MENU_PROGRESS_TRACK_H);
    lv_obj_set_pos(s_app_menu.progress_track, APP_MENU_PROGRESS_TRACK_X, APP_MENU_PROGRESS_TRACK_Y);
    lv_obj_set_style_bg_color(s_app_menu.progress_track, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_bg_opa(s_app_menu.progress_track, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_app_menu.progress_track, 0, 0);
    lv_obj_set_style_radius(s_app_menu.progress_track, 2, 0);
    lv_obj_set_style_pad_all(s_app_menu.progress_track, 0, 0);
    lv_obj_remove_flag(s_app_menu.progress_track, LV_OBJ_FLAG_SCROLLABLE);

    s_app_menu.progress_thumb = lv_obj_create(menu_cont);
    lv_obj_set_size(s_app_menu.progress_thumb, APP_MENU_PROGRESS_THUMB_W, APP_MENU_PROGRESS_THUMB_H);
    lv_obj_set_style_bg_color(s_app_menu.progress_thumb, lv_color_hex(UI_WARN), 0);
    lv_obj_set_style_bg_opa(s_app_menu.progress_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_app_menu.progress_thumb, 0, 0);
    lv_obj_set_style_border_color(s_app_menu.progress_thumb, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_radius(s_app_menu.progress_thumb, 0, 0);
    lv_obj_set_style_pad_all(s_app_menu.progress_thumb, 0, 0);
    lv_obj_remove_flag(s_app_menu.progress_thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_app_menu.progress_thumb, app_menu_progress_thumb_x(), app_menu_progress_thumb_y(0));
}

static void app_menu_selected_style_init(void)
{
    if (s_app_menu_selected_style_inited)
    {
        return;
    }

    lv_style_init(&s_app_menu_selected_style);
    lv_style_set_bg_color(&s_app_menu_selected_style, lv_color_hex(UI_PANEL_HL));
    lv_style_set_bg_opa(&s_app_menu_selected_style, LV_OPA_COVER);
    lv_style_set_border_width(&s_app_menu_selected_style, 2);
    lv_style_set_border_color(&s_app_menu_selected_style, lv_color_hex(UI_SECONDARY));
    lv_style_set_shadow_width(&s_app_menu_selected_style, 0);

    s_app_menu_selected_style_inited = true;
}

static void home_app_status_label_set(home_app_item_t *item, int line, const char *text, uint32_t color)
{
    (void)color;
    if (item == NULL || line < 0 || line >= 4 || item->status_label[line] == NULL)
    {
        return;
    }

    lv_label_set_text(item->status_label[line], (text && text[0]) ? text : "");
}

static void home_music_status_update_apply(void)
{
    home_app_item_t *item = &s_app_menu.items[0];
    if (item->status_cont == NULL)
    {
        return;
    }

    char line[64];
    snprintf(line, sizeof(line), "Tracks: %d", s_music_track_count);
    home_app_status_label_set(item, 0, line, UI_MUTED);

    const char *track_name = "--";
    if (s_music_current_index >= 0 && s_music_current_index < s_music_track_count)
    {
        track_name = s_music_track_names[s_music_current_index];
    }
    snprintf(line, sizeof(line), "Now: %.48s", track_name);
    home_app_status_label_set(item, 1, line, UI_MUTED);

    snprintf(line, sizeof(line), "Vol: %d%%", s_music_volume_percent);
    home_app_status_label_set(item, 2, line, UI_MUTED);
    home_app_status_label_set(item, 3, "", UI_MUTED);
}

static void home_camera_status_update_apply(void)
{
    home_app_item_t *item = &s_app_menu.items[1];
    if (item->status_cont == NULL)
    {
        return;
    }

    char line[40];
    snprintf(line, sizeof(line), "Mode: %s", s_camera_home_mode[0] ? s_camera_home_mode : "--");
    home_app_status_label_set(item, 0, line, UI_MUTED);

    if (s_camera_home_fps_x10 > 0)
    {
        snprintf(line, sizeof(line), "FPS: %d", (s_camera_home_fps_x10 + 5) / 10);
    }
    else
    {
        snprintf(line, sizeof(line), "FPS: --");
    }
    home_app_status_label_set(item, 1, line, UI_MUTED);
    home_app_status_label_set(item, 2, "", UI_MUTED);
    home_app_status_label_set(item, 3, "", UI_MUTED);
}

static void home_lora_status_update_apply(void)
{
    home_app_item_t *item = &s_app_menu.items[2];
    if (item->status_cont == NULL)
    {
        return;
    }

    lora_app_chip_t chip = lora_app_get_chip();
    const char *freq_line_1 = "Freq: --";
    const char *freq_line_2 = "";
    const char *power_line = "Power: --";
    switch (chip)
    {
    case LORA_APP_CHIP_SX1276:
        freq_line_1 = "Freq: 137~1020M";
        power_line = "Power: +20dBm";
        break;
    case LORA_APP_CHIP_SX1262:
        freq_line_1 = "Freq: 150~960M";
        power_line = "Power: +22dBm";
        break;
    case LORA_APP_CHIP_LR2021:
        freq_line_1 = "SubG: 150~960M";
        freq_line_2 = "High: 1.6~2.5G";
        power_line = "Pwr: +22/+12dBm";
        break;
    case LORA_APP_CHIP_LR1121:
        freq_line_1 = "SubG: 150~960M";
        freq_line_2 = "S:1.9~2.1 2G:2.4~2.5";
        power_line = "Pwr: +22/+13dBm";
        break;
    default:
        break;
    }

    char line[48];
    snprintf(line, sizeof(line), "Module: %s", lora_page_chip_name(chip));
    home_app_status_label_set(item, 0, line, UI_MUTED);

    home_app_status_label_set(item, 1, freq_line_1, UI_MUTED);
    home_app_status_label_set(item, 2, freq_line_2[0] ? freq_line_2 : power_line, UI_MUTED);
    home_app_status_label_set(item, 3, freq_line_2[0] ? power_line : "", UI_MUTED);
}

static void home_file_status_update_apply(const factory_storage_info_t *storage)
{
    home_app_item_t *item = &s_app_menu.items[3];
    if (item->status_cont == NULL)
    {
        return;
    }

    bool has_card = storage != NULL && strcmp(storage->type, "NO CARD") != 0;
    home_app_status_label_set(item, 0, has_card ? "SD: Mounted" : "SD: No Card", has_card ? UI_MUTED : UI_ERROR);

    char line[64];
    snprintf(line, sizeof(line), "Path: %.48s", s_file_current_path);
    home_app_status_label_set(item, 1, line, UI_MUTED);

    snprintf(line, sizeof(line), "Mode: %s", (storage && storage->usb_mode) ? "USB" : "APP");
    home_app_status_label_set(item, 2, line, UI_MUTED);
    home_app_status_label_set(item, 3, "", UI_MUTED);
}

static void home_bmu_status_update_apply(const bmu_info_t *bmu)
{
    home_app_item_t *item = &s_app_menu.items[4];
    if (item->status_cont == NULL)
    {
        return;
    }

    char line[48];
    if (bmu == NULL || !bmu->ready)
    {
        home_app_status_label_set(item, 0, "VBUS: --", UI_MUTED);
        home_app_status_label_set(item, 1, "Charge: --", UI_MUTED);
        home_app_status_label_set(item, 2, "VBAT: --", UI_MUTED);
        return;
    }

    if (bmu->vbus_mv >= 0)
    {
        snprintf(line, sizeof(line), "VBUS: %d.%02dV", bmu->vbus_mv / 1000, (bmu->vbus_mv % 1000) / 10);
    }
    else
    {
        snprintf(line, sizeof(line), "VBUS: --");
    }
    home_app_status_label_set(item, 0, line, UI_MUTED);

    snprintf(line, sizeof(line), "Charge: %dmA", s_bmu_charge_current_ma);
    home_app_status_label_set(item, 1, line, UI_MUTED);

    if (bmu->vbat_mv >= 0)
    {
        snprintf(line, sizeof(line), "VBAT: %dmV", bmu->vbat_mv);
    }
    else
    {
        snprintf(line, sizeof(line), "VBAT: --");
    }
    home_app_status_label_set(item, 2, line, UI_MUTED);
}

static void home_set_status_update_apply(const factory_storage_info_t *storage)
{
    home_app_item_t *item = &s_app_menu.items[5];
    if (item->status_cont == NULL)
    {
        return;
    }

    char line[48];

    snprintf(line, sizeof(line), "LCD: %d%%", s_set_brightness_percent);
    home_app_status_label_set(item, 0, line, UI_MUTED);

    snprintf(line, sizeof(line), "USB: %s", (storage && storage->usb_mode) ? "USB" : "APP");
    home_app_status_label_set(item, 1, line, UI_MUTED);

    snprintf(line, sizeof(line), "Low Power: %s", s_set_low_power_enabled ? "ON" : "OFF");
    home_app_status_label_set(item, 2, line, UI_MUTED);
}

static void home_app_status_update(void)
{
    factory_storage_info_t storage = {0};
    bool storage_ok = factory_storage_info_get(&storage);
    factory_storage_info_t *storage_ptr = storage_ok ? &storage : NULL;

    bmu_info_t bmu = {0};
    bmu_info_t *bmu_ptr = bmu_status_info_get(&bmu) ? &bmu : NULL;

    ui_lock();
    home_music_status_update_apply();
    home_camera_status_update_apply();
    home_lora_status_update_apply();
    home_file_status_update_apply(storage_ptr);
    home_bmu_status_update_apply(bmu_ptr);
    home_set_status_update_apply(storage_ptr);
    ui_unlock();
}

static void app_menu_click_event_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    ESP_LOGI(TAG, "app_menu_short_click:%d selected:%d", index, s_app_menu.selected);
    if (index != s_app_menu.selected)
    {
        return;
    }

    switch (index)
    {
    case 0:
        ESP_LOGI(TAG, "MUSIC menu clicked");
        ui_page_switch_async(PAGE_MUSIC);
        break;
    case 1:
        ESP_LOGI(TAG, "CAMERA menu clicked");
        ui_page_switch_async(PAGE_CAMERA);
        break;
    case 2:
        ESP_LOGI(TAG, "LORA menu clicked");
        ui_page_switch_async(PAGE_LORA);
        break;
    case 3:
        ESP_LOGI(TAG, "FILE menu clicked");
        ui_page_switch_async(PAGE_FILE);
        break;
    case 4:
        ESP_LOGI(TAG, "BMU menu clicked");
        ui_page_switch_async(PAGE_BMU);
        break;
    case 5:
        ESP_LOGI(TAG, "SET menu clicked");
        ui_page_switch_async(PAGE_SET);
        break;
    default:
        break;
    }
}

static void app_menu_create(lv_obj_t *menu_cont)
{
    app_menu_selected_style_init();

    s_app_menu.cont = menu_cont;
    s_app_menu.selected = 0;
    s_app_menu.displayed_selected = -1;
    s_app_menu.position_fp = 0;
    s_app_menu.last_vect_y = 0;

    for (int i = 0; i < HOME_APP_MENU_COUNT; i++)
    {
        home_app_item_t *item = &s_app_menu.items[i];

        item->item = lv_obj_create(menu_cont);
        lv_obj_set_size(item->item, 100, 100);
        lv_obj_set_style_bg_color(item->item, lv_color_hex(UI_PANEL_HL), 0);
        lv_obj_set_style_border_width(item->item, 1, 0);
        lv_obj_set_style_border_color(item->item, lv_color_hex(UI_LINE), 0);
        lv_obj_set_style_radius(item->item, 20, 0);
        lv_obj_set_style_pad_all(item->item, 0, 0);
        lv_obj_set_style_shadow_width(item->item, 0, 0);
        app_menu_bind_press(item->item, true);
        lv_obj_add_event_cb(item->item, app_menu_click_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        item->icon = lv_label_create(item->item);
        lv_label_set_text(item->icon, s_app_menu_icons[i]);
        lv_obj_set_style_text_color(item->icon, lv_color_hex(UI_PRIMARY), 0);
        lv_obj_set_style_text_font(item->icon, &lv_font_montserrat_28, 0);
        lv_obj_align(item->icon, LV_ALIGN_CENTER, 0, 0);
        app_menu_bind_press(item->icon, true);
        lv_obj_add_event_cb(item->icon, app_menu_click_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        item->status_cont = lv_obj_create(menu_cont);
        lv_obj_set_size(item->status_cont, 240, 100);
        lv_obj_set_style_bg_color(item->status_cont, lv_color_hex(UI_PANEL), 0);
        lv_obj_set_style_border_width(item->status_cont, 0, 0);
        lv_obj_set_style_border_color(item->status_cont, lv_color_hex(UI_LINE), 0);
        lv_obj_set_style_radius(item->status_cont, 20, 0);
        lv_obj_set_style_pad_all(item->status_cont, 5, 0);
        lv_obj_set_style_shadow_width(item->status_cont, 0, 0);
        lv_obj_remove_flag(item->status_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(item->status_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item->status_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(item->status_cont, 6, 0);

        for (int j = 0; j < 4; j++)
        {
            item->status_label[j] = lv_label_create(item->status_cont);
            lv_obj_set_width(item->status_label[j], LV_PCT(100));
            lv_obj_set_height(item->status_label[j], LV_PCT(20));
            lv_label_set_long_mode(item->status_label[j], LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_color(item->status_label[j], lv_color_hex(UI_MUTED), 0);
            lv_obj_set_style_text_font(item->status_label[j], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_align(item->status_label[j], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(item->status_label[j], "");
        }
    }

    home_music_status_update_apply();
    home_camera_status_update_apply();
    home_lora_status_update_apply();
    home_file_status_update_apply(NULL);
    home_bmu_status_update_apply(NULL);
    home_set_status_update_apply(NULL);
    app_menu_anim_position_cb(menu_cont, 0);
    app_menu_bind_press(menu_cont, false);
}

static void home_second_body_cont_create(lv_obj_t *parent)
{
    // menu panel
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, 700, 400);
    lv_obj_set_style_bg_color(menu_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_border_width(menu_cont, 0, 0);
    lv_obj_set_style_radius(menu_cont, 16, 0);
    lv_obj_set_style_pad_all(menu_cont, 10, 0);
    lv_obj_add_flag(menu_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(menu_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_opa(menu_cont, LV_OPA_100, 0);

    app_menu_meta_create(menu_cont);
    app_menu_create(menu_cont);
}

static void home_third_body_cont_create(lv_obj_t *parent)
{
    home_mic_meter_reset_cache();

    // bottom panel
    lv_obj_t *bottom_cont = lv_obj_create(parent);
    lv_obj_set_size(bottom_cont, 700, 80);
    lv_obj_set_style_bg_color(bottom_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_border_width(bottom_cont, 0, 0);
    lv_obj_set_style_radius(bottom_cont, 16, 0);
    lv_obj_set_style_pad_all(bottom_cont, 8, 0);
    lv_obj_set_style_pad_row(bottom_cont, 4, 0);
    lv_obj_set_style_opa(bottom_cont, 255, 0);
    ui_obj_set_flex(bottom_cont,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_START);

    for (int mic = 0; mic < MIC_METER_COUNT; mic++)
    {
        lv_obj_t *row = ui_flex_container_create(bottom_cont,
                                                 LV_PCT(70),
                                                 28,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);
        lv_obj_set_width(icon, 24);
        lv_obj_set_style_text_color(icon, lv_color_hex(mic == 0 ? UI_PRIMARY : UI_SECONDARY), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "MIC%d", mic + 1);
        lv_obj_set_width(name, 48);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

        lv_obj_t *meter = ui_flex_container_create(row,
                                                   MIC_METER_W,
                                                   22,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(meter, MIC_SEGMENT_GAP, 0);

        for (int i = 0; i < MIC_METER_SEGMENTS; i++)
        {
            lv_obj_t *seg = lv_obj_create(meter);
            lv_obj_set_size(seg, MIC_SEGMENT_W, MIC_SEGMENT_H);
            lv_obj_set_style_radius(seg, 2, 0);
            lv_obj_set_style_border_width(seg, 0, 0);
            lv_obj_set_style_pad_all(seg, 0, 0);
            lv_obj_set_style_bg_color(seg, lv_color_hex(UI_LINE), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_40, 0);
            lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
            s_home_ui.mic_segments[mic][i] = seg;
        }
    }

    home_mic_meter_update(0, s_home_mic_db[0]);
    home_mic_meter_update(1, s_home_mic_db[1]);
}

static int home_clamp_percent(int percent)
{
    if (percent < 0)
    {
        return -1;
    }
    if (percent > 100)
    {
        return 100;
    }
    return percent;
}

static const char *home_battery_symbol(int percent)
{
    if (percent < 0)
    {
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (percent >= 90)
    {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 65)
    {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 35)
    {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 10)
    {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void home_metric_set(home_monitor_metric_t metric, const char *value, int percent)
{
    percent = home_clamp_percent(percent);
    if (metric < 0 || metric >= HOME_MONITOR_METRIC_COUNT)
    {
        return;
    }

    if (s_home_ui.monitor_metric_value[metric])
    {
        lv_label_set_text(s_home_ui.monitor_metric_value[metric], (value && value[0]) ? value : "--");
    }

    if (s_home_ui.monitor_metric_bar[metric])
    {
        lv_bar_set_value(s_home_ui.monitor_metric_bar[metric], percent >= 0 ? percent : 0, LV_ANIM_OFF);
    }
}

void home_info_update(const home_info_t *info)
{
    const home_info_t empty_info = {
        .fps = -1,
        .sram_percent = -1,
        .psram_percent = -1,
        .sram_free_kb = 0,
        .psram_free_kb = 0,
        .temp_c_x10 = -1,
    };
    if (info == NULL)
    {
        info = &empty_info;
    }

    ui_lock();

    char value_text[24];
    snprintf(value_text, sizeof(value_text), "%" PRIu32 "KB", info->sram_free_kb);
    home_metric_set(HOME_MONITOR_SRAM, info->sram_free_kb > 0 ? value_text : "--", info->sram_percent);

    if (info->psram_free_kb >= 1024)
    {
        uint32_t psram_free_mb_x10 = (info->psram_free_kb * 10U + 512U) / 1024U;
        snprintf(value_text, sizeof(value_text), "%" PRIu32 ".%" PRIu32 "MB",
                 psram_free_mb_x10 / 10U,
                 psram_free_mb_x10 % 10U);
    }
    else
    {
        snprintf(value_text, sizeof(value_text), "%" PRIu32 "KB", info->psram_free_kb);
    }
    home_metric_set(HOME_MONITOR_PSRAM, info->psram_free_kb > 0 ? value_text : "--", info->psram_percent);

    home_calendar_update(&timeinfo);
    if (s_home_ui.temp_value_label)
    {
        if (info->temp_c_x10 >= 0)
        {
            lv_label_set_text_fmt(s_home_ui.temp_value_label, "%d.%dC", info->temp_c_x10 / 10, info->temp_c_x10 % 10);
        }
        else
        {
            lv_label_set_text(s_home_ui.temp_value_label, "-- C");
        }
    }
    ui_unlock();
}

static void home_body_cont_create(Page *page)
{
    lv_obj_t *main_cont = lv_obj_create(page->root);
    ui_main_cont_style_init(main_cont);

    // first_body
    home_first_body_cont_create(main_cont);

    // second_body
    home_second_body_cont_create(main_cont);

    // third_body
    home_third_body_cont_create(main_cont);
}

static void home_page_create(Page *page)
{
    s_home_ui.root = page->root;
    lv_obj_set_style_bg_color(s_home_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_home_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_home_ui.root, 0, 0);

    status_bar_create();

    home_body_cont_create(page);
}

static void home_page_enter(Page *page)
{
    (void)page;
}

static void home_page_timer(Page *page)
{
    (void)page;

    home_info_t home_info = {0};
    if (home_status_info_get(&home_info))
    {
        home_info_update(&home_info);
    }
    else
    {
        home_info_update(NULL);
    }
    home_app_status_update();
}

/*********music Page************/
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

static void music_spectrum_static_create(lv_obj_t *parent)
{
    static const uint32_t colors[3] = {
        UI_SECONDARY,
        0x9A36FF,
        UI_PRIMARY,
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
        for (int seg = 0; seg < MUSIC_SPECTRUM_SEGMENT_COUNT; seg++)
        {
            int y = spectrum_h - ((seg + 1) * seg_h) - (seg * seg_gap);
            int color_index = 0;
            if (seg >= 8)
            {
                color_index = 2;
            }
            else if (seg >= 4)
            {
                color_index = 1;
            }

            lv_obj_t *block = lv_obj_create(spectrum);
            lv_obj_set_size(block, bar_w, seg_h);
            lv_obj_set_pos(block, bar * (bar_w + bar_gap), y);
            lv_obj_set_style_bg_color(block, lv_color_hex(colors[color_index]), 0);
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
            }
        }
    }
    else
    {
        for (uint8_t seg = level; seg < old_level; seg++)
        {
            if (s_music_ui.spectrum_segment[bar][seg])
            {
                lv_obj_add_flag(s_music_ui.spectrum_segment[bar][seg], LV_OBJ_FLAG_HIDDEN);
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
    for (int i = 0; i < 3; i++)
    {
        if (s_music_ui.paly_lyric_text[i])
        {
            lv_label_set_text(s_music_ui.paly_lyric_text[i], lyrics[i]);
        }
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
        lv_label_set_text(s_music_ui.song_name_label, s_music_track_ptrs[s_music_current_index]);
    }

    ESP_LOGI(TAG, "Music track selected: %d %s", s_music_current_index, s_music_track_ptrs[s_music_current_index]);
    if (!music_page_on_control(MUSIC_CONTROL_TRACK_SELECT, s_music_current_index))
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
        return LV_SYMBOL_LIST;
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
        lv_label_set_text(s_music_ui.song_name_label, s_music_track_ptrs[s_music_current_index]);
    }
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

    DIR *dir = opendir(MUSIC_SCAN_DIR);
    if (dir == NULL)
    {
        ESP_LOGW(TAG, "Failed to open music dir: %s", MUSIC_SCAN_DIR);
        s_music_track_count = 0;
        s_music_current_index = -1;
        s_music_playlist_start = 0;
        music_page_set_playlist(NULL, 0, -1);
        if (s_music_ui.song_name_label)
        {
            lv_label_set_text(s_music_ui.song_name_label, "No music files");
        }
        home_music_status_update_apply();
        return -1;
    }

    s_music_track_count = 0;
    s_music_playlist_start = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_music_track_count < MUSIC_SCAN_MAX_TRACKS)
    {
        if (entry->d_name[0] == '.' || !music_filename_has_audio_ext(entry->d_name))
        {
            continue;
        }

        size_t name_len = strlen(entry->d_name);
        if (name_len >= MUSIC_TRACK_NAME_MAX_LEN)
        {
            name_len = MUSIC_TRACK_NAME_MAX_LEN - 1;
        }
        memcpy(s_music_track_names[s_music_track_count], entry->d_name, name_len);
        s_music_track_names[s_music_track_count][name_len] = '\0';
        s_music_track_ptrs[s_music_track_count] = s_music_track_names[s_music_track_count];
        s_music_track_count++;
    }
    closedir(dir);

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
    music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);

    if (s_music_current_index >= 0 && s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label, s_music_track_ptrs[s_music_current_index]);
    }
    else if (s_music_ui.song_name_label)
    {
        lv_label_set_text(s_music_ui.song_name_label, "No music files");
    }

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

    music_control_action_t action = (music_control_action_t)(intptr_t)lv_event_get_user_data(e);
    bool handled = false;

    switch (action)
    {
    case MUSIC_CONTROL_PLAY_PAUSE:
        handled = music_page_on_control(action, s_music_playing ? 0 : 1);
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
        music_page_on_control(action, s_music_play_mode);
        break;
    default:
        music_page_on_control(action, 0);
        break;
    }
}

static void music_volume_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    int value = lv_slider_get_value(slider);
    music_page_set_volume(value);
    music_page_on_control(MUSIC_CONTROL_VOLUME, value);
}

static void music_playlist_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED)
    {
        return;
    }

    int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (row < 0 || row >= MUSIC_PLAYLIST_VISIBLE_COUNT)
    {
        return;
    }

    music_page_select_track(s_music_playlist_row_track_index[row]);
}

static void music_playlist_scroll_by(int rows)
{
    if (rows == 0 || s_music_track_count <= MUSIC_PLAYLIST_VISIBLE_COUNT)
    {
        return;
    }

    int max_start = s_music_track_count - MUSIC_PLAYLIST_VISIBLE_COUNT;
    int next_start = s_music_playlist_start + rows;
    if (next_start < 0)
    {
        next_start = 0;
    }
    else if (next_start > max_start)
    {
        next_start = max_start;
    }

    if (next_start == s_music_playlist_start)
    {
        return;
    }

    s_music_playlist_start = next_start;
    music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);
}

static void music_playlist_scroll_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)
    {
        s_music_playlist_drag_accum = 0;
        return;
    }

    if (code != LV_EVENT_PRESSING)
    {
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL)
    {
        return;
    }

    lv_point_t vect = {0};
    lv_indev_get_vect(indev, &vect);
    if (vect.y == 0)
    {
        return;
    }

    s_music_playlist_drag_accum += vect.y;
    while (s_music_playlist_drag_accum <= -30)
    {
        music_playlist_scroll_by(1);
        s_music_playlist_drag_accum += 30;
    }
    while (s_music_playlist_drag_accum >= 30)
    {
        music_playlist_scroll_by(-1);
        s_music_playlist_drag_accum -= 30;
    }

    lv_event_stop_bubbling(e);
}

static lv_obj_t *music_control_button_create(lv_obj_t *parent, const char *symbol, bool primary, music_control_action_t action)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, primary ? 62 : 48, primary ? 62 : 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? UI_PRIMARY : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(primary ? UI_PRIMARY : UI_LINE), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, music_control_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(primary ? UI_BG : UI_TEXT), 0);
    lv_obj_set_style_text_font(label, primary ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
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

    if (s_music_ui.play_status_label)
    {
        lv_label_set_text(s_music_ui.play_status_label, playing ? "PLAYING" : "PAUSED");
        lv_obj_set_style_text_color(s_music_ui.play_status_label,
                                    lv_color_hex(playing ? UI_PRIMARY : UI_MUTED),
                                    0);
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
        music_playlist_row_set(row, idx < track_count ? idx : -1, name, idx == current_index);
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

static void music_page_timer(Page *page)
{
    (void)page;
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

    lv_obj_t *main_cont = lv_obj_create(page->root);
    ui_main_cont_style_init(main_cont);

    lv_obj_t *music_title_cont = ui_flex_container_create(main_cont, 680, 30, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(music_title_cont, 20, 0);
    lv_obj_set_style_bg_color(music_title_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_t *music_title_label = lv_label_create(music_title_cont);
    lv_label_set_text(music_title_label, "MUSIC");
    lv_obj_set_style_text_font(music_title_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(music_title_label, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_text_align(music_title_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *music_path_label = lv_label_create(music_title_cont);
    lv_label_set_text(music_path_label, "SD CARD / LOCAL PLAYER");
    lv_obj_set_style_text_font(music_path_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(music_path_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(music_path_label, LV_TEXT_ALIGN_LEFT, 0);

    ui_flex_spacer_create(music_title_cont);

    lv_obj_t *music_path_count = ui_flex_container_create(music_title_cont, 200, 20, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *boll = lv_obj_create(music_path_count);
    lv_obj_set_style_bg_color(boll, lv_color_hex(UI_OK), 0);
    lv_obj_set_size(boll, 10, 10);
    lv_obj_set_style_radius(boll, 5, 0);
    lv_obj_set_style_border_width(boll, 0, 0);

    lv_obj_t *music_path_count_label = lv_label_create(music_path_count);
    lv_label_set_text(music_path_count_label, "/sdcard/music");
    lv_obj_set_style_text_font(music_path_count_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(music_path_count_label, lv_color_hex(UI_MUTED), 0);

    lv_obj_t *music_info_cont = ui_flex_container_create(main_cont, 680, 280, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(music_info_cont, 40, 0);
    lv_obj_t *music_img_cont = lv_obj_create(music_info_cont);
    lv_obj_set_size(music_img_cont, 280, 280);
    lv_obj_set_style_radius(music_img_cont, 20, 0);
    lv_obj_set_style_border_width(music_img_cont, 1, 0);
    lv_obj_set_style_bg_color(music_img_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_border_color(music_img_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_scrollbar_mode(music_img_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(music_img_cont, LV_OBJ_FLAG_SCROLLABLE);

    circle_create(music_img_cont, 0, -50, 78, UI_PRIMARY, MUSIC_CIRCLE_FILL_DISC);
    circle_create(music_img_cont, 0, -50, 39, UI_PANEL, MUSIC_CIRCLE_FILL_PANEL);
    circle_create(music_img_cont, 0, -50, 24, UI_SECONDARY, MUSIC_CIRCLE_FILL_ACCENT);
    circle_create(music_img_cont, 0, -50, 5, UI_TEXT, MUSIC_CIRCLE_FILL_SOLID);
    music_spectrum_static_create(music_img_cont);
    music_spectrum_update_timer();

    lv_obj_t *music_info = ui_flex_container_create(music_info_cont, 360, 280, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *song_info_cont = ui_flex_container_create(music_info, 360, 120, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(song_info_cont, 10, 0);
    lv_obj_set_style_pad_row(song_info_cont, 4, 0);
    lv_obj_set_style_border_width(song_info_cont, 1, 0);
    lv_obj_set_style_border_color(song_info_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(song_info_cont, 12, 0);
    s_music_ui.play_status_label = lv_label_create(song_info_cont);
    lv_label_set_text(s_music_ui.play_status_label, "NOW PLAYING");
    lv_obj_set_style_text_font(s_music_ui.play_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_music_ui.play_status_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.play_status_label, LV_TEXT_ALIGN_LEFT, 0);
    s_music_ui.song_name_label = lv_label_create(song_info_cont);
    lv_label_set_text(s_music_ui.song_name_label, "Track_001.mp3");
    lv_obj_set_style_text_font(s_music_ui.song_name_label, MUSIC_NAME_FONT, 0);
    lv_obj_set_style_text_color(s_music_ui.song_name_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(s_music_ui.song_name_label, LV_TEXT_ALIGN_LEFT, 0);
    s_music_ui.artist_album_label = lv_label_create(song_info_cont);
    lv_label_set_text(s_music_ui.artist_album_label, "Unknown artist / Unknown album");
    lv_obj_set_style_text_font(s_music_ui.artist_album_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_music_ui.artist_album_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.artist_album_label, LV_TEXT_ALIGN_LEFT, 0);
    s_music_ui.paly_progress_bar = lv_bar_create(song_info_cont);
    lv_obj_set_size(s_music_ui.paly_progress_bar, 340, 8);
    lv_bar_set_range(s_music_ui.paly_progress_bar, 0, 100);
    lv_bar_set_value(s_music_ui.paly_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_music_ui.paly_progress_bar, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_color(s_music_ui.paly_progress_bar, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.paly_progress_bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_music_ui.paly_progress_bar, 0, 0);
    lv_obj_set_style_radius(s_music_ui.paly_progress_bar, 4, 0);
    lv_obj_t *music_time_cont = ui_flex_container_create(song_info_cont, 340, 20, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_music_ui.paly_progress_current_time_label = lv_label_create(music_time_cont);
    lv_label_set_text(s_music_ui.paly_progress_current_time_label, "00:00");
    lv_obj_set_style_text_font(s_music_ui.paly_progress_current_time_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_music_ui.paly_progress_current_time_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.paly_progress_current_time_label, LV_TEXT_ALIGN_LEFT, 0);

    ui_flex_spacer_create(music_time_cont);

    s_music_ui.paly_progress_total_time_label = lv_label_create(music_time_cont);
    lv_label_set_text(s_music_ui.paly_progress_total_time_label, "03:42");
    lv_obj_set_style_text_font(s_music_ui.paly_progress_total_time_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_music_ui.paly_progress_total_time_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.paly_progress_total_time_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *song_lyrics_cont = ui_flex_container_create(music_info, 360, 140, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(song_lyrics_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(song_lyrics_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(song_lyrics_cont, 1, 0);
    lv_obj_set_style_border_color(song_lyrics_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(song_lyrics_cont, 12, 0);
    lv_obj_set_style_pad_all(song_lyrics_cont, 10, 0);
    lv_obj_set_style_pad_row(song_lyrics_cont, 8, 0);

    s_music_ui.paly_lyric_label = lv_label_create(song_lyrics_cont);
    lv_obj_set_width(s_music_ui.paly_lyric_label, LV_PCT(100));
    lv_label_set_text(s_music_ui.paly_lyric_label, "LYRICS");
    lv_obj_set_style_text_font(s_music_ui.paly_lyric_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_music_ui.paly_lyric_label, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_text_align(s_music_ui.paly_lyric_label, LV_TEXT_ALIGN_LEFT, 0);

    static const char *lyric_defaults[] = {
        "SD card local music",
        "No lyrics for this track",
        "Add .lrc with the same file name",
    };
    static const lv_font_t *lyric_fonts[] = {
        MUSIC_LYRIC_FONT_SMALL,
        MUSIC_LYRIC_FONT_CURRENT,
        MUSIC_LYRIC_FONT_SMALL,
    };
    static const uint32_t lyric_colors[] = {
        UI_MUTED,
        UI_TEXT,
        UI_MUTED,
    };
    static const lv_opa_t lyric_opas[] = {
        LV_OPA_70,
        LV_OPA_COVER,
        LV_OPA_70,
    };

    for (int i = 0; i < 3; i++)
    {
        bool current = i == 1;
        s_music_ui.paly_lyric_text[i] = lv_label_create(song_lyrics_cont);
        lv_obj_set_width(s_music_ui.paly_lyric_text[i], LV_PCT(100));
        lv_label_set_text(s_music_ui.paly_lyric_text[i], lyric_defaults[i]);
        lv_label_set_long_mode(s_music_ui.paly_lyric_text[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(s_music_ui.paly_lyric_text[i], lyric_fonts[i], 0);
        lv_obj_set_style_text_color(s_music_ui.paly_lyric_text[i], lv_color_hex(lyric_colors[i]), 0);
        lv_obj_set_style_text_opa(s_music_ui.paly_lyric_text[i], lyric_opas[i], 0);
        lv_obj_set_style_text_align(s_music_ui.paly_lyric_text[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_hor(s_music_ui.paly_lyric_text[i], current ? 10 : 0, 0);
        lv_obj_set_style_pad_ver(s_music_ui.paly_lyric_text[i], current ? 7 : 0, 0);
        lv_obj_set_style_radius(s_music_ui.paly_lyric_text[i], 8, 0);
        lv_obj_set_style_bg_color(s_music_ui.paly_lyric_text[i], lv_color_hex(current ? UI_PANEL_HL : UI_BG), 0);
        lv_obj_set_style_bg_opa(s_music_ui.paly_lyric_text[i], current ? LV_OPA_80 : LV_OPA_TRANSP, 0);
    }

    lv_obj_t *music_crtl_cont = ui_flex_container_create(main_cont, 680, 100, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(music_crtl_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(music_crtl_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(music_crtl_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(music_crtl_cont, 1, 0);
    lv_obj_set_style_radius(music_crtl_cont, 12, 0);
    lv_obj_set_style_pad_all(music_crtl_cont, 14, 0);
    lv_obj_set_style_pad_column(music_crtl_cont, 28, 0);

    lv_obj_t *ctrl_btn_cont = ui_flex_container_create(music_crtl_cont, 280, 72, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_btn_cont, 12, 0);
    s_music_ui.play_mode_btn_label = music_control_button_create(ctrl_btn_cont, music_play_mode_symbol(s_music_play_mode), false, MUSIC_CONTROL_PLAY_MODE);
    music_control_button_create(ctrl_btn_cont, LV_SYMBOL_PREV, false, MUSIC_CONTROL_PREV);
    s_music_ui.play_btn_label = music_control_button_create(ctrl_btn_cont, LV_SYMBOL_PAUSE, true, MUSIC_CONTROL_PLAY_PAUSE);
    music_control_button_create(ctrl_btn_cont, LV_SYMBOL_NEXT, false, MUSIC_CONTROL_NEXT);

    lv_obj_t *volume_cont = ui_flex_container_create(music_crtl_cont, 280, 72, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(volume_cont, 10, 0);

    lv_obj_t *volume_icon = lv_label_create(volume_cont);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(volume_icon, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_text_font(volume_icon, &lv_font_montserrat_20, 0);

    s_music_ui.volume_slider = lv_slider_create(volume_cont);
    lv_obj_set_size(s_music_ui.volume_slider, 180, 10);
    lv_slider_set_range(s_music_ui.volume_slider, 0, 100);
    lv_slider_set_value(s_music_ui.volume_slider, s_music_volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music_ui.volume_slider, lv_color_hex(UI_WARN), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_music_ui.volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_music_ui.volume_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_music_ui.volume_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_music_ui.volume_slider, music_volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_music_ui.volume_value_label = lv_label_create(volume_cont);
    lv_obj_set_width(s_music_ui.volume_value_label, 46);
    lv_label_set_text_fmt(s_music_ui.volume_value_label, "%d%%", s_music_volume_percent);
    lv_obj_set_style_text_font(s_music_ui.volume_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_music_ui.volume_value_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(s_music_ui.volume_value_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *music_playlist_cont = ui_flex_container_create(main_cont, 680, 200, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(music_playlist_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(music_playlist_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(music_playlist_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(music_playlist_cont, 1, 0);
    lv_obj_set_style_radius(music_playlist_cont, 12, 0);
    lv_obj_set_style_pad_all(music_playlist_cont, 12, 0);
    lv_obj_set_style_pad_column(music_playlist_cont, 16, 0);

    lv_obj_t *playlist_list_cont = ui_flex_container_create(music_playlist_cont, 398, 176, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(playlist_list_cont, 8, 0);
    lv_obj_add_flag(playlist_list_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(playlist_list_cont, music_playlist_scroll_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(playlist_list_cont, music_playlist_scroll_event_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *playlist_head = ui_flex_container_create(playlist_list_cont, 398, 24, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *playlist_icon = lv_label_create(playlist_head);
    lv_label_set_text(playlist_icon, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(playlist_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(playlist_icon, lv_color_hex(UI_SECONDARY), 0);

    lv_obj_t *playlist_title = lv_label_create(playlist_head);
    lv_label_set_text(playlist_title, "PLAYLIST");
    lv_obj_set_style_text_font(playlist_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(playlist_title, lv_color_hex(UI_TEXT), 0);

    ui_flex_spacer_create(playlist_head);

    s_music_ui.playlist_count_label = lv_label_create(playlist_head);
    lv_label_set_text(s_music_ui.playlist_count_label, "4 TRACKS");
    lv_obj_set_width(s_music_ui.playlist_count_label, 84);
    lv_obj_set_style_text_font(s_music_ui.playlist_count_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_music_ui.playlist_count_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_music_ui.playlist_count_label, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < MUSIC_PLAYLIST_VISIBLE_COUNT; i++)
    {
        lv_obj_t *row = ui_flex_container_create(playlist_list_cont, 398, 30, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, music_playlist_scroll_event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, music_playlist_scroll_event_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(row, music_playlist_row_event_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        s_music_ui.playlist_row[i] = row;

        s_music_ui.playlist_index_label[i] = lv_label_create(row);
        lv_obj_set_width(s_music_ui.playlist_index_label[i], 30);
        lv_obj_set_style_text_font(s_music_ui.playlist_index_label[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(s_music_ui.playlist_index_label[i], LV_TEXT_ALIGN_LEFT, 0);

        s_music_ui.playlist_file_label[i] = lv_label_create(row);
        lv_obj_set_width(s_music_ui.playlist_file_label[i], 330);
        lv_label_set_long_mode(s_music_ui.playlist_file_label[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(s_music_ui.playlist_file_label[i], MUSIC_NAME_FONT, 0);
    }

    lv_obj_t *track_info_cont = ui_flex_container_create(music_playlist_cont, 238, 176, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(track_info_cont, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(track_info_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(track_info_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(track_info_cont, 1, 0);
    lv_obj_set_style_radius(track_info_cont, 10, 0);
    lv_obj_set_style_pad_all(track_info_cont, 12, 0);
    lv_obj_set_style_pad_row(track_info_cont, 8, 0);

    lv_obj_t *track_info_title = lv_label_create(track_info_cont);
    lv_obj_set_width(track_info_title, LV_PCT(100));
    lv_label_set_text(track_info_title, "TRACK INFO");
    lv_obj_set_style_text_font(track_info_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(track_info_title, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_text_align(track_info_title, LV_TEXT_ALIGN_LEFT, 0);

    static const char *param_names[MUSIC_TRACK_PARAM_COUNT] = {
        "FORMAT",
        "SAMPLE",
        "BITRATE",
        "CHANNEL",
    };

    for (int i = 0; i < MUSIC_TRACK_PARAM_COUNT; i++)
    {
        lv_obj_t *row = ui_flex_container_create(track_info_cont, 214, 26, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_width(name, 76);
        lv_label_set_text(name, param_names[i]);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_MUTED), 0);

        ui_flex_spacer_create(row);

        s_music_ui.track_param_value[i] = lv_label_create(row);
        lv_obj_set_width(s_music_ui.track_param_value[i], 104);
        lv_label_set_text(s_music_ui.track_param_value[i], "--");
        lv_label_set_long_mode(s_music_ui.track_param_value[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(s_music_ui.track_param_value[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_music_ui.track_param_value[i], lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_align(s_music_ui.track_param_value[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    static const char *default_tracks[] = {
        "Track_001.mp3",
        "Night_Drive.wav",
        "Local_Session.flac",
        "Demo_Record.mp3",
    };
    if (s_music_track_count > 0)
    {
        music_page_set_playlist(s_music_track_ptrs, s_music_track_count, s_music_current_index);
        if (s_music_current_index >= 0 && s_music_current_index < s_music_track_count)
        {
            lv_label_set_text(s_music_ui.song_name_label, s_music_track_ptrs[s_music_current_index]);
        }
    }
    else
    {
        music_page_set_playlist(default_tracks, (int)(sizeof(default_tracks) / sizeof(default_tracks[0])), 0);
        music_page_set_track_params("MP3", "44.1kHz", "320kbps", "Stereo");
    }
    music_page_set_play_state(s_music_playing);
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
    memset(&s_music_ui, 0, sizeof(s_music_ui));
    s_music_playlist_drag_accum = 0;
    for (int i = 0; i < MUSIC_PLAYLIST_VISIBLE_COUNT; i++)
    {
        s_music_playlist_row_track_index[i] = -1;
    }
}

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

static void camera_page_create(Page *page)
{
    s_camera_ui.root = page->root;
    lv_obj_set_style_bg_color(s_camera_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_camera_ui.root, 0, 0);
    lv_obj_add_flag(s_camera_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_camera_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.main_cont = lv_obj_create(s_camera_ui.root);
    ui_main_cont_style_init(s_camera_ui.main_cont);
    lv_obj_set_style_pad_row(s_camera_ui.main_cont, 16, 0);

    lv_obj_t *title_cont = ui_flex_container_create(s_camera_ui.main_cont,
                                                    680,
                                                    46,
                                                    LV_FLEX_FLOW_ROW,
                                                    LV_FLEX_ALIGN_START,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_cont, 16, 0);

    ui_label_create(title_cont, "CAMERA", &lv_font_montserrat_28, UI_PRIMARY);
    ui_label_create(title_cont, "MIPI CSI / LIVE PREVIEW", &lv_font_montserrat_16, UI_MUTED);

    ui_flex_spacer_create(title_cont);

    s_camera_ui.status_label = ui_label_create(title_cont, "READY", &lv_font_montserrat_16, UI_OK);
    lv_obj_set_size(s_camera_ui.status_label, 90, 28);
    lv_obj_set_style_text_align(s_camera_ui.status_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_camera_ui.preview_cont = lv_obj_create(s_camera_ui.main_cont);
    lv_obj_set_size(s_camera_ui.preview_cont, 680, 470);
    lv_obj_set_style_bg_color(s_camera_ui.preview_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.preview_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.preview_cont, 1, 0);
    lv_obj_set_style_border_color(s_camera_ui.preview_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_camera_ui.preview_cont, 8, 0);
    lv_obj_set_style_pad_all(s_camera_ui.preview_cont, 10, 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.preview_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_camera_ui.preview_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.preview_img = lv_obj_create(s_camera_ui.preview_cont);
    lv_obj_set_size(s_camera_ui.preview_img, CAMERA_UI_PREVIEW_WIDTH, CAMERA_UI_PREVIEW_HEIGHT);
    lv_obj_align(s_camera_ui.preview_img, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_color(s_camera_ui.preview_img, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.preview_img, 1, 0);
    lv_obj_set_style_border_color(s_camera_ui.preview_img, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_radius(s_camera_ui.preview_img, 6, 0);
    lv_obj_set_style_pad_all(s_camera_ui.preview_img, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_ui.preview_img, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_camera_ui.preview_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preview_icon = ui_label_create(s_camera_ui.preview_img, LV_SYMBOL_IMAGE, &lv_font_montserrat_28, UI_MUTED);
    lv_obj_center(preview_icon);

    lv_obj_t *preview_info = ui_flex_container_create(s_camera_ui.preview_cont,
                                                      CAMERA_UI_PREVIEW_WIDTH,
                                                      48,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    lv_obj_align(preview_info, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_pad_column(preview_info, 10, 0);
    lv_obj_set_style_bg_color(preview_info, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(preview_info, LV_OPA_80, 0);
    lv_obj_set_style_radius(preview_info, 8, 0);
    lv_obj_set_style_pad_left(preview_info, 14, 0);
    lv_obj_set_style_pad_right(preview_info, 14, 0);

    s_camera_ui.resolution_label = ui_label_create(preview_info, "--P", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(s_camera_ui.resolution_label, 92);
    lv_obj_t *format_label = ui_label_create(preview_info, "RGB", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(format_label, 34);
    s_camera_ui.fps_label = ui_label_create(preview_info, "--.- FPS", &lv_font_montserrat_14, UI_OK);
    lv_obj_set_width(s_camera_ui.fps_label, 82);
    ui_flex_spacer_create(preview_info);
    s_camera_ui.shot_count_label = ui_label_create(preview_info, "SHOT 000", &lv_font_montserrat_14, UI_SECONDARY);
    lv_obj_set_width(s_camera_ui.shot_count_label, 92);
    lv_obj_set_style_text_align(s_camera_ui.shot_count_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *control_cont = ui_flex_container_create(s_camera_ui.main_cont,
                                                      680,
                                                      130,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(control_cont, 28, 0);

    lv_obj_t *left_ctrl = ui_flex_container_create(control_cont,
                                                   236,
                                                   100,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_t *center_ctrl = ui_flex_container_create(control_cont,
                                                     88,
                                                     100,
                                                     LV_FLEX_FLOW_ROW,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER);
    lv_obj_t *right_ctrl = ui_flex_container_create(control_cont,
                                                    236,
                                                    100,
                                                    LV_FLEX_FLOW_ROW,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);

    s_camera_ui.last_photo_btn = lv_button_create(left_ctrl);
    lv_obj_set_size(s_camera_ui.last_photo_btn, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_set_style_radius(s_camera_ui.last_photo_btn, 8, 0);
    lv_obj_set_style_bg_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.last_photo_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_set_style_border_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_width(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_set_style_shadow_color(s_camera_ui.last_photo_btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(s_camera_ui.last_photo_btn, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(s_camera_ui.last_photo_btn, 0, 0);
    lv_obj_remove_flag(s_camera_ui.last_photo_btn, LV_OBJ_FLAG_SCROLLABLE);

    s_camera_ui.last_photo_img = lv_image_create(s_camera_ui.last_photo_btn);
    lv_obj_set_size(s_camera_ui.last_photo_img, CAMERA_UI_LAST_PHOTO_WIDTH, CAMERA_UI_LAST_PHOTO_HEIGHT);
    lv_obj_add_flag(s_camera_ui.last_photo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_camera_ui.last_photo_img);

    s_camera_ui.last_photo_icon = ui_label_create(s_camera_ui.last_photo_btn, LV_SYMBOL_IMAGE, &lv_font_montserrat_24, UI_SECONDARY);
    lv_obj_center(s_camera_ui.last_photo_icon);

    s_camera_ui.shutter_btn = lv_button_create(center_ctrl);
    lv_obj_set_size(s_camera_ui.shutter_btn, 88, 88);
    lv_obj_set_style_radius(s_camera_ui.shutter_btn, 44, 0);
    lv_obj_set_style_bg_color(s_camera_ui.shutter_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s_camera_ui.shutter_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_camera_ui.shutter_btn, 6, 0);
    lv_obj_set_style_border_color(s_camera_ui.shutter_btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_shadow_width(s_camera_ui.shutter_btn, 22, 0);
    lv_obj_set_style_shadow_color(s_camera_ui.shutter_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_camera_ui.shutter_btn, LV_OPA_40, 0);
    lv_obj_remove_flag(s_camera_ui.shutter_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_camera_ui.shutter_btn, camera_shutter_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *shutter_icon = ui_label_create(s_camera_ui.shutter_btn, LV_SYMBOL_IMAGE, &lv_font_montserrat_28, UI_BG);
    lv_obj_center(shutter_icon);

    s_camera_ui.storage_label = ui_label_create(right_ctrl, "SD --", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_set_size(s_camera_ui.storage_label, 120, 28);
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
    camera_preview_start(1920, 1080);
    if (s_camera_ui.resolution_label)
    {
        lv_label_set_text(s_camera_ui.resolution_label, "1080P");
    }
    if (s_camera_ui.fps_label)
    {
        lv_label_set_text(s_camera_ui.fps_label, "--.- FPS");
    }
}

static void camera_page_leave(Page *page)
{
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
        if (lv_obj_get_child_count(preview) > 0)
        {
            lv_obj_clean(preview);
        }
        lv_image_set_src(preview, &preview_dsc);
        lv_obj_set_size(preview, width, height);
        lv_obj_align(preview, LV_ALIGN_TOP_MID, 0, 18);
    }
    ui_unlock();
}

/*********lora Page************/
static void lora_param_event_cb(lv_event_t *e);
static void lora_freq_event_cb(lv_event_t *e);
static void lora_bw_event_cb(lv_event_t *e);
static void lora_button_event_cb(lv_event_t *e);
static void lora_tx_task(void *arg);
static void lora_rx_task(void *arg);

static void lora_page_update_param_labels(void)
{
    if (s_lora_ui.sf_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.sf_value_label, "SF%d", s_lora_sf);
    }
    if (s_lora_ui.cr_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.cr_value_label, "4/%d", s_lora_cr);
    }
    if (s_lora_ui.power_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.power_value_label, "%d dBm", s_lora_power_dbm);
    }
    if (s_lora_ui.tx_interval_value_label)
    {
        if (s_lora_tx_interval_ms < 1000)
        {
            lv_label_set_text_fmt(s_lora_ui.tx_interval_value_label, "%d ms", s_lora_tx_interval_ms);
        }
        else
        {
            lv_label_set_text_fmt(s_lora_ui.tx_interval_value_label, "%d.%ds",
                                  s_lora_tx_interval_ms / 1000,
                                  (s_lora_tx_interval_ms % 1000) / 100);
        }
    }
}

static uint32_t lora_choice_index_from_value(const float *choices, size_t count, float value, uint32_t fallback)
{
    if (choices == NULL || count == 0)
    {
        return fallback;
    }

    uint32_t best = fallback < count ? fallback : 0;
    float best_diff = fabsf(choices[best] - value);
    for (size_t i = 0; i < count; i++)
    {
        float diff = fabsf(choices[i] - value);
        if (diff < best_diff)
        {
            best = (uint32_t)i;
            best_diff = diff;
        }
    }
    return best;
}

static int lora_text_line_count(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return 0;
    }

    int lines = 1;
    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p == '\n' && p[1] != '\0')
        {
            lines++;
        }
    }
    return lines;
}

static void lora_page_timestamp(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year >= 120)
    {
        snprintf(buf, len, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        return;
    }

    int64_t uptime_ms = esp_timer_get_time() / 1000;
    snprintf(buf, len, "%" PRId64 "ms", uptime_ms);
}

static void lora_page_ui_update(const char *mode, uint32_t mode_color,
                                const char *status, uint32_t status_color,
                                const char *tx_text,
                                const char *rx_text,
                                const char *log_text)
{
    if (!s_lora_page_active)
    {
        return;
    }

    ui_lock();
    if (mode && s_lora_ui.mode_label)
    {
        lv_label_set_text(s_lora_ui.mode_label, mode);
        lv_obj_set_style_text_color(s_lora_ui.mode_label, lv_color_hex(mode_color), 0);
    }
    if (status && s_lora_ui.status_label)
    {
        lv_label_set_text(s_lora_ui.status_label, status);
        lv_obj_set_style_text_color(s_lora_ui.status_label, lv_color_hex(status_color), 0);
    }
    if (tx_text && s_lora_ui.tx_input)
    {
        lv_label_set_text(s_lora_ui.tx_input, tx_text);
    }
    if (rx_text && s_lora_ui.rx_data_textarea)
    {
        char timestamp[24] = {0};
        char rx_line[LORA_PACKET_MAX_LEN + 40] = {0};
        lora_page_timestamp(timestamp, sizeof(timestamp));
        snprintf(rx_line, sizeof(rx_line), "[%s] %s", timestamp, rx_text);

        const char *old_text = lv_textarea_get_text(s_lora_ui.rx_data_textarea);
        int old_lines = lora_text_line_count(old_text);
        int new_lines = lora_text_line_count(rx_line);
        if (old_lines + new_lines > LORA_RX_DATA_VISIBLE_LINES)
        {
            lv_textarea_set_text(s_lora_ui.rx_data_textarea, "");
        }
        lv_textarea_add_text(s_lora_ui.rx_data_textarea, rx_line);
        lv_textarea_add_text(s_lora_ui.rx_data_textarea, "\n");
        lv_obj_scroll_to_y(s_lora_ui.rx_data_textarea, LV_COORD_MAX, LV_ANIM_OFF);
    }
    if (log_text && s_lora_ui.lora_log)
    {
        lv_label_set_text(s_lora_ui.lora_log, log_text);
    }
    ui_unlock();
}

static lv_obj_t *lora_action_button_create(lv_obj_t *parent, const char *text, uint32_t color, lora_control_action_t action)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 108, 38);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(btn, (void *)(intptr_t)action);
    lv_obj_add_event_cb(btn, lora_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = ui_label_create(btn, text, &lv_font_montserrat_16, color == UI_PANEL_HL ? UI_TEXT : UI_BG);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *lora_param_slider_create(lv_obj_t *parent, const char *name, int min, int max, int value, lora_param_t param)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             48,
                                             LV_FLEX_FLOW_COLUMN,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, 4, 0);

    lv_obj_t *head = ui_flex_container_create(row,
                                              LV_PCT(100),
                                              18,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(head, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 110);

    ui_flex_spacer_create(head);

    lv_obj_t *value_label = ui_label_create(head, "", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(value_label, 95);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, LV_PCT(100), 12);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(param == LORA_PARAM_POWER ? UI_SECONDARY : UI_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_WARN), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, lora_param_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)param);

    switch (param)
    {
    case LORA_PARAM_SF:
        s_lora_ui.sf_slider = slider;
        s_lora_ui.sf_value_label = value_label;
        break;
    case LORA_PARAM_CR:
        s_lora_ui.cr_slider = slider;
        s_lora_ui.cr_value_label = value_label;
        break;
    case LORA_PARAM_POWER:
        s_lora_ui.power_slider = slider;
        s_lora_ui.power_value_label = value_label;
        break;
    case LORA_PARAM_TX_INTERVAL:
        s_lora_ui.tx_interval_slider = slider;
        s_lora_ui.tx_interval_value_label = value_label;
        break;
    }

    return slider;
}

static lv_obj_t *lora_textarea_create(lv_obj_t *parent, int32_t w, int32_t h, const char *text, bool one_line)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, h);
    lv_textarea_set_text(ta, text);
    lv_textarea_set_max_length(ta, one_line ? LORA_TEXT_MAX_LEN - 1 : 1800);
    lv_textarea_set_one_line(ta, one_line);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 8, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    return ta;
}

static lv_obj_t *lora_label_box_create(lv_obj_t *parent, int32_t w, int32_t h, const char *text, bool one_line)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, one_line ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_color(label, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(label, 1, 0);
    lv_obj_set_style_border_color(label, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(label, 8, 0);
    lv_obj_set_style_pad_all(label, 8, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static const char *lora_page_chip_name(lora_app_chip_t chip)
{
    switch (chip)
    {
    case LORA_APP_CHIP_SX1262:
        return "SX1262";
    case LORA_APP_CHIP_SX1276:
        return "SX1276";
    case LORA_APP_CHIP_LR1121:
        return "LR1121";
    case LORA_APP_CHIP_LR2021:
        return "LR2021";
    default:
        return "UNKNOWN";
    }
}

static void lora_param_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *slider = lv_event_get_target_obj(e);
    lora_param_t param = (lora_param_t)(intptr_t)lv_event_get_user_data(e);
    int value = (int)lv_slider_get_value(slider);

    switch (param)
    {
    case LORA_PARAM_SF:
        s_lora_sf = value;
        lora_app_set_spreading_factor(s_lora_sf);
        break;
    case LORA_PARAM_CR:
        s_lora_cr = value;
        lora_app_set_coding_rate(s_lora_cr);
        break;
    case LORA_PARAM_POWER:
        s_lora_power_dbm = value;
        lora_app_set_output_power(s_lora_power_dbm);
        break;
    case LORA_PARAM_TX_INTERVAL:
        s_lora_tx_interval_ms = value;
        break;
    }
    lora_page_update_param_labels();
    home_lora_status_update_apply();
    ESP_LOGI(TAG, "LORA param changed: SF:%d CR:%d POWER:%d,Time:%d", s_lora_sf, s_lora_cr, s_lora_power_dbm, s_lora_tx_interval_ms);
}

static void lora_freq_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    uint32_t selected = s_lora_ui.freq_dropdown ? lv_dropdown_get_selected(s_lora_ui.freq_dropdown) : LORA_FREQ_DEFAULT_INDEX;
    if (selected >= (uint32_t)(sizeof(s_lora_freq_choices) / sizeof(s_lora_freq_choices[0])))
    {
        selected = LORA_FREQ_DEFAULT_INDEX;
    }

    s_lora_freq = s_lora_freq_choices[selected];
    ESP_LOGI(TAG, "LORA frequency changed to %.2f MHz", s_lora_freq);
    lora_app_set_frequency(s_lora_freq);
    home_lora_status_update_apply();
}

static void lora_bw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    uint32_t selected = s_lora_ui.bw_dropdown ? lv_dropdown_get_selected(s_lora_ui.bw_dropdown) : LORA_BW_DEFAULT_INDEX;
    if (selected >= (uint32_t)(sizeof(s_lota_bw_choices) / sizeof(s_lota_bw_choices[0])))
    {
        selected = LORA_BW_DEFAULT_INDEX;
    }

    s_lora_bw = s_lota_bw_choices[selected];
    ESP_LOGI(TAG, "LORA bandwidth changed to %.1f kHz", (double)s_lora_bw);
    lora_app_set_bandwidth(s_lora_bw);
    home_lora_status_update_apply();
}

static bool lora_page_start_mode(lora_control_action_t action)
{
    if (action == LORA_CONTROL_TX || action == LORA_CONTROL_TX_ONCE)
    {
        s_lora_status = action;
        irq_flag = true;
        if (s_lora_tx_task_handle == NULL)
        {
            if (xTaskCreate(lora_tx_task, "lora_tx_task", LORA_TASK_STACK_SIZE, NULL, 3, &s_lora_tx_task_handle) != pdPASS)
            {
                s_lora_status = LORA_CONTROL_STOP;
                ESP_LOGE(TAG, "create lora_tx_task failed");
                return false;
            }
        }
        return true;
    }

    if (action == LORA_CONTROL_RX)
    {
        s_lora_status = LORA_CONTROL_RX;
        s_lora_rx_enabled = true;
        if (s_lora_rx_task_handle == NULL)
        {
            if (xTaskCreate(lora_rx_task, "lora_rx_task", LORA_TASK_STACK_SIZE, NULL, 3, &s_lora_rx_task_handle) != pdPASS)
            {
                s_lora_status = LORA_CONTROL_STOP;
                s_lora_rx_enabled = false;
                ESP_LOGE(TAG, "create lora_rx_task failed");
                return false;
            }
        }
        return true;
    }

    return true;
}

static void lora_page_request_mode(lora_control_action_t action)
{
    irq_flag = false;
    s_lora_status = LORA_CONTROL_STOP;
    s_lora_rx_enabled = false;
    s_lora_periodic_tx_enabled = false;
    s_lora_next_periodic_tx_us = 0;
    s_lora_pending_action = action;
    s_lora_mode_switch_pending = true;
    s_lora_mode_switch_start_us = esp_timer_get_time();

    lora_page_ui_update("STBY", UI_MUTED, NULL, UI_MUTED, NULL, NULL,
                        action == LORA_CONTROL_STOP ? "STOP requested" : "switching mode");
}

static void lora_page_process_mode_switch(void)
{
    if (!s_lora_mode_switch_pending)
    {
        return;
    }

    if (s_lora_tx_task_handle != NULL || s_lora_rx_task_handle != NULL)
    {
        int64_t elapsed_ms = (esp_timer_get_time() - s_lora_mode_switch_start_us) / 1000;
        if (elapsed_ms > LORA_MODE_SWITCH_WAIT_MS)
        {
            s_lora_mode_switch_pending = false;
            lora_page_ui_update(NULL, UI_MUTED, NULL, UI_MUTED, NULL, NULL, "mode switch timeout");
        }
        return;
    }

    lora_control_action_t action = s_lora_pending_action;
    s_lora_mode_switch_pending = false;

    if (lora_app_is_started())
    {
        int state = lora_app_standby();
        char log_text[48] = {0};
        snprintf(log_text, sizeof(log_text), "standby state:%d", state);
        lora_page_ui_update("STBY",
                            state == 0 ? UI_MUTED : UI_ERROR,
                            NULL,
                            state == 0 ? UI_MUTED : UI_ERROR,
                            NULL,
                            NULL,
                            log_text);
        if (state != 0)
        {
            return;
        }
    }
    else
    {
        lora_page_ui_update("STBY", UI_MUTED, NULL, UI_MUTED, NULL, NULL, "standby state:0");
    }

    if (action != LORA_CONTROL_STOP)
    {
        lora_page_start_mode(action);
    }
}

static void lora_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    lv_obj_t *btn = lv_event_get_target_obj(e);
    lora_control_action_t action = (lora_control_action_t)(intptr_t)lv_obj_get_user_data(btn);

    ESP_LOGI(TAG, "LORA button clicked: %d", action);
    if (action == LORA_CONTROL_CLEAR_LOG)
    {
        if (s_lora_ui.rx_data_textarea)
        {
            lv_textarea_set_text(s_lora_ui.rx_data_textarea, "");
        }
        return;
    }

    if (action == LORA_CONTROL_TX ||
        action == LORA_CONTROL_RX ||
        action == LORA_CONTROL_TX_ONCE ||
        action == LORA_CONTROL_STOP)
    {
        lora_page_request_mode(action);
    }
}

static void lora_tx_task(void *arg)
{
    (void)arg;
    static uint16_t count = 0;
    while (s_lora_status == LORA_CONTROL_TX || s_lora_status == LORA_CONTROL_TX_ONCE)
    {
        if (irq_flag)
        {
            irq_flag = false;
            count++;
            snprintf(s_lora_tx_text, sizeof(s_lora_tx_text), "T-Panel-P4 lora send #%d", count);
            s_lora_tx_busy = true;
            int state = lora_app_transmit_text(s_lora_tx_text);
            s_lora_tx_busy = false;
            char log_text[48] = {0};
            snprintf(log_text, sizeof(log_text), "transmit state:%d", state);
            lora_page_ui_update("TX", state == 0 ? UI_PRIMARY : UI_ERROR,
                                NULL, state == 0 ? UI_PRIMARY : UI_ERROR,
                                s_lora_tx_text,
                                NULL,
                                log_text);
            if (s_lora_status == LORA_CONTROL_TX_ONCE)
            {
                s_lora_status = LORA_CONTROL_STOP;
                break;
            }
        }
        int elapsed_ms = 0;
        while (s_lora_status == LORA_CONTROL_TX && elapsed_ms < s_lora_tx_interval_ms)
        {
            vTaskDelay(pdMS_TO_TICKS(LORA_TASK_STOP_POLL_MS));
            elapsed_ms += LORA_TASK_STOP_POLL_MS;
        }
    }
    int standby_state = lora_app_standby();
    char standby_log[48] = {0};
    snprintf(standby_log, sizeof(standby_log), "standby state:%d", standby_state);
    lora_page_ui_update("STBY",
                        standby_state == 0 ? UI_MUTED : UI_ERROR,
                        NULL,
                        standby_state == 0 ? UI_MUTED : UI_ERROR,
                        NULL,
                        NULL,
                        standby_log);
    s_lora_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

static void lora_page_read_rx_packet(void)
{
    size_t packet_len = lora_app_get_packet_length();
    if (packet_len == 0)
    {
        return;
    }

    uint8_t packet[LORA_PACKET_MAX_LEN] = {0};
    size_t read_len = packet_len;
    if (read_len >= sizeof(packet))
    {
        read_len = sizeof(packet) - 1;
    }

    int state = lora_app_read_data(packet, read_len);
    if (state == 0)
    {
        packet[read_len] = '\0';
        float rssi = lora_app_get_rssi();
        float snr = lora_app_get_snr();
        ESP_LOGI(TAG, "RX packet len:%d rssi:%.1f snr:%.1f", packet_len, rssi, snr);

        char log_text[64] = {0};
        snprintf(log_text, sizeof(log_text), "RX readData state:0");
        lora_page_ui_update("RX", UI_OK, NULL, UI_PRIMARY, NULL, (char *)packet, log_text);
        ui_lock();
        if (s_lora_ui.lora_rssi_snr_label)
        {
            lv_label_set_text_fmt(s_lora_ui.lora_rssi_snr_label, "RSSI %.1f / SNR %.1f", (double)rssi, (double)snr);
            lv_obj_set_style_text_color(s_lora_ui.lora_rssi_snr_label, lv_color_hex(UI_TEXT), 0);
        }
        ui_unlock();
        lora_app_start_receive();
    }
    else
    {
        char log_text[48] = {0};
        lora_app_start_receive();
        snprintf(log_text, sizeof(log_text), "readData state:%d", state);
        lora_page_ui_update("RX", UI_ERROR, NULL, UI_ERROR, NULL, NULL, log_text);
    }
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    if (!lora_app_is_started())
    {
        int start_state = lora_app_start();
        if (start_state != 0)
        {
            char start_log[48] = {0};
            snprintf(start_log, sizeof(start_log), "start state:%d", start_state);
            lora_page_ui_update("RX", UI_ERROR, NULL, UI_ERROR, NULL, NULL, start_log);
            s_lora_status = LORA_CONTROL_STOP;
            s_lora_rx_enabled = false;
            s_lora_rx_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    int state = lora_app_start_receive();
    char log_text[48] = {0};
    snprintf(log_text, sizeof(log_text), "startReceive state:%d", state);
    lora_page_ui_update("RX", state == 0 ? UI_OK : UI_ERROR,
                        NULL, state == 0 ? UI_PRIMARY : UI_ERROR,
                        NULL,
                        NULL,
                        log_text);
    if (state != 0)
    {
        s_lora_status = LORA_CONTROL_STOP;
        s_lora_rx_enabled = false;
    }

    while (s_lora_status == LORA_CONTROL_RX)
    {
        if (irq_flag)
        {
            irq_flag = false;
            lora_page_ui_update("RX", UI_OK, NULL, state == 0 ? UI_PRIMARY : UI_ERROR, NULL, NULL, "RX packet interrupt");
            lora_page_read_rx_packet();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int standby_state = lora_app_standby();
    char standby_log[48] = {0};
    snprintf(standby_log, sizeof(standby_log), "standby state:%d", standby_state);
    lora_page_ui_update("STBY",
                        standby_state == 0 ? UI_MUTED : UI_ERROR,
                        NULL,
                        standby_state == 0 ? UI_MUTED : UI_ERROR,
                        NULL,
                        NULL,
                        standby_log);
    s_lora_rx_task_handle = NULL;
    vTaskDelete(NULL);
}

static void lora_page_create(Page *page)
{
    s_lora_ui.root = page->root;
    s_lora_page_active = true;
    lv_obj_set_style_bg_color(s_lora_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_lora_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_lora_ui.root, 0, 0);
    lv_obj_add_flag(s_lora_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_lora_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_cont = lv_obj_create(s_lora_ui.root);
    ui_main_cont_style_init(main_cont);

    lv_obj_t *title_cont = ui_flex_container_create(main_cont,
                                                    680,
                                                    40,
                                                    LV_FLEX_FLOW_ROW,
                                                    LV_FLEX_ALIGN_START,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_cont, 16, 0);

    lv_obj_t *title = lv_label_create(title_cont);
    lv_label_set_text(title, "LORA");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_PRIMARY), 0);

    lv_obj_t *subtitle = lv_label_create(title_cont);
    lv_label_set_text(subtitle, "RADIO CONSOLE / PACKET TEST");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(UI_MUTED), 0);

    ui_flex_spacer_create(title_cont);

    s_lora_ui.status_label = lv_label_create(title_cont);
    lv_label_set_text(s_lora_ui.status_label, "STANDBY");
    lv_obj_set_width(s_lora_ui.status_label, 110);
    lv_obj_set_style_text_font(s_lora_ui.status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lora_ui.status_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_lora_ui.status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *body = ui_flex_container_create(main_cont, LV_PCT(100), 600, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, 20, 0);

    lv_obj_t *left = ui_flex_container_create(body,
                                              280,
                                              LV_PCT(100),
                                              LV_FLEX_FLOW_COLUMN,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(left, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_color(left, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(left, 12, 0);
    lv_obj_set_style_pad_all(left, 20, 0);
    lv_obj_set_style_pad_row(left, 8, 0);
    lv_obj_set_style_pad_column(left, 20, 0);

    lv_obj_t *right = ui_flex_container_create(body,
                                               380,
                                               LV_PCT(100),
                                               LV_FLEX_FLOW_COLUMN,
                                               LV_FLEX_ALIGN_START,
                                               LV_FLEX_ALIGN_CENTER,
                                               LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right, 20, 0);

    char chip_name[16];
    snprintf(chip_name, sizeof(chip_name), "RADIO %s", lora_page_chip_name(lora_app_get_chip()));
    lv_obj_t *radio_title = ui_label_create(left, chip_name, &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_width(radio_title, LV_PCT(100));

    s_lora_ui.freq_dropdown = lv_dropdown_create(left);
    lv_obj_set_size(s_lora_ui.freq_dropdown, LV_PCT(100), 42);
    lv_dropdown_set_options(s_lora_ui.freq_dropdown, "433 MHz\n868 MHz\n915 MHz\n923 MHz\n2400 MHz\n2450 MHz");
    lv_dropdown_set_selected(s_lora_ui.freq_dropdown,
                             lora_choice_index_from_value(s_lora_freq_choices,
                                                          sizeof(s_lora_freq_choices) / sizeof(s_lora_freq_choices[0]),
                                                          s_lora_freq,
                                                          LORA_FREQ_DEFAULT_INDEX));
    lv_obj_set_style_bg_color(s_lora_ui.freq_dropdown, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_lora_ui.freq_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lora_ui.freq_dropdown, 1, 0);
    lv_obj_set_style_border_color(s_lora_ui.freq_dropdown, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_lora_ui.freq_dropdown, 8, 0);
    lv_obj_set_style_text_font(s_lora_ui.freq_dropdown, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lora_ui.freq_dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_add_event_cb(s_lora_ui.freq_dropdown, lora_freq_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lora_ui.bw_dropdown = lv_dropdown_create(left);
    lv_obj_set_size(s_lora_ui.bw_dropdown, LV_PCT(100), 42);
    lv_dropdown_set_options(s_lora_ui.bw_dropdown, "62.5 kHz\n125 kHz\n250 kHz\n406 kHz\n500 kHz\n812 kHz\n1000 kHz");
    lv_dropdown_set_selected(s_lora_ui.bw_dropdown,
                             lora_choice_index_from_value(s_lota_bw_choices,
                                                          sizeof(s_lota_bw_choices) / sizeof(s_lota_bw_choices[0]),
                                                          s_lora_bw,
                                                          LORA_BW_DEFAULT_INDEX));
    lv_obj_set_style_bg_color(s_lora_ui.bw_dropdown, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(s_lora_ui.bw_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lora_ui.bw_dropdown, 1, 0);
    lv_obj_set_style_border_color(s_lora_ui.bw_dropdown, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_lora_ui.bw_dropdown, 8, 0);
    lv_obj_set_style_text_font(s_lora_ui.bw_dropdown, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lora_ui.bw_dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_add_event_cb(s_lora_ui.bw_dropdown, lora_bw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *mode_row = ui_flex_container_create(left,
                                                  LV_PCT(100),
                                                  42,
                                                  LV_FLEX_FLOW_ROW,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(mode_row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mode_row, 1, 0);
    lv_obj_set_style_border_color(mode_row, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(mode_row, 8, 0);
    lv_obj_set_style_pad_left(mode_row, 12, 0);
    lv_obj_set_style_pad_right(mode_row, 12, 0);

    lv_obj_t *mode_name = lv_label_create(mode_row);
    lv_label_set_text(mode_name, "MODE");
    lv_obj_set_style_text_font(mode_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mode_name, lv_color_hex(UI_MUTED), 0);
    ui_flex_spacer_create(mode_row);

    s_lora_ui.mode_label = lv_label_create(mode_row);
    lv_label_set_text(s_lora_ui.mode_label, "STBY");
    lv_obj_set_style_text_font(s_lora_ui.mode_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lora_ui.mode_label, lv_color_hex(UI_MUTED), 0);

    ui_flex_spacer_create(left);

    lv_obj_t *param_title = ui_label_create(left, "PARAMETERS", &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_width(param_title, LV_PCT(100));
    lora_param_slider_create(left, "SF", LORA_SF_MIN, LORA_SF_MAX, s_lora_sf, LORA_PARAM_SF);
    lora_param_slider_create(left, "CR", LORA_CR_MIN, LORA_CR_MAX, s_lora_cr, LORA_PARAM_CR);
    lora_param_slider_create(left, "POWER", LORA_POWER_MIN_DBM, LORA_POWER_MAX_DBM, s_lora_power_dbm, LORA_PARAM_POWER);
    lora_param_slider_create(left, "TX INTERVAL", LORA_TX_INTERVAL_MIN_MS, LORA_TX_INTERVAL_MAX_MS, s_lora_tx_interval_ms, LORA_PARAM_TX_INTERVAL);
    lora_page_update_param_labels();

    ui_flex_spacer_create(left);

    lv_obj_t *action_grid = ui_flex_container_create(left, LV_PCT(100), 84, LV_FLEX_FLOW_ROW_WRAP, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(action_grid, 8, 0);
    lv_obj_set_style_pad_column(action_grid, 12, 0);
    s_lora_ui.tx_btn = lora_action_button_create(action_grid, "TX", UI_PRIMARY, LORA_CONTROL_TX);
    s_lora_ui.rx_btn = lora_action_button_create(action_grid, "RX", UI_OK, LORA_CONTROL_RX);
    lora_action_button_create(action_grid, "ONCE TX", UI_WARN, LORA_CONTROL_TX_ONCE);
    lora_action_button_create(action_grid, "STOP", UI_MUTED, LORA_CONTROL_STOP);

    lv_obj_t *tx_panel = ui_flex_container_create(right, LV_PCT(100), 180, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(tx_panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(tx_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tx_panel, 1, 0);
    lv_obj_set_style_border_color(tx_panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(tx_panel, 12, 0);
    lv_obj_set_style_pad_all(tx_panel, 20, 0);
    lv_obj_set_style_pad_row(tx_panel, 8, 0);
    lv_obj_set_style_pad_column(tx_panel, 12, 0);

    lv_obj_t *tx_head = ui_flex_container_create(tx_panel,
                                                 LV_PCT(100),
                                                 24,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    lv_obj_t *tx_title = ui_label_create(tx_head, "TRANSMIT", &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_width(tx_title, LV_PCT(100));
    s_lora_ui.tx_input = lora_label_box_create(tx_panel, LV_PCT(100), 40, "Hello LoRa", true);
    s_lora_ui.lora_log = lora_label_box_create(tx_panel, LV_PCT(100), 40, "READY", true);

    lv_obj_t *rx_panel = ui_flex_container_create(right,
                                                  LV_PCT(100),
                                                  400,
                                                  LV_FLEX_FLOW_COLUMN,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(rx_panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(rx_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rx_panel, 1, 0);
    lv_obj_set_style_border_color(rx_panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(rx_panel, 12, 0);
    lv_obj_set_style_pad_all(rx_panel, 12, 0);
    lv_obj_set_style_pad_row(rx_panel, 8, 0);
    lv_obj_set_style_pad_column(rx_panel, 12, 0);

    lv_obj_t *rx_title = ui_label_create(rx_panel, "RECEIVE", &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_width(rx_title, LV_PCT(100));

    s_lora_ui.rx_data_textarea = lora_textarea_create(rx_panel, LV_PCT(100), 320, "", false);
    lv_textarea_set_placeholder_text(s_lora_ui.rx_data_textarea, "RX packets will be shown here");
    lv_obj_remove_flag(s_lora_ui.rx_data_textarea, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rx_head = ui_flex_container_create(rx_panel,
                                                 LV_PCT(100),
                                                 24,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    s_lora_ui.lora_rssi_snr_label = lv_label_create(rx_head);
    lv_label_set_text(s_lora_ui.lora_rssi_snr_label, "RSSI / SNR");
    lv_obj_set_style_text_font(s_lora_ui.lora_rssi_snr_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lora_ui.lora_rssi_snr_label, lv_color_hex(UI_MUTED), 0);
    ui_flex_spacer_create(rx_head);
    lora_action_button_create(rx_head, "CLEAR", UI_MUTED, LORA_CONTROL_CLEAR_LOG);
}

static void lora_page_enter(Page *page)
{
    (void)page;
    char buf[64];
    bool lora_init_failed = (s_init_error & INIT_LORA_ERROR) != 0;
    snprintf(buf, sizeof(buf), "%s", lora_init_failed ? "INIT ERROR" : "INIT OK");
    lora_page_ui_update("STBY", UI_MUTED, buf, lora_init_failed ? UI_ERROR : UI_OK, NULL, NULL, NULL);
}

static void lora_page_timer(Page *page)
{
    (void)page;
    lora_page_process_mode_switch();
}

static void lora_page_leave(Page *page)
{
    irq_flag = false;
    s_lora_status = LORA_CONTROL_STOP;
    s_lora_rx_enabled = false;
    s_lora_periodic_tx_enabled = false;
    s_lora_mode_switch_pending = false;
    s_lora_next_periodic_tx_us = 0;
    s_lora_page_active = false;
    if (!s_lora_tx_busy)
    {
        lora_app_stop();
    }
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void lora_page_destroy(Page *page)
{
    (void)page;
    s_lora_page_active = false;
    memset(&s_lora_ui, 0, sizeof(s_lora_ui));
}

/*********file Page************/
static int file_page_scan_dir(const char *dir_path);

static const char *file_page_ext_name(const char *name, bool is_dir)
{
    if (is_dir)
    {
        return strcmp(name, "..") == 0 ? "UP" : "FOLDER";
    }

    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0')
    {
        return "FILE";
    }
    return dot + 1;
}

static void file_page_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
    {
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_size)
    {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool file_page_build_path(char *dst, size_t dst_size, const char *dir_path, const char *name)
{
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(name);
    if (dir_len + 1 + name_len >= dst_size)
    {
        return false;
    }

    memcpy(dst, dir_path, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, name, name_len + 1);
    return true;
}

static bool file_page_parent_path(char *path)
{
    if (strcmp(path, FILE_SCAN_DIR) == 0)
    {
        return false;
    }

    char *slash = strrchr(path, '/');
    if (slash == NULL || slash <= path + strlen(FILE_SCAN_DIR))
    {
        file_page_copy_text(path, FILE_PATH_MAX_LEN, FILE_SCAN_DIR);
        return true;
    }

    *slash = '\0';
    return true;
}

static bool file_page_is_dir(const char *dir_path, const char *name)
{
    char path[FILE_PATH_MAX_LEN] = {0};
    if (!file_page_build_path(path, sizeof(path), dir_path, name))
    {
        return false;
    }

    struct stat st = {0};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_page_has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL && strcasecmp(dot + 1, ext) == 0;
}

static uint16_t file_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t file_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t file_read_le32s(const uint8_t *p)
{
    return (int32_t)file_read_le32(p);
}

static void file_page_image_buf_free(void)
{
    if (s_file_image_buf)
    {
        if (s_file_ui.viewer_img)
        {
            lv_image_set_src(s_file_ui.viewer_img, NULL);
        }
        lv_image_cache_drop(&s_file_image_dsc);
        heap_caps_free(s_file_image_buf);
        s_file_image_buf = NULL;
        memset(&s_file_image_dsc, 0, sizeof(s_file_image_dsc));
    }
}

static void file_page_image_show_scaled(uint32_t width, uint32_t height)
{
    if (s_file_ui.viewer_img == NULL)
    {
        return;
    }

    lv_obj_clear_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_cache_drop(&s_file_image_dsc);
    lv_image_set_src(s_file_ui.viewer_img, &s_file_image_dsc);

    const uint32_t stage_w = 620;
    const uint32_t stage_h = 500;
    uint32_t zoom_w = width > 0 ? (stage_w * 256U) / width : 256U;
    uint32_t zoom_h = height > 0 ? (stage_h * 256U) / height : 256U;
    uint32_t zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom > 256U)
    {
        zoom = 256U;
    }
    if (zoom == 0)
    {
        zoom = 1;
    }
    lv_image_set_scale(s_file_ui.viewer_img, zoom);
    lv_obj_center(s_file_ui.viewer_img);
}

static void file_page_viewer_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_file_ui.viewer_cont)
    {
        lv_obj_add_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);
    }
    file_page_image_buf_free();
}

static void file_page_viewer_show(const char *title, const char *status)
{
    if (s_file_ui.viewer_cont == NULL)
    {
        return;
    }

    lv_obj_clear_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_file_ui.viewer_cont);
    if (s_file_ui.viewer_title_label)
    {
        lv_label_set_text(s_file_ui.viewer_title_label, title ? title : "VIEWER");
    }
    if (s_file_ui.viewer_status_label)
    {
        lv_label_set_text(s_file_ui.viewer_status_label, status ? status : "");
        lv_obj_set_style_text_color(s_file_ui.viewer_status_label, lv_color_hex(UI_MUTED), 0);
    }
    if (s_file_ui.viewer_textarea)
    {
        lv_obj_add_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_file_ui.viewer_img)
    {
        lv_obj_add_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void file_page_viewer_status_set(const char *status, uint32_t color)
{
    if (s_file_ui.viewer_status_label)
    {
        lv_label_set_text(s_file_ui.viewer_status_label, status ? status : "");
        lv_obj_set_style_text_color(s_file_ui.viewer_status_label, lv_color_hex(color), 0);
    }
}

static void file_page_open_txt(const char *path, const char *name)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        file_page_viewer_show(name, "TXT open failed");
        file_page_viewer_status_set("TXT open failed", UI_ERROR);
        ESP_LOGW(TAG, "Open TXT failed: %s errno=%d", path, errno);
        return;
    }

    char *buf = heap_caps_malloc(FILE_TXT_MAX_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        buf = heap_caps_malloc(FILE_TXT_MAX_BYTES + 1, MALLOC_CAP_DEFAULT);
    }
    if (buf == NULL)
    {
        fclose(file);
        file_page_viewer_show(name, "TXT no memory");
        file_page_viewer_status_set("TXT no memory", UI_ERROR);
        return;
    }

    size_t got = fread(buf, 1, FILE_TXT_MAX_BYTES, file);
    bool truncated = !feof(file);
    fclose(file);
    buf[got] = '\0';

    file_page_image_buf_free();
    file_page_viewer_show(name, truncated ? "TXT preview truncated" : "TXT");
    if (s_file_ui.viewer_textarea)
    {
        lv_obj_clear_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_file_ui.viewer_textarea, buf);
        lv_obj_scroll_to_y(s_file_ui.viewer_textarea, 0, LV_ANIM_OFF);
    }
    file_page_viewer_status_set(truncated ? "TXT preview truncated" : "TXT", truncated ? UI_WARN : UI_OK);
    heap_caps_free(buf);
}

static bool file_page_decode_bmp_to_rgb888(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ESP_LOGW(TAG, "Open BMP failed: %s errno=%d", path, errno);
        return false;
    }

    uint8_t header[54] = {0};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'B' ||
        header[1] != 'M')
    {
        fclose(file);
        return false;
    }

    uint32_t pixel_offset = file_read_le32(&header[10]);
    uint32_t dib_size = file_read_le32(&header[14]);
    int32_t width = file_read_le32s(&header[18]);
    int32_t height_signed = file_read_le32s(&header[22]);
    uint16_t planes = file_read_le16(&header[26]);
    uint16_t bpp = file_read_le16(&header[28]);
    uint32_t compression = file_read_le32(&header[30]);
    if (dib_size < 40 || width <= 0 || height_signed == 0 || planes != 1 ||
        compression != 0 || (bpp != 24 && bpp != 32))
    {
        fclose(file);
        ESP_LOGW(TAG, "Unsupported BMP: %s %ldx%ld bpp=%u comp=%" PRIu32,
                 path,
                 (long)width,
                 (long)height_signed,
                 bpp,
                 compression);
        return false;
    }

    bool top_down = height_signed < 0;
    uint32_t height = top_down ? (uint32_t)(-height_signed) : (uint32_t)height_signed;
    uint32_t row_bytes = ((uint32_t)width * bpp + 31U) / 32U * 4U;
    uint32_t rgb_bytes = (uint32_t)width * height * 3U;
    if (rgb_bytes == 0 || rgb_bytes > FILE_BMP_MAX_DECODE_BYTES)
    {
        fclose(file);
        ESP_LOGW(TAG, "BMP decode buffer too large: %s %ldx%" PRIu32 " %" PRIu32 " bytes",
                 path,
                 (long)width,
                 height,
                 rgb_bytes);
        return false;
    }

    file_page_image_buf_free();

    uint8_t *rgb = heap_caps_malloc(rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rgb == NULL)
    {
        rgb = heap_caps_malloc(rgb_bytes, MALLOC_CAP_DEFAULT);
    }
    uint8_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (row == NULL)
    {
        row = heap_caps_malloc(row_bytes, MALLOC_CAP_DEFAULT);
    }
    if (rgb == NULL || row == NULL)
    {
        heap_caps_free(rgb);
        heap_caps_free(row);
        fclose(file);
        return false;
    }

    bool ok = true;
    for (uint32_t y = 0; y < height; y++)
    {
        uint32_t file_y = top_down ? y : height - 1U - y;
        if (fseek(file, (long)pixel_offset + (long)file_y * (long)row_bytes, SEEK_SET) != 0 ||
            fread(row, 1, row_bytes, file) != row_bytes)
        {
            ok = false;
            break;
        }

        uint8_t *dst = rgb + ((size_t)y * (uint32_t)width * 3U);
        for (uint32_t x = 0; x < (uint32_t)width; x++)
        {
            const uint8_t *src = row + x * (bpp / 8U);
            dst[x * 3U + 0U] = src[2];
            dst[x * 3U + 1U] = src[1];
            dst[x * 3U + 2U] = src[0];
        }
    }

    heap_caps_free(row);
    fclose(file);
    if (!ok)
    {
        heap_caps_free(rgb);
        return false;
    }

    s_file_image_buf = rgb;
    s_file_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_file_image_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_file_image_dsc.header.flags = 0;
    s_file_image_dsc.header.w = (uint32_t)width;
    s_file_image_dsc.header.h = height;
    s_file_image_dsc.header.stride = (uint32_t)width * 3U;
    s_file_image_dsc.data_size = rgb_bytes;
    s_file_image_dsc.data = s_file_image_buf;
    *out_w = (uint32_t)width;
    *out_h = height;
    return true;
}

static void file_page_open_bmp(const char *path, const char *name)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!file_page_decode_bmp_to_rgb888(path, &width, &height))
    {
        file_page_viewer_show(name, "BMP decode failed");
        file_page_viewer_status_set("BMP decode failed", UI_ERROR);
        return;
    }

    char status[64] = {0};
    snprintf(status, sizeof(status), "BMP %" PRIu32 "x%" PRIu32, width, height);
    file_page_viewer_show(name, status);
    file_page_image_show_scaled(width, height);
    file_page_viewer_status_set(status, UI_OK);
}

static bool file_page_decode_jpeg_to_rgb888(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ESP_LOGW(TAG, "Open JPEG failed: %s errno=%d", path, errno);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > FILE_JPEG_MAX_INPUT_BYTES)
    {
        fclose(file);
        ESP_LOGW(TAG, "JPEG input too large: %s %ld bytes", path, file_size);
        return false;
    }
    rewind(file);

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_buf_size = 0;
    uint8_t *in_buf = jpeg_alloc_decoder_mem((size_t)file_size, &in_mem_cfg, &in_buf_size);
    if (in_buf == NULL)
    {
        fclose(file);
        ESP_LOGW(TAG, "Alloc JPEG input failed: %s %ld bytes", path, file_size);
        return false;
    }

    size_t read_size = fread(in_buf, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Read JPEG failed: %s %u/%ld", path, (unsigned)read_size, file_size);
        return false;
    }

    jpeg_decode_picture_info_t pic_info = {0};
    esp_err_t ret = jpeg_decoder_get_info(in_buf, (uint32_t)file_size, &pic_info);
    if (ret != ESP_OK)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Get JPEG info failed: %s %s", path, esp_err_to_name(ret));
        return false;
    }

    uint32_t aligned_w = (pic_info.width + 15U) & ~15U;
    uint32_t aligned_h = (pic_info.height + 15U) & ~15U;
    uint32_t out_bytes = aligned_w * aligned_h * 3U;
    if (pic_info.width == 0 || pic_info.height == 0 || out_bytes == 0 || out_bytes > FILE_BMP_MAX_DECODE_BYTES)
    {
        free(in_buf);
        ESP_LOGW(TAG, "JPEG decode buffer too large: %s %" PRIu32 "x%" PRIu32 " %" PRIu32 " bytes",
                 path,
                 pic_info.width,
                 pic_info.height,
                 out_bytes);
        return false;
    }

    file_page_image_buf_free();

    jpeg_decode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_buf_size = 0;
    uint8_t *out_buf = jpeg_alloc_decoder_mem(out_bytes, &out_mem_cfg, &out_buf_size);
    if (out_buf == NULL)
    {
        free(in_buf);
        ESP_LOGW(TAG, "Alloc JPEG output failed: %s %" PRIu32 " bytes", path, out_bytes);
        return false;
    }

    jpeg_decoder_handle_t decoder = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK)
    {
        free(out_buf);
        free(in_buf);
        ESP_LOGW(TAG, "Create JPEG decoder failed: %s", esp_err_to_name(ret));
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(decoder,
                               &decode_cfg,
                               in_buf,
                               (uint32_t)file_size,
                               out_buf,
                               (uint32_t)out_buf_size,
                               &decoded_size);
    esp_err_t del_ret = jpeg_del_decoder_engine(decoder);
    free(in_buf);
    if (ret != ESP_OK)
    {
        free(out_buf);
        ESP_LOGW(TAG, "Decode JPEG failed: %s %s", path, esp_err_to_name(ret));
        return false;
    }
    if (del_ret != ESP_OK)
    {
        free(out_buf);
        ESP_LOGW(TAG, "Delete JPEG decoder failed: %s", esp_err_to_name(del_ret));
        return false;
    }

    s_file_image_buf = out_buf;
    s_file_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_file_image_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_file_image_dsc.header.flags = 0;
    s_file_image_dsc.header.w = pic_info.width;
    s_file_image_dsc.header.h = pic_info.height;
    s_file_image_dsc.header.stride = aligned_w * 3U;
    s_file_image_dsc.data_size = decoded_size > 0 ? decoded_size : out_bytes;
    s_file_image_dsc.data = s_file_image_buf;
    *out_w = pic_info.width;
    *out_h = pic_info.height;
    return true;
}

static void file_page_open_jpeg(const char *path, const char *name)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (!file_page_decode_jpeg_to_rgb888(path, &width, &height))
    {
        file_page_viewer_show(name, "JPEG decode failed");
        file_page_viewer_status_set("JPEG decode failed", UI_ERROR);
        return;
    }

    char status[64] = {0};
    snprintf(status, sizeof(status), "JPEG %" PRIu32 "x%" PRIu32, width, height);
    file_page_viewer_show(name, status);
    file_page_image_show_scaled(width, height);
    file_page_viewer_status_set(status, UI_OK);
}

static void file_page_empty_show(int shown)
{
    if (shown == 0 && s_file_ui.list_cont)
    {
        lv_obj_t *empty = ui_label_create(s_file_ui.list_cont, "No files found", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void file_page_scan_async_cb(void *user_data)
{
    (void)user_data;
    Page *current = ui_page_get_current();
    if (current == NULL || current->id != PAGE_FILE || s_file_ui.root == NULL || s_file_ui.list_cont == NULL)
    {
        return;
    }

    file_page_copy_text(s_file_current_path, sizeof(s_file_current_path), s_file_pending_path);
    int shown = file_page_scan_dir(s_file_current_path);
    file_page_empty_show(shown);
}

static void file_page_open_async_cb(void *user_data)
{
    (void)user_data;
    Page *current = ui_page_get_current();
    if (current == NULL || current->id != PAGE_FILE || s_file_ui.root == NULL)
    {
        return;
    }

    if (file_page_has_ext(s_file_open_pending_path, "txt"))
    {
        file_page_open_txt(s_file_open_pending_path, s_file_open_pending_name);
    }
    else if (file_page_has_ext(s_file_open_pending_path, "bmp"))
    {
        file_page_open_bmp(s_file_open_pending_path, s_file_open_pending_name);
    }
    else if (file_page_has_ext(s_file_open_pending_path, "jpg") ||
             file_page_has_ext(s_file_open_pending_path, "jpeg"))
    {
        file_page_open_jpeg(s_file_open_pending_path, s_file_open_pending_name);
    }
}

static void file_page_open_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    file_page_open_async_cb(NULL);
}

static void file_page_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const char *name = (const char *)lv_event_get_user_data(e);
    if (name == NULL || name[0] == '\0')
    {
        return;
    }

    if (strcmp(name, "..") == 0)
    {
        if (file_page_parent_path(s_file_current_path))
        {
            file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), s_file_current_path);
            lv_async_call(file_page_scan_async_cb, NULL);
        }
        return;
    }

    char next_path[FILE_PATH_MAX_LEN] = {0};
    if (!file_page_build_path(next_path, sizeof(next_path), s_file_current_path, name))
    {
        return;
    }

    struct stat st = {0};
    if (stat(next_path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), next_path);
        lv_async_call(file_page_scan_async_cb, NULL);
        return;
    }

    if (file_page_has_ext(next_path, "txt") ||
        file_page_has_ext(next_path, "bmp") ||
        file_page_has_ext(next_path, "jpg") ||
        file_page_has_ext(next_path, "jpeg"))
    {
        file_page_copy_text(s_file_open_pending_path, sizeof(s_file_open_pending_path), next_path);
        file_page_copy_text(s_file_open_pending_name, sizeof(s_file_open_pending_name), name);
        file_page_viewer_show(name, "LOADING...");
        file_page_viewer_status_set("LOADING...", UI_PRIMARY);
        lv_timer_t *timer = lv_timer_create(file_page_open_timer_cb, 60, NULL);
        if (timer)
        {
            lv_timer_set_repeat_count(timer, 1);
        }
    }
    else
    {
        file_page_viewer_show(name, "Unsupported file");
        file_page_viewer_status_set("Only TXT/BMP/JPG/JPEG preview now", UI_WARN);
    }
}

static void file_page_add_row(lv_obj_t *parent, int index, const char *name, bool is_dir, const char *event_name)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             36,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(index % 2 == 0 ? UI_BG : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, index % 2 == 0 ? LV_OPA_TRANSP : LV_OPA_60, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = ui_label_create(row, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, &lv_font_montserrat_16, is_dir ? UI_PRIMARY : UI_SECONDARY);
    lv_obj_set_width(icon, 28);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name_label = ui_label_create(row, name, MUSIC_NAME_FONT, UI_TEXT);
    lv_obj_set_width(name_label, 430);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);

    ui_flex_spacer_create(row);

    lv_obj_t *type_label = ui_label_create(row, file_page_ext_name(name, is_dir), &lv_font_montserrat_14, is_dir ? UI_PRIMARY : UI_MUTED);
    lv_obj_set_width(type_label, 86);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_RIGHT, 0);

    if (event_name)
    {
        lv_obj_add_event_cb(row, file_page_row_event_cb, LV_EVENT_CLICKED, (void *)event_name);
    }
}

static int file_page_scan_dir(const char *dir_path)
{
    if (s_file_ui.list_cont == NULL)
    {
        return -1;
    }

    if (s_file_ui.list_cont)
    {
        lv_obj_clean(s_file_ui.list_cont);
    }
    if (s_file_ui.path_label)
    {
        lv_label_set_text(s_file_ui.path_label, dir_path);
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL)
    {
        if (s_file_ui.count_label)
        {
            lv_label_set_text(s_file_ui.count_label, "OPEN FAILED");
            lv_obj_set_style_text_color(s_file_ui.count_label, lv_color_hex(UI_ERROR), 0);
        }
        ESP_LOGW(TAG, "Failed to open file dir: %s", dir_path);
        return -1;
    }

    int shown = 0;
    int total = 0;
    static char row_names[FILE_LIST_MAX_ROWS][FILE_NAME_MAX_LEN];
    if (strcmp(dir_path, FILE_SCAN_DIR) != 0 && shown < FILE_LIST_MAX_ROWS)
    {
        file_page_copy_text(row_names[shown], sizeof(row_names[shown]), "..");
        file_page_add_row(s_file_ui.list_cont, shown, row_names[shown], true, row_names[shown]);
        shown++;
    }

    for (int pass = 0; pass < 2; pass++)
    {
        rewinddir(dir);
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] == '.')
            {
                continue;
            }

            bool is_dir = file_page_is_dir(dir_path, entry->d_name);
            if ((pass == 0 && !is_dir) || (pass == 1 && is_dir))
            {
                continue;
            }

            total++;
            if (shown < FILE_LIST_MAX_ROWS)
            {
                file_page_copy_text(row_names[shown], sizeof(row_names[shown]), entry->d_name);
                file_page_add_row(s_file_ui.list_cont, shown, row_names[shown], is_dir, row_names[shown]);
                shown++;
            }
        }
    }
    closedir(dir);

    int shown_items = shown;
    if (strcmp(dir_path, FILE_SCAN_DIR) != 0 && shown_items > 0)
    {
        shown_items--;
    }
    if (s_file_ui.count_label)
    {
        if (shown_items < total)
        {
            lv_label_set_text_fmt(s_file_ui.count_label, "%d/%d ITEMS", shown_items, total);
        }
        else
        {
            lv_label_set_text_fmt(s_file_ui.count_label, "%d ITEMS", total);
        }
        lv_obj_set_style_text_color(s_file_ui.count_label, lv_color_hex(UI_MUTED), 0);
    }
    return shown;
}

static void file_page_viewer_create(lv_obj_t *parent)
{
    s_file_ui.viewer_cont = lv_obj_create(parent);
    lv_obj_set_size(s_file_ui.viewer_cont, 660, 580);
    lv_obj_align(s_file_ui.viewer_cont, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(s_file_ui.viewer_cont, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_file_ui.viewer_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_file_ui.viewer_cont, 1, 0);
    lv_obj_set_style_border_color(s_file_ui.viewer_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_file_ui.viewer_cont, 12, 0);
    lv_obj_set_style_pad_all(s_file_ui.viewer_cont, 14, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_file_ui.viewer_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *head = ui_flex_container_create(s_file_ui.viewer_cont,
                                              LV_PCT(100),
                                              38,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);

    s_file_ui.viewer_title_label = ui_label_create(head, "VIEWER", MUSIC_NAME_FONT, UI_TEXT);
    lv_obj_set_width(s_file_ui.viewer_title_label, 390);
    lv_label_set_long_mode(s_file_ui.viewer_title_label, LV_LABEL_LONG_MODE_DOTS);

    s_file_ui.viewer_status_label = ui_label_create(head, "", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_file_ui.viewer_status_label, 150);
    lv_label_set_long_mode(s_file_ui.viewer_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_file_ui.viewer_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 36, 32);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, file_page_viewer_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_16, UI_TEXT);
    lv_obj_center(close_label);

    s_file_ui.viewer_body = lv_obj_create(s_file_ui.viewer_cont);
    lv_obj_set_size(s_file_ui.viewer_body, LV_PCT(100), 500);
    lv_obj_align(s_file_ui.viewer_body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_file_ui.viewer_body, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_file_ui.viewer_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_file_ui.viewer_body, 1, 0);
    lv_obj_set_style_border_color(s_file_ui.viewer_body, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_file_ui.viewer_body, 8, 0);
    lv_obj_set_style_pad_all(s_file_ui.viewer_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_file_ui.viewer_body, LV_OBJ_FLAG_SCROLLABLE);

    s_file_ui.viewer_textarea = lv_textarea_create(s_file_ui.viewer_body);
    lv_obj_set_size(s_file_ui.viewer_textarea, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_text(s_file_ui.viewer_textarea, "");
    lv_textarea_set_one_line(s_file_ui.viewer_textarea, false);
    lv_textarea_set_max_length(s_file_ui.viewer_textarea, FILE_TXT_MAX_BYTES);
    lv_obj_set_style_bg_opa(s_file_ui.viewer_textarea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_ui.viewer_textarea, 0, 0);
    lv_obj_set_style_text_color(s_file_ui.viewer_textarea, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_file_ui.viewer_textarea, MUSIC_NAME_FONT, 0);
    lv_obj_set_scrollbar_mode(s_file_ui.viewer_textarea, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_file_ui.viewer_textarea, LV_OBJ_FLAG_HIDDEN);

    s_file_ui.viewer_img = lv_image_create(s_file_ui.viewer_body);
    lv_obj_center(s_file_ui.viewer_img);
    lv_obj_add_flag(s_file_ui.viewer_img, LV_OBJ_FLAG_HIDDEN);
}

static void file_page_create(Page *page)
{
    file_page_copy_text(s_file_current_path, sizeof(s_file_current_path), FILE_SCAN_DIR);
    file_page_copy_text(s_file_pending_path, sizeof(s_file_pending_path), FILE_SCAN_DIR);
    s_file_ui.root = page->root;
    lv_obj_set_style_bg_color(s_file_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_file_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_file_ui.root, 0, 0);
    lv_obj_add_flag(s_file_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_file_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_cont = lv_obj_create(s_file_ui.root);
    ui_main_cont_style_init(main_cont);

    lv_obj_t *title_cont = ui_flex_container_create(main_cont,
                                                    680,
                                                    40,
                                                    LV_FLEX_FLOW_ROW,
                                                    LV_FLEX_ALIGN_START,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_cont, 16, 0);

    lv_obj_t *title = ui_label_create(title_cont, "FILES", &lv_font_montserrat_28, UI_PRIMARY);
    lv_obj_t *subtitle = ui_label_create(title_cont, "SD CARD / DIRECTORY", &lv_font_montserrat_16, UI_MUTED);
    (void)title;
    (void)subtitle;
    ui_flex_spacer_create(title_cont);

    s_file_ui.count_label = ui_label_create(title_cont, "-- ITEMS", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_set_width(s_file_ui.count_label, 120);
    lv_obj_set_style_text_align(s_file_ui.count_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *panel = ui_flex_container_create(main_cont,
                                               680,
                                               600,
                                               LV_FLEX_FLOW_COLUMN,
                                               LV_FLEX_ALIGN_START,
                                               LV_FLEX_ALIGN_CENTER,
                                               LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);

    lv_obj_t *path_row = ui_flex_container_create(panel,
                                                  LV_PCT(100),
                                                  32,
                                                  LV_FLEX_FLOW_ROW,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(path_row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(path_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(path_row, 1, 0);
    lv_obj_set_style_border_color(path_row, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(path_row, 8, 0);
    lv_obj_set_style_pad_left(path_row, 12, 0);
    lv_obj_set_style_pad_right(path_row, 12, 0);
    lv_obj_set_style_pad_column(path_row, 8, 0);

    ui_label_create(path_row, LV_SYMBOL_SD_CARD, &lv_font_montserrat_16, UI_SECONDARY);
    s_file_ui.path_label = ui_label_create(path_row, FILE_SCAN_DIR, &lv_font_montserrat_16, UI_TEXT);
    lv_label_set_long_mode(s_file_ui.path_label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *head = ui_flex_container_create(panel,
                                              LV_PCT(100),
                                              28,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(head, 50, 0);
    lv_obj_set_style_pad_right(head, 12, 0);
    ui_label_create(head, "NAME", &lv_font_montserrat_12, UI_MUTED);
    ui_flex_spacer_create(head);
    lv_obj_t *format_head = ui_label_create(head, "FORMAT", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(format_head, 86);
    lv_obj_set_style_text_align(format_head, LV_TEXT_ALIGN_RIGHT, 0);

    s_file_ui.list_cont = lv_obj_create(panel);
    lv_obj_set_size(s_file_ui.list_cont, LV_PCT(100), 488);
    lv_obj_set_style_bg_opa(s_file_ui.list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_ui.list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_file_ui.list_cont, 0, 0);
    lv_obj_set_style_pad_row(s_file_ui.list_cont, 4, 0);
    lv_obj_set_flex_flow(s_file_ui.list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_file_ui.list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_file_ui.list_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_file_ui.list_cont, LV_OBJ_FLAG_SCROLLABLE);

    file_page_viewer_create(s_file_ui.root);

    int shown = file_page_scan_dir(s_file_current_path);
    file_page_empty_show(shown);
}

static void file_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void file_page_destroy(Page *page)
{
    (void)page;
    file_page_image_buf_free();
    memset(&s_file_ui, 0, sizeof(s_file_ui));
}

/*********bmu Page************/
typedef enum
{
    BMU_MODULE_CHARGER = 0,
    BMU_MODULE_BAT_DETECT,
    BMU_MODULE_FUEL_GAUGE,
    BMU_MODULE_BC12,
} bmu_module_t;

typedef enum
{
    BMU_CONFIG_CHARGE_CURRENT = 0,
    BMU_CONFIG_INPUT_CURRENT,
    BMU_CONFIG_LOW_WARN,
    BMU_CONFIG_COUNT,
} bmu_config_param_t;

static void bmu_page_timer(Page *page);

static void bmu_charger_switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    if (s_bmu_ui_updating)
    {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(e);
    lv_obj_t *state_label = lv_event_get_user_data(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    bool ok = bmu_charger_enable_set(enabled);

    if (state_label)
    {
        lv_label_set_text(state_label, ok ? (enabled ? "ON" : "OFF") : "FAIL");
        lv_obj_set_style_text_color(state_label, lv_color_hex(ok ? (enabled ? UI_PRIMARY : UI_MUTED) : UI_ERROR), 0);
    }

    if (s_bmu_ui.status_label)
    {
        lv_label_set_text(s_bmu_ui.status_label, ok ? "SET OK" : "SET FAIL");
        lv_obj_set_style_text_color(s_bmu_ui.status_label, lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
    }
}

static void bmu_config_slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    bmu_config_param_t param = (bmu_config_param_t)(intptr_t)lv_event_get_user_data(e);
    if (param >= BMU_CONFIG_COUNT)
    {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(slider);
    bool ok = false;

    char buf[24];
    switch (param)
    {
    case BMU_CONFIG_CHARGE_CURRENT:
        ok = bmu_charge_current_set(value);
        if (ok)
        {
            s_bmu_charge_current_ma = value;
        }
        snprintf(buf, sizeof(buf), "%d mA", value);
        if (s_bmu_ui.charge_current_value_label)
        {
            lv_label_set_text(s_bmu_ui.charge_current_value_label, buf);
        }
        break;
    case BMU_CONFIG_INPUT_CURRENT:
        ok = bmu_input_current_limit_set(value);
        if (ok)
        {
            s_bmu_input_current_limit_ma = value;
        }
        snprintf(buf, sizeof(buf), "%d mA", value);
        if (s_bmu_ui.input_current_value_label)
        {
            lv_label_set_text(s_bmu_ui.input_current_value_label, buf);
        }
        break;
    case BMU_CONFIG_LOW_WARN:
        ok = bmu_low_battery_warn_set(value);
        if (ok)
        {
            s_bmu_low_warn_percent = value;
        }
        snprintf(buf, sizeof(buf), "%d%%", value);
        if (s_bmu_ui.low_warn_value_label)
        {
            lv_label_set_text(s_bmu_ui.low_warn_value_label, buf);
        }
        break;
    default:
        break;
    }

    if (s_bmu_ui.status_label)
    {
        lv_label_set_text(s_bmu_ui.status_label, ok ? "SET OK" : "SET FAIL");
        lv_obj_set_style_text_color(s_bmu_ui.status_label, lv_color_hex(ok ? UI_OK : UI_ERROR), 0);
    }
}

static const char *bmu_charge_status_text(int status)
{
    switch (status)
    {
    case 0:
        return "Trickle";
    case 1:
        return "Pre";
    case 2:
        return "CC";
    case 3:
        return "CV";
    case 4:
        return "Done";
    case 5:
        return "Not";
    default:
        return "--";
    }
}

static const char *bmu_bc12_text(int type)
{
    switch (type)
    {
    case 1:
        return "SDP";
    case 2:
        return "CDP";
    case 3:
        return "DCP";
    case 0:
        return "Unknown";
    default:
        return "--";
    }
}

static void bmu_value_text_set(bmu_value_t id, const char *text, uint32_t color)
{
    if (id >= BMU_VALUE_LABEL_COUNT || s_bmu_ui.value_label[id] == NULL)
    {
        return;
    }

    lv_label_set_text(s_bmu_ui.value_label[id], text ? text : "--");
    lv_obj_set_style_text_color(s_bmu_ui.value_label[id], lv_color_hex(color), 0);
}

static void bmu_value_int_set(bmu_value_t id, int value, const char *unit, uint32_t color)
{
    char buf[24];
    if (value >= 0)
    {
        snprintf(buf, sizeof(buf), "%d%s", value, unit ? unit : "");
    }
    else
    {
        snprintf(buf, sizeof(buf), "--%s", unit ? unit : "");
    }
    bmu_value_text_set(id, buf, color);
}

static void bmu_value_temp_set(bmu_value_t id, int temp_c_x10, uint32_t color)
{
    char buf[24];
    if (temp_c_x10 >= 0)
    {
        snprintf(buf, sizeof(buf), "%d.%d C", temp_c_x10 / 10, temp_c_x10 % 10);
    }
    else
    {
        snprintf(buf, sizeof(buf), "-- C");
    }
    bmu_value_text_set(id, buf, color);
}

static void bmu_module_state_set(bmu_module_t module, const char *text, uint32_t color)
{
    if (module >= BMU_MODULE_STATE_COUNT || s_bmu_ui.module_state_label[module] == NULL)
    {
        return;
    }

    lv_label_set_text(s_bmu_ui.module_state_label[module], text ? text : "--");
    lv_obj_set_style_text_color(s_bmu_ui.module_state_label[module], lv_color_hex(color), 0);
}

static void bmu_switch_checked_set(lv_obj_t *sw, bool checked)
{
    if (sw == NULL)
    {
        return;
    }

    s_bmu_ui_updating = true;
    if (checked)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    s_bmu_ui_updating = false;
}

static void bmu_page_ui_update(const bmu_info_t *info)
{
    if (s_bmu_ui.root == NULL)
    {
        return;
    }

    bool ready = (info != NULL && info->ready);
    if (s_bmu_ui.status_label)
    {
        lv_label_set_text(s_bmu_ui.status_label, ready ? "ONLINE" : "OFFLINE");
        lv_obj_set_style_text_color(s_bmu_ui.status_label, lv_color_hex(ready ? UI_OK : UI_ERROR), 0);
    }

    if (!ready)
    {
        if (s_bmu_ui.battery_bar)
        {
            lv_bar_set_value(s_bmu_ui.battery_bar, 0, LV_ANIM_OFF);
        }
        for (int i = 0; i < BMU_VALUE_LABEL_COUNT; i++)
        {
            bmu_value_text_set((bmu_value_t)i, "--", UI_MUTED);
        }
        bmu_module_state_set(BMU_MODULE_CHARGER, "OFF", UI_MUTED);
        bmu_module_state_set(BMU_MODULE_BAT_DETECT, "--", UI_MUTED);
        bmu_module_state_set(BMU_MODULE_FUEL_GAUGE, "--", UI_MUTED);
        bmu_module_state_set(BMU_MODULE_BC12, "--", UI_MUTED);
        if (s_bmu_ui.charger_switch)
        {
            lv_obj_add_state(s_bmu_ui.charger_switch, LV_STATE_DISABLED);
            bmu_switch_checked_set(s_bmu_ui.charger_switch, false);
        }
        return;
    }

    if (s_bmu_ui.charger_switch)
    {
        lv_obj_remove_state(s_bmu_ui.charger_switch, LV_STATE_DISABLED);
        bmu_switch_checked_set(s_bmu_ui.charger_switch, info->charger_enabled);
    }

    int battery_percent = home_clamp_percent(info->battery_percent);
    if (s_bmu_ui.battery_bar)
    {
        lv_bar_set_value(s_bmu_ui.battery_bar, battery_percent >= 0 ? battery_percent : 0, LV_ANIM_OFF);
    }
    bmu_value_int_set(BMU_VALUE_BATTERY_PERCENT, battery_percent, "%", battery_percent >= 0 && battery_percent < 15 ? UI_ERROR : UI_TEXT);
    bmu_value_int_set(BMU_VALUE_BATTERY_SOH, info->battery_soh, "%", UI_OK);
    bmu_value_text_set(BMU_VALUE_CHARGE_STATUS, bmu_charge_status_text(info->charge_status), UI_TEXT);
    bmu_value_text_set(BMU_VALUE_BAT_PRESENT, info->bat_present ? "Yes" : "No", info->bat_present ? UI_TEXT : UI_WARN);
    bmu_value_int_set(BMU_VALUE_VBUS, info->vbus_mv, " mV", UI_TEXT);
    bmu_value_int_set(BMU_VALUE_VSYS, info->vsys_mv, " mV", UI_TEXT);
    bmu_value_int_set(BMU_VALUE_VBAT, info->vbat_mv, " mV", UI_TEXT);
    bmu_value_int_set(BMU_VALUE_IBUS, info->ibus_ma, " mA", UI_PRIMARY);
    bmu_value_int_set(BMU_VALUE_ICHG, info->ichg_ma, " mA", UI_OK);
    bmu_value_int_set(BMU_VALUE_IDIS, info->idis_ma, " mA", UI_TEXT);
    bmu_value_int_set(BMU_VALUE_TS, info->ts_mv, " mV", UI_TEXT);
    bmu_value_temp_set(BMU_VALUE_DIE_TEMP, info->die_temp_c_x10, info->die_temp_c_x10 >= 700 ? UI_ERROR : UI_WARN);

    const char *bc12 = bmu_bc12_text(info->bc12_type);
    bmu_value_text_set(BMU_VALUE_BC12, bc12, UI_PRIMARY);

    const char *fault_text = info->fault_text[0] ? info->fault_text : "Clear";
    bool has_fault = strcmp(fault_text, "Clear") != 0;
    bmu_value_text_set(BMU_VALUE_FAULT, fault_text, has_fault ? UI_ERROR : UI_OK);
    bmu_value_text_set(BMU_VALUE_VINDPM, info->vindpm ? "Limit" : "Normal", info->vindpm ? UI_WARN : UI_OK);
    bmu_value_text_set(BMU_VALUE_THERMAL, info->thermal_regulation ? "Reg" : "Normal", info->thermal_regulation ? UI_WARN : UI_OK);

    const char *charger_module_text = info->charger_enabled ? (info->charger_hw_enabled ? "ON" : "ON?") : (info->charger_hw_enabled ? "OFF?" : "OFF");
    uint32_t charger_module_color = (info->charger_enabled == info->charger_hw_enabled) ? (info->charger_enabled ? UI_PRIMARY : UI_MUTED) : UI_WARN;
    bmu_module_state_set(BMU_MODULE_CHARGER, charger_module_text, charger_module_color);
    bmu_module_state_set(BMU_MODULE_BAT_DETECT, info->battery_detection_enabled ? "ACTIVE" : "OFF", info->battery_detection_enabled ? UI_OK : UI_MUTED);
    bmu_module_state_set(BMU_MODULE_FUEL_GAUGE, info->fuel_gauge_enabled ? "ACTIVE" : "OFF", info->fuel_gauge_enabled ? UI_OK : UI_MUTED);
    bmu_module_state_set(BMU_MODULE_BC12, info->bc12_enabled ? bc12 : "OFF", info->bc12_enabled ? UI_SECONDARY : UI_MUTED);
}

static void bmu_page_create(Page *page)
{
    s_bmu_ui.root = page->root;
    lv_obj_set_style_bg_color(s_bmu_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_bmu_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_bmu_ui.root, 0, 0);
    lv_obj_add_flag(s_bmu_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_bmu_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_cont = lv_obj_create(s_bmu_ui.root);
    lv_obj_set_size(main_cont, 680, 680);
    lv_obj_align(main_cont, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_cont, 0, 0);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_set_scrollbar_mode(main_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_cont = ui_flex_container_create(main_cont,
                                                    680,
                                                    40,
                                                    LV_FLEX_FLOW_ROW,
                                                    LV_FLEX_ALIGN_START,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(title_cont, 0, 0);
    lv_obj_set_style_pad_column(title_cont, 16, 0);

    lv_obj_t *title = lv_label_create(title_cont);
    lv_label_set_text(title, "BMU");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_PRIMARY), 0);

    lv_obj_t *subtitle = lv_label_create(title_cont);
    lv_label_set_text(subtitle, "AXP517 POWER / CHARGER CONSOLE");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(UI_MUTED), 0);

    ui_flex_spacer_create(title_cont);

    s_bmu_ui.status_label = lv_label_create(title_cont);
    lv_label_set_text(s_bmu_ui.status_label, "MONITOR");
    lv_obj_set_size(s_bmu_ui.status_label, 130, 24);
    lv_obj_set_style_text_font(s_bmu_ui.status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_bmu_ui.status_label, lv_color_hex(UI_OK), 0);
    lv_obj_set_style_text_align(s_bmu_ui.status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *battery_panel = lv_obj_create(main_cont);
    lv_obj_t *rail_panel = lv_obj_create(main_cont);
    lv_obj_t *health_panel = lv_obj_create(main_cont);
    lv_obj_t *charger_panel = lv_obj_create(main_cont);
    lv_obj_t *module_panel = lv_obj_create(main_cont);
    lv_obj_t *protect_panel = lv_obj_create(main_cont);
    lv_obj_t *panels[] = {battery_panel, rail_panel, health_panel, charger_panel, module_panel, protect_panel};
    const int16_t panel_pos[][4] = {
        {0, 52, 330, 180},
        {350, 52, 330, 180},
        {0, 248, 330, 218},
        {350, 248, 330, 282},
        {0, 486, 330, 160},
        {350, 536, 330, 110},
    };

    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++)
    {
        lv_obj_set_pos(panels[i], panel_pos[i][0], panel_pos[i][1]);
        lv_obj_set_size(panels[i], panel_pos[i][2], panel_pos[i][3]);
        lv_obj_set_style_bg_color(panels[i], lv_color_hex(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(panels[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(panels[i], 1, 0);
        lv_obj_set_style_border_color(panels[i], lv_color_hex(UI_LINE), 0);
        lv_obj_set_style_radius(panels[i], 12, 0);
        lv_obj_set_style_pad_all(panels[i], 0, 0);
        lv_obj_set_scrollbar_mode(panels[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    const int16_t bmu_panel_x = 18;
    const int16_t bmu_title_y = 16;
    const int16_t bmu_row_h = 20;
    const int16_t bmu_row_step = 26;
    const int16_t bmu_row_start_y = 52;
    const int16_t bmu_battery_row_start_y = 90;

    lv_obj_t *battery_head = ui_flex_container_create(battery_panel,
                                                      294,
                                                      46,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(battery_head, bmu_panel_x, 14);
    lv_obj_set_style_pad_column(battery_head, 12, 0);

    lv_obj_t *battery_icon = lv_label_create(battery_head);
    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_size(battery_icon, 38, 34);
    lv_obj_set_style_text_font(battery_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(battery_icon, lv_color_hex(UI_OK), 0);
    lv_obj_set_style_text_align(battery_icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *battery_title_cont = ui_flex_container_create(battery_head,
                                                            132,
                                                            46,
                                                            LV_FLEX_FLOW_COLUMN,
                                                            LV_FLEX_ALIGN_CENTER,
                                                            LV_FLEX_ALIGN_START,
                                                            LV_FLEX_ALIGN_START);

    lv_obj_t *battery_title = ui_label_create(battery_title_cont, "BATTERY", &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_size(battery_title, 120, 22);

    lv_obj_t *battery_subtitle = ui_label_create(battery_title_cont, "Li-ion pack", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_size(battery_subtitle, 120, 18);

    ui_flex_spacer_create(battery_head);

    s_bmu_ui.value_label[BMU_VALUE_BATTERY_PERCENT] = ui_label_create(battery_head, "100%", &lv_font_montserrat_28, UI_TEXT);
    lv_obj_set_size(s_bmu_ui.value_label[BMU_VALUE_BATTERY_PERCENT], 78, 34);
    lv_obj_set_style_text_align(s_bmu_ui.value_label[BMU_VALUE_BATTERY_PERCENT], LV_TEXT_ALIGN_RIGHT, 0);

    s_bmu_ui.battery_bar = lv_bar_create(battery_panel);
    lv_obj_set_pos(s_bmu_ui.battery_bar, bmu_panel_x, 68);
    lv_obj_set_size(s_bmu_ui.battery_bar, 294, 10);
    lv_bar_set_range(s_bmu_ui.battery_bar, 0, 100);
    lv_bar_set_value(s_bmu_ui.battery_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bmu_ui.battery_bar, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bmu_ui.battery_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bmu_ui.battery_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bmu_ui.battery_bar, lv_color_hex(UI_OK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bmu_ui.battery_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bmu_ui.battery_bar, 5, LV_PART_INDICATOR);

    typedef struct
    {
        bmu_value_t id;
        const char *name;
        const char *value;
        uint32_t color;
    } bmu_value_row_t;

    const bmu_value_row_t battery_rows[] = {
        {BMU_VALUE_BATTERY_SOH, "SOH", "100%", UI_OK},
        {BMU_VALUE_CHARGE_STATUS, "CHARGE", "Done", UI_TEXT},
        {BMU_VALUE_BAT_PRESENT, "PRESENT", "Yes", UI_TEXT},
    };

    for (size_t i = 0; i < sizeof(battery_rows) / sizeof(battery_rows[0]); i++)
    {
        int y = bmu_battery_row_start_y + (int)i * bmu_row_step;
        lv_obj_t *row = ui_flex_container_create(battery_panel,
                                                 294,
                                                 bmu_row_h,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_pos(row, bmu_panel_x, y);
        lv_obj_t *name = ui_label_create(row, battery_rows[i].name, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_set_size(name, 110, 20);
        ui_flex_spacer_create(row);
        s_bmu_ui.value_label[battery_rows[i].id] = ui_label_create(row, battery_rows[i].value, &lv_font_montserrat_14, battery_rows[i].color);
        lv_obj_set_size(s_bmu_ui.value_label[battery_rows[i].id], 120, 20);
        lv_obj_set_style_text_align(s_bmu_ui.value_label[battery_rows[i].id], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *rail_title = ui_label_create(rail_panel, "POWER RAILS", &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_pos(rail_title, bmu_panel_x, bmu_title_y);
    lv_obj_set_size(rail_title, 180, 22);
    const bmu_value_row_t rail_rows[] = {
        {BMU_VALUE_VBUS, "VBUS", "4860 mV", UI_TEXT},
        {BMU_VALUE_VSYS, "VSYS", "4195 mV", UI_TEXT},
        {BMU_VALUE_VBAT, "VBAT", "4143 mV", UI_TEXT},
        {BMU_VALUE_IBUS, "IBUS", "9 mA", UI_PRIMARY},
    };

    for (size_t i = 0; i < sizeof(rail_rows) / sizeof(rail_rows[0]); i++)
    {
        int y = bmu_row_start_y + (int)i * bmu_row_step;
        lv_obj_t *row = ui_flex_container_create(rail_panel,
                                                 294,
                                                 bmu_row_h,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_pos(row, bmu_panel_x, y);
        lv_obj_t *name = ui_label_create(row, rail_rows[i].name, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_set_size(name, 110, 20);
        ui_flex_spacer_create(row);
        s_bmu_ui.value_label[rail_rows[i].id] = ui_label_create(row, rail_rows[i].value, &lv_font_montserrat_14, rail_rows[i].color);
        lv_obj_set_size(s_bmu_ui.value_label[rail_rows[i].id], 122, 20);
        lv_obj_set_style_text_align(s_bmu_ui.value_label[rail_rows[i].id], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *health_title = ui_label_create(health_panel, "ADC / FAULTS", &lv_font_montserrat_16, UI_WARN);
    lv_obj_set_pos(health_title, bmu_panel_x, bmu_title_y);
    lv_obj_set_size(health_title, 180, 22);
    const bmu_value_row_t health_rows[] = {
        {BMU_VALUE_ICHG, "ICHG", "17 mA", UI_OK},
        {BMU_VALUE_IDIS, "IDIS", "0 mA", UI_TEXT},
        {BMU_VALUE_TS, "TS", "414 mV", UI_TEXT},
        {BMU_VALUE_DIE_TEMP, "DIE", "66.3 C", UI_WARN},
        {BMU_VALUE_BC12, "BC1.2", "DCP", UI_PRIMARY},
        {BMU_VALUE_FAULT, "FAULT", "Clear", UI_OK},
    };

    for (size_t i = 0; i < sizeof(health_rows) / sizeof(health_rows[0]); i++)
    {
        int y = bmu_row_start_y + (int)i * bmu_row_step;
        lv_obj_t *row = ui_flex_container_create(health_panel,
                                                 294,
                                                 bmu_row_h,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_pos(row, bmu_panel_x, y);
        lv_obj_t *name = ui_label_create(row, health_rows[i].name, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_set_size(name, 110, 20);
        ui_flex_spacer_create(row);
        s_bmu_ui.value_label[health_rows[i].id] = ui_label_create(row, health_rows[i].value, &lv_font_montserrat_14, health_rows[i].color);
        lv_obj_set_size(s_bmu_ui.value_label[health_rows[i].id], 122, 20);
        lv_obj_set_style_text_align(s_bmu_ui.value_label[health_rows[i].id], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *charger_title = ui_label_create(charger_panel, "CHARGER CONFIG", &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_pos(charger_title, bmu_panel_x, bmu_title_y);
    lv_obj_set_size(charger_title, 180, 22);

    const int16_t bmu_slider_gap = 28;
    const int16_t bmu_slider_block_step = 62;

    lv_obj_t *charge_current_head = ui_flex_container_create(charger_panel,
                                                             294,
                                                             bmu_row_h,
                                                             LV_FLEX_FLOW_ROW,
                                                             LV_FLEX_ALIGN_START,
                                                             LV_FLEX_ALIGN_CENTER,
                                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(charge_current_head, bmu_panel_x, bmu_row_start_y);
    lv_obj_t *charge_current_name = ui_label_create(charge_current_head, "CHG CURRENT", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_size(charge_current_name, 130, 20);
    ui_flex_spacer_create(charge_current_head);
    s_bmu_ui.charge_current_value_label = ui_label_create(charge_current_head, "", &lv_font_montserrat_14, UI_TEXT);
    lv_label_set_text_fmt(s_bmu_ui.charge_current_value_label, "%d mA", s_bmu_charge_current_ma);
    lv_obj_set_size(s_bmu_ui.charge_current_value_label, 90, 20);
    lv_obj_set_style_text_align(s_bmu_ui.charge_current_value_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_bmu_ui.charge_current_slider = lv_slider_create(charger_panel);
    lv_obj_set_pos(s_bmu_ui.charge_current_slider, bmu_panel_x, bmu_row_start_y + bmu_slider_gap);
    lv_obj_set_size(s_bmu_ui.charge_current_slider, 294, 10);
    lv_slider_set_range(s_bmu_ui.charge_current_slider, 0, 3000);
    lv_slider_set_value(s_bmu_ui.charge_current_slider, s_bmu_charge_current_ma, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bmu_ui.charge_current_slider, bmu_config_slider_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)BMU_CONFIG_CHARGE_CURRENT);

    lv_obj_t *input_current_head = ui_flex_container_create(charger_panel,
                                                            294,
                                                            bmu_row_h,
                                                            LV_FLEX_FLOW_ROW,
                                                            LV_FLEX_ALIGN_START,
                                                            LV_FLEX_ALIGN_CENTER,
                                                            LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(input_current_head, bmu_panel_x, bmu_row_start_y + bmu_slider_block_step);
    lv_obj_t *input_current_name = ui_label_create(input_current_head, "INPUT LIMIT", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_size(input_current_name, 130, 20);
    ui_flex_spacer_create(input_current_head);
    s_bmu_ui.input_current_value_label = ui_label_create(input_current_head, "", &lv_font_montserrat_14, UI_TEXT);
    lv_label_set_text_fmt(s_bmu_ui.input_current_value_label, "%d mA", s_bmu_input_current_limit_ma);
    lv_obj_set_size(s_bmu_ui.input_current_value_label, 90, 20);
    lv_obj_set_style_text_align(s_bmu_ui.input_current_value_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_bmu_ui.input_current_slider = lv_slider_create(charger_panel);
    lv_obj_set_pos(s_bmu_ui.input_current_slider, bmu_panel_x, bmu_row_start_y + bmu_slider_block_step + bmu_slider_gap);
    lv_obj_set_size(s_bmu_ui.input_current_slider, 294, 10);
    lv_slider_set_range(s_bmu_ui.input_current_slider, 100, 3200);
    lv_slider_set_value(s_bmu_ui.input_current_slider, s_bmu_input_current_limit_ma, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bmu_ui.input_current_slider, bmu_config_slider_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)BMU_CONFIG_INPUT_CURRENT);

    lv_obj_t *low_warn_head = ui_flex_container_create(charger_panel,
                                                       294,
                                                       bmu_row_h,
                                                       LV_FLEX_FLOW_ROW,
                                                       LV_FLEX_ALIGN_START,
                                                       LV_FLEX_ALIGN_CENTER,
                                                       LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(low_warn_head, bmu_panel_x, bmu_row_start_y + bmu_slider_block_step * 2);
    lv_obj_t *low_warn_name = ui_label_create(low_warn_head, "LOW WARN", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_size(low_warn_name, 130, 20);
    ui_flex_spacer_create(low_warn_head);
    s_bmu_ui.low_warn_value_label = ui_label_create(low_warn_head, "", &lv_font_montserrat_14, UI_WARN);
    lv_label_set_text_fmt(s_bmu_ui.low_warn_value_label, "%d%%", s_bmu_low_warn_percent);
    lv_obj_set_size(s_bmu_ui.low_warn_value_label, 90, 20);
    lv_obj_set_style_text_align(s_bmu_ui.low_warn_value_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_bmu_ui.low_warn_slider = lv_slider_create(charger_panel);
    lv_obj_set_pos(s_bmu_ui.low_warn_slider, bmu_panel_x, bmu_row_start_y + bmu_slider_block_step * 2 + bmu_slider_gap);
    lv_obj_set_size(s_bmu_ui.low_warn_slider, 294, 10);
    lv_slider_set_range(s_bmu_ui.low_warn_slider, 5, 20);
    lv_slider_set_value(s_bmu_ui.low_warn_slider, s_bmu_low_warn_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bmu_ui.low_warn_slider, bmu_config_slider_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)BMU_CONFIG_LOW_WARN);

    lv_obj_t *sliders[] = {s_bmu_ui.charge_current_slider, s_bmu_ui.input_current_slider, s_bmu_ui.low_warn_slider};
    for (size_t i = 0; i < sizeof(sliders) / sizeof(sliders[0]); i++)
    {
        lv_obj_set_style_bg_color(sliders[i], lv_color_hex(UI_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sliders[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_radius(sliders[i], 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sliders[i], lv_color_hex(i == 2 ? UI_WARN : UI_PRIMARY), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(sliders[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(sliders[i], 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sliders[i], lv_color_hex(UI_SECONDARY), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sliders[i], LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_border_width(sliders[i], 0, LV_PART_KNOB);
    }

    lv_obj_t *voltage_row = ui_flex_container_create(charger_panel,
                                                     294,
                                                     34,
                                                     LV_FLEX_FLOW_ROW,
                                                     LV_FLEX_ALIGN_START,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(voltage_row, bmu_panel_x, bmu_row_start_y + bmu_slider_block_step * 3);
    lv_obj_t *voltage_name = ui_label_create(voltage_row, "CHG VOLTAGE", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_size(voltage_name, 124, 20);

    ui_flex_spacer_create(voltage_row);

    s_bmu_ui.charge_voltage_label = ui_label_create(voltage_row, "4.20 V", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_style_text_align(s_bmu_ui.charge_voltage_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_bmu_ui.charge_voltage_label, 136, 34);
    lv_label_set_text(s_bmu_ui.charge_voltage_label, "4.20 V");
    lv_obj_remove_flag(s_bmu_ui.charge_voltage_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *module_title = ui_label_create(module_panel, "MODULES", &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_pos(module_title, bmu_panel_x, bmu_title_y);
    lv_obj_set_size(module_title, 180, 22);

    typedef struct
    {
        const char *name;
        const char *state;
        uint32_t color;
        bool use_switch;
    } bmu_module_row_t;

    const bmu_module_row_t module_rows[] = {
        {"CHARGER", "ON", UI_PRIMARY, true},
        {"BAT DETECT", "ACTIVE", UI_OK, false},
        {"FUEL GAUGE", "ACTIVE", UI_OK, false},
        {"BC1.2", "DCP", UI_SECONDARY, false},
    };

    for (size_t i = 0; i < sizeof(module_rows) / sizeof(module_rows[0]); i++)
    {
        int y = bmu_row_start_y + (int)i * bmu_row_step;
        lv_obj_t *row = ui_flex_container_create(module_panel,
                                                 294,
                                                 bmu_row_h,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_pos(row, bmu_panel_x, y);

        lv_obj_t *name = ui_label_create(row, module_rows[i].name, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_set_size(name, 140, 20);

        ui_flex_spacer_create(row);

        lv_obj_t *state = ui_label_create(row, module_rows[i].state, &lv_font_montserrat_14, module_rows[i].color);
        if (i < BMU_MODULE_STATE_COUNT)
        {
            s_bmu_ui.module_state_label[i] = state;
        }
        lv_obj_set_size(state, module_rows[i].use_switch ? 34 : 90, 20);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);

        if (module_rows[i].use_switch)
        {
            s_bmu_ui.charger_switch = lv_switch_create(row);
            lv_obj_set_size(s_bmu_ui.charger_switch, 50, 20);
            lv_obj_add_state(s_bmu_ui.charger_switch, LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(s_bmu_ui.charger_switch, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_bmu_ui.charger_switch, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(s_bmu_ui.charger_switch, 10, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_bmu_ui.charger_switch, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_bmu_ui.charger_switch, lv_color_hex(UI_LINE), LV_PART_MAIN);

            lv_obj_set_style_bg_color(s_bmu_ui.charger_switch, lv_color_hex(UI_LINE), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(s_bmu_ui.charger_switch, LV_OPA_60, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_bmu_ui.charger_switch, lv_color_hex(UI_PRIMARY), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(s_bmu_ui.charger_switch, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_radius(s_bmu_ui.charger_switch, 10, LV_PART_INDICATOR);

            lv_obj_set_style_bg_color(s_bmu_ui.charger_switch, lv_color_hex(UI_TEXT), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(s_bmu_ui.charger_switch, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(s_bmu_ui.charger_switch, 10, LV_PART_KNOB);
            lv_obj_add_event_cb(s_bmu_ui.charger_switch, bmu_charger_switch_event_cb, LV_EVENT_VALUE_CHANGED, state);
        }
    }

    lv_obj_t *protect_title = ui_label_create(protect_panel, "PROTECTION", &lv_font_montserrat_16, UI_WARN);
    lv_obj_set_pos(protect_title, bmu_panel_x, bmu_title_y);
    lv_obj_set_size(protect_title, 180, 22);

    const bmu_value_row_t protect_rows[] = {
        {BMU_VALUE_VINDPM, "VINDPM", "Normal", UI_OK},
        {BMU_VALUE_THERMAL, "THERMAL", "Normal", UI_OK},
    };

    for (size_t i = 0; i < sizeof(protect_rows) / sizeof(protect_rows[0]); i++)
    {
        int y = bmu_row_start_y + (int)i * bmu_row_step;
        lv_obj_t *row = ui_flex_container_create(protect_panel,
                                                 294,
                                                 bmu_row_h,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
        lv_obj_set_pos(row, bmu_panel_x, y);
        lv_obj_t *name = ui_label_create(row, protect_rows[i].name, &lv_font_montserrat_14, UI_MUTED);
        lv_obj_set_size(name, 120, 20);
        ui_flex_spacer_create(row);
        s_bmu_ui.value_label[protect_rows[i].id] = ui_label_create(row, protect_rows[i].value, &lv_font_montserrat_14, protect_rows[i].color);
        lv_obj_set_size(s_bmu_ui.value_label[protect_rows[i].id], 122, 20);
        lv_obj_set_style_text_align(s_bmu_ui.value_label[protect_rows[i].id], LV_TEXT_ALIGN_RIGHT, 0);
    }

    bmu_page_timer(page);
}

static void bmu_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void bmu_page_destroy(Page *page)
{
    (void)page;
    s_bmu_ui_updating = false;
    memset(&s_bmu_ui, 0, sizeof(s_bmu_ui));
}

static void bmu_page_timer(Page *page)
{
    (void)page;

    bmu_info_t bmu_info = {0};
    if (bmu_status_info_get(&bmu_info))
    {
        bmu_page_ui_update(&bmu_info);
    }
    else
    {
        bmu_page_ui_update(NULL);
    }
}

/*********set Page************/
static void set_page_power_status_set(const char *text, uint32_t color);
static void set_page_low_power_enter_request(void);
static void set_page_low_power_exit_request(void);

static uint32_t set_page_timeout_ms_from_index(uint32_t index)
{
    static const uint32_t timeout_ms[] = {
        0,
        30000,
        60000,
        300000,
        600000,
    };

    if (index >= sizeof(timeout_ms) / sizeof(timeout_ms[0]))
    {
        return timeout_ms[1];
    }
    return timeout_ms[index];
}

static uint32_t set_page_timeout_index_from_ms(uint32_t timeout_ms)
{
    switch (timeout_ms)
    {
    case 0:
        return 0;
    case 60000:
        return 2;
    case 300000:
        return 3;
    case 600000:
        return 4;
    case 30000:
    default:
        return 1;
    }
}

static void set_page_display_restore(void)
{
    if (!s_display_dimmed)
    {
        return;
    }

    esp_err_t ret = lcd_backlight_set_brightness((uint8_t)s_set_brightness_percent);
    if (ret == ESP_OK)
    {
        s_display_dimmed = false;
    }
    else
    {
        ESP_LOGW(TAG, "Restore backlight failed: %s", esp_err_to_name(ret));
    }
}

static void display_indev_activity_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
    {
        return;
    }

    s_display_last_activity_ms = lv_tick_get();
    set_page_display_restore();
    set_page_low_power_exit_request();
}

static void display_indev_activity_hook(void)
{
    if (s_display_indev_hooked)
    {
        return;
    }

    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL)
    {
        lv_indev_add_event_cb(indev, display_indev_activity_event_cb, LV_EVENT_ALL, NULL);
        s_display_indev_hooked = true;
    }
}

static void display_dim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_indev_activity_hook();

    if ((!s_set_auto_dim_enabled && !s_set_low_power_enabled) || s_set_screen_timeout_ms == 0)
    {
        set_page_display_restore();
        set_page_low_power_exit_request();
        return;
    }

    uint32_t now_ms = lv_tick_get();
    uint32_t inactive_ms = now_ms - s_display_last_activity_ms;
    if (inactive_ms < 1000)
    {
        set_page_display_restore();
        set_page_low_power_exit_request();
        return;
    }

    if (inactive_ms < s_set_screen_timeout_ms)
    {
        return;
    }

    if (s_set_auto_dim_enabled && !s_display_dimmed)
    {
        esp_err_t ret = lcd_backlight_set_brightness((uint8_t)SET_BRIGHTNESS_MIN);
        if (ret == ESP_OK)
        {
            s_display_dimmed = true;
        }
        else
        {
            ESP_LOGW(TAG, "Dim backlight failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_set_low_power_enabled)
    {
        set_page_low_power_enter_request();
    }
}

static void display_dim_timer_start(void)
{
    if (s_display_dim_timer == NULL)
    {
        s_display_last_activity_ms = lv_tick_get();
        display_indev_activity_hook();
        s_display_dim_timer = lv_timer_create(display_dim_timer_cb, 1000, NULL);
    }
}

static void set_page_brightness_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    int value = (int)lv_slider_get_value(slider);

    if (value < SET_BRIGHTNESS_MIN)
    {
        value = SET_BRIGHTNESS_MIN;
    }
    else if (value > SET_BRIGHTNESS_MAX)
    {
        value = SET_BRIGHTNESS_MAX;
    }

    if (s_set_ui.brightness_label)
    {
        lv_label_set_text_fmt(s_set_ui.brightness_label, "%d%%", value);
    }

    if (value == s_set_brightness_percent)
    {
        set_page_display_restore();
        return;
    }

    esp_err_t ret = lcd_backlight_set_brightness((uint8_t)value);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set backlight brightness failed: %s", esp_err_to_name(ret));
        return;
    }

    s_display_dimmed = false;
    s_set_brightness_percent = value;
}

static void set_page_auto_dim_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(e);
    s_set_auto_dim_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!s_set_auto_dim_enabled)
    {
        set_page_display_restore();
    }
}

static void set_page_screen_timeout_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target_obj(e);
    s_set_screen_timeout_ms = set_page_timeout_ms_from_index(lv_dropdown_get_selected(dropdown));
    set_page_display_restore();
    if (s_set_screen_timeout_ms == 0)
    {
        set_page_low_power_exit_request();
    }
}

static void set_page_usb_otg_storage_status_apply(bool usb_mode, bool ok, const char *text)
{
    if (s_set_ui.storage_label)
    {
        lv_label_set_text(s_set_ui.storage_label, text);
        lv_obj_set_style_text_color(s_set_ui.storage_label,
                                    lv_color_hex(ok ? (usb_mode ? UI_OK : UI_PRIMARY) : UI_ERROR),
                                    0);
    }
    if (s_set_ui.storage_bus_label)
    {
        lv_label_set_text(s_set_ui.storage_bus_label, usb_mode ? "USB MSC" : "SDMMC 4-bit");
        lv_obj_set_style_text_color(s_set_ui.storage_bus_label,
                                    lv_color_hex(usb_mode ? UI_OK : UI_MUTED),
                                    0);
    }
}

static void set_page_usb_otg_storage_status_set(bool usb_mode, bool ok, const char *text)
{
    ui_lock();
    set_page_usb_otg_storage_status_apply(usb_mode, ok, text);
    ui_unlock();
}

static void set_page_storage_info_apply(const factory_storage_info_t *info)
{
    if (info == NULL)
    {
        set_page_usb_otg_storage_status_apply(false, false, "NO SD");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_MUTED), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_MUTED), 0);
        }
        return;
    }

    if (info->usb_mode)
    {
        set_page_usb_otg_storage_status_apply(true, true, "USB MSC");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, "HOST ACCESS");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_OK), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "APP OFF");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    if (!info->app_mounted)
    {
        set_page_usb_otg_storage_status_apply(false, false, "NO SD");
        if (s_set_ui.storage_type_label)
        {
            lv_label_set_text(s_set_ui.storage_type_label, info->type[0] ? info->type : "--");
            lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_MUTED), 0);
        }
        if (s_set_ui.storage_size_label)
        {
            lv_label_set_text(s_set_ui.storage_size_label, "--");
            lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_MUTED), 0);
        }
        return;
    }

    set_page_usb_otg_storage_status_apply(false, true, "SD READY");
    if (s_set_ui.storage_type_label)
    {
        lv_label_set_text(s_set_ui.storage_type_label, info->type[0] ? info->type : "SD CARD");
        lv_obj_set_style_text_color(s_set_ui.storage_type_label, lv_color_hex(UI_TEXT), 0);
    }
    if (s_set_ui.storage_size_label)
    {
        char size_text[56] = {0};
        const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;
        const uint64_t mb = 1024ULL * 1024ULL;
        if (info->total_bytes >= gb)
        {
            uint64_t free_x10 = (info->free_bytes * 10ULL + gb / 2ULL) / gb;
            uint64_t total_x10 = (info->total_bytes * 10ULL + gb / 2ULL) / gb;
            snprintf(size_text,
                     sizeof(size_text),
                     "%" PRIu64 ".%" PRIu64 "/%" PRIu64 ".%" PRIu64 " GB",
                     free_x10 / 10ULL,
                     free_x10 % 10ULL,
                     total_x10 / 10ULL,
                     total_x10 % 10ULL);
        }
        else
        {
            uint64_t free_mb = (info->free_bytes + mb / 2ULL) / mb;
            uint64_t total_mb = (info->total_bytes + mb / 2ULL) / mb;
            snprintf(size_text, sizeof(size_text), "%" PRIu64 "/%" PRIu64 " MB", free_mb, total_mb);
        }
        lv_label_set_text(s_set_ui.storage_size_label, size_text);
        lv_obj_set_style_text_color(s_set_ui.storage_size_label, lv_color_hex(UI_SECONDARY), 0);
    }
    if (s_set_ui.storage_bus_label)
    {
        lv_label_set_text(s_set_ui.storage_bus_label, info->bus[0] ? info->bus : "SDMMC 4-bit");
        lv_obj_set_style_text_color(s_set_ui.storage_bus_label, lv_color_hex(UI_MUTED), 0);
    }
}

static void set_page_storage_refresh(bool force)
{
    uint32_t now = lv_tick_get();
    if (!force && now - s_set_storage_last_update_ms < 2000)
    {
        return;
    }
    s_set_storage_last_update_ms = now;

    factory_storage_info_t info = {0};
    if (factory_storage_info_get(&info))
    {
        s_set_usb_otg_usb_mode = info.usb_mode;
        set_page_storage_info_apply(&info);
    }
    else
    {
        set_page_storage_info_apply(NULL);
    }
}

static void set_page_usb_otg_switch_state_set(bool checked)
{
    ui_lock();
    if (s_set_ui.usb_otg_switch)
    {
        if (checked)
        {
            lv_obj_add_state(s_set_ui.usb_otg_switch, LV_STATE_CHECKED);
        }
        else
        {
            lv_obj_remove_state(s_set_ui.usb_otg_switch, LV_STATE_CHECKED);
        }
    }
    ui_unlock();
}

static void set_page_usb_otg_task(void *arg)
{
    bool usb_mode = (bool)(intptr_t)arg;
    bool ok = factory_usb_otg_msc_set(usb_mode);

    if (ok)
    {
        s_set_usb_otg_usb_mode = usb_mode;
        set_page_usb_otg_storage_status_set(usb_mode, true, usb_mode ? "USB MSC" : "SD READY");
    }
    else
    {
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode,
                                            false,
                                            usb_mode ? "USB FAIL" : "APP FAIL");
    }

    s_set_usb_otg_switch_busy = false;
    vTaskDelete(NULL);
}

static void set_page_usb_otg_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool usb_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (s_set_usb_otg_switch_busy)
    {
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode, false, "USB BUSY");
        return;
    }

    s_set_usb_otg_switch_busy = true;
    set_page_usb_otg_storage_status_set(usb_mode, true, "SWITCHING");
    if (xTaskCreate(set_page_usb_otg_task,
                    "usb_otg_msc",
                    4096,
                    (void *)(intptr_t)usb_mode,
                    5,
                    NULL) != pdPASS)
    {
        s_set_usb_otg_switch_busy = false;
        set_page_usb_otg_switch_state_set(s_set_usb_otg_usb_mode);
        set_page_usb_otg_storage_status_set(s_set_usb_otg_usb_mode, false, "TASK FAIL");
    }
}

typedef enum
{
    SET_POWER_ACTION_SHIP_MODE = 0,
    SET_POWER_ACTION_RESTART,
    SET_POWER_ACTION_LOW_POWER,
} set_power_action_t;

static void set_page_power_status_set(const char *text, uint32_t color)
{
    ui_lock();
    if (s_set_ui.power_status_label)
    {
        lv_label_set_text(s_set_ui.power_status_label, text);
        lv_obj_set_style_text_color(s_set_ui.power_status_label, lv_color_hex(color), 0);
    }
    ui_unlock();
}

typedef enum
{
    SET_LOW_POWER_TASK_ENTER = 0,
    SET_LOW_POWER_TASK_EXIT,
} set_low_power_task_action_t;

static void set_page_low_power_task(void *arg)
{
    set_low_power_task_action_t action = (set_low_power_task_action_t)(intptr_t)arg;
    bool ok = false;

    if (action == SET_LOW_POWER_TASK_ENTER)
    {
        ok = factory_power_enter_low_power();
        s_set_low_power_active = ok;
        if (ok)
        {
            s_display_dimmed = true;
        }
        if (s_set_low_power_enabled)
        {
            set_page_power_status_set(ok ? "LOW IDLE" : "LOW PWR ERR", ok ? UI_OK : UI_ERROR);
        }
    }
    else
    {
        ok = factory_power_exit_low_power();
        s_set_low_power_active = false;
        if (s_set_low_power_enabled)
        {
            set_page_power_status_set(ok ? "ACTIVE" : "RESTORE ERR", ok ? UI_PRIMARY : UI_ERROR);
        }
        else
        {
            set_page_power_status_set(ok ? "FACTORY" : "RESTORE ERR", ok ? UI_PRIMARY : UI_ERROR);
        }
    }

    s_set_low_power_transition = false;
    vTaskDelete(NULL);
}

static void set_page_low_power_task_start(set_low_power_task_action_t action)
{
    if (s_set_low_power_transition)
    {
        return;
    }

    s_set_low_power_transition = true;
    if (xTaskCreate(set_page_low_power_task,
                    "set_low_power",
                    4096,
                    (void *)(intptr_t)action,
                    5,
                    NULL) != pdPASS)
    {
        s_set_low_power_transition = false;
        set_page_power_status_set("TASK FAIL", UI_ERROR);
    }
}

static void set_page_low_power_enter_request(void)
{
    if (!s_set_low_power_enabled || s_set_low_power_active || s_set_low_power_transition)
    {
        return;
    }

    set_page_power_status_set("LOW IDLE", UI_OK);
    set_page_low_power_task_start(SET_LOW_POWER_TASK_ENTER);
}

static void set_page_low_power_exit_request(void)
{
    if (!s_set_low_power_active || s_set_low_power_transition)
    {
        return;
    }

    set_page_power_status_set("ACTIVE", UI_PRIMARY);
    set_page_low_power_task_start(SET_LOW_POWER_TASK_EXIT);
}

static void set_page_power_action_task(void *arg)
{
    set_power_action_t action = (set_power_action_t)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(350));

    switch (action)
    {
    case SET_POWER_ACTION_SHIP_MODE:
    {
        bool ok = factory_power_enter_ship_mode();
        set_page_power_status_set(ok ? "SHIP OK" : "SHIP FAIL", ok ? UI_OK : UI_ERROR);
        s_set_power_action_busy = false;
        break;
    }
    case SET_POWER_ACTION_RESTART:
        factory_power_restart();
        break;
    case SET_POWER_ACTION_LOW_POWER:
        s_set_power_action_busy = false;
        break;
    default:
        s_set_power_action_busy = false;
        break;
    }

    vTaskDelete(NULL);
}

static void set_page_power_action_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    set_power_action_t action = (set_power_action_t)(intptr_t)lv_event_get_user_data(e);
    const char *text = "POWER";
    uint32_t color = UI_PRIMARY;

    if (s_set_power_action_busy)
    {
        set_page_power_status_set("BUSY", UI_WARN);
        return;
    }

    switch (action)
    {
    case SET_POWER_ACTION_SHIP_MODE:
        text = "SHIP MODE";
        color = UI_WARN;
        break;
    case SET_POWER_ACTION_RESTART:
        text = "RESTARTING";
        color = UI_PRIMARY;
        break;
    case SET_POWER_ACTION_LOW_POWER:
        s_set_low_power_enabled = !s_set_low_power_enabled;
        s_display_last_activity_ms = lv_tick_get();
        display_dim_timer_start();
        if (s_set_low_power_enabled)
        {
            set_page_power_status_set("LOW AUTO", UI_OK);
        }
        else
        {
            set_page_power_status_set("LOW OFF", UI_MUTED);
            set_page_display_restore();
            set_page_low_power_exit_request();
        }
        return;
    default:
        return;
    }

    s_set_power_action_busy = true;
    set_page_power_status_set(text, color);
    if (xTaskCreate(set_page_power_action_task,
                    "set_power",
                    4096,
                    (void *)(intptr_t)action,
                    5,
                    NULL) != pdPASS)
    {
        s_set_power_action_busy = false;
        set_page_power_status_set("TASK FAIL", UI_ERROR);
    }
}

static lv_obj_t *set_page_panel_create(lv_obj_t *parent, int32_t w, int32_t h, const char *title, uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(panel,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START);

    lv_obj_t *head = ui_flex_container_create(panel,
                                              LV_PCT(100),
                                              22,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);

    lv_obj_t *mark = lv_obj_create(head);
    lv_obj_set_size(mark, 4, 18);
    lv_obj_set_style_bg_color(mark, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_radius(mark, 2, 0);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_label_create(head, title, &lv_font_montserrat_16, color);
    lv_obj_set_width(label, LV_PCT(80));

    return panel;
}

static void set_page_slider_style(lv_obj_t *slider, uint32_t color)
{
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(color), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
}

static void set_page_switch_style(lv_obj_t *sw, uint32_t color)
{
    lv_obj_set_size(sw, 50, 22);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_PANEL_HL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 11, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_LINE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 11, LV_PART_KNOB);
}

static lv_obj_t *set_page_switch_row_create(lv_obj_t *parent, const char *icon, const char *name,
                                            const char *desc, uint32_t color, bool checked)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             48,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_column(row, 10, 0);

    lv_obj_t *icon_label = ui_label_create(row, icon, &lv_font_montserrat_16, color);
    lv_obj_set_width(icon_label, 28);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *text_cont = ui_flex_container_create(row,
                                                   145,
                                                   40,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_cont, 2, 0);

    lv_obj_t *name_label = ui_label_create(text_cont, name, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = ui_label_create(text_cont, desc, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(row);

    lv_obj_t *sw = lv_switch_create(row);
    set_page_switch_style(sw, color);
    if (checked)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *set_page_action_create(lv_obj_t *parent, const char *icon, const char *name,
                                        const char *desc, uint32_t color, bool danger)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(btn, lv_color_hex(danger ? UI_ERROR : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, danger ? LV_OPA_80 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, danger ? 0 : 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    lv_obj_set_style_pad_column(btn, 10, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(btn,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon_label = ui_label_create(btn, icon, &lv_font_montserrat_16, danger ? UI_BG : color);
    lv_obj_set_width(icon_label, 28);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *text_cont = ui_flex_container_create(btn,
                                                   190,
                                                   40,
                                                   LV_FLEX_FLOW_COLUMN,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_cont, 2, 0);

    lv_obj_t *name_label = ui_label_create(text_cont, name, &lv_font_montserrat_14, danger ? UI_BG : UI_TEXT);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *desc_label = ui_label_create(text_cont, desc, &lv_font_montserrat_14, danger ? UI_PANEL : UI_MUTED);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(btn);

    lv_obj_t *arrow = ui_label_create(btn, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, danger ? UI_BG : UI_MUTED);
    lv_obj_set_width(arrow, 18);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);

    return btn;
}

static lv_obj_t *set_page_info_line_create(lv_obj_t *parent, const char *name, const char *value, uint32_t color)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             20,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(row, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 100);

    ui_flex_spacer_create(row);

    lv_obj_t *value_label = ui_label_create(row, value, &lv_font_montserrat_14, color);
    lv_obj_set_width(value_label, 200);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
    return value_label;
}

static void set_page_dropdown_style(lv_obj_t *dropdown, uint32_t color)
{
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dropdown, 1, 0);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(color), 0);
    lv_obj_set_style_radius(dropdown, 9, 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_pad_left(dropdown, 12, 0);
    lv_obj_set_style_pad_right(dropdown, 10, 0);
}

static void set_page_wifi_list_clear(void)
{
    if (s_set_ui.wifi_list_cont)
    {
        lv_obj_clean(s_set_ui.wifi_list_cont);
    }
}

static void set_page_wifi_password_dialog_show(const wifi_scan_ap_info_t *ap);

static void set_page_wifi_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const wifi_scan_ap_info_t *ap = (const wifi_scan_ap_info_t *)lv_event_get_user_data(e);
    if (ap == NULL)
    {
        return;
    }
    set_page_wifi_password_dialog_show(ap);
}

static void set_page_wifi_row_create(lv_obj_t *parent, const char *ssid, int rssi, const char *type, bool header, const wifi_scan_ap_info_t *ap)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             header ? 30 : 38,
                                             LV_FLEX_FLOW_ROW,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, lv_color_hex(header ? UI_BG : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(row, header ? LV_OPA_TRANSP : LV_OPA_70, 0);
    lv_obj_set_style_radius(row, 7, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    if (!header)
    {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, set_page_wifi_row_event_cb, LV_EVENT_CLICKED, (void *)ap);
    }

    lv_obj_t *ssid_label = ui_label_create(row, ssid, header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_TEXT);
    lv_obj_set_width(ssid_label, 300);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *rssi_label = ui_label_create(row, "", header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_SECONDARY);
    lv_obj_set_width(rssi_label, 80);
    if (header)
    {
        lv_label_set_text(rssi_label, rssi == 0 ? "RSSI" : "");
    }
    else
    {
        lv_label_set_text_fmt(rssi_label, "%d dBm", rssi);
    }
    lv_obj_set_style_text_align(rssi_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *type_label = ui_label_create(row, type, header ? &lv_font_montserrat_12 : &lv_font_montserrat_14, header ? UI_MUTED : UI_OK);
    lv_obj_set_width(type_label, 120);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_MODE_DOTS);
}

static void set_page_wifi_scan_ui_update(void)
{
    if (s_set_ui.wifi_dialog == NULL || s_set_ui.wifi_list_cont == NULL)
    {
        return;
    }

    if (s_set_ui.wifi_status_label)
    {
        lv_label_set_text(s_set_ui.wifi_status_label, s_set_wifi_scan_status);
        lv_obj_set_style_text_color(s_set_ui.wifi_status_label,
                                    lv_color_hex(s_set_wifi_scan_busy ? UI_PRIMARY : (s_set_wifi_ap_count > 0 ? UI_OK : UI_WARN)),
                                    0);
    }

    set_page_wifi_list_clear();
    set_page_wifi_row_create(s_set_ui.wifi_list_cont, "SSID", 0, "TYPE", true, NULL);
    if (s_set_wifi_scan_busy)
    {
        lv_obj_t *scan_label = ui_label_create(s_set_ui.wifi_list_cont, "Scanning WiFi networks...", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(scan_label, LV_PCT(100));
        lv_obj_set_style_text_align(scan_label, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    if (s_set_wifi_ap_count <= 0)
    {
        lv_obj_t *empty_label = ui_label_create(s_set_ui.wifi_list_cont, "No AP found", &lv_font_montserrat_16, UI_MUTED);
        lv_obj_set_width(empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_set_wifi_ap_count; i++)
    {
        set_page_wifi_row_create(s_set_ui.wifi_list_cont,
                                 s_set_wifi_aps[i].ssid,
                                 s_set_wifi_aps[i].rssi,
                                 s_set_wifi_aps[i].type,
                                 false,
                                 &s_set_wifi_aps[i]);
    }
}

static void set_page_wifi_scan_task(void *arg)
{
    (void)arg;
    int ap_count = 0;
    char err[32] = {0};
    bool ok = wifi_remote_scan(s_set_wifi_aps, SET_WIFI_SCAN_MAX_APS, &ap_count, err, sizeof(err));

    if (ok)
    {
        s_set_wifi_ap_count = ap_count;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "Scan done: %d AP", ap_count);
    }
    else
    {
        s_set_wifi_ap_count = 0;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "Scan failed: %.32s", err[0] ? err : "unknown");
    }
    s_set_wifi_scan_busy = false;
    s_set_wifi_scan_task_handle = NULL;

    ui_lock();
    Page *current = ui_page_get_current();
    if (current && current->id == PAGE_SET)
    {
        set_page_wifi_scan_ui_update();
    }
    ui_unlock();

    vTaskDelete(NULL);
}

static void set_page_wifi_scan_start(void)
{
    if (s_set_wifi_scan_busy)
    {
        set_page_wifi_scan_ui_update();
        return;
    }

    s_set_wifi_ap_count = 0;
    s_set_wifi_scan_busy = true;
    snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "%s", "Scanning...");
    set_page_wifi_scan_ui_update();

    BaseType_t ok = xTaskCreate(set_page_wifi_scan_task,
                                "wifi_scan_ui",
                                8192,
                                NULL,
                                4,
                                &s_set_wifi_scan_task_handle);
    if (ok != pdPASS)
    {
        s_set_wifi_scan_busy = false;
        s_set_wifi_scan_task_handle = NULL;
        snprintf(s_set_wifi_scan_status, sizeof(s_set_wifi_scan_status), "%s", "Scan task failed");
        set_page_wifi_scan_ui_update();
    }
}

static void set_page_wifi_password_dialog_hide(void)
{
    if (s_set_ui.wifi_password_dialog)
    {
        lv_obj_add_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_set_ui.wifi_keyboard)
    {
        lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, NULL);
    }
}

static void set_page_wifi_password_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        set_page_wifi_password_dialog_hide();
    }
}

static void set_page_wifi_connect_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    const char *password = "";
    if (s_set_ui.wifi_password_ta)
    {
        password = lv_textarea_get_text(s_set_ui.wifi_password_ta);
    }

    if (s_set_wifi_selected_auth != WIFI_AUTH_OPEN && strlen(password) == 0)
    {
        if (s_set_ui.wifi_status_label)
        {
            lv_label_set_text(s_set_ui.wifi_status_label, "Password required");
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    char err[32] = {0};
    bool ok = wifi_remote_connect(s_set_wifi_selected_ssid,
                                  password,
                                  (int)s_set_wifi_selected_auth,
                                  err,
                                  sizeof(err));

    if (s_set_ui.wifi_status_label)
    {
        if (ok)
        {
            lv_label_set_text_fmt(s_set_ui.wifi_status_label, "Connecting: %.24s", s_set_wifi_selected_ssid);
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_PRIMARY), 0);
        }
        else
        {
            lv_label_set_text_fmt(s_set_ui.wifi_status_label, "Connect failed: %.24s", err[0] ? err : "unknown");
            lv_obj_set_style_text_color(s_set_ui.wifi_status_label, lv_color_hex(UI_ERROR), 0);
        }
    }

    if (ok)
    {
        snprintf(s_set_wifi_last_connected_ssid, sizeof(s_set_wifi_last_connected_ssid), "%.32s", s_set_wifi_selected_ssid);
        set_page_wifi_password_dialog_hide();
    }
}

static void set_page_wifi_password_dialog_create(void)
{
    if (s_set_ui.wifi_password_dialog || s_set_ui.root == NULL)
    {
        return;
    }

    s_set_ui.wifi_password_dialog = lv_obj_create(s_set_ui.root);
    lv_obj_set_size(s_set_ui.wifi_password_dialog, 640, 360);
    lv_obj_align(s_set_ui.wifi_password_dialog, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_set_ui.wifi_password_dialog, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_password_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_password_dialog, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_password_dialog, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_set_ui.wifi_password_dialog, 12, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_password_dialog, 12, 0);
    lv_obj_remove_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(s_set_ui.wifi_password_dialog,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(s_set_ui.wifi_password_dialog,
                                              LV_PCT(100),
                                              34,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_t *icon = ui_label_create(head, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_PRIMARY);
    lv_obj_set_width(icon, 28);
    s_set_ui.wifi_ssid_list_cont_label = ui_label_create(head, "SSID", &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_width(s_set_ui.wifi_ssid_list_cont_label, 430);
    lv_label_set_long_mode(s_set_ui.wifi_ssid_list_cont_label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 34, 30);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, set_page_wifi_password_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(close_label);

    lv_obj_t *input_row = ui_flex_container_create(s_set_ui.wifi_password_dialog,
                                                   LV_PCT(100),
                                                   48,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 10, 0);
    s_set_ui.wifi_password_ta = lv_textarea_create(input_row);
    lv_obj_set_size(s_set_ui.wifi_password_ta, 470, 42);
    lv_textarea_set_one_line(s_set_ui.wifi_password_ta, true);
    lv_textarea_set_password_mode(s_set_ui.wifi_password_ta, true);
    lv_textarea_set_placeholder_text(s_set_ui.wifi_password_ta, "WiFi password");
    lv_obj_set_style_bg_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_password_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_password_ta, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_set_ui.wifi_password_ta, 8, 0);
    lv_obj_set_style_text_color(s_set_ui.wifi_password_ta, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_set_ui.wifi_password_ta, &lv_font_montserrat_14, 0);

    lv_obj_t *connect_btn = lv_button_create(input_row);
    lv_obj_set_size(connect_btn, 110, 42);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(connect_btn, 0, 0);
    lv_obj_set_style_radius(connect_btn, 8, 0);
    lv_obj_set_style_shadow_width(connect_btn, 0, 0);
    lv_obj_add_event_cb(connect_btn, set_page_wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = ui_label_create(connect_btn, "CONNECT", &lv_font_montserrat_14, UI_BG);
    lv_obj_center(connect_label);

    s_set_ui.wifi_keyboard = lv_keyboard_create(s_set_ui.wifi_password_dialog);
    lv_obj_set_size(s_set_ui.wifi_keyboard, LV_PCT(100), 240);
    lv_keyboard_set_mode(s_set_ui.wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, s_set_ui.wifi_password_ta);
    lv_obj_set_style_bg_color(s_set_ui.wifi_keyboard, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_keyboard, LV_OPA_COVER, 0);

    lv_obj_add_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void set_page_wifi_password_dialog_show(const wifi_scan_ap_info_t *ap)
{
    if (ap == NULL)
    {
        return;
    }

    snprintf(s_set_wifi_selected_ssid, sizeof(s_set_wifi_selected_ssid), "%.32s", ap->ssid);
    s_set_wifi_selected_auth = ap->authmode;
    set_page_wifi_password_dialog_create();

    if (s_set_ui.wifi_ssid_list_cont_label)
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_list_cont_label, "%.28s  %s", s_set_wifi_selected_ssid, ap->type);
    }
    if (s_set_ui.wifi_password_ta)
    {
        lv_textarea_set_text(s_set_ui.wifi_password_ta, "");
        lv_textarea_set_password_mode(s_set_ui.wifi_password_ta, ap->authmode != WIFI_AUTH_OPEN);
        lv_textarea_set_placeholder_text(s_set_ui.wifi_password_ta,
                                         ap->authmode == WIFI_AUTH_OPEN ? "Open network, password optional" : "WiFi password");
    }
    if (s_set_ui.wifi_keyboard && s_set_ui.wifi_password_ta)
    {
        lv_keyboard_set_textarea(s_set_ui.wifi_keyboard, s_set_ui.wifi_password_ta);
    }
    if (s_set_ui.wifi_password_dialog)
    {
        lv_obj_clear_flag(s_set_ui.wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_set_ui.wifi_password_dialog);
    }
}

static void set_page_wifi_dialog_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (s_set_ui.wifi_dialog)
    {
        lv_obj_add_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    set_page_wifi_password_dialog_hide();
}

static void set_page_wifi_dialog_create(void)
{
    if (s_set_ui.wifi_dialog || s_set_ui.root == NULL)
    {
        return;
    }

    s_set_ui.wifi_dialog = lv_obj_create(s_set_ui.root);
    lv_obj_set_size(s_set_ui.wifi_dialog, 640, 500);
    lv_obj_align(s_set_ui.wifi_dialog, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_style_bg_color(s_set_ui.wifi_dialog, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_dialog, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_dialog, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_set_ui.wifi_dialog, 12, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_dialog, 14, 0);
    lv_obj_remove_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(s_set_ui.wifi_dialog,
                    LV_FLEX_FLOW_COLUMN,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(s_set_ui.wifi_dialog,
                                              LV_PCT(100),
                                              38,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_t *icon = ui_label_create(head, LV_SYMBOL_WIFI, &lv_font_montserrat_20, UI_PRIMARY);
    lv_obj_set_width(icon, 28);
    lv_obj_t *title = ui_label_create(head, "WIFI REMOTE", &lv_font_montserrat_20, UI_TEXT);
    lv_obj_set_width(title, 210);
    s_set_ui.wifi_status_label = ui_label_create(head, s_set_wifi_scan_status, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_set_ui.wifi_status_label, 270);
    lv_obj_set_style_text_align(s_set_ui.wifi_status_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *close_btn = lv_button_create(head);
    lv_obj_set_size(close_btn, 34, 30);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, set_page_wifi_dialog_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = ui_label_create(close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, UI_TEXT);
    lv_obj_center(close_label);

    s_set_ui.wifi_list_cont = ui_flex_container_create(s_set_ui.wifi_dialog,
                                                       LV_PCT(100),
                                                       420,
                                                       LV_FLEX_FLOW_COLUMN,
                                                       LV_FLEX_ALIGN_START,
                                                       LV_FLEX_ALIGN_CENTER,
                                                       LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_set_ui.wifi_list_cont, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_set_ui.wifi_list_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_set_ui.wifi_list_cont, 1, 0);
    lv_obj_set_style_border_color(s_set_ui.wifi_list_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(s_set_ui.wifi_list_cont, 8, 0);
    lv_obj_set_style_pad_all(s_set_ui.wifi_list_cont, 10, 0);
    lv_obj_set_style_pad_row(s_set_ui.wifi_list_cont, 6, 0);
    lv_obj_set_scrollbar_mode(s_set_ui.wifi_list_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_set_ui.wifi_list_cont, LV_OBJ_FLAG_SCROLLABLE);

    set_page_wifi_scan_ui_update();
}

static void set_page_wifi_remote_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    set_page_wifi_dialog_create();
    if (s_set_ui.wifi_dialog)
    {
        lv_obj_clear_flag(s_set_ui.wifi_dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_set_ui.wifi_dialog);
    }
    set_page_wifi_scan_start();
}

static void set_page_wifi_refresh(void)
{
    if (s_set_ui.wifi_ssid_label == NULL || s_set_ui.wifi_ip_label == NULL)
    {
        return;
    }

    status_info_t status = {0};
    if (!status_bar_info_get(&status) || !status.wifi_connected)
    {
        lv_label_set_text(s_set_ui.wifi_ssid_label, "----");
        lv_label_set_text(s_set_ui.wifi_ip_label, "IP --");
        return;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        const char *ssid = ap.ssid[0] ? (const char *)ap.ssid : s_set_wifi_last_connected_ssid;
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_label, "%s", ssid && ssid[0] ? ssid : "WiFi connected");
    }
    else
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ssid_label, "%s", s_set_wifi_last_connected_ssid[0] ? s_set_wifi_last_connected_ssid : "WiFi connected");
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        lv_label_set_text_fmt(s_set_ui.wifi_ip_label, IPSTR, IP2STR(&ip_info.ip));
    }
    else
    {
        lv_label_set_text(s_set_ui.wifi_ip_label, "IP --");
    }
}

static void set_page_timer(Page *page)
{
    (void)page;
    set_page_wifi_refresh();
    set_page_storage_refresh(false);
}

static void set_page_create(Page *page)
{
    s_set_ui.root = page->root;
    lv_obj_set_style_bg_color(s_set_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_set_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_set_ui.root, 0, 0);
    lv_obj_add_flag(s_set_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_set_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    s_set_ui.main_cont = lv_obj_create(s_set_ui.root);
    ui_main_cont_style_init(s_set_ui.main_cont);
    lv_obj_set_style_pad_row(s_set_ui.main_cont, 12, 0);

    lv_obj_t *title_row = ui_flex_container_create(s_set_ui.main_cont,
                                                   680,
                                                   54,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, 14, 0);

    lv_obj_t *title_icon = ui_label_create(title_row, LV_SYMBOL_SETTINGS, &lv_font_montserrat_24, UI_PRIMARY);
    lv_obj_set_width(title_icon, 32);
    lv_obj_set_style_text_align(title_icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *title_text = ui_flex_container_create(title_row,
                                                    250,
                                                    44,
                                                    LV_FLEX_FLOW_COLUMN,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_START,
                                                    LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(title_text, 2, 0);

    lv_obj_t *title = ui_label_create(title_text, "SETTINGS", &lv_font_montserrat_28, UI_TEXT);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *subtitle = ui_label_create(title_text, "DEVICE CONTROL PANEL", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(subtitle, LV_PCT(100));

    ui_flex_spacer_create(title_row);

    lv_obj_t *mode_chip = lv_obj_create(title_row);
    lv_obj_set_size(mode_chip, 118, 30);
    lv_obj_set_style_bg_color(mode_chip, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(mode_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mode_chip, 1, 0);
    lv_obj_set_style_border_color(mode_chip, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_radius(mode_chip, 15, 0);
    lv_obj_set_style_pad_all(mode_chip, 0, 0);
    lv_obj_remove_flag(mode_chip, LV_OBJ_FLAG_SCROLLABLE);

    const char *power_state_text = "FACTORY";
    uint32_t power_state_color = UI_PRIMARY;
    if (s_set_low_power_enabled)
    {
        power_state_text = s_set_low_power_active ? "LOW IDLE" : "LOW AUTO";
        power_state_color = s_set_low_power_active ? UI_OK : UI_WARN;
    }
    s_set_ui.power_status_label = ui_label_create(mode_chip, power_state_text, &lv_font_montserrat_14, power_state_color);
    lv_obj_center(s_set_ui.power_status_label);

    lv_obj_t *top_row = ui_flex_container_create(s_set_ui.main_cont,
                                                 680,
                                                 220,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_row, 14, 0);

    lv_obj_t *display_panel = set_page_panel_create(top_row, 333, 220, "DISPLAY", UI_PRIMARY);

    lv_obj_t *brightness_head = ui_flex_container_create(display_panel,
                                                         LV_PCT(100),
                                                         30,
                                                         LV_FLEX_FLOW_ROW,
                                                         LV_FLEX_ALIGN_START,
                                                         LV_FLEX_ALIGN_CENTER,
                                                         LV_FLEX_ALIGN_CENTER);
    lv_obj_t *brightness_icon = ui_label_create(brightness_head, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_16, UI_PRIMARY);
    lv_obj_set_width(brightness_icon, 28);
    lv_obj_set_style_text_align(brightness_icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *brightness_name = ui_label_create(brightness_head, "Brightness", &lv_font_montserrat_16, UI_TEXT);
    lv_obj_set_width(brightness_name, 132);

    ui_flex_spacer_create(brightness_head);

    s_set_ui.brightness_label = ui_label_create(brightness_head, "", &lv_font_montserrat_20, UI_PRIMARY);
    lv_label_set_text_fmt(s_set_ui.brightness_label, "%d%%", s_set_brightness_percent);
    lv_obj_set_width(s_set_ui.brightness_label, 62);
    lv_obj_set_style_text_align(s_set_ui.brightness_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_set_ui.brightness_slider = lv_slider_create(display_panel);
    lv_obj_set_size(s_set_ui.brightness_slider, LV_PCT(100), 14);
    lv_slider_set_range(s_set_ui.brightness_slider, SET_BRIGHTNESS_MIN, SET_BRIGHTNESS_MAX);
    lv_slider_set_value(s_set_ui.brightness_slider, s_set_brightness_percent, LV_ANIM_OFF);
    set_page_slider_style(s_set_ui.brightness_slider, UI_PRIMARY);
    lv_obj_add_event_cb(s_set_ui.brightness_slider, set_page_brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_set_ui.auto_dim_switch = set_page_switch_row_create(display_panel,
                                                          LV_SYMBOL_EYE_OPEN,
                                                          "Auto Dim",
                                                          "Idle screen saver",
                                                          UI_WARN,
                                                          s_set_auto_dim_enabled);
    lv_obj_add_event_cb(s_set_ui.auto_dim_switch, set_page_auto_dim_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *timeout_row = ui_flex_container_create(display_panel,
                                                     LV_PCT(100),
                                                     46,
                                                     LV_FLEX_FLOW_ROW,
                                                     LV_FLEX_ALIGN_START,
                                                     LV_FLEX_ALIGN_CENTER,
                                                     LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(timeout_row, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(timeout_row, LV_OPA_80, 0);
    lv_obj_set_style_radius(timeout_row, 10, 0);
    lv_obj_set_style_pad_left(timeout_row, 10, 0);
    lv_obj_set_style_pad_right(timeout_row, 10, 0);
    lv_obj_set_style_pad_column(timeout_row, 10, 0);

    lv_obj_t *timeout_icon = ui_label_create(timeout_row, LV_SYMBOL_EYE_CLOSE, &lv_font_montserrat_16, UI_SECONDARY);
    lv_obj_set_width(timeout_icon, 28);
    lv_obj_set_style_text_align(timeout_icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *timeout_text = ui_flex_container_create(timeout_row,
                                                      118,
                                                      38,
                                                      LV_FLEX_FLOW_COLUMN,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(timeout_text, 2, 0);
    lv_obj_t *timeout_name = ui_label_create(timeout_text, "Screen Timeout", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(timeout_name, LV_PCT(100));
    lv_label_set_long_mode(timeout_name, LV_LABEL_LONG_CLIP);
    lv_obj_t *timeout_desc = ui_label_create(timeout_text, "Backlight idle", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(timeout_desc, LV_PCT(100));
    lv_label_set_long_mode(timeout_desc, LV_LABEL_LONG_CLIP);

    ui_flex_spacer_create(timeout_row);
    s_set_ui.screen_timeout_dropdown = lv_dropdown_create(timeout_row);
    lv_obj_set_size(s_set_ui.screen_timeout_dropdown, 108, 34);
    lv_dropdown_set_options(s_set_ui.screen_timeout_dropdown, "OFF\n30 sec\n1 min\n5 min\n10 min");
    lv_dropdown_set_selected(s_set_ui.screen_timeout_dropdown, set_page_timeout_index_from_ms(s_set_screen_timeout_ms));
    set_page_dropdown_style(s_set_ui.screen_timeout_dropdown, UI_SECONDARY);
    lv_obj_add_event_cb(s_set_ui.screen_timeout_dropdown, set_page_screen_timeout_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *power_panel = set_page_panel_create(top_row, 333, 220, "POWER", UI_WARN);
    lv_obj_t *ship_mode_btn = set_page_action_create(power_panel, LV_SYMBOL_POWER, "Ship Mode", "Battery shelf mode", UI_WARN, false);
    lv_obj_add_event_cb(ship_mode_btn,
                        set_page_power_action_event_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)SET_POWER_ACTION_SHIP_MODE);
    lv_obj_t *restart_btn = set_page_action_create(power_panel, LV_SYMBOL_REFRESH, "Restart", "Soft reboot request", UI_PRIMARY, false);
    lv_obj_add_event_cb(restart_btn,
                        set_page_power_action_event_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)SET_POWER_ACTION_RESTART);
    lv_obj_t *low_power_btn = set_page_action_create(power_panel, LV_SYMBOL_CHARGE, "Low Power", "Auto idle profile", UI_OK, false);
    lv_obj_add_event_cb(low_power_btn,
                        set_page_power_action_event_cb,
                        LV_EVENT_CLICKED,
                        (void *)(intptr_t)SET_POWER_ACTION_LOW_POWER);

    lv_obj_t *mid_row = ui_flex_container_create(s_set_ui.main_cont,
                                                 680,
                                                 220,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_START,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid_row, 14, 0);

    lv_obj_t *connect_panel = set_page_panel_create(mid_row, 333, 220, "CONNECT", UI_SECONDARY);
    lv_obj_t *wifi_remote_btn = set_page_action_create(connect_panel, LV_SYMBOL_WIFI, "WiFi Remote", "ESP32-C5 hosted", UI_PRIMARY, false);
    lv_obj_add_event_cb(wifi_remote_btn, set_page_wifi_remote_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_info_cont = lv_obj_create(connect_panel);
    lv_obj_set_size(wifi_info_cont, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(wifi_info_cont, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(wifi_info_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_info_cont, 1, 0);
    lv_obj_set_style_border_color(wifi_info_cont, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_radius(wifi_info_cont, 10, 0);
    lv_obj_set_style_pad_left(wifi_info_cont, 10, 0);
    lv_obj_set_style_pad_right(wifi_info_cont, 10, 0);
    lv_obj_set_style_pad_top(wifi_info_cont, 8, 0);
    lv_obj_set_style_pad_bottom(wifi_info_cont, 8, 0);
    lv_obj_set_style_pad_column(wifi_info_cont, 10, 0);
    lv_obj_remove_flag(wifi_info_cont, LV_OBJ_FLAG_SCROLLABLE);
    ui_obj_set_flex(wifi_info_cont,
                    LV_FLEX_FLOW_ROW,
                    LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER,
                    LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wifi_icon = ui_label_create(wifi_info_cont, LV_SYMBOL_PLUS, &lv_font_montserrat_16, UI_OK);
    lv_obj_center(wifi_icon);
    lv_obj_set_width(wifi_icon, 28);
    lv_obj_set_style_text_align(wifi_icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *wifi_text_cont = ui_flex_container_create(wifi_info_cont,
                                                        240,
                                                        42,
                                                        LV_FLEX_FLOW_COLUMN,
                                                        LV_FLEX_ALIGN_CENTER,
                                                        LV_FLEX_ALIGN_START,
                                                        LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(wifi_text_cont, 2, 0);

    lv_obj_t *ssid_row = ui_flex_container_create(wifi_text_cont,
                                                  LV_PCT(100),
                                                  20,
                                                  LV_FLEX_FLOW_ROW,
                                                  LV_FLEX_ALIGN_START,
                                                  LV_FLEX_ALIGN_CENTER,
                                                  LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ssid_name = ui_label_create(ssid_row, "SSID", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(ssid_name, 44);
    s_set_ui.wifi_ssid_label = ui_label_create(ssid_row, "----", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(s_set_ui.wifi_ssid_label, 190);
    lv_label_set_long_mode(s_set_ui.wifi_ssid_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    lv_obj_t *ip_row = ui_flex_container_create(wifi_text_cont,
                                                LV_PCT(100),
                                                20,
                                                LV_FLEX_FLOW_ROW,
                                                LV_FLEX_ALIGN_START,
                                                LV_FLEX_ALIGN_CENTER,
                                                LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ip_name = ui_label_create(ip_row, "IP", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(ip_name, 44);
    s_set_ui.wifi_ip_label = ui_label_create(ip_row, "IP --", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(s_set_ui.wifi_ip_label, 190);
    lv_obj_set_style_text_font(s_set_ui.wifi_ip_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_set_ui.wifi_ip_label, LV_LABEL_LONG_MODE_CLIP);
    set_page_wifi_refresh();

    s_set_usb_otg_usb_mode = factory_usb_otg_msc_is_usb_mode();
    s_set_ui.usb_otg_switch = set_page_switch_row_create(connect_panel,
                                                         LV_SYMBOL_USB,
                                                         "USB OTG",
                                                         "MSC device mode",
                                                         UI_OK,
                                                         s_set_usb_otg_usb_mode);
    lv_obj_add_event_cb(s_set_ui.usb_otg_switch, set_page_usb_otg_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *storage_panel = set_page_panel_create(mid_row, 333, 220, "STORAGE", UI_OK);
    lv_obj_set_style_pad_row(storage_panel, 8, 0);
    s_set_ui.storage_label = set_page_info_line_create(storage_panel, "Status", "SD READY", UI_OK);
    s_set_ui.storage_type_label = set_page_info_line_create(storage_panel, "Type", "SDHC / SDXC", UI_TEXT);
    s_set_ui.storage_size_label = set_page_info_line_create(storage_panel, "Size", "-- GB", UI_SECONDARY);
    s_set_ui.storage_bus_label = set_page_info_line_create(storage_panel, "Bus", "SDMMC 4-bit", UI_MUTED);
    set_page_storage_refresh(true);

    lv_obj_t *info_panel = set_page_panel_create(s_set_ui.main_cont, 680, 120, "DEVICE INFO", UI_MUTED);
    lv_obj_set_style_pad_row(info_panel, 5, 0);
    s_set_ui.timesync_label = set_page_info_line_create(info_panel, "Time Sync", "UTC+8", UI_TEXT);
    s_set_ui.firmware_label = set_page_info_line_create(info_panel, "Firmware", "T-Panel-P4-Factory-V1.0.0", UI_PRIMARY);
    set_page_info_line_create(info_panel, "Board", "LILYGO T-Panel-P4", UI_SECONDARY);
}

static void set_page_leave(Page *page)
{
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void set_page_destroy(Page *page)
{
    (void)page;
    memset(&s_set_ui, 0, sizeof(s_set_ui));
}

/*********create Page************/
static void app_page_back_gesture(Page *page, GestureDirection direction)
{
    ESP_LOGI(TAG, "app_page_back_gesture page:%d direction:%d", page->id, direction);
    if (direction == GESTURE_RIGHT)
    {
        ui_page_switch_async(PAGE_HOME);
    }
}

void create_page_ui(void)
{
    Page start_page = {
        .id = PAGE_START,
        .name = "start",
        .on_create = start_page_create,
        .on_destroy = start_page_destroy,
    };
    ui_page_register(&start_page);

    Page home_page = {
        .id = PAGE_HOME,
        .name = "home",
        .on_create = home_page_create,
        .on_enter = home_page_enter,
        .page_timer = home_page_timer,
        .timer_interval_ms = STATUS_TIMER_MS,
    };
    ui_page_register(&home_page);

    Page music_page = {
        .id = PAGE_MUSIC,
        .name = "music",
        .on_create = music_page_create,
        .on_enter = music_page_enter,
        .on_leave = music_page_leave,
        .on_destroy = music_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = music_page_timer,
        .timer_interval_ms = 50,
    };
    ui_page_register(&music_page);

    Page camera_page = {
        .id = PAGE_CAMERA,
        .name = "camera",
        .on_create = camera_page_create,
        .on_enter = camera_page_enter,
        .on_leave = camera_page_leave,
        .on_destroy = camera_page_destroy,
        .on_gesture = camera_page_gesture,
    };
    ui_page_register(&camera_page);

    Page lora_page = {
        .id = PAGE_LORA,
        .name = "lora",
        .on_create = lora_page_create,
        .on_enter = lora_page_enter,
        .on_leave = lora_page_leave,
        .on_destroy = lora_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = lora_page_timer,
        .timer_interval_ms = LORA_RX_POLL_MS,
    };
    ui_page_register(&lora_page);

    Page file_page = {
        .id = PAGE_FILE,
        .name = "file",
        .on_create = file_page_create,
        .on_leave = file_page_leave,
        .on_destroy = file_page_destroy,
        .on_gesture = app_page_back_gesture,
    };
    ui_page_register(&file_page);

    Page bmu_page = {
        .id = PAGE_BMU,
        .name = "bmu",
        .on_create = bmu_page_create,
        .on_leave = bmu_page_leave,
        .on_destroy = bmu_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = bmu_page_timer,
        .timer_interval_ms = BMU_POLL_MS,
    };
    ui_page_register(&bmu_page);

    Page set_page = {
        .id = PAGE_SET,
        .name = "set",
        .on_create = set_page_create,
        .on_leave = set_page_leave,
        .on_destroy = set_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = set_page_timer,
        .timer_interval_ms = 1000,
    };
    ui_page_register(&set_page);
}
