#ifndef __UI_H__
#define __UI_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "lvgl.h"

#define HOME_MIC_METER_COUNT 2
#define HOME_MIC_METER_SEGMENTS 20
#define HOME_MONITOR_METRIC_COUNT 2
#define MUSIC_PLAYLIST_VISIBLE_COUNT 4
#define MUSIC_TRACK_PARAM_COUNT 4
#define START_DEVICE_COUNT 8

#define HOME_APP_MENU_COUNT 6

#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
#define MUSIC_LYRIC_FONT_SMALL (&lv_font_source_han_sans_sc_16_cjk)
#define MUSIC_LYRIC_FONT_CURRENT (&lv_font_source_han_sans_sc_16_cjk)
#define MUSIC_NAME_FONT (&lv_font_source_han_sans_sc_16_cjk)
#else
#define MUSIC_LYRIC_FONT_SMALL (&lv_font_montserrat_14)
#define MUSIC_LYRIC_FONT_CURRENT (&lv_font_montserrat_20)
#define MUSIC_NAME_FONT (&lv_font_montserrat_16)
#endif

#define APP_MENU_CENTER_X 235
#define APP_MENU_CENTER_Y 144
#define APP_MENU_CENTER_W 150
#define APP_MENU_CENTER_H 100
#define APP_MENU_CENTER_OPA 255

#define APP_MENU_SIDE_X 295
#define APP_MENU_TOP_Y 18
#define APP_MENU_BOTTOM_Y 284
#define APP_MENU_SIDE_W 120
#define APP_MENU_SIDE_H 80
#define APP_MENU_SIDE_OPA 80

#define APP_MENU_ROLL_SPEED 40
#define APP_MENU_HIDE_X 320
#define APP_MENU_HIDE_TOP_Y -90
#define APP_MENU_HIDE_BOTTOM_Y 410
#define APP_MENU_HIDE_W 240
#define APP_MENU_HIDE_H 70
#define APP_MENU_HIDE_OPA 0

#define APP_MENU_DESC_X 18
#define APP_MENU_DESC_Y 132
#define APP_MENU_DESC_W 150
#define APP_MENU_DESC_H 120

#define APP_MENU_PROGRESS_TRACK_X 196
#define APP_MENU_PROGRESS_TRACK_Y 82
#define APP_MENU_PROGRESS_TRACK_W 5
#define APP_MENU_PROGRESS_TRACK_H 236
#define APP_MENU_PROGRESS_THUMB_W 10
#define APP_MENU_PROGRESS_THUMB_H 20

