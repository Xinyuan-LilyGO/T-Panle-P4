set(PROJECT_PATH "")
set(SRCS "")
set(EMBED_FILES "")
set(EXAMPLE_INCLUDE_DIRS ".")
set(EXAMPLE_REQUIRES "")

macro(select_example example_path example_source)
    set(PROJECT_PATH "${example_path}")
    set(SRCS "${PROJECT_PATH}/${example_source}")
    foreach(extra_source ${ARGN})
        list(APPEND SRCS "${PROJECT_PATH}/${extra_source}")
    endforeach()
    set(EXAMPLE_INCLUDE_DIRS "." "${PROJECT_PATH}")
endmacro()

macro(add_example_requires)
    list(APPEND EXAMPLE_REQUIRES ${ARGN})
endmacro()

macro(add_example_embed_files)
    foreach(embed_file ${ARGN})
        list(APPEND EMBED_FILES "${PROJECT_PATH}/${embed_file}")
    endforeach()
endmacro()

macro(configure_factory_example)
    select_example("examples/factory" "main.c" "ui.c" "audio.c" "camera.c")
    add_example_requires(
        esp_driver_i2c
        esp_driver_i2s
        esp_codec_dev
        esp_audio_codec
        esp_audio_simple_player
        lcd_jd9365
        sgm38121
        axp517
        radio_hal
        button
        fatfs
        sdmmc
        vfs
        nvs_flash
        spi_flash
        app_update
        esp_timer
        esp_tinyusb
        heap
        lvgl
        sheldonix__esp_io_expander_xl9555
        lvgl_page_manager
        esp_event
        esp_hosted
        esp_netif
        esp_wifi
        esp_mm
        esp_video
        esp_h264
        esp_driver_jpeg
        esp_driver_isp
        esp_driver_ppa
        t_panle_p4_config
    )
endmacro()

if(CONFIG_EXAMPLE_FACTORY)
    configure_factory_example()

elseif(CONFIG_EXAMPLE_LORA_SX1276)
    select_example("examples/lora/sx1276" "main.cpp")
    add_example_requires(
        radio_hal
        esp_driver_i2c
        jgromes__radiolib
        esp_timer
        esp_driver_gpio
        esp_driver_spi
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )
elseif(CONFIG_EXAMPLE_LORA_SX1262)
    select_example("examples/lora/sx1262" "main.cpp")
    add_example_requires(
        radio_hal
        jgromes__radiolib
        esp_timer
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_spi
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )
elseif(CONFIG_EXAMPLE_LORA_LR1121)
    select_example("examples/lora/lr1121" "main.cpp")
    add_example_requires(
        radio_hal
        esp_driver_i2c
        jgromes__radiolib
        esp_timer
        esp_driver_gpio
        esp_driver_spi
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_XL9555)
    select_example("examples/xl9555" "main.c")
    add_example_requires(
        esp_driver_i2c
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_AXP517)
    select_example("examples/axp517" "main.c")
    add_example_requires(
        axp517
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_LCD)
    select_example("examples/lcd" "main.c")
    add_example_requires(
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_jpeg
        lcd_jd9365
        t_panle_p4_config
    )
    add_example_embed_files("wallhaven-8gr6l2_resized.jpg")

elseif(CONFIG_EXAMPLE_AUDIO_PLAY_WAV)
    select_example("examples/audio/play_wav" "main.c")
    add_example_requires(
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
        sheldonix__esp_io_expander_xl9555
        heap
        t_panle_p4_config
    )
    add_example_embed_files("sample-3s.wav")

elseif(CONFIG_EXAMPLE_AUDIO_RECORD)
    select_example("examples/audio/record" "main.c")
    add_example_requires(
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_AUDIO_LOOPBACK)
    select_example("examples/audio/loopback" "main.c")
    add_example_requires(
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
        sheldonix__esp_io_expander_xl9555
        heap
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_CAMERA)
    select_example("examples/camera" "main.c")
    add_example_requires(
        esp_cam_sensor
        esp_driver_i2c
        esp_driver_isp
        esp_mm
        esp_timer
        heap
        lcd_jd9365
        sgm38121
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_CAMERA_ESP_VIDEO)
    select_example("examples/camera_esp_video" "main.c")
    add_example_requires(
        esp_driver_i2c
        esp_mm
        esp_timer
        esp_video
        lcd_jd9365
        sgm38121
        t_panle_p4_config
        vfs
    )

elseif(CONFIG_EXAMPLE_ESP32P4_HOST_WIFI)
    select_example("examples/sdio/esp32p4_host_wifi" "main.c")
    add_example_requires(
        button
        esp_driver_i2c
        esp_event
        esp_hosted
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_ESP32P4_HOST_TEST)
    select_example("examples/sdio/esp32p4_host_test" "main.c")
    add_example_requires(
        button
        esp_driver_i2c
        esp_serial_slave_link
        sdmmc
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_SD)
    select_example("examples/sd" "main.c")
    add_example_requires(
        esp_driver_i2c
        esp_mm
        fatfs
        sdmmc
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
        vfs
    )

elseif(CONFIG_EXAMPLE_SGM38121)
    select_example("examples/sgm38121" "main.c")
    add_example_requires(
        esp_driver_i2c
        sgm38121
        t_panle_p4_config
    )

elseif(CONFIG_EXAMPLE_LVGL)
    select_example("examples/lvgl" "main.c")
    add_example_requires(
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        heap
        lcd_jd9365
        lvgl
        t_panle_p4_config
    )
elseif(CONFIG_EXAMPLE_USB_OTG_MSC)
    select_example("examples/usb_otg_msc" "main.c")
    add_example_requires(
        esp_driver_i2c
        esp_driver_gpio
        esp_hw_support
        esp_timer
        esp_tinyusb
        fatfs
        heap
        sdmmc
        sheldonix__esp_io_expander_xl9555
        t_panle_p4_config
    )
endif()

if(NOT PROJECT_PATH)
    message(WARNING "No CONFIG_EXAMPLE_* selected yet. Use factory example as a temporary CMake fallback so menuconfig can run.")
    configure_factory_example()
endif()

message(STATUS "Current Project Path: ${PROJECT_PATH}")

set(COMPONENT_ARGS
    SRCS ${SRCS}
    INCLUDE_DIRS ${EXAMPLE_INCLUDE_DIRS}
)

if(EXAMPLE_REQUIRES)
    list(APPEND COMPONENT_ARGS REQUIRES ${EXAMPLE_REQUIRES})
endif()

if(EMBED_FILES)
    idf_component_register(${COMPONENT_ARGS} EMBED_FILES ${EMBED_FILES})
else()
    idf_component_register(${COMPONENT_ARGS})
endif()
