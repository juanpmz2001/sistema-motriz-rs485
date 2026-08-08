#include "host_test.h"

#include "as5600_device.h"
#include "as5600_position_endpoint_adapter.h"
#include "fake_as5600_register_port.h"
#include "robot_capabilities.h"

#define RESPONSE_TIMEOUT_MS 17U
#define STALE_TIMEOUT_MS 100U

typedef struct {
    fake_as5600_register_port_t register_port;
    as5600_device_t device;
    uint32_t now_ms;
} adapter_fixture_t;

typedef struct {
    size_t calls;
    robot_capability_error_t result;
    float logical_degrees;
} fake_coordinate_frame_t;

static robot_capability_error_t fake_project_cyclic_phase(
    void *context,
    float corrected_cyclic_degrees,
    uint32_t sample_timestamp_ms,
    float *logical_degrees)
{
    (void)corrected_cyclic_degrees;
    (void)sample_timestamp_ms;
    fake_coordinate_frame_t *frame = context;
    if (frame == NULL || logical_degrees == NULL) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++frame->calls;
    if (frame->result == ROBOT_CAP_OK) {
        *logical_degrees = frame->logical_degrees;
    }
    return frame->result;
}

static bool bind_coordinate_frame(as5600_position_endpoint_adapter_t *adapter,
                                  fake_coordinate_frame_t *frame)
{
    const as5600_position_coordinate_frame_t coordinate_frame = {
        .project_cyclic_phase = fake_project_cyclic_phase,
        .context = frame,
    };
    return as5600_position_endpoint_adapter_set_coordinate_frame(
        adapter, &coordinate_frame);
}

static uint32_t fixture_clock_ms(void *context)
{
    const adapter_fixture_t *fixture = context;
    return fixture != NULL ? fixture->now_ms : 0U;
}

static as5600_calibration_lut_t calibration_lut(const char *identifier)
{
    return (as5600_calibration_lut_t){
        .metadata = {
            .format_version = AS5600_CALIBRATION_FORMAT_VERSION,
            .calibration_id = identifier,
            .hardware_identity = "host-adapter-fixture",
            .provenance = "host-test synthetic calibration",
        },
        .correction_centidegrees = {0},
    };
}

static bool fixture_init(adapter_fixture_t *fixture,
                         const as5600_calibration_lut_t *calibration)
{
    if (fixture == NULL || !fake_as5600_register_port_init(&fixture->register_port)) {
        return false;
    }
    fixture->now_ms = 0U;
    const as5600_device_config_t config = {
        .device_id = 9U,
        .i2c_address = AS5600_DEFAULT_I2C_ADDRESS,
        .register_read = fake_as5600_register_port_as_port(&fixture->register_port),
        .clock_ms = fixture_clock_ms,
        .clock_context = fixture,
        .response_timeout_ms = RESPONSE_TIMEOUT_MS,
        .stale_timeout_ms = STALE_TIMEOUT_MS,
        .calibration = calibration,
    };
    return as5600_device_init(&fixture->device, &config);
}

static bool queue_sample(adapter_fixture_t *fixture,
                         uint8_t status,
                         uint16_t raw_angle)
{
    if (fixture == NULL) {
        return false;
    }
    const uint8_t primary_bytes[] = {
        status,
        (uint8_t)((raw_angle >> 8U) & 0x0FU),
        (uint8_t)(raw_angle & 0xFFU),
    };
    return fake_as5600_register_port_expect_read(&fixture->register_port,
                                                  AS5600_DEFAULT_I2C_ADDRESS,
                                                  0x0BU,
                                                  sizeof(primary_bytes),
                                                  RESPONSE_TIMEOUT_MS,
                                                  AS5600_DEVICE_OK,
                                                  primary_bytes);
}

static bool queue_status_error(adapter_fixture_t *fixture,
                               as5600_device_result_t result)
{
    return fixture != NULL &&
           fake_as5600_register_port_expect_read(&fixture->register_port,
                                                  AS5600_DEFAULT_I2C_ADDRESS,
                                                  0x0BU,
                                                  3U,
                                                  RESPONSE_TIMEOUT_MS,
                                                  result,
                                                  NULL);
}

