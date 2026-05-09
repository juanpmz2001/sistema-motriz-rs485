idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES svd48 robot_control serial_gateway config_manager nvs_flash)
