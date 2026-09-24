#ifndef __UI_H__
#define __UI_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "audio.h"
#include "bmu.h"
#include "lvgl_page_manager.h"
#include "factory_service_types.h"

#define HOME_MIC_METER_COUNT 2
#define HOME_MIC_METER_SEGMENTS 20
#define MUSIC_PLAYLIST_VISIBLE_COUNT 4
#define MUSIC_TRACK_PARAM_COUNT 4
#if CONFIG_T_PANEL_P4_BOARD_STANDARD || CONFIG_T_PANEL_P4_BOARD_ROUND
#define HOME_APP_MENU_COUNT 7
#else
#define HOME_APP_MENU_COUNT 6
#endif

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
#define APP_MENU_CENTER_Y 244
#define APP_MENU_CENTER_W 150
#define APP_MENU_CENTER_H 100
#define APP_MENU_CENTER_OPA 255

#define APP_MENU_SIDE_X 295
#define APP_MENU_TOP_Y 118
#define APP_MENU_BOTTOM_Y 384
#define APP_MENU_SIDE_W 120
#define APP_MENU_SIDE_H 80
#define APP_MENU_SIDE_OPA 80

#define APP_MENU_ROLL_SPEED 24
#define APP_MENU_HIDE_X 320
#define APP_MENU_HIDE_TOP_Y 10
#define APP_MENU_HIDE_BOTTOM_Y 510
#define APP_MENU_HIDE_W 72
#define APP_MENU_HIDE_H 48
#define APP_MENU_HIDE_OPA 0

#define APP_MENU_DESC_W 150

#define APP_MENU_PROGRESS_TRACK_X 196
#define APP_MENU_PROGRESS_TRACK_Y 182
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
#define SET_BRIGHTNESS_MIN 5
#define SET_BRIGHTNESS_MAX 100
#define SET_BRIGHTNESS_DEFAULT 40
#define MUSIC_SCAN_DIR AUDIO_MUSIC_DIR
#define MUSIC_SCAN_MAX_TRACKS AUDIO_MUSIC_MAX_TRACKS
#define MUSIC_TRACK_NAME_MAX_LEN AUDIO_MUSIC_TRACK_NAME_MAX_LEN
#define MUSIC_SPECTRUM_BAR_COUNT AUDIO_MUSIC_SPECTRUM_BAR_COUNT
#define MUSIC_SPECTRUM_SEGMENT_COUNT AUDIO_MUSIC_SPECTRUM_LEVEL_MAX
#define VIDEO_GALLERY_VISIBLE_COUNT 9
#define FILE_SCAN_DIR "/sdcard"
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
#define LORA_TX_INTERVAL_MIN_S 1
#define LORA_TX_INTERVAL_MAX_S 10
#define LORA_RX_POLL_MS 300
#define LORA_TEXT_MAX_LEN 256
#define LORA_PACKET_MAX_LEN 256
#define LORA_RX_DATA_VISIBLE_LINES 17
#define CAMERA_UI_PREVIEW_X 0
#define CAMERA_UI_PREVIEW_Y 0
#define CAMERA_UI_PREVIEW_WIDTH 720
#define CAMERA_UI_PREVIEW_HEIGHT 480
#define CAMERA_UI_LAST_PHOTO_WIDTH 128
#define CAMERA_UI_LAST_PHOTO_HEIGHT 72

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

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *logo_label;
    lv_obj_t *board_name_label;
} start_page_ui_t;

typedef struct
{
    lv_obj_t *status_cont;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *wifi_icon;
    lv_obj_t *bluetooth_icon;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_label;
} status_bar_ui_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *logo_label;
    lv_obj_t *main_cont;
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
    lv_obj_t *preview_icon;
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
    lv_obj_t *wifi_enable_switch;
    lv_obj_t *wifi_state_label;
    lv_obj_t *c5_sleep_switch;
    lv_obj_t *c5_state_label;
    lv_obj_t *wifi_password_ta;
    lv_obj_t *wifi_keyboard;
    lv_obj_t *usb_otg_switch;
    lv_obj_t *pmu_charger_switch;
    lv_obj_t *pmu_charge_status_label;
    lv_obj_t *pmu_charge_voltage_label;
    lv_obj_t *pmu_ibus_label;
    lv_obj_t *pmu_ichg_label;
    lv_obj_t *pmu_charge_current_dropdown;
    lv_obj_t *pmu_battery_soh_label;
    lv_obj_t *pmu_battery_detect_label;
    lv_obj_t *pmu_vbus_label;
    lv_obj_t *pmu_vsys_label;
    lv_obj_t *pmu_vbat_label;
    lv_obj_t *pmu_input_limit_label;
    lv_obj_t *settings_detail_dialog;
    lv_obj_t *settings_detail_body;
    lv_obj_t *power_confirm_dialog;
} set_page_ui_t;

void create_page_ui(void);
void app_page_destroy_async_cb(void *user_data);
void app_page_back_gesture(Page *page, GestureDirection direction);

LV_FONT_DECLARE(lv_font_SourceHanSansCN_Bold_2_20);
LV_FONT_DECLARE(lv_font_SourceHanSansCN_Bold_2_24);
LV_FONT_DECLARE(lv_font_SourceHanSansCN_Bold_2_28);
LV_FONT_DECLARE(lv_font_SourceHanSansCN_Bold_2_30);
LV_FONT_DECLARE(lv_font_SourceHanSansCN_Bold_2_50);
LV_FONT_DECLARE(lv_font_SourceHanSansSC_Regular_2_16);
LV_FONT_DECLARE(lv_font_SourceHanSansSC_Regular_2_20);
LV_FONT_DECLARE(lv_font_SourceHanSansSC_Regular_2_24);
LV_FONT_DECLARE(lv_font_SourceHanSansSC_Regular_2_28);

#endif
