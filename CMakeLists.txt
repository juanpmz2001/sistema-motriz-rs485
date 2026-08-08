# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(BOTFARMS_FW_GIT_SHA_OVERRIDE "" CACHE STRING
    "Reproducible firmware Git SHA override (40 hex characters or UNKNOWN)")
set(BOTFARMS_FW_GIT_DIRTY_OVERRIDE "" CACHE STRING
    "Reproducible firmware dirty-state override (0 or 1)")

find_package(Git QUIET)

if(NOT BOTFARMS_FW_GIT_SHA_OVERRIDE STREQUAL "")
    string(LENGTH "${BOTFARMS_FW_GIT_SHA_OVERRIDE}"
           BOTFARMS_FW_GIT_SHA_OVERRIDE_LENGTH)
    if(NOT BOTFARMS_FW_GIT_SHA_OVERRIDE STREQUAL "UNKNOWN" AND
       (NOT BOTFARMS_FW_GIT_SHA_OVERRIDE MATCHES "^[0-9a-fA-F]+$" OR
        NOT BOTFARMS_FW_GIT_SHA_OVERRIDE_LENGTH EQUAL 40))
        message(FATAL_ERROR
                "BOTFARMS_FW_GIT_SHA_OVERRIDE must be 40 hex characters or UNKNOWN")
    endif()
endif()

if(NOT BOTFARMS_FW_GIT_DIRTY_OVERRIDE STREQUAL "")
    if(NOT BOTFARMS_FW_GIT_DIRTY_OVERRIDE MATCHES "^[01]$")
        message(FATAL_ERROR
                "BOTFARMS_FW_GIT_DIRTY_OVERRIDE must be 0 or 1")
    endif()
endif()

if(BOTFARMS_FW_GIT_SHA_OVERRIDE STREQUAL "UNKNOWN" AND
   BOTFARMS_FW_GIT_DIRTY_OVERRIDE STREQUAL "0")
    message(FATAL_ERROR
            "UNKNOWN firmware Git SHA cannot be reported as a clean build identity")
endif()

set(BOTFARMS_FW_IDENTITY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_firmware_identity.cmake")
set(BOTFARMS_FW_IDENTITY_HEADER
    "${CMAKE_BINARY_DIR}/generated/botfarms_firmware_identity.h")

# Create the header once during configure so dependency scanning can open it. The
# component target below regenerates it on every build, which prevents an incremental
# source edit from retaining a stale GIT_DIRTY value.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DBOTFARMS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DBOTFARMS_OUTPUT_FILE=${BOTFARMS_FW_IDENTITY_HEADER}"
            "-DBOTFARMS_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DBOTFARMS_SHA_OVERRIDE=${BOTFARMS_FW_GIT_SHA_OVERRIDE}"
            "-DBOTFARMS_DIRTY_OVERRIDE=${BOTFARMS_FW_GIT_DIRTY_OVERRIDE}"
            -P "${BOTFARMS_FW_IDENTITY_SCRIPT}"
    RESULT_VARIABLE BOTFARMS_FW_IDENTITY_RESULT)
if(NOT BOTFARMS_FW_IDENTITY_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to generate firmware identity header")
endif()

set(COMPONENTS
    main
    svd48
    robot_control
    robot_safety
    robot_state
    command_authority
    robot_kinematics
    serial_gateway
    ibus_receiver
    ota_announce
    maintenance_lan
    control_lan
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sistema-motriz-rs485)