#define MIC_DB_MIN (-60)
#define MIC_DB_MAX 0
#define MIC_METER_COUNT HOME_MIC_METER_COUNT
#define MIC_METER_SEGMENTS HOME_MIC_METER_SEGMENTS
#define MIC_SEGMENT_GAP 4
#define MIC_SEGMENT_W 6
#define MIC_SEGMENT_H 20
#define MIC_METER_W ((MIC_METER_SEGMENTS * MIC_SEGMENT_W) + ((MIC_METER_SEGMENTS - 1) * MIC_SEGMENT_GAP))
#define STATUS_TIMER_MS 3000
#define HOME_CALENDAR_CELL_COUNT 42
#define HOME_CALENDAR_BUTTON_COUNT 49
#define HOME_CALENDAR_MAP_COUNT 56
#define SET_BRIGHTNESS_MIN 5
#define SET_BRIGHTNESS_MAX 100
#define SET_BRIGHTNESS_DEFAULT 40
#define MUSIC_SCAN_DIR "/sdcard/music"
#define MUSIC_SCAN_MAX_TRACKS 64
#define MUSIC_TRACK_NAME_MAX_LEN 256
#define MUSIC_SPECTRUM_BAR_COUNT 16
#define MUSIC_SPECTRUM_SEGMENT_COUNT 12
#define VIDEO_SCAN_DIR "/sdcard"
#define VIDEO_SCAN_MAX_ITEMS 64
#define VIDEO_GALLERY_VISIBLE_COUNT 9
#define VIDEO_SCAN_MAX_DEPTH 4
#define FILE_SCAN_DIR "/sdcard"
#define FILE_SCAN_MAX_ITEMS 64
#define FILE_LIST_MAX_ROWS 24
#define FILE_PATH_MAX_LEN 320
#define FILE_NAME_MAX_LEN 256
#define LORA_FREQ_DEFAULT_X10 868.0
#define LORA_FREQ_DEFAULT_INDEX 1
#define LORA_BW_DEFAULT_INDEX 1
#define LORA_SF_MIN 6
#define LORA_SF_MAX 12
#define LORA_SF_DEFAULT 7
#define LORA_CR_MIN 5
#define LORA_CR_MAX 8
#define LORA_CR_DEFAULT 5
#define LORA_POWER_MIN_DBM (-9)
#define LORA_POWER_MAX_DBM 22
#define LORA_POWER_DEFAULT_DBM 16
#define LORA_TX_INTERVAL_MIN_MS 500
#define LORA_TX_INTERVAL_MAX_MS 10000
#define LORA_TX_INTERVAL_DEFAULT_MS 1000
#define LORA_RX_POLL_MS 300
#define LORA_TEXT_MAX_LEN 256
#define LORA_PACKET_MAX_LEN 256
#define LORA_RX_DATA_VISIBLE_LINES 17
#define BMU_VALUE_LABEL_COUNT 16
#define BMU_MODULE_STATE_COUNT 4
#define BMU_POLL_MS 1000
#define BMU_FAULT_TEXT_LEN 64
#define CAMERA_UI_PREVIEW_X 40
#define CAMERA_UI_PREVIEW_Y 120
#define CAMERA_UI_PREVIEW_WIDTH 640
#define CAMERA_UI_PREVIEW_HEIGHT 360
#define CAMERA_UI_LAST_PHOTO_WIDTH 128
#define CAMERA_UI_LAST_PHOTO_HEIGHT 72

typedef enum
{
    START_DEVICE_LCD = 0,
    START_DEVICE_TOUCH,
    START_DEVICE_SD,
    START_DEVICE_BMU,
    START_DEVICE_LORA,
    START_DEVICE_AUDIO,
    START_DEVICE_ESP32C5,
    START_DEVICE_CAMERA,
} start_device_t;

typedef enum
{
    HOME_MONITOR_SRAM = 0,
    HOME_MONITOR_PSRAM,
} home_monitor_metric_t;

typedef enum
{
    MUSIC_CONTROL_PLAY_MODE = 0,
    MUSIC_CONTROL_PREV,
    MUSIC_CONTROL_PLAY_PAUSE,
    MUSIC_CONTROL_NEXT,
    MUSIC_CONTROL_VOLUME,
    MUSIC_CONTROL_TRACK_SELECT,
} music_control_action_t;

typedef enum
{
    MUSIC_CIRCLE_FILL_SOLID = 0,
    MUSIC_CIRCLE_FILL_DISC,
    MUSIC_CIRCLE_FILL_PANEL,
    MUSIC_CIRCLE_FILL_ACCENT,
} music_circle_fill_t;

typedef enum
{
    MUSIC_CIRCLE_GRAD_DISC = 0,
    MUSIC_CIRCLE_GRAD_PANEL,
    MUSIC_CIRCLE_GRAD_ACCENT,
    MUSIC_CIRCLE_GRAD_COUNT,
} music_circle_grad_t;

typedef enum
{
    LORA_PARAM_SF = 0,
    LORA_PARAM_CR,
    LORA_PARAM_POWER,
    LORA_PARAM_TX_INTERVAL,
} lora_param_t;

typedef enum
{
    LORA_CONTROL_TX = 0,
    LORA_CONTROL_RX,
    LORA_CONTROL_TX_ONCE,
    LORA_CONTROL_STOP,
    LORA_CONTROL_CLEAR_LOG,
} lora_control_action_t;

