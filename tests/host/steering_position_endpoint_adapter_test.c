#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_test.h"

#include "as5600_device.h"
#include "fake_as5600_register_port.h"
#include "robot_capabilities.h"
#include "steering_position_endpoint_adapter.h"

#define RESPONSE_TIMEOUT_MS 17U
#define SENSOR_STALE_TIMEOUT_MS 100U
#define ADAPTER_TTL_MS 50U
#define MAX_RAW_STEP_COUNTS 64U
#define PWM_OUTPUT_CAPACITY 32U

typedef struct {
    fake_as5600_register_port_t register_port;
    as5600_device_t as5600;
    steering_position_endpoint_adapter_t adapter;
    uint64_t now_ms;
    bool pwm_accepts;
    uint16_t pwm_outputs[PWM_OUTPUT_CAPACITY];
    size_t pwm_output_count;
    bool lock_accepts;
    size_t lock_acquires;
    size_t lock_releases;
} adapter_fixture_t;

static uint32_t fixture_as5600_clock_ms(void *context)
{
    const adapter_fixture_t *fixture = context;
    return fixture != NULL ? (uint32_t)fixture->now_ms : 0U;
}

static uint64_t fixture_controller_clock_ms(void *context)
{
    const adapter_fixture_t *fixture = context;
    return fixture != NULL ? fixture->now_ms : 0U;
}

static bool fixture_pwm_output(void *context, uint16_t pulse_us)
{
    adapter_fixture_t *fixture = context;
    if (fixture == NULL || !fixture->pwm_accepts ||
        fixture->pwm_output_count >= PWM_OUTPUT_CAPACITY) {
        return false;
    }
    fixture->pwm_outputs[fixture->pwm_output_count++] = pulse_us;
    return true;
}

static bool fixture_lock_acquire(void *context)
{
    adapter_fixture_t *fixture = context;
    if (fixture == NULL) {
        return false;
    }
    fixture->lock_acquires++;
    return fixture->lock_accepts;
}

static void fixture_lock_release(void *context)
{
    adapter_fixture_t *fixture = context;
    if (fixture != NULL) {
        fixture->lock_releases++;
    }
}

static as5600_calibration_lut_t test_calibration(void)
{
    return (as5600_calibration_lut_t){
        .metadata = {
            .format_version = AS5600_CALIBRATION_FORMAT_VERSION,
            .calibration_id = "steering-adapter-host-lut-v1",
            .hardware_identity = "host-steering-fixture",
            .provenance = "synthetic host test calibration",
        },
        .correction_centidegrees = {0},
    };
}

