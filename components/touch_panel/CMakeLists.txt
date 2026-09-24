set(TOUCH_PANEL_SRCS "src/touch_panel.c")
set(TOUCH_PANEL_REQUIRES
    driver
    esp_lcd
    espressif__esp_io_expander
    t_panel_p4_bsp
)

if(CONFIG_T_PANEL_P4_BOARD_STANDARD)
    list(APPEND TOUCH_PANEL_SRCS "controllers/jadard/touch_jadard.c")
    list(APPEND TOUCH_PANEL_REQUIRES espressif__esp_lcd_touch)
elseif(CONFIG_T_PANEL_P4_BOARD_ROUND)
    list(APPEND TOUCH_PANEL_SRCS
        "controllers/gt911/touch_gt911.c"
        "controllers/gt911/esp_lcd_touch_gt911.c"
    )
    list(APPEND TOUCH_PANEL_REQUIRES espressif__esp_lcd_touch)
elseif(CONFIG_T_PANEL_P4_BOARD_RECT)
    list(APPEND TOUCH_PANEL_SRCS "controllers/ili9882/touch_ili9882.c")
else()
    # Component dependency discovery runs before the project Kconfig is loaded.
    list(APPEND TOUCH_PANEL_SRCS "controllers/jadard/touch_jadard.c")
    list(APPEND TOUCH_PANEL_REQUIRES espressif__esp_lcd_touch)
endif()

idf_component_register(
    SRCS ${TOUCH_PANEL_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "private_include"
        "controllers/jadard"
        "controllers/gt911"
        "controllers/ili9882"
    REQUIRES ${TOUCH_PANEL_REQUIRES}
)
