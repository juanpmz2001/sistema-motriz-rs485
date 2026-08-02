idf_component_register(SRCS "robot_safety.c"
                    INCLUDE_DIRS "include"
                    REQUIRES robot_control actuation_coordinator ibus_receiver svd48)
