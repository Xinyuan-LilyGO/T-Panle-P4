set(DISPLAY_PANEL_SRCS "src/display_panel.c")
set(DISPLAY_PANEL_REQUIRES
    driver
    esp_lcd
    espressif__esp_io_expander
    t_panel_p4_bsp
)

if(CONFIG_T_PANEL_P4_BOARD_STANDARD)
    list(APPEND DISPLAY_PANEL_SRCS
        "controllers/jd9365/esp_lcd_jd9365.c"
        "profiles/panel_standard_jd9365.c"
    )
elseif(CONFIG_T_PANEL_P4_BOARD_ROUND)
    list(APPEND DISPLAY_PANEL_SRCS
        "controllers/jd9365/esp_lcd_jd9365.c"
        "profiles/panel_round_jd9365da.c"
    )
elseif(CONFIG_T_PANEL_P4_BOARD_RECT)
    list(APPEND DISPLAY_PANEL_SRCS
        "controllers/ili9882/esp_lcd_ili9882.c"
        "profiles/panel_rect_ili9882.c"
    )
else()
    # Component requirements are scanned before project Kconfig is loaded.
    list(APPEND DISPLAY_PANEL_SRCS
        "controllers/jd9365/esp_lcd_jd9365.c"
        "profiles/panel_standard_jd9365.c"
    )
endif()

idf_component_register(
    SRCS ${DISPLAY_PANEL_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "private_include"
        "controllers/jd9365"
        "controllers/ili9882"
    REQUIRES ${DISPLAY_PANEL_REQUIRES}
)
