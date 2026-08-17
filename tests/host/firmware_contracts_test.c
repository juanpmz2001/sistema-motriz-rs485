#include <stdint.h>
#include <string.h>

#include "host_test.h"
#include "serial_gateway_framing.h"
#include "serial_gateway_policy.h"
#include "serial_gateway_result.h"
#include "svd48_protocol.h"

static void append_test_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = svd48_crc16_uumotor(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc >> 8);
    frame[payload_length + 1] = (uint8_t)(crc & 0xFF);
}

static bool test_svd48_request_builders(void)
{
    const uint8_t expected_read[] = { 0xEE, 0x03, 0x54, 0x10, 0x00, 0x02, 0x61, 0xC3 };
    const uint8_t expected_write[] = { 0x01, 0x06, 0x53, 0x04, 0x00, 0x64, 0xA4, 0xD8 };
    const uint8_t expected_multi[] = {
        0xEE, 0x10, 0x51, 0x00, 0x00, 0x02, 0x04, 0x00, 0x02, 0x00, 0x02, 0xEA, 0x45,
    };
    uint8_t frame[32] = { 0 };

    HOST_TEST_CHECK(svd48_build_read_request(0xEE, 0x5410, 2, frame) == sizeof(expected_read));
    HOST_TEST_CHECK(memcmp(frame, expected_read, sizeof(expected_read)) == 0);
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(frame, sizeof(expected_read)));
    HOST_TEST_CHECK(svd48_build_read_request(0U, 0x5410U, 2U, frame) == 0U);
    HOST_TEST_CHECK(svd48_build_read_request(248U, 0x5410U, 2U, frame) == 0U);
    HOST_TEST_CHECK(svd48_build_read_request(1U,
                                             0x5410U,
                                             SVD48_READ_MAX_REGISTERS + 1U,
                                             frame) == 0U);
    HOST_TEST_CHECK(svd48_build_read_request(1U, 0xFFFFU, 2U, frame) == 0U);

    HOST_TEST_CHECK(svd48_build_write_single_request(0x01, 0x5304, 100, frame) == sizeof(expected_write));
    HOST_TEST_CHECK(memcmp(frame, expected_write, sizeof(expected_write)) == 0);
    HOST_TEST_CHECK(svd48_build_write_single_request(0U,
                                                     0x5304U,
                                                     100U,
                                                     frame) == 0U);

    const uint16_t values[] = { 2, 2 };
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0xEE, 0x5100, values, 2, frame, sizeof(frame)) ==
          sizeof(expected_multi));
    HOST_TEST_CHECK(memcmp(frame, expected_multi, sizeof(expected_multi)) == 0);
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(frame, sizeof(expected_multi)));
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0xEE, 0x5100, values, 0, frame, sizeof(frame)) == 0);
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0xEE, 0x5100, values, 2, frame, 12) == 0);
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0xEE, 0x5100, NULL, 2, frame, sizeof(frame)) == 0);
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0xEE, 0x5100, values, 2, NULL, sizeof(frame)) == 0);
    HOST_TEST_CHECK(!svd48_write_multiple_range_is_valid(0x0000, 0));
    HOST_TEST_CHECK(svd48_write_multiple_range_is_valid(0xFFFF, 1));
    HOST_TEST_CHECK(!svd48_write_multiple_range_is_valid(0xFFFF, 2));
    HOST_TEST_CHECK(svd48_write_multiple_range_is_valid(0xFF85, SVD48_WRITE_MULTIPLE_MAX_REGISTERS));
    HOST_TEST_CHECK(!svd48_write_multiple_range_is_valid(0xFF86, SVD48_WRITE_MULTIPLE_MAX_REGISTERS));

    uint16_t many_values[SVD48_WRITE_MULTIPLE_MAX_REGISTERS];
    uint8_t large_frame[SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE + 1];
    for (size_t i = 0; i < SVD48_WRITE_MULTIPLE_MAX_REGISTERS; i++) {
        many_values[i] = (uint16_t)i;
    }
    const uint16_t quantities[] = { 1, 2, 8, 32, SVD48_WRITE_MULTIPLE_MAX_REGISTERS };
    for (size_t i = 0; i < sizeof(quantities) / sizeof(quantities[0]); i++) {
        size_t expected_length = 9U + (size_t)quantities[i] * 2U;
        HOST_TEST_CHECK(svd48_build_write_multiple_request(0x01,
                                                 0x5100,
                                                 many_values,
                                                 quantities[i],
                                                 large_frame,
                                                 sizeof(large_frame)) == expected_length);
        HOST_TEST_CHECK(svd48_frame_has_valid_crc(large_frame, expected_length));
    }
    memset(large_frame, 0xA5, sizeof(large_frame));
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0x01,
                                             0xFF85,
                                             many_values,
                                             SVD48_WRITE_MULTIPLE_MAX_REGISTERS,
                                             large_frame,
                                             SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE) ==
          SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE);
    HOST_TEST_CHECK(large_frame[SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE] == 0xA5);
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(large_frame, SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE));
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0x01,
                                             0x5100,
                                             many_values,
                                             SVD48_WRITE_MULTIPLE_MAX_REGISTERS + 1,
                                             large_frame,
                                             sizeof(large_frame)) == 0);
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0x01,
                                             0xFF86,
                                             many_values,
                                             SVD48_WRITE_MULTIPLE_MAX_REGISTERS,
                                             large_frame,
                                             sizeof(large_frame)) == 0);
    HOST_TEST_CHECK(svd48_build_write_multiple_request(0x01,
                                             0x5100,
                                             many_values,
                                             SVD48_WRITE_MULTIPLE_MAX_REGISTERS,
                                             large_frame,
                                             SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE - 1) == 0);
    return true;
}

