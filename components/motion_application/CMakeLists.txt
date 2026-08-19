idf_component_register(
    SRCS "motion_application_model.c" "motion_application_service.c"
    INCLUDE_DIRS "include"
    REQUIRES command_authority robot_kinematics robot_profile robot_capabilities esp_timer
)
