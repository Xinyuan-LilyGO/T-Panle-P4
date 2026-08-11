idf_component_register(
    SRCS "lora_app.cpp"
    INCLUDE_DIRS "." "include"
    REQUIRES
        jgromes__radiolib
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        t_panle_p4_config
)