static bool test_svd48_write_multiple_response(void)
{
    const uint8_t frame[SVD48_WRITE_MULTIPLE_RESPONSE_SIZE] = {
        0xEE, SVD48_FUNC_WRITE_MULTI, 0x51, 0x00, 0x00, 0x02, 0xAB, 0x47,
    };
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(frame, sizeof(frame)));

    svd48_write_multiple_response_t response = { 0 };
    HOST_TEST_CHECK(svd48_parse_write_multiple_response(frame,
                                              sizeof(frame),
                                              0xEE,
                                              0x5100,
                                              2,
                                              &response));
    HOST_TEST_CHECK(response.start_register == 0x5100);
    HOST_TEST_CHECK(response.quantity == 2);

    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(NULL,
                                               sizeof(frame),
                                               0xEE,
                                               0x5100,
                                               2,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0xEE,
                                               0x5100,
                                               2,
                                               NULL));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame) - 1,
                                               0xEE,
                                               0x5100,
                                               2,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0x01,
                                               0x5100,
                                               2,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0xEE,
                                               0x5101,
                                               2,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0xEE,
                                               0x5100,
                                               1,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0xEE,
                                               0x5100,
                                               0,
                                               &response));
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(frame,
                                               sizeof(frame),
                                               0xEE,
                                               0xFFFF,
                                               2,
                                               &response));

    uint8_t malformed[SVD48_WRITE_MULTIPLE_RESPONSE_SIZE];
    memcpy(malformed, frame, sizeof(malformed));
    malformed[1] = SVD48_FUNC_WRITE_SINGLE;
    append_test_crc(malformed, sizeof(malformed) - 2);
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(malformed,
                                               sizeof(malformed),
                                               0xEE,
                                               0x5100,
                                               2,
                                               &response));

    memcpy(malformed, frame, sizeof(malformed));
    malformed[sizeof(malformed) - 1] ^= 0x01;
    HOST_TEST_CHECK(!svd48_parse_write_multiple_response(malformed,
                                               sizeof(malformed),
                                               0xEE,
                                               0x5100,
                                               2,
                                               &response));
    return true;
}

