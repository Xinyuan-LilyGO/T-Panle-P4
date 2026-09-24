#pragma once

#include <time.h>

#include "ui.h"

extern struct tm g_ui_timeinfo;

void status_bar_create(void);
void status_bar_update(const status_info_t *status);
lv_obj_t *status_bar_get_container(void);
