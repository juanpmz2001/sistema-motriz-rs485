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
    NAME firmware_contracts_test
    SOURCES
        host/firmware_contracts_test.c
        ../components/svd48/svd48_protocol.c
        ../components/serial_gateway/serial_gateway_framing.c
        ../components/serial_gateway/serial_gateway_result.c
    INCLUDE_DIRS
        ../components/svd48/include
        ../components/serial_gateway/include
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
