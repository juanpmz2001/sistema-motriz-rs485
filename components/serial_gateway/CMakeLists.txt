idf_component_register(SRCS "serial_gateway.c"
                    INCLUDE_DIRS "include"
                    REQUIRES robot_control config_manager app_update esp_system)
