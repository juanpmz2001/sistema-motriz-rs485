idf_component_register(SRCS "rs485_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bus_transport esp_driver_uart esp_timer freertos)
