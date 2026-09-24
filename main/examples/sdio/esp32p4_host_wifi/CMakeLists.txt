list(APPEND EXAMPLE_SRCS "${CMAKE_CURRENT_LIST_DIR}/main.c")
list(APPEND EXAMPLE_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")
list(APPEND EXAMPLE_REQUIRES esp_event esp_hosted esp_netif esp_timer esp_wifi nvs_flash)
