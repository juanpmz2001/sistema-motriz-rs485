idf_component_register(SRCS "bluetooth_controller.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt esp_timer nvs_flash
                    PRIV_REQUIRES driver) 