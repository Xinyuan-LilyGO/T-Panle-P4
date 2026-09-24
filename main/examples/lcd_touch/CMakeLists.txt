list(APPEND EXAMPLE_SRCS "${CMAKE_CURRENT_LIST_DIR}/main.c")
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES esp_driver_i2c esp_driver_gpio esp_mm display_panel touch_panel t_panel_p4_bsp)
