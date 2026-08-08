idf_component_register(SRCS "robot_composition.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_timer esp_driver_uart
                                actuation_coordinator robot_capabilities
                                robot_profile robot_driver_factory robot_control
                                bus_transport rs485_transport svd48
                                svd48_channel_endpoint_adapter
                                as5600_device as5600_position_endpoint_adapter
                                i2c_bitbang_transport motor_mode_pwm
                                steering_position_controller
                                steering_position_endpoint_adapter)