static bool test_svd48_exceptions(void)
{
    const uint8_t functions[] = { 0x83, 0x86, 0x90 };
    const uint8_t requests[] = { 0x03, 0x06, 0x10 };

    for (size_t i = 0; i < sizeof(functions); i++) {
        uint8_t frame[5] = { 0x01, functions[i], 0x02, 0, 0 };
        append_test_crc(frame, 3);
        svd48_exception_response_t exception = { 0 };
        HOST_TEST_CHECK(svd48_parse_exception_response(frame, sizeof(frame), 0x01, requests[i], &exception));
        HOST_TEST_CHECK(exception.function == functions[i]);
        HOST_TEST_CHECK(exception.code == 0x02);
        HOST_TEST_CHECK(!svd48_parse_exception_response(frame, sizeof(frame) - 1, 0x01, requests[i], &exception));

        frame[4] ^= 0x01;
        HOST_TEST_CHECK(!svd48_parse_exception_response(frame, sizeof(frame), 0x01, requests[i], &exception));
    }

    uint8_t frame[5] = { 0x02, 0x83, 0x01, 0, 0 };
    append_test_crc(frame, 3);
    svd48_exception_response_t exception = { 0 };
    HOST_TEST_CHECK(!svd48_parse_exception_response(frame, sizeof(frame), 0x01, 0x03, &exception));
    HOST_TEST_CHECK(!svd48_parse_exception_response(frame, sizeof(frame), 0x02, 0x06, &exception));
    return true;
}

static bool test_svd48_runtime_actuation_register_classification(void)
{
    const uint16_t blocked[] = {
        0x5300U, 0x5301U, 0x5304U, 0x5305U, 0x5308U, 0x5309U,
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        HOST_TEST_CHECK(svd48_register_is_runtime_actuation(blocked[i]));
    }

    HOST_TEST_CHECK(!svd48_register_is_runtime_actuation(0x5018U));
    HOST_TEST_CHECK(!svd48_register_is_runtime_actuation(0x5100U));
    HOST_TEST_CHECK(!svd48_register_is_runtime_actuation(0x5200U));
    HOST_TEST_CHECK(!svd48_register_is_runtime_actuation(0x5302U));
    HOST_TEST_CHECK(!svd48_register_is_runtime_actuation(0x5400U));
    HOST_TEST_CHECK(svd48_register_range_has_runtime_actuation(0x52FFU, 2U));
    HOST_TEST_CHECK(svd48_register_range_has_runtime_actuation(0x5302U, 3U));
    HOST_TEST_CHECK(svd48_register_range_has_runtime_actuation(0x5306U, 4U));
    HOST_TEST_CHECK(!svd48_register_range_has_runtime_actuation(0x5100U, 16U));
    HOST_TEST_CHECK(!svd48_register_range_has_runtime_actuation(0x5302U, 2U));
    HOST_TEST_CHECK(!svd48_register_range_has_runtime_actuation(0x530AU, 1U));
    HOST_TEST_CHECK(!svd48_register_range_has_runtime_actuation(0xFFFFU, 2U));
    return true;
}

static bool test_serial_framing(void)
{
    serial_gateway_line_framer_t framer;
    serial_gateway_line_framer_init(&framer);

    for (size_t i = 0; i < SERIAL_GATEWAY_COMMAND_MAX - 1; i++) {
        HOST_TEST_CHECK(serial_gateway_line_framer_feed(&framer, 'A') == SERIAL_GATEWAY_FRAME_NONE);
    }
    HOST_TEST_CHECK(serial_gateway_line_framer_feed(&framer, '\n') == SERIAL_GATEWAY_FRAME_LINE_READY);
    HOST_TEST_CHECK(strlen(serial_gateway_line_framer_line(&framer)) == SERIAL_GATEWAY_COMMAND_MAX - 1);

    for (size_t i = 0; i < SERIAL_GATEWAY_COMMAND_MAX; i++) {
        HOST_TEST_CHECK(serial_gateway_line_framer_feed(&framer, 'B') == SERIAL_GATEWAY_FRAME_NONE);
    }
    const char *dangerous_suffix = " MOVE_VEL 1 0 0";
    for (const char *cursor = dangerous_suffix; *cursor; cursor++) {
        HOST_TEST_CHECK(serial_gateway_line_framer_feed(&framer, *cursor) == SERIAL_GATEWAY_FRAME_NONE);
    }
    HOST_TEST_CHECK(serial_gateway_line_framer_feed(&framer, '\n') == SERIAL_GATEWAY_FRAME_LINE_TOO_LONG);

    const char *ping = "PING\r";
    serial_gateway_frame_event_t event = SERIAL_GATEWAY_FRAME_NONE;
    for (const char *cursor = ping; *cursor; cursor++) {
        event = serial_gateway_line_framer_feed(&framer, *cursor);
    }
    HOST_TEST_CHECK(event == SERIAL_GATEWAY_FRAME_LINE_READY);
    HOST_TEST_CHECK(strcmp(serial_gateway_line_framer_line(&framer), "PING") == 0);
    return true;
}