static steering_position_controller_config_t controller_config(void)
{
    return (steering_position_controller_config_t){
        .minimum_position_deg = -90.0,
        .maximum_position_deg = 90.0,
        .neutral_pulse_us = 1500U,
        .positive_far_pulse_us = 1040U,
        .positive_near_pulse_us = 1390U,
        .negative_far_pulse_us = 2000U,
        .negative_near_pulse_us = 1680U,
        .arrival_min_error_deg = 0.0,
        .arrival_max_error_deg = 3.0,
        .full_speed_error_deg = 6.0,
        .reacquire_error_deg = 4.5,
        .stable_sample_count = 5U,
        .reacquire_sample_count = 5U,
        .reversal_settle_ms = 240U,
        .sensor_stale_timeout_ms = 100U,
        .sensor_fault_timeout_ms = 400U,
        .max_command_ttl_ms = 1000U,
        .move_timeout_ms = 45000U,
        /* Matches the measured but still qualified ML warning policy. */
        .allow_degraded_sensor_health = true,
    };
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

static bool fixture_init_with_diagnostics(adapter_fixture_t *fixture,
                                          as5600_calibration_lut_t *calibration,
                                          bool read_diagnostics)
{
    if (fixture == NULL || calibration == NULL) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (!fake_as5600_register_port_init(&fixture->register_port)) {
        return false;
    }
    fixture->pwm_accepts = true;
    fixture->lock_accepts = true;

    const as5600_device_config_t sensor_config = {
        .device_id = 8U,
        .i2c_address = AS5600_DEFAULT_I2C_ADDRESS,
        .register_read = fake_as5600_register_port_as_port(&fixture->register_port),
        .clock_ms = fixture_as5600_clock_ms,
        .clock_context = fixture,
        .response_timeout_ms = RESPONSE_TIMEOUT_MS,
        .stale_timeout_ms = SENSOR_STALE_TIMEOUT_MS,
        .read_diagnostics = read_diagnostics,
        .calibration = calibration,
    };
    if (!as5600_device_init(&fixture->as5600, &sensor_config)) {
        return false;
    }

    const steering_position_endpoint_adapter_config_t adapter_config = {
        .as5600 = &fixture->as5600,
        .approved_calibration = calibration,
        .endpoint_id = 81U,
        .endpoint_name = "bench_steering",
        .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
        .controller_config = controller_config(),
        .clock_ms = fixture_controller_clock_ms,
        .clock_context = fixture,
        .pwm_output = fixture_pwm_output,
        .pwm_output_context = fixture,
        .position_command_ttl_ms = ADAPTER_TTL_MS,
        .max_raw_circular_step_counts = MAX_RAW_STEP_COUNTS,
        .allow_magnet_too_weak_for_development = true,
        .lock = {
            .acquire = fixture_lock_acquire,
            .release = fixture_lock_release,
            .context = fixture,
        },
    };
    return steering_position_endpoint_adapter_init(&fixture->adapter, &adapter_config);
}

static bool fixture_init(adapter_fixture_t *fixture,
                         as5600_calibration_lut_t *calibration)
{
    return fixture_init_with_diagnostics(fixture, calibration, false);
}

static bool poll_sample(adapter_fixture_t *fixture,
                        uint8_t status,
                        uint16_t raw_angle)
{
    return queue_sample(fixture, status, raw_angle) &&
           as5600_device_poll(&fixture->as5600) == AS5600_DEVICE_OK;
}

static bool queue_diagnostic_error(adapter_fixture_t *fixture,
                                   as5600_device_result_t result)
{
    return fixture != NULL &&
           fake_as5600_register_port_expect_read(&fixture->register_port,
                                                 AS5600_DEFAULT_I2C_ADDRESS,
                                                 0x1AU,
                                                 3U,
                                                 RESPONSE_TIMEOUT_MS,
                                                 result,
                                                 NULL);
}

static uint16_t latest_pwm(const adapter_fixture_t *fixture)
{
    return fixture->pwm_outputs[fixture->pwm_output_count - 1U];
}

static bool set_reference_from_current_sample(adapter_fixture_t *fixture,
                                              uint16_t raw_angle)
{
    return poll_sample(fixture, AS5600_STATUS_MAGNET_DETECTED, raw_angle) &&
           steering_position_endpoint_adapter_set_reference(&fixture->adapter, 0.0f) ==
               ROBOT_CAP_OK;
}

static bool test_endpoint_exposes_position_stop_and_reference_not_observation(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    HOST_TEST_CHECK(robot_endpoint_capabilities(&fixture.adapter.endpoint) ==
                    (ROBOT_CAPABILITY_POSITION | ROBOT_CAPABILITY_STOPPABLE |
                     ROBOT_CAPABILITY_POSITION_REFERENCE));
    HOST_TEST_CHECK(fixture.adapter.endpoint.position_observation == NULL);
    HOST_TEST_CHECK(fixture.adapter.endpoint.position_sensor == NULL);
    HOST_TEST_CHECK(!robot_endpoint_has_position_observation(&fixture.adapter.endpoint));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 10.0f) ==
                    ROBOT_CAP_UNAVAILABLE);
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 1000U));
    HOST_TEST_CHECK(robot_position_set_reference_degrees(
                        &fixture.adapter.endpoint, 0.0f) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(robot_endpoint_stop(&fixture.adapter.endpoint) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_explicit_reference_and_position_port_use_fixed_ttl(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_snapshot_t snapshot;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(steering_position_controller_snapshot(&fixture.adapter.controller,
                                                           &snapshot) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(snapshot.target_active);
    HOST_TEST_CHECK(snapshot.command_deadline_ms == 60U);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1040U);

    HOST_TEST_CHECK(robot_endpoint_stop(&fixture.adapter.endpoint) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(steering_position_controller_snapshot(&fixture.adapter.controller,
                                                           &snapshot) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(!snapshot.target_active);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_project_phase_requires_current_referenced_sample(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    float logical_degrees = 123.0f;
    const float phase = as5600_calibration_corrected_degrees(&calibration, 1000U);
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));

    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 0U, &logical_degrees) ==
                    ROBOT_CAP_UNAVAILABLE);

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 10U, &logical_degrees) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(fabsf(logical_degrees) < 1.0e-6f);

    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 11U, &logical_degrees) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase + 1.0f, 10U, &logical_degrees) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, NAN, 10U, &logical_degrees) ==
                    ROBOT_CAP_INVALID_ARGUMENT);

    fixture.now_ms = 111U;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 10U, &logical_degrees) ==
                    ROBOT_CAP_UNAVAILABLE);
    fixture.lock_accepts = false;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 10U, &logical_degrees) ==
                    ROBOT_CAP_IO_ERROR);
    return true;
}

