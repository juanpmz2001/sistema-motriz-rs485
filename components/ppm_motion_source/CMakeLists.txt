idf_component_register(
    SRCS "ppm_motion_model.c" "ppm_motion_source.c"
    INCLUDE_DIRS "include"
    REQUIRES ibus_receiver motion_application robot_profile robot_capabilities esp_timer
)