static bool test_gateway_error_result(void)
{
    char code[32] = { 0 };
    HOST_TEST_CHECK(serial_gateway_error_code_from_line("ERR LAN_COMMAND_BLOCKED MOVE_VEL", code, sizeof(code)));
    HOST_TEST_CHECK(strcmp(code, "LAN_COMMAND_BLOCKED") == 0);
    HOST_TEST_CHECK(serial_gateway_error_code_from_line("  ERR STOP_FAILED", code, sizeof(code)));
    HOST_TEST_CHECK(strcmp(code, "STOP_FAILED") == 0);
    HOST_TEST_CHECK(serial_gateway_error_code_from_line("ERR", code, sizeof(code)));
    HOST_TEST_CHECK(strcmp(code, "COMMAND_ERROR") == 0);
    HOST_TEST_CHECK(!serial_gateway_error_code_from_line("DATA ERROR:0x00000000", code, sizeof(code)));
    HOST_TEST_CHECK(!serial_gateway_error_code_from_line("ERROR NOT_AN_ERR_LINE", code, sizeof(code)));
    return true;
}

static bool test_gateway_lan_maintenance_policy(void)
{
    const char *read_one[] = { "READ_REG", "1", "0x5018" };
    const char *read_many[] = { "read_reg", "1", "0x5200", "2" };
    const char *probe_address[] = { "SVD48_PROBE", "247" };
    const char *probe_bad_shape[] = { "SVD48_PROBE" };
    const char *typed_config[] = { "GET_SVD48_CONFIG", "1", "M1" };
    const char *write_one[] = { "WRITE_REG", "1", "0x5018", "10", "CONFIRM" };
    const char *write_many[] = { "WRITE_REGS", "1", "0x5200", "0x3f80", "0", "confirm" };
    const char *write_missing_confirm[] = { "WRITE_REG", "1", "0x5018", "10" };
    const char *write_bad_confirm[] = { "WRITE_REGS", "1", "0x5200", "0x3f80", "APPLY" };
    const char *save_config[] = { "SAVE_SVD48_CONFIG", "1", "confirm" };
    const char *save_config_missing_confirm[] = { "SAVE_SVD48_CONFIG", "1" };
    const char *set_gear_ratio[] = { "SET_SVD48_GEAR_RATIO", "1", "1", "5", "confirm" };
    const char *set_gear_ratio_missing_confirm[] = { "SET_SVD48_GEAR_RATIO", "1", "1", "5" };
    const char *identify_status[] = { "SVD48_IDENTIFY_STATUS", "1", "M1" };
    const char *identify_start[] = { "SVD48_IDENTIFY", "1", "M2", "START", "confirm" };
    const char *identify_missing_confirm[] = { "SVD48_IDENTIFY", "1", "M2", "START" };
    const char *move[] = { "MOVE_VEL", "1", "0", "0" };
    const char *set_speed[] = { "SET_SPEED", "2", "5" };
    const char *set_speed_bad_shape[] = { "SET_SPEED", "2" };
    const char *stop_one[] = { "STOP", "0" };
    const char *stop_all[] = { "STOP", "ALL" };
    const char *profile_status[] = { "PROFILE_STATUS" };
    const char *composition_status[] = { "COMPOSITION_STATUS" };
    const char *endpoints[] = { "ENDPOINTS" };
    const char *set_endpoint_speed[] = { "SET_ENDPOINT_SPEED", "1", "5" };
    const char *set_endpoint_position[] = { "SET_ENDPOINT_POSITION", "1", "5" };
    const char *set_endpoint_position_reference[] = {
        "SET_ENDPOINT_POSITION_REFERENCE", "1", "5", "CONFIRM"};
    const char *stop_endpoint[] = { "STOP_ENDPOINT", "1" };
    const char *endpoint_observation[] = { "GET_ENDPOINT_OBSERVATION", "1" };
    const char *position_observation[] = {
        "GET_ENDPOINT_POSITION_OBSERVATION", "1"};
    const char *as5600_diagnostics[] = {
        "GET_AS5600_DIAGNOSTICS", "2"};

    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(3, read_one));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(4, read_many));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(2, probe_address));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(1, probe_bad_shape));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(3, typed_config));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(5, write_one));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(6, write_many));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(4, write_missing_confirm));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(5, write_bad_confirm));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(3, save_config));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, save_config_missing_confirm));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(5, set_gear_ratio));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(4, set_gear_ratio_missing_confirm));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(3, identify_status));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(5, identify_start));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(4, identify_missing_confirm));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(4, move));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(3, set_speed));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, set_speed_bad_shape));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(2, stop_one));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(2, stop_all));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(1, profile_status));
    HOST_TEST_CHECK(serial_gateway_lan_command_allowed(1, composition_status));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(1, endpoints));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(3, set_endpoint_speed));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(3, set_endpoint_position));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(
        4, set_endpoint_position_reference));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, stop_endpoint));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, endpoint_observation));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, position_observation));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(2, as5600_diagnostics));
    HOST_TEST_CHECK(!serial_gateway_lan_command_allowed(0, NULL));
    return true;
}

