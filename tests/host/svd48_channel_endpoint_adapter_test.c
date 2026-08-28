#include "host_test.h"

#include <stdint.h>
#include <string.h>

#include "fake_bus_transport.h"
#include "svd48_channel_endpoint_adapter.h"

typedef struct {
    fake_bus_transport_t bus;
    svd48_device_t device;
    uint32_t now_ms;
    size_t state_lock_acquire_count;
} adapter_fixture_t;

static void append_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = svd48_crc16_uumotor(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc >> 8);
    frame[payload_length + 1U] = (uint8_t)(crc & 0xFFU);
}

static bool state_lock_acquire(void *context)
{
    adapter_fixture_t *fixture = context;
    if (!fixture) {
        return false;
    }
    ++fixture->state_lock_acquire_count;
    return true;
}

static void state_lock_release(void *context)
{
    (void)context;
}

static uint32_t fixture_clock_ms(void *context)
{
    const adapter_fixture_t *fixture = context;
    return fixture ? fixture->now_ms : 0U;
}

static bool fixture_init(adapter_fixture_t *fixture)
{
    if (!fixture) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (!fake_bus_transport_init(&fixture->bus, 10U)) {
        return false;
    }
    const svd48_device_config_t config = {
        .device_id = 7U,
        .address = 3U,
        .transport = fake_bus_transport_port(&fixture->bus),
        .response_timeout_ms = 10U,
        .retries = 0U,
        .stale_timeout_ms = 1000U,
        .state_lock = {
            .acquire = state_lock_acquire,
            .release = state_lock_release,
            .context = fixture,
        },
        .clock_ms = fixture_clock_ms,
        .clock_context = fixture,
    };
    return svd48_device_init(&fixture->device, &config);
}

static bool expect_write(adapter_fixture_t *fixture,
                         uint16_t reg,
                         uint16_t value,
                         bus_transport_result_t result,
                         bool echo)
{
    uint8_t request[8];
    size_t request_length = svd48_build_write_single_request(3U,
                                                            reg,
                                                            value,
                                                            request);
    if (request_length != sizeof(request)) {
        return false;
    }
    return echo ? fake_bus_transport_expect_echo(&fixture->bus,
                                                 request,
                                                 request_length,
                                                 result)
                : fake_bus_transport_expect(&fixture->bus,
                                            request,
                                            request_length,
                                            result,
                                            NULL,
                                            0U);
}

static bool expect_read(adapter_fixture_t *fixture,
                        uint16_t reg,
                        const uint16_t *values,
                        uint16_t quantity)
{
    if (!fixture || !values || quantity == 0U || quantity > 16U) {
        return false;
    }
    uint8_t request[8];
    uint8_t response[64] = {0};
    size_t request_length = svd48_build_read_request(3U, reg, quantity, request);
    response[0] = 3U;
    response[1] = SVD48_FUNC_READ_HOLDING;
    response[2] = (uint8_t)(quantity * 2U);
    for (uint16_t index = 0; index < quantity; ++index) {
        response[3U + index * 2U] = (uint8_t)(values[index] >> 8);
        response[4U + index * 2U] = (uint8_t)(values[index] & 0xFFU);
    }
    size_t response_length = 5U + (size_t)quantity * 2U;
    append_crc(response, response_length - 2U);
    return request_length == sizeof(request) &&
           fake_bus_transport_expect(&fixture->bus,
                                     request,
                                     request_length,
                                     BUS_TRANSPORT_OK,
                                     response,
                                     response_length);
}

static bool expect_complete_poll(adapter_fixture_t *fixture)
{
    const uint16_t position[] = {0U, 1U, 0U, 2U};
    const uint16_t speed[] = {12U, (uint16_t)(int16_t)-13};
    const uint16_t current[] = {10U, 11U};
    const uint16_t status[] = {1U, 1U};
    const uint16_t motor_temp[] = {250U, 251U};
    const uint16_t voltage[] = {480U, 481U};
    const uint16_t mos_temp[] = {300U, 301U};
    const uint16_t error[] = {0U, 0U, 0U, 0U};
    return expect_read(fixture, 0x5418U, position, 4U) &&
           expect_read(fixture, 0x5410U, speed, 2U) &&
           expect_read(fixture, 0x5414U, current, 2U) &&
           expect_read(fixture, 0x5400U, status, 2U) &&
           expect_read(fixture, 0x5404U, motor_temp, 2U) &&
           expect_read(fixture, 0x540CU, voltage, 2U) &&
           expect_read(fixture, 0x5408U, mos_temp, 2U) &&
           expect_read(fixture, 0x5420U, error, 4U);
}

