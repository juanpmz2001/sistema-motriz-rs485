#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_test.h"
#include "steering_position_controller.h"

enum { TEST_OUTPUT_CAPACITY = 64 };

typedef struct {
    uint64_t now_ms;
    bool output_succeeds;
    uint16_t outputs[TEST_OUTPUT_CAPACITY];
    size_t output_count;
} controller_fixture_t;

static uint64_t fixture_clock_ms(void *context)
{
    const controller_fixture_t *fixture = context;
    return fixture != NULL ? fixture->now_ms : 0U;
}

static bool fixture_pwm_output(void *context, uint16_t pulse_us)
{
    controller_fixture_t *fixture = context;
    if (fixture == NULL || !fixture->output_succeeds ||
        fixture->output_count >= TEST_OUTPUT_CAPACITY) {
        return false;
    }
    fixture->outputs[fixture->output_count++] = pulse_us;
    return true;
}

static steering_position_controller_config_t test_config(void)
{
    const steering_position_controller_config_t config = {
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
        .allow_degraded_sensor_health = true,
    };
    return config;
}

static steering_position_controller_sample_t sample_at(
    const controller_fixture_t *fixture,
    double corrected_cyclic_position_deg)
{
    const steering_position_controller_sample_t sample = {
        .corrected_cyclic_position_deg = corrected_cyclic_position_deg,
        .valid = true,
        .magnet_detected = true,
        .health = STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY,
        .timestamp_ms = fixture->now_ms,
    };
    return sample;
}

static bool init_fixture(controller_fixture_t *fixture,
                         steering_position_controller_t *controller)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->output_succeeds = true;
    steering_position_controller_config_t config = test_config();
    HOST_TEST_CHECK(steering_position_controller_init(controller,
                                                      &config,
                                                      fixture_clock_ms,
                                                      fixture,
                                                      fixture_pwm_output,
                                                      fixture) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(fixture->output_count == 1U);
    HOST_TEST_CHECK(fixture->outputs[0] == config.neutral_pulse_us);
    return true;
}

static bool reference_at(controller_fixture_t *fixture,
                         steering_position_controller_t *controller,
                         double corrected_cyclic_position_deg,
                         double logical_position_deg)
{
    steering_position_controller_sample_t sample =
        sample_at(fixture, corrected_cyclic_position_deg);
    return steering_position_controller_set_reference(controller,
                                                      &sample,
                                                      logical_position_deg,
                                                      NULL) ==
           STEERING_POSITION_CONTROLLER_OK;
}

static uint16_t latest_output(const controller_fixture_t *fixture)
{
    return fixture->outputs[fixture->output_count - 1U];
}

static bool test_unhomed_target_is_rejected(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));

    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 100U, &report) ==
                    STEERING_POSITION_CONTROLLER_ERROR_UNHOMED);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_UNHOMED);
    HOST_TEST_CHECK(!report.target_active);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    return true;
}

static bool test_explicit_reference_and_cyclic_unwrap_drive_both_directions(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 350.0, 0.0));

    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    fixture.now_ms = 1U;
    steering_position_controller_sample_t sample = sample_at(&fixture, 0.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);
    HOST_TEST_CHECK(report.current_position_valid);
    HOST_TEST_CHECK(fabs(report.current_position_deg - 10.0) < 1.0e-9);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);

    HOST_TEST_CHECK(steering_position_controller_stop(&controller, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    fixture.now_ms = 241U;
    sample = sample_at(&fixture, 0.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, -20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    sample = sample_at(&fixture, 0.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_NEGATIVE);
    HOST_TEST_CHECK(latest_output(&fixture) == 2000U);
    return true;
}

static bool test_rejects_nonfinite_and_out_of_bound_targets(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));

    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, NAN, 100U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 90.1, 100U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, -90.1, 100U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 0U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 1001U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL);
    return true;
}

