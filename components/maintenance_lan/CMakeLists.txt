idf_component_register(SRCS "maintenance_lan.c"
                    INCLUDE_DIRS "include"
                    REQUIRES config_manager wifi_manager serial_gateway json esp_system lwip)
