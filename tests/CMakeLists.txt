cmake_minimum_required(VERSION 3.16)

project(botfarms_firmware_host_tests LANGUAGES C)

option(BOTFARMS_HOST_TEST_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
set(BOTFARMS_HOST_TEST_TIMEOUT_SECONDS "10" CACHE STRING "Per-test timeout in seconds")

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake/host_tests")
include(BotfarmsHostTest)

add_library(botfarms_host_test_support STATIC
    host/host_test.c
    fakes/fake_clock.c
    fakes/fake_event_sink.c
)
botfarms_configure_host_c_target(botfarms_host_test_support)
target_include_directories(botfarms_host_test_support PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/host"
    "${CMAKE_CURRENT_LIST_DIR}/fakes"
)

botfarms_add_host_test(
    NAME host_test_support_test
    SOURCES host/host_test_support_test.c
)

botfarms_add_host_test(
    NAME robot_state_model_test
    SOURCES
        host/robot_state_model_test.c
        ../components/robot_state/robot_state_model.c
    INCLUDE_DIRS
        ../components/robot_state/include
)

botfarms_add_host_test(
    NAME command_authority_model_test
    SOURCES
        host/command_authority_model_test.c
        ../components/command_authority/command_authority_model.c
    INCLUDE_DIRS
        ../components/command_authority/include
)

botfarms_add_host_test(
    NAME robot_kinematics_test
    SOURCES
        host/robot_kinematics_test.c
        ../components/robot_kinematics/robot_kinematics.c
    INCLUDE_DIRS
        ../components/robot_kinematics/include
    LINK_LIBRARIES
        m
)

botfarms_add_host_test(
    NAME firmware_contracts_test
    SOURCES
        host/firmware_contracts_test.c
        ../components/svd48/svd48_protocol.c
        ../components/serial_gateway/serial_gateway_framing.c
        ../components/serial_gateway/serial_gateway_policy.c
        ../components/serial_gateway/serial_gateway_result.c
    INCLUDE_DIRS
        ../components/svd48/include
        ../components/serial_gateway/include
)

botfarms_add_host_test(
    NAME ppm_decoder_model_test
    SOURCES
        host/ppm_decoder_model_test.c
        ../components/ppm_decoder/ppm_decoder_model.c
    INCLUDE_DIRS
        ../components/ppm_decoder/include
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME svd48_protocol_reference_test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/test_svd48_protocol.py"
)
set_tests_properties(svd48_protocol_reference_test PROPERTIES
    LABELS "host;protocol"
    TIMEOUT "${BOTFARMS_HOST_TEST_TIMEOUT_SECONDS}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

botfarms_add_host_test(
    NAME layered_actuation_test
    SOURCES
        host/layered_actuation_test.c
        ../components/robot_capabilities/robot_capabilities.c
        ../components/robot_profile/robot_profile.c
        ../components/actuation_coordinator/actuation_coordinator.c
        ../components/robot_control_endpoint_adapter/robot_control_endpoint_adapter.c
    INCLUDE_DIRS
        ../components/robot_capabilities/include
        ../components/robot_profile/include
        ../components/actuation_coordinator/include
        ../components/robot_control_endpoint_adapter/include
    LINK_LIBRARIES m pthread
)

add_test(
    NAME dependency_contracts_test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/test_dependency_contracts.py"
)
set_tests_properties(dependency_contracts_test PROPERTIES LABELS "host;architecture")
