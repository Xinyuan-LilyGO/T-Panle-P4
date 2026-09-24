cmake_minimum_required(VERSION 3.16)

set(T_PANEL_P4_BOARD "standard" CACHE STRING "Board variant: standard, round, or rect")
set_property(CACHE T_PANEL_P4_BOARD PROPERTY STRINGS standard round rect)

set(T_PANEL_P4_EXAMPLE "factory" CACHE STRING "Example to build")
set_property(CACHE T_PANEL_P4_EXAMPLE PROPERTY STRINGS
    factory
    xl9555
    lora_sx1276
    lora_sx1262
    lora_lr1121
    lora_lr2021
    axp517
    lcd
    lcd1
    lcd_touch
    audio_play_wav
    audio_record
    audio_loopback
    camera
    camera_esp_video
    camera_flash_led
    esp32p4_host_wifi
    esp32p4_host_test
    sd
    sgm38121
    drv2605
    qmc6309
    qmi8658c
    lvgl
    usb_otg_msc
    codex_keyboard
)

string(TOLOWER "${T_PANEL_P4_BOARD}" T_PANEL_P4_BOARD)
string(TOLOWER "${T_PANEL_P4_EXAMPLE}" T_PANEL_P4_EXAMPLE)

set(_supported_boards standard round rect)
list(FIND _supported_boards "${T_PANEL_P4_BOARD}" _board_index)
if(_board_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported T_PANEL_P4_BOARD='${T_PANEL_P4_BOARD}'. "
        "Choose one of: ${_supported_boards}")
endif()

set(_example_names
    factory
    xl9555
    lora_sx1276
    lora_sx1262
    lora_lr1121
    lora_lr2021
    axp517
    lcd
    lcd1
    lcd_touch
    audio_play_wav
    audio_record
    audio_loopback
    camera
    camera_esp_video
    camera_flash_led
    esp32p4_host_wifi
    esp32p4_host_test
    sd
    sgm38121
    drv2605
    qmc6309
    qmi8658c
    lvgl
    usb_otg_msc
    codex_keyboard
)
set(_example_dirs
    factory
    xl9555
    lora/sx1276
    lora/sx1262
    lora/lr1121
    lora/lr2021
    axp517
    lcd
    lcd1
    lcd_touch
    audio/play_wav
    audio/record
    audio/loopback
    camera
    camera_esp_video
    camera_flash_led
    sdio/esp32p4_host_wifi
    sdio/esp32p4_host_test
    sd
    sgm38121
    drv2605
    qmc6309
    qmi8658c
    lvgl
    usb_otg_msc
    codex_keyboard
)

list(FIND _example_names "${T_PANEL_P4_EXAMPLE}" _example_index)
if(_example_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported T_PANEL_P4_EXAMPLE='${T_PANEL_P4_EXAMPLE}'. "
        "Choose one of: ${_example_names}")
endif()

if(T_PANEL_P4_EXAMPLE MATCHES "^(camera_flash_led|drv2605|qmc6309|qmi8658c)$" AND
        NOT T_PANEL_P4_BOARD STREQUAL "rect")
    message(FATAL_ERROR
        "Example '${T_PANEL_P4_EXAMPLE}' is only supported by the rect board")
endif()
list(GET _example_dirs ${_example_index} _example_relative_dir)

if(T_PANEL_P4_EXAMPLE STREQUAL "lcd1" AND NOT T_PANEL_P4_BOARD STREQUAL "round")
    message(FATAL_ERROR "Example 'lcd1' is only supported by the round board")
endif()

set(_standard_round_only_examples
    xl9555
    lora_sx1276
    lora_sx1262
    lora_lr1121
    lora_lr2021
    esp32p4_host_wifi
    esp32p4_host_test
)
if(T_PANEL_P4_BOARD STREQUAL "rect")
    list(FIND _standard_round_only_examples "${T_PANEL_P4_EXAMPLE}" _unsupported_index)
    if(NOT _unsupported_index EQUAL -1)
        message(FATAL_ERROR
            "Example '${T_PANEL_P4_EXAMPLE}' requires XL9555, LoRa, or ESP32-C5 "
            "hardware and cannot be built for the rect board")
    endif()
