idf_component_register(
    SRCS "camera_ov2710.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_cam_sensor
        esp_driver_i2c
        esp_hw_support
        esp_video
        sgm38121
        t_panel_p4_bsp
        vfs
)
