idf_component_get_property(SENSORLIB_DIR lewisxhe__sensorlib COMPONENT_DIR)

idf_component_register(
    SRCS
        "drv2605_driver.cpp"
        "${SENSORLIB_DIR}/src/haptic_drivers/HapticDriver_DRV2605.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c lewisxhe__sensorlib
)

set_source_files_properties(
    "${SENSORLIB_DIR}/src/haptic_drivers/HapticDriver_DRV2605.cpp"
    PROPERTIES COMPILE_OPTIONS
        "-include${CMAKE_CURRENT_LIST_DIR}/sensorlib_espidf_compat.h"
)
