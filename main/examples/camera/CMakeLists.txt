list(APPEND EXAMPLE_SRCS "${CMAKE_CURRENT_LIST_DIR}/main.c")
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES esp_cam_sensor esp_driver_i2c esp_driver_isp esp_mm esp_timer heap display_panel sgm38121 t_panel_p4_bsp)
