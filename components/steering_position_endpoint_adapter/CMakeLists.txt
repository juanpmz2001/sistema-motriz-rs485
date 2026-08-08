idf_component_register(
    SRCS "steering_position_endpoint_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES as5600_device robot_capabilities steering_position_controller)