typedef enum
{
    BMU_VALUE_BATTERY_PERCENT = 0,
    BMU_VALUE_BATTERY_SOH,
    BMU_VALUE_CHARGE_STATUS,
    BMU_VALUE_BAT_PRESENT,
    BMU_VALUE_VBAT,
    BMU_VALUE_VBUS,
    BMU_VALUE_VSYS,
    BMU_VALUE_IBUS,
    BMU_VALUE_ICHG,
    BMU_VALUE_IDIS,
    BMU_VALUE_TS,
    BMU_VALUE_DIE_TEMP,
    BMU_VALUE_BC12,
    BMU_VALUE_FAULT,
    BMU_VALUE_VINDPM,
    BMU_VALUE_THERMAL,
} bmu_value_t;

typedef struct
{
    bool wifi_connected;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool battery_charging;
    int battery_percent;
} status_info_t;

typedef struct
{
    char ssid[33];
    int rssi;
    char type[16];
    int authmode;
} wifi_scan_ap_info_t;

typedef struct
{
    int fps;
    int sram_percent;
    int psram_percent;
    uint32_t sram_free_kb;
    uint32_t psram_free_kb;
    int temp_c_x10;
} home_info_t;

typedef struct
{
    bool usb_mode;
    bool app_mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char type[24];
    char bus[24];
} factory_storage_info_t;

typedef struct
{
    bool ready;
    bool charger_enabled;
    bool charger_hw_enabled;
    bool battery_detection_enabled;
    bool fuel_gauge_enabled;
    bool bc12_enabled;
    bool bat_present;
    int bat_current_dir;
    bool vindpm;
    bool thermal_regulation;
    bool current_limit;
    int battery_percent;
    int battery_soh;
    int charge_status;
    int bc12_type;
    int vbat_mv;
    int vbus_mv;
    int vsys_mv;
    int ibus_ma;
    int ichg_ma;
    int idis_ma;
    int ts_mv;
    int die_temp_c_x10;
    uint8_t fault0;
    uint8_t fault1;
    char fault_text[BMU_FAULT_TEXT_LEN];
} bmu_info_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;

    lv_obj_t *device_name_label;
    lv_obj_t *init_label;
    lv_obj_t *init_device_cont;
    lv_obj_t *status_label;
    lv_obj_t *sub_status_label;
    lv_obj_t *progress_bar;
    lv_obj_t *logo_label;
    lv_obj_t *device_icon[START_DEVICE_COUNT];
    lv_obj_t *device_status[START_DEVICE_COUNT];
} start_page_ui_t;