static bool test_arrival_stability_and_reacquire_hysteresis(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);

    for (uint32_t count = 0U; count < 5U; count++) {
        fixture.now_ms++;
        steering_position_controller_sample_t sample = sample_at(&fixture, 108.0);
        HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                        STEERING_POSITION_CONTROLLER_OK);
        if (count < 4U) {
            HOST_TEST_CHECK(report.action ==
                            STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE);
        }
        HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    }
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET);
    HOST_TEST_CHECK(report.controller_estimated_at_target);

    for (uint32_t count = 0U; count < 5U; count++) {
        fixture.now_ms++;
        steering_position_controller_sample_t sample = sample_at(&fixture, 104.0);
        HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                        STEERING_POSITION_CONTROLLER_OK);
        if (count < 4U) {
            HOST_TEST_CHECK(report.action ==
                            STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET);
            HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
        }
    }
    HOST_TEST_CHECK(!report.controller_estimated_at_target);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);
    return true;
}

static bool test_arrival_stability_requires_distinct_sensor_samples(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);

    fixture.now_ms = 1U;
    steering_position_controller_sample_t sample = sample_at(&fixture, 108.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE);

    /* A scheduler may run more often than the AS5600 poller.  Replaying the
     * same timestamp must not manufacture four additional stable samples. */
    for (uint32_t count = 0U; count < 4U; ++count) {
        fixture.now_ms++;
        HOST_TEST_CHECK(steering_position_controller_tick(&controller,
                                                          &sample,
                                                          &report) ==
                        STEERING_POSITION_CONTROLLER_OK);
        HOST_TEST_CHECK(report.action ==
                        STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE);
        HOST_TEST_CHECK(!report.controller_estimated_at_target);
    }

    for (uint32_t count = 0U; count < 4U; ++count) {
        fixture.now_ms++;
        sample = sample_at(&fixture, 108.0);
        HOST_TEST_CHECK(steering_position_controller_tick(&controller,
                                                          &sample,
                                                          &report) ==
                        STEERING_POSITION_CONTROLLER_OK);
    }
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET);
    HOST_TEST_CHECK(report.controller_estimated_at_target);
    return true;
}

static bool test_direction_inversion_requires_neutral_settle(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);

    /* The original positive command can have been running for a long time;
     * a new reverse target still has to start a fresh neutral-settle window. */
    fixture.now_ms = 900U;
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, -20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_REVERSAL_SETTLING);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);

    fixture.now_ms = 1139U;
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_REVERSAL_SETTLING);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);

    fixture.now_ms = 1140U;
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_NEGATIVE);
    HOST_TEST_CHECK(latest_output(&fixture) == 2000U);
    return true;
}

static bool test_stale_neutral_then_sensor_timeout_fault(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);

    fixture.now_ms = 101U;
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, NULL, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_SENSOR_STALE);
    HOST_TEST_CHECK(report.fault == STEERING_POSITION_CONTROLLER_FAULT_NONE);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);

    fixture.now_ms = 400U;
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, NULL, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_FAULT);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    return true;
}

static bool test_stale_position_request_cannot_refresh_lease_or_keep_drive(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);

    fixture.now_ms = 101U;
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, &report) ==
                    STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    return true;
}

static bool test_explicit_magnet_fault_latches_immediately(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    sample.magnet_detected = false;
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_FAULT);
    return true;
}

static bool test_reference_cannot_clear_a_latched_fault(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));

    steering_position_controller_sample_t fault_sample = sample_at(&fixture, 100.0);
    fault_sample.magnet_detected = false;
    HOST_TEST_CHECK(steering_position_controller_tick(&controller,
                                                      &fault_sample,
                                                      &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_FAULT);

    steering_position_controller_sample_t recovery_sample = sample_at(&fixture, 100.0);
    size_t outputs_before = fixture.output_count;
    HOST_TEST_CHECK(steering_position_controller_set_reference(&controller,
                                                               &recovery_sample,
                                                               0.0,
                                                               &report) ==
                    STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED);
    HOST_TEST_CHECK(report.fault ==
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH);
    HOST_TEST_CHECK(report.state == STEERING_POSITION_CONTROLLER_STATE_FAULT);
    HOST_TEST_CHECK(fixture.output_count == outputs_before);
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 10.0, 100U, NULL) ==
                    STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED);
    return true;
}

