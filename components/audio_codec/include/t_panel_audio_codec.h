#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "t_panel_p4_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T_PANEL_AUDIO_ES7210_MIC1 (1U << 0)
#define T_PANEL_AUDIO_ES7210_MIC2 (1U << 1)
#define T_PANEL_AUDIO_ES7210_MIC3 (1U << 2)
#define T_PANEL_AUDIO_ES7210_MIC4 (1U << 3)

typedef struct t_panel_audio_codec *t_panel_audio_codec_handle_t;

typedef enum {
    T_PANEL_AUDIO_INPUT_AUTO = 0,
    T_PANEL_AUDIO_INPUT_ES8389,
    T_PANEL_AUDIO_INPUT_ES7210,
} t_panel_audio_input_t;

typedef enum {
    T_PANEL_AUDIO_CHIP_ES8389 = 0,
    T_PANEL_AUDIO_CHIP_ES7210,
} t_panel_audio_chip_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t bits_per_sample;
    uint8_t playback_channels;
    uint8_t capture_channels;
    i2s_mclk_multiple_t mclk_multiple;
    bool enable_playback;
    bool enable_capture;
    bool enable_speaker;
    t_panel_audio_input_t input;
    uint8_t es7210_mic_mask;
    int output_volume;
    float input_gain_db;
} t_panel_audio_codec_config_t;

#define T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT()                         \
    {                                                                \
        .sample_rate_hz = 48000,                                     \
        .bits_per_sample = 16,                                       \
        .playback_channels = 2,                                      \
        .capture_channels = 2,                                       \
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,                      \
        .enable_playback = true,                                     \
        .enable_capture = true,                                      \
        .enable_speaker = true,                                      \
        .input = T_PANEL_AUDIO_INPUT_AUTO,                           \
        .es7210_mic_mask = T_PANEL_AUDIO_ES7210_MIC1 |               \
                           T_PANEL_AUDIO_ES7210_MIC2,                 \
        .output_volume = 70,                                         \
        .input_gain_db = 30.0f,                                      \
    }

/**
 * Initialize the board audio path using an already initialized BSP instance.
 * AUTO selects ES7210 input on Rect and ES8389 input on Standard/Round.
 * The codec owns the BSP I2S channels until t_panel_audio_codec_deinit().
 */
esp_err_t t_panel_audio_codec_init(t_panel_p4_bsp_t *bsp,
                                   const t_panel_audio_codec_config_t *config,
                                   t_panel_audio_codec_handle_t *ret_codec);
esp_err_t t_panel_audio_codec_deinit(t_panel_audio_codec_handle_t codec);

esp_err_t t_panel_audio_codec_start(t_panel_audio_codec_handle_t codec);
esp_err_t t_panel_audio_codec_stop(t_panel_audio_codec_handle_t codec);

esp_err_t t_panel_audio_codec_write(t_panel_audio_codec_handle_t codec,
                                    void *data, size_t size);
esp_err_t t_panel_audio_codec_read(t_panel_audio_codec_handle_t codec,
                                   void *data, size_t size);

esp_err_t t_panel_audio_codec_set_volume(t_panel_audio_codec_handle_t codec,
                                         int volume);
esp_err_t t_panel_audio_codec_get_volume(t_panel_audio_codec_handle_t codec,
                                         int *volume);
esp_err_t t_panel_audio_codec_set_output_mute(t_panel_audio_codec_handle_t codec,
                                              bool mute);
esp_err_t t_panel_audio_codec_get_output_mute(t_panel_audio_codec_handle_t codec,
                                              bool *muted);

esp_err_t t_panel_audio_codec_set_input_gain(t_panel_audio_codec_handle_t codec,
                                             float gain_db);
esp_err_t t_panel_audio_codec_set_input_channel_gain(t_panel_audio_codec_handle_t codec,
                                                     uint16_t channel_mask,
                                                     float gain_db);
esp_err_t t_panel_audio_codec_get_input_gain(t_panel_audio_codec_handle_t codec,
                                             float *gain_db);
esp_err_t t_panel_audio_codec_set_input_mute(t_panel_audio_codec_handle_t codec,
                                             bool mute);
esp_err_t t_panel_audio_codec_get_input_mute(t_panel_audio_codec_handle_t codec,
                                             bool *muted);

/**
 * Calculate the dBFS level of signed 16-bit PCM samples.
 *
 * For interleaved audio, point samples at the first sample of the desired
 * channel and set stride to the channel count. Invalid or silent input returns
 * -96 dBFS.
 */
float t_panel_audio_codec_calculate_dbfs_s16(const int16_t *samples,
                                             size_t sample_count,
                                             size_t stride);

esp_err_t t_panel_audio_codec_set_speaker_enabled(t_panel_audio_codec_handle_t codec,
                                                  bool enabled);
bool t_panel_audio_codec_is_speaker_enabled(t_panel_audio_codec_handle_t codec);
t_panel_audio_input_t t_panel_audio_codec_get_input(t_panel_audio_codec_handle_t codec);

esp_err_t t_panel_audio_codec_read_reg(t_panel_audio_codec_handle_t codec,
                                       t_panel_audio_chip_t chip,
                                       int reg, int *value);
esp_err_t t_panel_audio_codec_write_reg(t_panel_audio_codec_handle_t codec,
                                        t_panel_audio_chip_t chip,
                                        int reg, int value);
esp_err_t t_panel_audio_codec_dump_regs(t_panel_audio_codec_handle_t codec,
                                        t_panel_audio_chip_t chip);

esp_codec_dev_handle_t t_panel_audio_codec_get_playback_dev(t_panel_audio_codec_handle_t codec);
esp_codec_dev_handle_t t_panel_audio_codec_get_capture_dev(t_panel_audio_codec_handle_t codec);

#ifdef __cplusplus
}
#endif
