set(RADIO_HAL_SRCS "")
set(RADIO_HAL_REQUIRES t_panel_p4_bsp)

if(NOT CONFIG_T_PANEL_P4_BOARD_RECT)
    list(APPEND RADIO_HAL_SRCS "lora_app.cpp")
    list(APPEND RADIO_HAL_REQUIRES
        jgromes__radiolib
        esp_driver_gpio
        esp_driver_spi
        esp_timer
    )
endif()

idf_component_register(
    SRCS ${RADIO_HAL_SRCS}
    INCLUDE_DIRS "." "include"
    REQUIRES ${RADIO_HAL_REQUIRES}
)
