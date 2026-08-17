#include "host_test.h"

#include <string.h>

#include "robot_profile.h"

static size_t count_bus_type(const robot_profile_t *profile,
                             robot_bus_type_t type)
{
    size_t count = 0U;
    for (size_t index = 0; index < profile->bus_count; ++index) {
        if (profile->buses[index].type == type) {
            count++;
        }
    }
    return count;
}

static bool common_profile_contract(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(profile != NULL);
    HOST_TEST_CHECK(robot_profile_validate(profile) == ROBOT_PROFILE_VALID);
#ifdef BOTFARMS_EXPECT_STEERING_PROFILE
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_UART_RS485) == 0U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_GPIO) == 0U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_PWM) == 1U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_I2C) == 1U);
    return true;
#else
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_UART_RS485) == 1U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_GPIO) == 1U);
    HOST_TEST_CHECK(profile->buses[0].type == ROBOT_BUS_UART_RS485);
    HOST_TEST_CHECK(profile->buses[0].id == 1U);
    HOST_TEST_CHECK(profile->buses[0].rate == 115200U);
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        HOST_TEST_CHECK(endpoint->id == index + 1U);
        HOST_TEST_CHECK(endpoint->capabilities ==
                        (ROBOT_CAPABILITY_VELOCITY_RPM |
                         ROBOT_CAPABILITY_STOPPABLE));
        HOST_TEST_CHECK(endpoint->min_rpm == -15);
        HOST_TEST_CHECK(endpoint->max_rpm == 15);
    }
    return true;
#endif
}