static bool init_adapter(svd48_channel_endpoint_adapter_t *adapter,
                         adapter_fixture_t *fixture,
                         svd48_channel_id_t channel,
                         uint32_t capabilities,
                         int16_t min_rpm,
                         int16_t max_rpm)
{
    return svd48_channel_endpoint_adapter_init(
        adapter,
        svd48_device_channel(&fixture->device, channel),
        101U,
        "traction_test",
        ROBOT_ENDPOINT_REQUIRED,
        capabilities,
        min_rpm,
        max_rpm);
}

static bool test_initialization_and_capabilities(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    const uint32_t capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                                  ROBOT_CAPABILITY_STOPPABLE;
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 capabilities,
                                 -15,
                                 15));
    HOST_TEST_CHECK(adapter.channel ==
                    svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1));
    HOST_TEST_CHECK(adapter.configured_capabilities == capabilities);
    HOST_TEST_CHECK(robot_endpoint_capabilities(&adapter.endpoint) == capabilities);
    HOST_TEST_CHECK(adapter.endpoint.id == 101U);
    HOST_TEST_CHECK(adapter.endpoint.criticality == ROBOT_ENDPOINT_REQUIRED);
    HOST_TEST_CHECK(adapter.endpoint.available);
    HOST_TEST_CHECK(adapter.velocity.min_rpm == -15);
    HOST_TEST_CHECK(adapter.velocity.max_rpm == 15);
    return true;
}

static bool test_invalid_and_unsupported_initialization(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    svd48_channel_t *channel = svd48_device_channel(&fixture.device,
                                                    SVD48_CHANNEL_M1);
    HOST_TEST_CHECK(!svd48_channel_endpoint_adapter_init(
        &adapter, channel, 101U, "traction", ROBOT_ENDPOINT_REQUIRED,
        ROBOT_CAPABILITY_POSITION, -15, 15));
    HOST_TEST_CHECK(!svd48_channel_endpoint_adapter_init(
        &adapter, channel, 101U, "traction", ROBOT_ENDPOINT_REQUIRED,
        ROBOT_CAPABILITY_VELOCITY_RPM, 16, 15));
    HOST_TEST_CHECK(!svd48_channel_endpoint_adapter_init(
        &adapter, channel, 101U, "traction", ROBOT_ENDPOINT_REQUIRED, 0U,
        -15, 15));
    HOST_TEST_CHECK(!svd48_channel_endpoint_adapter_init(
        &adapter, channel, 0U, "traction", ROBOT_ENDPOINT_REQUIRED,
        ROBOT_CAPABILITY_STOPPABLE, -15, 15));
    HOST_TEST_CHECK(!svd48_channel_endpoint_adapter_init(
        &adapter, channel, 101U, "", ROBOT_ENDPOINT_REQUIRED,
        ROBOT_CAPABILITY_STOPPABLE, -15, 15));
    return true;
}

static bool test_velocity_limits_include_both_boundaries(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, -16) ==
                    ROBOT_CAP_OUT_OF_RANGE);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 0U);
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U,
                                (uint16_t)(int16_t)-15,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 1U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, -15) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 15U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 1U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 15) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 16) ==
                    ROBOT_CAP_OUT_OF_RANGE);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 4U);
    return true;
}

static bool test_m2_velocity_uses_only_m2_registers(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M2,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5305U,
                                (uint16_t)(int16_t)-7,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5301U, 1U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, -7) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_zero_velocity_keeps_channel_enabled(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 1U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 0) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_target_failure_executes_stop_best_effort(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 5U,
                                BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 5) ==
                    ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 3U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_enable_failure_executes_stop_best_effort(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 5U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 1U,
                                BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 5) ==
                    ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 4U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_original_failure_wins_when_rollback_also_fails(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 5U,
                                BUS_TRANSPORT_CANCELLED, false));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5304U, 0U,
                                BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5300U, 0U,
                                BUS_TRANSPORT_IO_ERROR, false));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 5) ==
                    ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 3U);
    return true;
}