endif()

set(_example_dir "${CMAKE_CURRENT_LIST_DIR}/main/examples/${_example_relative_dir}")
set(_example_cmake "${_example_dir}/CMakeLists.txt")
set(_example_manifest "${_example_dir}/idf_component.yml")
set(_example_board_manifest "${_example_dir}/idf_component.${T_PANEL_P4_BOARD}.yml")
if(EXISTS "${_example_board_manifest}")
    set(_example_manifest "${_example_board_manifest}")
endif()
set(_example_defaults "${_example_dir}/sdkconfig.defaults")
foreach(_required_file ${_example_cmake} ${_example_manifest} ${_example_defaults})
    if(NOT EXISTS "${_required_file}")
        message(FATAL_ERROR "Missing selected-example file: ${_required_file}")
    endif()
endforeach()

set(_sdkconfig_defaults
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/components/t_panel_p4_bsp/configs/T-Panel-P4.defaults"
    "${_example_defaults}"
)
if(T_PANEL_P4_BOARD STREQUAL "round")
    list(REMOVE_ITEM _sdkconfig_defaults
        "${CMAKE_CURRENT_LIST_DIR}/components/t_panel_p4_bsp/configs/T-Panel-P4.defaults")
    list(INSERT _sdkconfig_defaults 1
        "${CMAKE_CURRENT_LIST_DIR}/components/t_panel_p4_bsp/configs/T-Panel-P4-Round.defaults")
elseif(T_PANEL_P4_BOARD STREQUAL "rect")
    list(REMOVE_ITEM _sdkconfig_defaults
        "${CMAKE_CURRENT_LIST_DIR}/components/t_panel_p4_bsp/configs/T-Panel-P4.defaults")
    list(INSERT _sdkconfig_defaults 1
        "${CMAKE_CURRENT_LIST_DIR}/components/t_panel_p4_bsp/configs/T-Panel-P4-Rect.defaults")
endif()

set(_example_board_defaults "${_example_dir}/sdkconfig.${T_PANEL_P4_BOARD}.defaults")
if(EXISTS "${_example_board_defaults}")
    list(APPEND _sdkconfig_defaults "${_example_board_defaults}")
endif()
if(NOT T_PANEL_P4_BOARD STREQUAL "rect")
    set(_example_standard_round_defaults "${_example_dir}/sdkconfig.standard_round.defaults")
    if(EXISTS "${_example_standard_round_defaults}")
        list(APPEND _sdkconfig_defaults "${_example_standard_round_defaults}")
    endif()
endif()
if(T_PANEL_P4_EXTRA_SDKCONFIG_DEFAULTS)
    list(APPEND _sdkconfig_defaults ${T_PANEL_P4_EXTRA_SDKCONFIG_DEFAULTS})
endif()

# A build directory belongs to exactly one board/example combination.
set(_build_variant "${T_PANEL_P4_BOARD}:${T_PANEL_P4_EXAMPLE}")
set(_build_variant_file "${CMAKE_BINARY_DIR}/t_panel_p4.variant")
if(EXISTS "${_build_variant_file}")
    file(READ "${_build_variant_file}" _configured_build_variant)
    string(STRIP "${_configured_build_variant}" _configured_build_variant)
    if(NOT _configured_build_variant STREQUAL _build_variant)
        message(FATAL_ERROR
            "Build directory is already configured for '${_configured_build_variant}', "
            "not '${_build_variant}'. Use a different -B directory.")
    endif()
else()
    file(WRITE "${_build_variant_file}" "${_build_variant}\n")
endif()

# Keep this form compatible with the ESP-IDF VS Code extension, which parses
# this assignment directly and only expands CMAKE_BINARY_DIR.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${_sdkconfig_defaults}")

