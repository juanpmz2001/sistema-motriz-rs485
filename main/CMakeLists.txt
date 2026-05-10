idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES svd48 robot_control serial_gateway config_manager wifi_manager ota_manager nvs_flash)
