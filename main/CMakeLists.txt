idf_component_register(SRCS "new_leg_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES new_leg_test config_manager wifi_manager ota_manager nvs_flash esp_system driver)
