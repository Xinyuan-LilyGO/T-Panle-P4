#pragma once

#include "ui.h"

void home_page_register(void);
void home_page_set_mic_levels(int mic0_db, int mic1_db);
int home_clamp_percent(int percent);
const char *home_battery_symbol(int percent);
void home_music_status_update_apply(void);
void home_camera_status_update_apply(void);
void home_lora_status_update_apply(void);
