idf_component_register(SRCS "wifi_manager.c" "wifi_manager_softap_profile.c"
                    INCLUDE_DIRS "include"
                    REQUIRES config_manager esp_wifi esp_netif esp_event)
