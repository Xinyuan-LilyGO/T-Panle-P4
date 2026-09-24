# T-Panel-P4 Audio Codec

This component initializes the board audio devices through `t_panel_p4_bsp`.
The caller owns the BSP and must initialize it before creating the audio codec.
Its managed dependency is declared in `idf_component.yml`; do not edit or copy
the generated `managed_components` directory by hand.

| Board | Playback | Capture | Speaker enable |
| --- | --- | --- | --- |
| Standard / Round | ES8389 | ES8389 | XL9555 |
| Rect | ES8389 | ES7210 | MCU GPIO |

Rect uses ES7210 MIC1 and MIC2 by default. MIC3 is the ES8389 analog output
feedback used as an AEC reference. Selecting three or more ES7210 inputs uses
a four-slot capture frame, so `capture_channels` must be set to 4.

```c
t_panel_p4_bsp_t bsp;
t_panel_audio_codec_handle_t audio = NULL;
t_panel_audio_codec_config_t config = T_PANEL_AUDIO_CODEC_CONFIG_DEFAULT();

ESP_ERROR_CHECK(t_panel_p4_bsp_init(&bsp));
ESP_ERROR_CHECK(t_panel_audio_codec_init(&bsp, &config, &audio));

ESP_ERROR_CHECK(t_panel_audio_codec_set_volume(audio, 60));
ESP_ERROR_CHECK(t_panel_audio_codec_write(audio, pcm, pcm_size));
ESP_ERROR_CHECK(t_panel_audio_codec_read(audio, capture, capture_size));

ESP_ERROR_CHECK(t_panel_audio_codec_deinit(audio));
ESP_ERROR_CHECK(t_panel_p4_bsp_deinit(&bsp));
```

For Rect capture with the two microphones plus the AEC reference:

```c
config.capture_channels = 4;
config.es7210_mic_mask = T_PANEL_AUDIO_ES7210_MIC1 |
                         T_PANEL_AUDIO_ES7210_MIC2 |
                         T_PANEL_AUDIO_ES7210_MIC3;
```
