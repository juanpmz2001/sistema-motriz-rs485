idf_component_register(SRCS "svd48.c" "svd48_protocol.c" "svd48_device.c"
                            "svd48_poll_service.c" "svd48_poll_task.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bus_transport communication_quality driver esp_timer freertos)
