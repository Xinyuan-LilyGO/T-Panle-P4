idf_component_register(
    SRCS "lvgl_page_manager.c"                 # 源文件
    INCLUDE_DIRS "."                 # 头文件目录，告诉构建系统去哪里找.h文件
    REQUIRES driver t_panel_p4_bsp esp_lcd display_panel lvgl
)
