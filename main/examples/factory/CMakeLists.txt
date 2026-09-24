set(FACTORY_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FACTORY_PAGES_DIR "${FACTORY_DIR}/ui/pages")

list(APPEND EXAMPLE_SRCS
    "${FACTORY_DIR}/app/main.c"
    "${FACTORY_DIR}/service/bmu.c"
    "${FACTORY_DIR}/service/button.c"
    "${FACTORY_DIR}/service/lcd.c"
    "${FACTORY_DIR}/service/storage.c"
    "${FACTORY_DIR}/service/system.c"
    "${FACTORY_DIR}/service/audio.c"
    "${FACTORY_DIR}/service/music_cover.c"
    "${FACTORY_DIR}/service/music_lyrics.c"
    "${FACTORY_DIR}/service/camera.c"
    "${FACTORY_DIR}/ui/core/ui.c"
    "${FACTORY_DIR}/ui/core/ui_widgets.c"
    "${FACTORY_DIR}/ui/core/status_bar.c"
)

list(APPEND EXAMPLE_EMBED_FILES
    "${FACTORY_DIR}/ui/assets/audio/lilygo.mp3"
)

if(T_PANEL_P4_BOARD STREQUAL "rect")
    set(FACTORY_BOARD_PAGES_DIR "${FACTORY_PAGES_DIR}/rect")
    list(APPEND EXAMPLE_EMBED_FILES "${FACTORY_DIR}/ui/assets/image/galaxy.jpg")
    list(APPEND EXAMPLE_SRCS "${FACTORY_DIR}/service/motion_sensor.c"
        "${FACTORY_DIR}/service/haptic_motor.c")
elseif(T_PANEL_P4_BOARD STREQUAL "round")
    set(FACTORY_BOARD_PAGES_DIR "${FACTORY_PAGES_DIR}/round")
    list(APPEND EXAMPLE_EMBED_FILES "${FACTORY_DIR}/ui/assets/image/black_gray_gradient.jpg")
else()
    # Standard is also the fallback during the early dependency scan.
    set(FACTORY_BOARD_PAGES_DIR "${FACTORY_PAGES_DIR}/standrad")
    list(APPEND EXAMPLE_EMBED_FILES "${FACTORY_DIR}/ui/assets/image/black_gray_gradient.jpg")
endif()

file(GLOB FACTORY_SHARED_PAGE_SRCS "${FACTORY_PAGES_DIR}/*.c")
file(GLOB FACTORY_BOARD_PAGE_SRCS "${FACTORY_BOARD_PAGES_DIR}/*.c")
file(GLOB FACTORY_FONT_SRCS "${FACTORY_DIR}/ui/assets/fonts/*.c")
file(GLOB FACTORY_IMAGE_SRCS "${FACTORY_DIR}/ui/assets/image/*.c")
list(APPEND EXAMPLE_SRCS
    ${FACTORY_SHARED_PAGE_SRCS}
    ${FACTORY_BOARD_PAGE_SRCS}
    ${FACTORY_FONT_SRCS}
    ${FACTORY_IMAGE_SRCS}
)

list(APPEND EXAMPLE_INCLUDE_DIRS
    "${FACTORY_DIR}"
    "${FACTORY_DIR}/app"
    "${FACTORY_DIR}/config"
    "${FACTORY_DIR}/service"
    "${FACTORY_DIR}/ui/core"
    "${FACTORY_DIR}/ui/pages"
    "${FACTORY_BOARD_PAGES_DIR}"
    "${FACTORY_DIR}/ui/assets"
)

list(APPEND EXAMPLE_REQUIRES
    audio_codec
    esp_driver_i2c
    esp_driver_i2s
    esp_codec_dev
    esp_audio_codec
    esp_audio_simple_player
    display_panel
    touch_panel
    sgm38121
    axp517
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
    lvgl_page_manager
    esp_mm
    esp_video
    esp_h264
    esp_driver_jpeg
    esp_driver_isp
    esp_driver_ppa
    camera_ov2710
    qmi8658c_driver
    drv2605
    t_panel_p4_bsp
)

if(NOT T_PANEL_P4_BOARD STREQUAL "rect")
    list(APPEND EXAMPLE_SRCS
        "${FACTORY_DIR}/service/lora.c"
    )
    list(APPEND EXAMPLE_REQUIRES
        esp32c5_sdio_slave
        esp_event
        esp_netif
        esp_wifi
        radio_hal
    )
endif()
