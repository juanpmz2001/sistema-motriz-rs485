idf_component_register(SRCS "serial_gateway.c" "serial_gateway_framing.c" "serial_gateway_policy.c" "serial_gateway_result.c"
                    INCLUDE_DIRS "include"
                    REQUIRES robot_control robot_capabilities robot_safety config_manager wifi_manager ota_manager ota_announce ibus_receiver as5600_device app_update esp_system esp_timer)