static bool test_ttl_expiry_and_move_timeout_neutralize(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 10U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    fixture.now_ms = 10U;
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_COMMAND_EXPIRED);
    HOST_TEST_CHECK(!report.target_active);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);

    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    steering_position_controller_config_t config = test_config();
    config.move_timeout_ms = 20U;
    memset(&fixture, 0, sizeof(fixture));
    fixture.output_succeeds = true;
    HOST_TEST_CHECK(steering_position_controller_init(&controller,
                                                      &config,
                                                      fixture_clock_ms,
                                                      &fixture,
                                                      fixture_pwm_output,
                                                      &fixture) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 100U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    fixture.now_ms = 20U;
    sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action ==
                    STEERING_POSITION_CONTROLLER_ACTION_MOVE_TIMEOUT);
    HOST_TEST_CHECK(report.fault == STEERING_POSITION_CONTROLLER_FAULT_MOVE_TIMEOUT);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    return true;
}

static bool test_stop_clears_target_and_prevents_reapply(void)
{
    controller_fixture_t fixture;
    steering_position_controller_t controller;
    steering_position_controller_report_t report;
    HOST_TEST_CHECK(init_fixture(&fixture, &controller));
    HOST_TEST_CHECK(reference_at(&fixture, &controller, 100.0, 0.0));
    HOST_TEST_CHECK(steering_position_controller_set_target(
                        &controller, 20.0, 1000U, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    steering_position_controller_sample_t sample = sample_at(&fixture, 100.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, NULL) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(latest_output(&fixture) == 1040U);

    fixture.now_ms = 1U;
    HOST_TEST_CHECK(steering_position_controller_stop(&controller, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(report.action == STEERING_POSITION_CONTROLLER_ACTION_STOPPED);
    HOST_TEST_CHECK(!report.target_active);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    size_t output_count_after_stop = fixture.output_count;

    fixture.now_ms = 2U;
    sample = sample_at(&fixture, 90.0);
    HOST_TEST_CHECK(steering_position_controller_tick(&controller, &sample, &report) ==
                    STEERING_POSITION_CONTROLLER_OK);
    HOST_TEST_CHECK(!report.target_active);
    HOST_TEST_CHECK(report.action == STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL);
    HOST_TEST_CHECK(fixture.output_count == output_count_after_stop);
    HOST_TEST_CHECK(latest_output(&fixture) == 1500U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_unhomed_target_is_rejected),
        HOST_TEST_CASE(test_explicit_reference_and_cyclic_unwrap_drive_both_directions),
        HOST_TEST_CASE(test_rejects_nonfinite_and_out_of_bound_targets),
        HOST_TEST_CASE(test_arrival_stability_and_reacquire_hysteresis),
        HOST_TEST_CASE(test_arrival_stability_requires_distinct_sensor_samples),
        HOST_TEST_CASE(test_direction_inversion_requires_neutral_settle),
        HOST_TEST_CASE(test_stale_neutral_then_sensor_timeout_fault),
        HOST_TEST_CASE(test_stale_position_request_cannot_refresh_lease_or_keep_drive),
        HOST_TEST_CASE(test_explicit_magnet_fault_latches_immediately),
        HOST_TEST_CASE(test_reference_cannot_clear_a_latched_fault),
        HOST_TEST_CASE(test_ttl_expiry_and_move_timeout_neutralize),
        HOST_TEST_CASE(test_stop_clears_target_and_prevents_reapply),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