typedef struct
{
    lv_obj_t *status_cont;
    lv_obj_t *time_label;
    lv_obj_t *wifi_icon;
    lv_obj_t *bluetooth_icon;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_label;
} status_bar_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *monitor_metric_value[HOME_MONITOR_METRIC_COUNT];
    lv_obj_t *monitor_metric_bar[HOME_MONITOR_METRIC_COUNT];
    lv_obj_t *temp_value_label;
    lv_obj_t *calendar_head;
    lv_obj_t *calendar_matrix;
    lv_obj_t *mic_db_label[HOME_MIC_METER_COUNT];
    lv_obj_t *mic_segments[HOME_MIC_METER_COUNT][HOME_MIC_METER_SEGMENTS];
} home_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *music_cover_img;
    lv_obj_t *play_status_label;
    lv_obj_t *song_name_label;
    lv_obj_t *artist_album_label;
    lv_obj_t *singer_label;
    lv_obj_t *paly_progress_bar;
    lv_obj_t *paly_progress_current_time_label;
    lv_obj_t *paly_progress_total_time_label;
    lv_obj_t *paly_lyric_label;
    lv_obj_t *paly_lyric_text[3];
    lv_obj_t *spectrum_segment[MUSIC_SPECTRUM_BAR_COUNT][MUSIC_SPECTRUM_SEGMENT_COUNT];
    uint8_t spectrum_level[MUSIC_SPECTRUM_BAR_COUNT];
    lv_obj_t *play_mode_btn_label;
    lv_obj_t *play_btn_label;
    lv_obj_t *volume_slider;
    lv_obj_t *volume_value_label;
    lv_obj_t *playlist_count_label;
    lv_obj_t *playlist_row[MUSIC_PLAYLIST_VISIBLE_COUNT];
    lv_obj_t *playlist_index_label[MUSIC_PLAYLIST_VISIBLE_COUNT];
    lv_obj_t *playlist_file_label[MUSIC_PLAYLIST_VISIBLE_COUNT];
    lv_obj_t *track_param_value[MUSIC_TRACK_PARAM_COUNT];
} music_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;
    lv_obj_t *preview_cont;
    lv_obj_t *preview_img;
    lv_obj_t *status_label;
    lv_obj_t *last_photo_btn;
    lv_obj_t *last_photo_img;
    lv_obj_t *last_photo_icon;
    lv_obj_t *shutter_btn;
    lv_obj_t *resolution_label;
    lv_obj_t *fps_label;
    lv_obj_t *shot_count_label;
    lv_obj_t *storage_label;
} camera_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;
    lv_obj_t *body_cont;
    lv_obj_t *count_label;
    lv_obj_t *page_label;
    lv_obj_t *preview_panel;
    lv_obj_t *preview_img;
    lv_obj_t *preview_icon;
    lv_obj_t *viewer_cont;
    lv_obj_t *viewer_stage;
    lv_obj_t *viewer_img;
    lv_obj_t *viewer_icon;
    lv_obj_t *viewer_name_label;
    lv_obj_t *viewer_status_label;
    lv_obj_t *selected_name_label;
    lv_obj_t *selected_type_label;
    lv_obj_t *selected_path_label;
    lv_obj_t *open_hint;
    lv_obj_t *grid_cont;
    lv_obj_t *grid_card[VIDEO_GALLERY_VISIBLE_COUNT];
    lv_obj_t *grid_icon[VIDEO_GALLERY_VISIBLE_COUNT];
    lv_obj_t *grid_name[VIDEO_GALLERY_VISIBLE_COUNT];
    lv_obj_t *grid_type[VIDEO_GALLERY_VISIBLE_COUNT];
    lv_obj_t *prev_btn;
    lv_obj_t *next_btn;
} video_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *status_label;
    lv_obj_t *chip_label;
    lv_obj_t *mode_label;
    lv_obj_t *freq_dropdown;
    lv_obj_t *bw_dropdown;
    lv_obj_t *sf_slider;
    lv_obj_t *sf_value_label;
    lv_obj_t *cr_slider;
    lv_obj_t *cr_value_label;
    lv_obj_t *power_slider;
    lv_obj_t *power_value_label;
    lv_obj_t *tx_interval_slider;
    lv_obj_t *tx_interval_value_label;
    lv_obj_t *tx_input;
    lv_obj_t *lora_log;
    lv_obj_t *rx_data_textarea;
    lv_obj_t *tx_btn;
    lv_obj_t *lora_rssi_snr_label;
    lv_obj_t *rx_btn;
} lora_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *path_label;
    lv_obj_t *count_label;
    lv_obj_t *list_cont;
    lv_obj_t *viewer_cont;
    lv_obj_t *viewer_title_label;
    lv_obj_t *viewer_status_label;
    lv_obj_t *viewer_body;
    lv_obj_t *viewer_textarea;
    lv_obj_t *viewer_img;
} file_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *status_label;
    lv_obj_t *battery_bar;
    lv_obj_t *value_label[BMU_VALUE_LABEL_COUNT];
    lv_obj_t *charge_current_slider;
    lv_obj_t *input_current_slider;
    lv_obj_t *low_warn_slider;
    lv_obj_t *charge_current_value_label;
    lv_obj_t *input_current_value_label;
    lv_obj_t *low_warn_value_label;
    lv_obj_t *charge_voltage_label;
    lv_obj_t *charger_switch;
    lv_obj_t *module_state_label[BMU_MODULE_STATE_COUNT];
    lv_obj_t *battery_detect_switch;
    lv_obj_t *fuel_gauge_switch;
    lv_obj_t *bc12_switch;
} bmu_page_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *main_cont;
    lv_obj_t *brightness_slider;
    lv_obj_t *brightness_label;
    lv_obj_t *auto_dim_switch;
    lv_obj_t *mute_switch;
    lv_obj_t *screen_timeout_dropdown;
    lv_obj_t *power_status_label;
    lv_obj_t *storage_label;
    lv_obj_t *storage_type_label;
    lv_obj_t *storage_size_label;
    lv_obj_t *storage_bus_label;
    lv_obj_t *timesync_label;
    lv_obj_t *firmware_label;
    lv_obj_t *wifi_dialog;
    lv_obj_t *wifi_status_label;
    lv_obj_t *wifi_list_cont;
    lv_obj_t *wifi_password_dialog;
    lv_obj_t *wifi_ssid_list_cont_label;
    lv_obj_t *wifi_ssid_label;
    lv_obj_t *wifi_ip_label;
    lv_obj_t *wifi_password_ta;
    lv_obj_t *wifi_keyboard;
    lv_obj_t *usb_otg_switch;
} set_page_ui_t;