static bool test_gateway_diagnostic_policy(void)
{
    const char *ping[] = {"ping"};
    const char *version[] = {"VERSION"};
    const char *profile[] = {"PROFILE_STATUS"};
    const char *composition[] = {"COMPOSITION_STATUS"};
    const char *stop_all[] = {"stop", "all"};
    const char *stop_one[] = {"STOP", "0"};
    const char *set_speed[] = {"SET_SPEED", "0", "1"};
    const char *write[] = {"WRITE_REG", "1", "0x5018", "1", "CONFIRM"};
    const char *probe_address[] = {"SVD48_PROBE", "7"};
    const char *endpoints[] = {"ENDPOINTS"};
    const char *set_endpoint_speed[] = {"SET_ENDPOINT_SPEED", "1", "5"};
    const char *set_endpoint_position[] = {"SET_ENDPOINT_POSITION", "1", "5"};
    const char *set_endpoint_position_reference[] = {
        "SET_ENDPOINT_POSITION_REFERENCE", "1", "5", "CONFIRM"};
    const char *stop_endpoint[] = {"STOP_ENDPOINT", "1"};
    const char *endpoint_observation[] = {"GET_ENDPOINT_OBSERVATION", "1"};
    const char *position_observation[] = {
        "GET_ENDPOINT_POSITION_OBSERVATION", "1"};
    const char *as5600_diagnostics[] = {
        "GET_AS5600_DIAGNOSTICS", "2"};

    HOST_TEST_CHECK(serial_gateway_diagnostic_command_allowed(1, ping));
    HOST_TEST_CHECK(serial_gateway_diagnostic_command_allowed(1, version));
    HOST_TEST_CHECK(serial_gateway_diagnostic_command_allowed(1, profile));
    HOST_TEST_CHECK(serial_gateway_diagnostic_command_allowed(1, composition));
    HOST_TEST_CHECK(serial_gateway_diagnostic_command_allowed(2, stop_all));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2, stop_one));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(3, set_speed));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(5, write));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2, probe_address));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(1, endpoints));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(3, set_endpoint_speed));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(3, set_endpoint_position));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(
        4, set_endpoint_position_reference));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2, stop_endpoint));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2, endpoint_observation));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2, position_observation));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(2,
                                                                as5600_diagnostics));
    HOST_TEST_CHECK(!serial_gateway_diagnostic_command_allowed(0, NULL));
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_svd48_request_builders),
        HOST_TEST_CASE(test_svd48_write_multiple_response),
        HOST_TEST_CASE(test_svd48_exceptions),
        HOST_TEST_CASE(test_svd48_runtime_actuation_register_classification),
        HOST_TEST_CASE(test_serial_framing),
        HOST_TEST_CASE(test_gateway_error_result),
        HOST_TEST_CASE(test_gateway_lan_maintenance_policy),
        HOST_TEST_CASE(test_gateway_diagnostic_policy),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
