idf_component_register(
    SRCS "esp32c5_sdio_slave.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_event
        esp_hosted
        esp_netif
        esp_wifi
        freertos
)