#ifdef BOTFARMS_EXPECT_STEERING_PROFILE
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "bench_single_steering_as5600") == 0);
    HOST_TEST_CHECK(profile->bus_count == 2U);
    HOST_TEST_CHECK(profile->buses[0].type == ROBOT_BUS_PWM);
    HOST_TEST_CHECK(profile->buses[0].pins[0] == 14);
    HOST_TEST_CHECK(profile->buses[1].type == ROBOT_BUS_I2C);
    HOST_TEST_CHECK(profile->buses[1].pins[0] == 5);
    HOST_TEST_CHECK(profile->buses[1].pins[1] == 7);
    HOST_TEST_CHECK(profile->buses[1].rate == 5000U);
    HOST_TEST_CHECK(profile->buses[1].response_timeout_ms == 25U);
    HOST_TEST_CHECK(profile->buses[1].telemetry_period_ms == 40U);
    HOST_TEST_CHECK(profile->buses[1].stale_timeout_ms == 120U);
    HOST_TEST_CHECK(profile->device_count == 3U);
    HOST_TEST_CHECK(profile->devices[0].driver_id == ROBOT_DRIVER_PWM_MOTOR_MODE);
    HOST_TEST_CHECK(profile->devices[1].driver_id == ROBOT_DRIVER_AS5600);
    HOST_TEST_CHECK(profile->devices[2].driver_id ==
                    ROBOT_DRIVER_STEERING_POSITION_CONTROLLER);
    HOST_TEST_CHECK(profile->devices[2].bus_id == ROBOT_PROFILE_NO_BUS);
    HOST_TEST_CHECK(profile->endpoint_count == 2U);
    HOST_TEST_CHECK(profile->endpoints[0].capabilities ==
                    (ROBOT_CAPABILITY_POSITION | ROBOT_CAPABILITY_STOPPABLE |
                     ROBOT_CAPABILITY_POSITION_REFERENCE));
    HOST_TEST_CHECK(profile->endpoints[0].min_position_degrees == -90.0f);
    HOST_TEST_CHECK(profile->endpoints[0].max_position_degrees == 90.0f);
    HOST_TEST_CHECK(profile->endpoints[1].capabilities ==
                    ROBOT_CAPABILITY_POSITION_OBSERVATION);
    HOST_TEST_CHECK(profile->steering_axis_count == 1U);
    HOST_TEST_CHECK(profile->steering_axes[0].calibration != NULL);
    HOST_TEST_CHECK(profile->steering_axes[0].calibration->correction_count ==
                    128U);
    HOST_TEST_CHECK(profile->steering_axes[0]
                        .allow_magnet_too_weak_for_development);
    /* One primary STATUS+RAW read and the one-shot diagnostics read, followed
     * by the selected service cadence, must leave time before stale-neutral.
     * This is a profile validation invariant rather than an assumption hidden
     * in the service task. */
    robot_profile_t poll_budget_violation = *profile;
    poll_budget_violation.steering_axes[0].sensor_neutral_after_ms =
        profile->buses[1].response_timeout_ms * 2U +
        profile->buses[1].telemetry_period_ms +
        ROBOT_PROFILE_STEERING_SERVICE_PERIOD_MS;
    HOST_TEST_CHECK(robot_profile_validate(&poll_budget_violation) ==
                    ROBOT_PROFILE_BAD_STEERING_AXIS);
    robot_profile_t wire_budget_violation = *profile;
    wire_budget_violation.buses[1].response_timeout_ms = 20U;
    HOST_TEST_CHECK(robot_profile_validate(&wire_budget_violation) ==
                    ROBOT_PROFILE_BAD_STEERING_AXIS);
    robot_profile_t pwm_period_violation = *profile;
    pwm_period_violation.steering_axes[0].pwm_maximum_us = 20000U;
    HOST_TEST_CHECK(robot_profile_validate(&pwm_period_violation) ==
                    ROBOT_PROFILE_BAD_STEERING_AXIS);
    HOST_TEST_CHECK(profile->application.kind == ROBOT_PROFILE_NO_GEOMETRY);
    return true;
}
#elif defined(BOTFARMS_EXPECT_RAFA_PROFILE)
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "rafa") == 0);
    HOST_TEST_CHECK(strcmp(profile->board->id, "botfarms_esp32s3_rev1") == 0);
    HOST_TEST_CHECK(profile->bus_count == 2U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_UART_RS485) == 1U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_GPIO) == 1U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_PWM) == 0U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_I2C) == 0U);
    HOST_TEST_CHECK(profile->buses[0].peripheral == 2U);
    HOST_TEST_CHECK(profile->buses[0].pins[0] == 17);
    HOST_TEST_CHECK(profile->buses[0].pins[1] == 16);
    HOST_TEST_CHECK(profile->buses[0].rate == 115200U);
    HOST_TEST_CHECK(profile->buses[1].type == ROBOT_BUS_GPIO);
    HOST_TEST_CHECK(profile->buses[1].pins[0] == 14);
    HOST_TEST_CHECK(profile->device_count == 1U);
    HOST_TEST_CHECK(profile->devices[0].driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(profile->devices[0].bus_id == 1U);
    HOST_TEST_CHECK(profile->devices[0].address == 2U);
    HOST_TEST_CHECK(profile->devices[0].channel_count == 2U);
    HOST_TEST_CHECK(profile->devices[0].criticality == ROBOT_ENDPOINT_REQUIRED);
    HOST_TEST_CHECK(profile->endpoint_count == 2U);
    HOST_TEST_CHECK(strcmp(profile->endpoints[0].name, "rafa_traction_m1") == 0);
    HOST_TEST_CHECK(strcmp(profile->endpoints[1].name, "rafa_traction_m2") == 0);
    HOST_TEST_CHECK(profile->endpoints[0].device_id == 1U);
    HOST_TEST_CHECK(profile->endpoints[1].device_id == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].channel == 0U);
    HOST_TEST_CHECK(profile->endpoints[1].channel == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].criticality == ROBOT_ENDPOINT_REQUIRED);
    HOST_TEST_CHECK(profile->endpoints[1].criticality == ROBOT_ENDPOINT_REQUIRED);
    HOST_TEST_CHECK(profile->steering_axis_count == 0U);
    HOST_TEST_CHECK(profile->application.kind == ROBOT_PROFILE_NO_GEOMETRY);
    HOST_TEST_CHECK(robot_profile_validate(profile) == ROBOT_PROFILE_VALID);

    robot_profile_t pin_conflict = *profile;
    pin_conflict.buses[1].pins[0] = 17;
    HOST_TEST_CHECK(robot_profile_validate(&pin_conflict) ==
                    ROBOT_PROFILE_PIN_CONFLICT);
    return true;
}
#elif defined(BOTFARMS_EXPECT_BENCH_PROFILE)
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "bench_single_svd48_motor") == 0);
    HOST_TEST_CHECK(profile->device_count == 1U);
    HOST_TEST_CHECK(profile->devices[0].driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(profile->devices[0].bus_id == 1U);
    HOST_TEST_CHECK(profile->devices[0].address == 1U);
    HOST_TEST_CHECK(profile->devices[0].channel_count == 2U);
    HOST_TEST_CHECK(profile->endpoint_count == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].device_id == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].channel == 0U);
    HOST_TEST_CHECK(profile->application.kind == ROBOT_PROFILE_NO_GEOMETRY);
    /* The endpoint array is the explicit legacy index mapping. */
    HOST_TEST_CHECK(profile->endpoint_count > 0U);
    HOST_TEST_CHECK(profile->endpoint_count <= 1U);
    return true;
}
#else
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "current_robot") == 0);
    HOST_TEST_CHECK(profile->device_count == 2U);
    for (size_t index = 0; index < profile->device_count; ++index) {
        HOST_TEST_CHECK(profile->devices[index].driver_id == ROBOT_DRIVER_SVD48);
        HOST_TEST_CHECK(profile->devices[index].bus_id == 1U);
        HOST_TEST_CHECK(profile->devices[index].address == index + 1U);
        HOST_TEST_CHECK(profile->devices[index].channel_count == 2U);
    }
    HOST_TEST_CHECK(profile->endpoint_count == 4U);
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        HOST_TEST_CHECK(profile->endpoints[index].device_id == index / 2U + 1U);
        HOST_TEST_CHECK(profile->endpoints[index].channel == index % 2U);
    }
    HOST_TEST_CHECK(profile->application.kind ==
                    ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY);
    HOST_TEST_CHECK(profile->application.wheelbase_m > 0.0f);
    HOST_TEST_CHECK(profile->application.track_width_m > 0.0f);
    HOST_TEST_CHECK(profile->application.wheel_radius_m > 0.0f);
    return true;
}
#endif

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(common_profile_contract),
        HOST_TEST_CASE(selected_profile_shape),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
