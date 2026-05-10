idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update config_manager esp_http_client esp_timer json mbedtls nvs_flash wifi_manager)