static bool test_stop_port_writes_zero_then_stop(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M2,
                                 ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5305U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(expect_write(&fixture, 0x5301U, 0U,
                                BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(robot_endpoint_stop(&adapter.endpoint) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_missing_capability_is_reported_without_io(void)
{
    adapter_fixture_t stop_only_fixture;
    svd48_channel_endpoint_adapter_t stop_only;
    HOST_TEST_CHECK(fixture_init(&stop_only_fixture));
    HOST_TEST_CHECK(init_adapter(&stop_only,
                                 &stop_only_fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&stop_only.endpoint, 0) ==
                    ROBOT_CAP_UNSUPPORTED);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&stop_only_fixture.bus) == 0U);

    adapter_fixture_t velocity_only_fixture;
    svd48_channel_endpoint_adapter_t velocity_only;
    HOST_TEST_CHECK(fixture_init(&velocity_only_fixture));
    HOST_TEST_CHECK(init_adapter(&velocity_only,
                                 &velocity_only_fixture,
                                 SVD48_CHANNEL_M1,
                                 ROBOT_CAPABILITY_VELOCITY_RPM,
                                 -15,
                                 15));
    HOST_TEST_CHECK(robot_endpoint_stop(&velocity_only.endpoint) ==
                    ROBOT_CAP_UNSUPPORTED);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&velocity_only_fixture.bus) == 0U);
    return true;
}

static bool test_diagnostics_map_identity_observation_and_health(void)
{
    adapter_fixture_t fixture;
    svd48_channel_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture));
    HOST_TEST_CHECK(init_adapter(&adapter,
                                 &fixture,
                                 SVD48_CHANNEL_M2,
                                 ROBOT_CAPABILITY_VELOCITY_RPM |
                                     ROBOT_CAPABILITY_STOPPABLE,
                                 -15,
                                 15));
    svd48_endpoint_diagnostics_t diagnostics;
    HOST_TEST_CHECK(svd48_channel_endpoint_adapter_get_diagnostics(&adapter,
                                                                  &diagnostics));
    HOST_TEST_CHECK(diagnostics.device_id == 7U);
    HOST_TEST_CHECK(diagnostics.device_address == 3U);
    HOST_TEST_CHECK(diagnostics.channel == SVD48_CHANNEL_M2);
    HOST_TEST_CHECK(diagnostics.health == SVD48_CHANNEL_HEALTH_OFFLINE);
    robot_velocity_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_velocity_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(observation.timestamp_ms == 0U);
    HOST_TEST_CHECK(observation.source ==
                    ROBOT_VELOCITY_OBSERVATION_SOURCE_UNKNOWN);
    HOST_TEST_CHECK(!observation.online);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_OFFLINE);

    fixture.now_ms = 100U;
    HOST_TEST_CHECK(expect_complete_poll(&fixture));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_endpoint_adapter_get_diagnostics(&adapter,
                                                                  &diagnostics));
    HOST_TEST_CHECK(diagnostics.observation.observed_speed_rpm == -13);
    HOST_TEST_CHECK(diagnostics.observation.valid_observations ==
                    SVD48_OBSERVATION_ALL);
    HOST_TEST_CHECK(diagnostics.health == SVD48_CHANNEL_HEALTH_HEALTHY);
    size_t lock_count = fixture.state_lock_acquire_count;
    HOST_TEST_CHECK(robot_endpoint_read_velocity_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(fixture.state_lock_acquire_count == lock_count + 1U);
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.rpm == -13);
    HOST_TEST_CHECK(observation.timestamp_ms == 100U);
    HOST_TEST_CHECK(observation.source ==
                    ROBOT_VELOCITY_OBSERVATION_SOURCE_DEVICE_FEEDBACK);
    HOST_TEST_CHECK(observation.online);
    HOST_TEST_CHECK(!observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_HEALTHY);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_initialization_and_capabilities),
        HOST_TEST_CASE(test_invalid_and_unsupported_initialization),
        HOST_TEST_CASE(test_velocity_limits_include_both_boundaries),
        HOST_TEST_CASE(test_m2_velocity_uses_only_m2_registers),
        HOST_TEST_CASE(test_zero_velocity_keeps_channel_enabled),
        HOST_TEST_CASE(test_target_failure_executes_stop_best_effort),
        HOST_TEST_CASE(test_enable_failure_executes_stop_best_effort),
        HOST_TEST_CASE(test_original_failure_wins_when_rollback_also_fails),
        HOST_TEST_CASE(test_stop_port_writes_zero_then_stop),
        HOST_TEST_CASE(test_missing_capability_is_reported_without_io),
        HOST_TEST_CASE(test_diagnostics_map_identity_observation_and_health),
    };
    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
