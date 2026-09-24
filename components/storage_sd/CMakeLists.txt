idf_component_register(
    SRCS "storage_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support fatfs freertos sdmmc t_panel_p4_bsp vfs
)
