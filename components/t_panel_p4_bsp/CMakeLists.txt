set(BSP_SRCS "src/bsp_common.c")
set(BSP_REQUIRES
    esp_driver_i2c
    esp_driver_gpio
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_spi
    esp_hw_support
    espressif__esp_io_expander
)

if(NOT CONFIG_T_PANEL_P4_BOARD_RECT)
    list(APPEND BSP_SRCS "src/bsp_standard_round.c")
    list(APPEND BSP_REQUIRES sheldonix__esp_io_expander_xl9555)
else()
    # Rect is dependency-free and is safe during the early component scan.
    list(APPEND BSP_SRCS "src/bsp_rect.c")
endif()

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS "include" "boards"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${BSP_REQUIRES}
)
