#pragma once

#include "t_panel_p4_bsp.h"

esp_err_t t_panel_p4_bsp_board_init(t_panel_p4_bsp_t *bsp);
esp_err_t t_panel_p4_bsp_board_deinit(t_panel_p4_bsp_t *bsp);
esp_err_t t_panel_p4_bsp_board_speaker_set(t_panel_p4_bsp_t *bsp, bool enabled);