static bool nearly_equal(float actual, float expected, float tolerance)
{
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

static bool test_endpoint_is_observation_only_and_maps_calibrated_sample(void)
{
    as5600_calibration_lut_t lut = calibration_lut("adapter-calibration-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        42U,
        "steering_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    HOST_TEST_CHECK(robot_endpoint_capabilities(&adapter.endpoint) ==
                    ROBOT_CAPABILITY_POSITION_OBSERVATION);
    HOST_TEST_CHECK(adapter.endpoint.position == NULL);
    HOST_TEST_CHECK(adapter.endpoint.position_sensor == NULL);
    HOST_TEST_CHECK(robot_endpoint_has_position_observation(&adapter.endpoint));

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0800U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_UNAVAILABLE);

    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_OK,
        .logical_degrees = -12.5f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(observation.referenced);
    HOST_TEST_CHECK(nearly_equal(observation.degrees, -12.5f, 0.0001f));
    HOST_TEST_CHECK(observation.timestamp_ms == 10U);
    HOST_TEST_CHECK(observation.source_endpoint_id == 42U);
    HOST_TEST_CHECK(observation.source ==
                    ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR);
    HOST_TEST_CHECK(observation.online);
    HOST_TEST_CHECK(!observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_HEALTHY);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_OK);
    HOST_TEST_CHECK(frame.calls == 1U);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_missing_explicit_approval_never_marks_physical_feedback_valid(void)
{
    as5600_calibration_lut_t lut = calibration_lut("unapproved-calibration-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    /* A device LUT alone is insufficient: the endpoint must approve it. */
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        43U,
        "unapproved_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        NULL));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0400U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);

    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(!observation.calibrated);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.source ==
                    ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_magnet_warnings_remain_degraded_with_calibrated_phase(void)
{
    as5600_calibration_lut_t lut = calibration_lut("magnet-warning-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        44U,
        "warning_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_OK,
        .logical_degrees = 5.0f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED |
                                     AS5600_STATUS_MAGNET_TOO_WEAK,
                                 0x0100U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(observation.referenced);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED |
                                     AS5600_STATUS_MAGNET_TOO_STRONG,
                                 0x0101U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.referenced);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_missing_magnet_invalidates_position_without_hiding_health(void)
{
    as5600_calibration_lut_t lut = calibration_lut("missing-magnet-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        45U,
        "missing_magnet_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_OK,
        .logical_degrees = 0.0f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture, 0U, 0x0100U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_OK);
    return true;
}

static bool test_stale_and_transport_failure_are_explicit(void)
{
    as5600_calibration_lut_t lut = calibration_lut("freshness-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        46U,
        "freshness_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_OK,
        .logical_degrees = 2.0f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0200U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);

    fixture.now_ms = 150U;
    HOST_TEST_CHECK(queue_status_error(&fixture, AS5600_DEVICE_TIMEOUT));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_TIMEOUT);
    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(observation.referenced);
    HOST_TEST_CHECK(observation.online);
    HOST_TEST_CHECK(!observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_IO_ERROR);

    fixture.now_ms = 201U;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(!observation.online);
    HOST_TEST_CHECK(observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_OFFLINE);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_unpolled_clock_staleness_stays_distinct_from_offline(void)
{
    as5600_calibration_lut_t lut = calibration_lut("stale-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        47U,
        "stale_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_OK,
        .logical_degrees = 2.0f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0200U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);

    fixture.now_ms = 201U;
    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(!observation.online);
    HOST_TEST_CHECK(observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_STALE);
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_OK);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_mismatched_approved_lut_is_rejected(void)
{
    as5600_calibration_lut_t device_lut = calibration_lut("device-lut-v1");
    as5600_calibration_lut_t other_lut = calibration_lut("other-lut-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &device_lut));
    HOST_TEST_CHECK(!as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        48U,
        "mismatched_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &other_lut));
    HOST_TEST_CHECK(!as5600_position_endpoint_adapter_init(
        &adapter,
        NULL,
        48U,
        "invalid_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &device_lut));
    return true;
}

static bool test_coordinate_frame_failure_never_invents_logical_position(void)
{
    as5600_calibration_lut_t lut = calibration_lut("frame-failure-v1");
    adapter_fixture_t fixture;
    as5600_position_endpoint_adapter_t adapter;
    HOST_TEST_CHECK(fixture_init(&fixture, &lut));
    HOST_TEST_CHECK(as5600_position_endpoint_adapter_init(
        &adapter,
        &fixture.device,
        49U,
        "frame_failure_feedback",
        ROBOT_ENDPOINT_DEVELOPMENT,
        &lut));
    fake_coordinate_frame_t frame = {
        .result = ROBOT_CAP_UNAVAILABLE,
        .logical_degrees = 123.0f,
    };
    HOST_TEST_CHECK(bind_coordinate_frame(&adapter, &frame));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0800U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);

    robot_position_observation_t observation;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&adapter.endpoint,
                                                             &observation) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!observation.valid);
    HOST_TEST_CHECK(observation.calibrated);
    HOST_TEST_CHECK(!observation.referenced);
    HOST_TEST_CHECK(nearly_equal(observation.degrees, 0.0f, 0.0001f));
    HOST_TEST_CHECK(observation.status == ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(frame.calls == 1U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_endpoint_is_observation_only_and_maps_calibrated_sample),
        HOST_TEST_CASE(test_missing_explicit_approval_never_marks_physical_feedback_valid),
        HOST_TEST_CASE(test_magnet_warnings_remain_degraded_with_calibrated_phase),
        HOST_TEST_CASE(test_missing_magnet_invalidates_position_without_hiding_health),
        HOST_TEST_CASE(test_stale_and_transport_failure_are_explicit),
        HOST_TEST_CASE(test_unpolled_clock_staleness_stays_distinct_from_offline),
        HOST_TEST_CASE(test_mismatched_approved_lut_is_rejected),
        HOST_TEST_CASE(test_coordinate_frame_failure_never_invents_logical_position),
    };
    return host_test_exit_code(host_test_run_cases(cases,
                                                   HOST_TEST_ARRAY_COUNT(cases),
                                                   stdout));
}
