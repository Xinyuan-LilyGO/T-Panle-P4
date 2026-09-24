list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/esp_lcd_jd9365.c"
    "${CMAKE_CURRENT_LIST_DIR}/main.c"
)
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_hw_support
    esp_io_expander
    esp_io_expander_xl9555
    esp_lcd
)
