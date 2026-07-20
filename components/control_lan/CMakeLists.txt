idf_component_register(SRCS "control_lan.c"
                    INCLUDE_DIRS "include"
                    REQUIRES config_manager json esp_timer lwip)