static bool test_project_phase_rejects_controller_fault_and_lock_failure(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    float logical_degrees = 123.0f;
    const float phase = as5600_calibration_corrected_degrees(&calibration, 1000U);
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 1000U));
    fixture.pwm_accepts = false;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(report.fault == STEERING_POSITION_CONTROLLER_FAULT_OUTPUT);
    fixture.pwm_accepts = true;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 11U, &logical_degrees) ==
                    ROBOT_CAP_UNAVAILABLE);

    fixture.lock_accepts = false;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_project_cyclic_phase(
                        &fixture.adapter, phase, 11U, &logical_degrees) ==
                    ROBOT_CAP_IO_ERROR);
    return true;
}

static bool test_circular_raw_wrap_is_accepted_but_large_jump_faults(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 4090U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);
    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 5U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1040U);

    fixture.now_ms = 12U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 200U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_missing_magnet_is_neutralized_and_latched(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture, 0U, 1001U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_expired_as5600_snapshot_uses_stale_then_fault_policy(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture, AS5600_STATUS_MAGNET_DETECTED, 1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);

    /* No new I2C poll: at 100 ms the controller must command neutral first,
     * and only latch the configured sensor fault deadline later. */
    fixture.now_ms = 112U;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE);
    HOST_TEST_CHECK(report.fault == STEERING_POSITION_CONTROLLER_FAULT_NONE);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);

    fixture.now_ms = 411U;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_explicit_weak_field_policy_can_remain_operable(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(poll_sample(&fixture,
                                AS5600_STATUS_MAGNET_DETECTED |
                                    AS5600_STATUS_MAGNET_TOO_WEAK,
                                1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_set_reference(
                        &fixture.adapter, 0.0f) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture,
                                AS5600_STATUS_MAGNET_DETECTED |
                                    AS5600_STATUS_MAGNET_TOO_WEAK,
                                1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);
    HOST_TEST_CHECK(report.fault == STEERING_POSITION_CONTROLLER_FAULT_NONE);
    return true;
}

static bool test_strong_magnet_warning_is_neutralized_and_latched(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(poll_sample(&fixture,
                                AS5600_STATUS_MAGNET_DETECTED |
                                    AS5600_STATUS_MAGNET_TOO_STRONG,
                                1000U));
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_partial_sensor_poll_is_not_a_degraded_motion_exception(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_OK);

    /* STATUS+RAW remains fresh, but an incomplete optional diagnostic is not
     * the profile's explicitly permitted ML condition. */
    fixture.as5600.snapshot.last_poll_result = AS5600_DEVICE_PARTIAL;
    fixture.as5600.snapshot.last_error = AS5600_DEVICE_TIMEOUT;
    fixture.now_ms = 11U;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    return true;
}

static bool test_initial_diagnostic_failure_latches_before_reference_and_persists(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    steering_position_controller_report_t report;
    as5600_device_snapshot_t snapshot;
    HOST_TEST_CHECK(fixture_init_with_diagnostics(&fixture, &calibration, true));

    /* The requested one-shot diagnostic fails before this axis has any
     * reference. The controller must latch it instead of allowing a later
     * healthy primary STATUS+RAW sample to make reference/motion possible. */
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 1000U));
    HOST_TEST_CHECK(queue_diagnostic_error(&fixture, AS5600_DEVICE_TIMEOUT));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.as5600) == AS5600_DEVICE_PARTIAL);
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);

    /* Diagnostics are one-shot, so this later primary poll is otherwise
     * healthy. The failed diagnostic must remain visible and must not re-arm
     * the controller. */
    fixture.now_ms = 20U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 1001U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.as5600) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.as5600, &snapshot));
    HOST_TEST_CHECK(snapshot.last_poll_result == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(snapshot.diagnostics_attempted);
    HOST_TEST_CHECK(!snapshot.diagnostics_valid);
    HOST_TEST_CHECK(snapshot.diagnostics_last_result == AS5600_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(snapshot.health == AS5600_DEVICE_HEALTH_DEGRADED);
    HOST_TEST_CHECK(steering_position_endpoint_adapter_tick(&fixture.adapter, &report) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);

    const size_t outputs_before_attempts = fixture.pwm_output_count;
    HOST_TEST_CHECK(steering_position_endpoint_adapter_set_reference(
                        &fixture.adapter, 0.0f) == ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(fixture.pwm_output_count == outputs_before_attempts);
    HOST_TEST_CHECK(latest_pwm(&fixture) == 1500U);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_lock_failure_prevents_position_write(void)
{
    as5600_calibration_lut_t calibration = test_calibration();
    adapter_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, &calibration));
    fixture.now_ms = 10U;
    HOST_TEST_CHECK(set_reference_from_current_sample(&fixture, 1000U));
    size_t outputs_before = fixture.pwm_output_count;
    fixture.lock_accepts = false;
    HOST_TEST_CHECK(robot_position_set_degrees(&fixture.adapter.endpoint, 20.0f) ==
                    ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(fixture.pwm_output_count == outputs_before);
    HOST_TEST_CHECK(fixture.lock_acquires == fixture.lock_releases + 1U);
    return true;
}

static bool test_lut_approval_is_required(void)
{
    as5600_calibration_lut_t configured = test_calibration();
    as5600_calibration_lut_t mismatched = test_calibration();
    adapter_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, &configured));

    steering_position_endpoint_adapter_t rejected;
    steering_position_endpoint_adapter_config_t config = {
        .as5600 = &fixture.as5600,
        .approved_calibration = &mismatched,
        .endpoint_id = 82U,
        .endpoint_name = "rejected_steering",
        .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
        .controller_config = controller_config(),
        .clock_ms = fixture_controller_clock_ms,
        .clock_context = &fixture,
        .pwm_output = fixture_pwm_output,
        .pwm_output_context = &fixture,
        .position_command_ttl_ms = ADAPTER_TTL_MS,
        .max_raw_circular_step_counts = MAX_RAW_STEP_COUNTS,
    };
    HOST_TEST_CHECK(!steering_position_endpoint_adapter_init(&rejected, &config));
    config.approved_calibration = NULL;
    HOST_TEST_CHECK(!steering_position_endpoint_adapter_init(&rejected, &config));
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_endpoint_exposes_position_stop_and_reference_not_observation),
        HOST_TEST_CASE(test_explicit_reference_and_position_port_use_fixed_ttl),
        HOST_TEST_CASE(test_project_phase_requires_current_referenced_sample),
        HOST_TEST_CASE(test_project_phase_rejects_controller_fault_and_lock_failure),
        HOST_TEST_CASE(test_circular_raw_wrap_is_accepted_but_large_jump_faults),
        HOST_TEST_CASE(test_missing_magnet_is_neutralized_and_latched),
        HOST_TEST_CASE(test_expired_as5600_snapshot_uses_stale_then_fault_policy),
        HOST_TEST_CASE(test_explicit_weak_field_policy_can_remain_operable),
        HOST_TEST_CASE(test_strong_magnet_warning_is_neutralized_and_latched),
        HOST_TEST_CASE(test_partial_sensor_poll_is_not_a_degraded_motion_exception),
        HOST_TEST_CASE(test_initial_diagnostic_failure_latches_before_reference_and_persists),
        HOST_TEST_CASE(test_lock_failure_prevents_position_write),
        HOST_TEST_CASE(test_lut_approval_is_required),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
