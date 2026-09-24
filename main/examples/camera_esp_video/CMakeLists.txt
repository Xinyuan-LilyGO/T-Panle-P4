list(APPEND EXAMPLE_SRCS "${CMAKE_CURRENT_LIST_DIR}/main.c")
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES camera_ov2710 display_panel esp_driver_ppa esp_mm esp_timer t_panel_p4_bsp)
