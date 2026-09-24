idf_component_register(
    SRCS
        "src/t_panel_audio_codec.c"
        "es8389/t_panel_es8389.c"
        "es7210/t_panel_es7210.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include" "es8389" "es7210"
    REQUIRES
        esp_codec_dev
        esp_driver_i2s
        t_panel_p4_bsp
)
