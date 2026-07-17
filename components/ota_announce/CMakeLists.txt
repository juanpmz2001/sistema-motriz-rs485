idf_component_register(SRCS "ota_announce.c"
                    INCLUDE_DIRS "include"
                    REQUIRES config_manager wifi_manager ota_manager robot_control json esp_system lwip)
