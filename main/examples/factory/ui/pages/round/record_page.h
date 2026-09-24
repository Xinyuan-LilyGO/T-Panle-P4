#pragma once

#define ROUND_RECORD_WAVE_SAMPLE_COUNT 32

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *record_wave_cont;
    lv_obj_t *record_wave_view;
    uint8_t record_wave_level[ROUND_RECORD_WAVE_SAMPLE_COUNT];
    lv_obj_t *record_status_lable;
    lv_obj_t *record_time_lable;
    lv_obj_t *record_play_btn;
    lv_obj_t *record_play_btn_lable;
    lv_obj_t *record_finish_btn;
    lv_obj_t *record_finish_btn_lable;
    lv_obj_t *record_preview_btn;
    lv_obj_t *record_preview_btn_lable;
    lv_obj_t *record_save_btn;
    lv_obj_t *record_save_btn_lable;
    lv_obj_t *record_list_body;
    lv_obj_t *record_list_page_lable;
    lv_obj_t *record_list_prev_btn;
    lv_obj_t *record_list_next_btn;
    uint16_t record_file_count;
    uint16_t record_file_page;
} record_page_ui_t;

void record_page_register(void);
