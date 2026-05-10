idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES config_manager esp_http_client json)
