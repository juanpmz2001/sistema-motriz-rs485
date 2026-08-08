idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES svd48 robot_profile robot_composition actuation_coordinator robot_control robot_safety robot_state serial_gateway config_manager wifi_manager ota_manager ota_announce maintenance_lan ibus_receiver nvs_flash)

add_custom_target(botfarms_firmware_identity ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DBOTFARMS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBOTFARMS_OUTPUT_FILE=${BOTFARMS_FW_IDENTITY_HEADER}"
            "-DBOTFARMS_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DBOTFARMS_SHA_OVERRIDE=${BOTFARMS_FW_GIT_SHA_OVERRIDE}"
            "-DBOTFARMS_DIRTY_OVERRIDE=${BOTFARMS_FW_GIT_DIRTY_OVERRIDE}"
            -P "${BOTFARMS_FW_IDENTITY_SCRIPT}"
    BYPRODUCTS "${BOTFARMS_FW_IDENTITY_HEADER}"
    COMMENT "Refreshing firmware Git identity"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} botfarms_firmware_identity)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_BINARY_DIR}/generated")
