idf_component_register(
    SRCS "qmi8658c_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp-idf-lib__qmi8658c
    PRIV_REQUIRES log
)
