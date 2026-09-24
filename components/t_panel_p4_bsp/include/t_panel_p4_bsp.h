#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_ldo_regulator.h"

#if CONFIG_T_PANEL_P4_BOARD_STANDARD
#include "standard.h"
#elif CONFIG_T_PANEL_P4_BOARD_ROUND
#include "round.h"
#elif CONFIG_T_PANEL_P4_BOARD_RECT
#include "rect.h"
#else
#error "No T-Panel-P4 hardware variant selected"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T_PANEL_P4_CAMERA_FLASH_1 = 0,
    T_PANEL_P4_CAMERA_FLASH_2,
    T_PANEL_P4_CAMERA_FLASH_COUNT,
} t_panel_p4_camera_flash_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    spi_host_device_t spi_host;
    bool spi_initialized;
    i2s_chan_handle_t i2s_tx;
    i2s_chan_handle_t i2s_rx;
    esp_ldo_channel_handle_t mipi_phy_ldo;
    bool speaker_enabled;
    bool camera_flash_pwm_initialized;
    uint8_t camera_flash_brightness[T_PANEL_P4_CAMERA_FLASH_COUNT];
} t_panel_p4_bsp_t;

typedef struct {
    uint32_t sample_rate_hz;
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_mclk_multiple_t mclk_multiple;
    i2s_role_t role;
    int dma_desc_num;
    int dma_frame_num;
    bool enable_tx;
    bool enable_rx;
} t_panel_p4_bsp_i2s_config_t;

#define T_PANEL_P4_BSP_I2S_CONFIG_DEFAULT()        \
    {                                               \
        .sample_rate_hz = 48000,                    \
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, \
        .slot_mode = I2S_SLOT_MODE_STEREO,          \
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,     \
        .role = I2S_ROLE_MASTER,                    \
        .dma_desc_num = 8,                          \
        .dma_frame_num = 512,                       \
        .enable_tx = true,                          \
        .enable_rx = true,                          \
    }

const char *t_panel_p4_bsp_get_board_name(void);
esp_err_t t_panel_p4_bsp_init(t_panel_p4_bsp_t *bsp);
esp_err_t t_panel_p4_bsp_deinit(t_panel_p4_bsp_t *bsp);
i2c_master_bus_handle_t t_panel_p4_bsp_get_i2c_bus(const t_panel_p4_bsp_t *bsp);
esp_io_expander_handle_t t_panel_p4_bsp_get_io_expander(const t_panel_p4_bsp_t *bsp);

esp_err_t t_panel_p4_bsp_spi_init(t_panel_p4_bsp_t *bsp,
                                  spi_host_device_t host,
                                  int max_transfer_size);
esp_err_t t_panel_p4_bsp_spi_deinit(t_panel_p4_bsp_t *bsp);

esp_err_t t_panel_p4_bsp_i2s_init(t_panel_p4_bsp_t *bsp,
                                  const t_panel_p4_bsp_i2s_config_t *config);
esp_err_t t_panel_p4_bsp_i2s_deinit(t_panel_p4_bsp_t *bsp);

esp_err_t t_panel_p4_bsp_speaker_set_enabled(t_panel_p4_bsp_t *bsp, bool enabled);
bool t_panel_p4_bsp_speaker_is_enabled(const t_panel_p4_bsp_t *bsp);

/* Camera flash PWM is available only on the Rect board. */
esp_err_t t_panel_p4_bsp_camera_flash_pwm_init(t_panel_p4_bsp_t *bsp);
esp_err_t t_panel_p4_bsp_camera_flash_set_brightness(
    t_panel_p4_bsp_t *bsp, t_panel_p4_camera_flash_t flash, uint8_t percent);
esp_err_t t_panel_p4_bsp_camera_flash_set_all_brightness(
    t_panel_p4_bsp_t *bsp, uint8_t percent);
esp_err_t t_panel_p4_bsp_camera_flash_pwm_deinit(t_panel_p4_bsp_t *bsp);

/* CSI and DSI share this internal 2.5 V MIPI PHY supply. */
esp_err_t t_panel_p4_bsp_mipi_phy_init(t_panel_p4_bsp_t *bsp);
esp_err_t t_panel_p4_bsp_mipi_phy_deinit(t_panel_p4_bsp_t *bsp);

#ifdef __cplusplus
}
#endif
