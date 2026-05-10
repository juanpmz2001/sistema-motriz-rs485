idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_update config_manager esp_http_client json mbedtls nvs_flash)
