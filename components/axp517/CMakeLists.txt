idf_component_register(
    SRCS "axp517.c" "axp517_user.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
)
