idf_component_register(
    SRCS "axp517.c"                  # 源文件
    INCLUDE_DIRS "."                 # 头文件目录，告诉构建系统去哪里找.h文件
    REQUIRES driver                  # 依赖的其它组件，这里以依赖ESP-IDF的driver组件为例
)