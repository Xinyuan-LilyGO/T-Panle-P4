#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    DISPLAY_CONTROLLER_JD9365,
    DISPLAY_CONTROLLER_JD9365DA,
    DISPLAY_CONTROLLER_ILI9882,
} display_controller_t;

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} display_panel_init_cmd_t;

typedef struct {
    const char *name;
    display_controller_t controller;
    uint16_t width;
    uint16_t height;
    uint8_t lane_num;
    uint16_t lane_bit_rate_mbps;
    uint8_t rgb_ele_order;
    uint8_t bits_per_pixel;
    uint8_t pixel_clock_mhz;
    uint16_t hsync;
    uint16_t hbp;
    uint16_t hfp;
    uint16_t vsync;
    uint16_t vbp;
    uint16_t vfp;
    const display_panel_init_cmd_t *init_cmds;
    size_t init_cmds_count;
} display_panel_profile_t;

extern const display_panel_profile_t display_panel_profile;
