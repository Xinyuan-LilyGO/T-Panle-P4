idf_component_register(
    SRCS "lvgl_page_manager.c"                 # 源文件
    INCLUDE_DIRS "."                 # 头文件目录，告诉构建系统去哪里找.h文件
    REQUIRES driver espressif__esp_io_expander t_panle_p4_config esp_lcd sheldonix__esp_io_expander_xl9555 espressif__esp_lcd_jd9365 lvgl
)