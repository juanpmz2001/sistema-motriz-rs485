idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES svd48 robot_control robot_safety robot_state serial_gateway config_manager wifi_manager ota_manager ota_announce maintenance_lan ibus_receiver nvs_flash)