# Materialize the selected example as the project's only main component. This
# lets the component manager resolve only that example's manifest dependencies.
set(T_PANEL_P4_EXAMPLE_CMAKE "${_example_cmake}")
set(_generated_main_dir "${CMAKE_BINARY_DIR}/selected_example/main")
file(MAKE_DIRECTORY "${_generated_main_dir}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/selected_example/CMakeLists.txt.in"
    "${_generated_main_dir}/CMakeLists.txt"
    @ONLY
)
configure_file("${_example_manifest}" "${_generated_main_dir}/idf_component.yml" COPYONLY)

# Only expose the local components used by the selected example. The component
# manager inspects every visible manifest, including components outside the
# minimal-build dependency closure.
set(_local_components)
if(NOT T_PANEL_P4_EXAMPLE STREQUAL "lcd1")
    list(APPEND _local_components t_panel_p4_bsp)
endif()
if(T_PANEL_P4_EXAMPLE STREQUAL "factory")
    list(APPEND _local_components
        audio_codec
        axp517
        camera_ov2710
        display_panel
        lvgl_page_manager
        sensor/drv2605
        sensor/qmi8658c_driver
        sgm38121
        touch_panel
    )
    if(NOT T_PANEL_P4_BOARD STREQUAL "rect")
        list(APPEND _local_components esp32c5_sdio_slave radio_hal)
    endif()
elseif(T_PANEL_P4_EXAMPLE MATCHES "^lora_")
    list(APPEND _local_components radio_hal)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "axp517")
    list(APPEND _local_components axp517)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "sgm38121")
    list(APPEND _local_components sgm38121)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "drv2605")
    list(APPEND _local_components sensor/drv2605)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "qmc6309")
    list(APPEND _local_components sensor/qmc6309)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "qmi8658c")
    list(APPEND _local_components sensor/qmi8658c_driver)
elseif(T_PANEL_P4_EXAMPLE STREQUAL "sd")
    list(APPEND _local_components storage_sd)
elseif(T_PANEL_P4_EXAMPLE MATCHES "^audio_")
    list(APPEND _local_components audio_codec)
elseif(T_PANEL_P4_EXAMPLE MATCHES "^(lcd|lcd_touch|lvgl|codex_keyboard)$")
    list(APPEND _local_components display_panel touch_panel)
elseif(T_PANEL_P4_EXAMPLE MATCHES "^(camera|camera_esp_video)$")
    list(APPEND _local_components camera_ov2710 display_panel sgm38121)
endif()

set(COMPONENT_DIRS
    "$ENV{IDF_PATH}/components"
    "${_generated_main_dir}"
)
foreach(_local_component ${_local_components})
    list(APPEND COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/components/${_local_component}")
endforeach()

set(_build_dependencies_lock "${CMAKE_BINARY_DIR}/dependencies.lock")
if(NOT EXISTS "${_build_dependencies_lock}" AND
        EXISTS "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock"
        "${_build_dependencies_lock}"
        COPYONLY
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${_build_dependencies_lock}")
idf_build_set_property(MINIMAL_BUILD ON)
project(T-Panel-P4-${T_PANEL_P4_BOARD}-${T_PANEL_P4_EXAMPLE})

set(_merged_firmware "${CMAKE_BINARY_DIR}/${PROJECT_NAME}-merged.bin")
add_custom_target(merged_firmware ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/merged-binary.bin"
        "${_merged_firmware}"
    DEPENDS merge-bin
    BYPRODUCTS "${_merged_firmware}"
    COMMENT "Creating merged firmware: ${PROJECT_NAME}-merged.bin"
    VERBATIM
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-unused-const-variable" APPEND)

message(STATUS "T-Panel-P4 board: ${T_PANEL_P4_BOARD}")
message(STATUS "T-Panel-P4 example: ${T_PANEL_P4_EXAMPLE}")
message(STATUS "T-Panel-P4 sdkconfig: ${SDKCONFIG}")