extern volatile bool irq_flag;

void create_page_ui(void);
void start_page_set_status(const char *text, int progress);
void start_page_set_device_status(start_device_t device, bool ok);
bool status_bar_info_get(status_info_t *status);
void status_bar_update(const status_info_t *status);
bool wifi_remote_scan(wifi_scan_ap_info_t *aps, int max_count, int *out_count, char *err, size_t err_len);
bool wifi_remote_connect(const char *ssid, const char *password, int authmode, char *err, size_t err_len);
bool home_status_info_get(home_info_t *status);
void home_info_update(const home_info_t *info);
bool bmu_status_info_get(bmu_info_t *status);
bool bmu_charger_enable_set(bool enable);
bool bmu_charge_current_set(int ma);
bool bmu_input_current_limit_set(int ma);
bool bmu_low_battery_warn_set(int warn_percent);
bool factory_power_enter_ship_mode(void);
void factory_power_restart(void);
bool factory_power_enter_low_power(void);
bool factory_power_exit_low_power(void);
bool factory_usb_otg_msc_set(bool usb_mode);
bool factory_usb_otg_msc_is_usb_mode(void);
bool factory_storage_info_get(factory_storage_info_t *info);
void camera_page_set_status(const char *text, bool ok);
void camera_page_set_shot_count(uint32_t count);
void camera_page_set_preview_info(uint32_t width, uint32_t height, uint32_t fps_x10);
void camera_page_set_last_photo(const uint8_t *rgb888, uint32_t width, uint32_t height);
void camera_page_set_storage_ready(bool ready);
void camera_page_set_preview_frame(const uint8_t *rgb888, uint32_t width, uint32_t height);

void home_page_set_mic_levels(int mic0_db, int mic1_db);
void music_page_set_lyrics(const char *prev, const char *current, const char *next);
void music_page_set_play_state(bool playing);
void music_page_set_volume(int volume_percent);
void music_page_set_progress(int current_sec, int total_sec);
void music_page_set_playlist(const char *const *tracks, int track_count, int current_index);
void music_page_set_current_track(int index);
void music_page_set_track_params(const char *format, const char *sample_rate, const char *bitrate, const char *channels);
void music_page_set_spectrum_levels(const uint8_t *levels, int count);
int music_page_scan_sd_music(void);
int music_page_get_track_count(void);
const char *music_page_get_track_name(int index);
bool music_page_on_control(music_control_action_t action, int value);

#endif
