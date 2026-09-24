list(APPEND EXAMPLE_SRCS "${CMAKE_CURRENT_LIST_DIR}/main.cpp")
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES radio_hal esp_driver_i2c esp_timer esp_driver_gpio esp_driver_spi t_panel_p4_bsp)
