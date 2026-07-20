#include <math.h>
#include <string.h>

#include "host_test.h"
#include "robot_kinematics.h"

static const double TEST_TOLERANCE = 1.0e-12;
static const double TEST_RPM_PER_RADIAN_PER_SECOND =
    60.0 / 6.283185307179586476925286766559005768;

static bool nearly_equal(double actual, double expected)
{
    double magnitude = fmax(1.0, fabs(expected));
    return fabs(actual - expected) <= TEST_TOLERANCE * magnitude;
}

static double expected_rpm(double velocity_mps,
                           double radius_m,
                           double ratio,
                           int direction_sign)
{
    return velocity_mps / radius_m * TEST_RPM_PER_RADIAN_PER_SECOND * ratio *
           (double)direction_sign;
}

static bool command_matches(const robot_kinematics_motor_command_t *command,
                            const robot_kinematics_motor_command_t *expected)
{
    return command->actuator_id == expected->actuator_id &&
           command->motor_rpm == expected->motor_rpm;
}

static bool report_matches(const robot_kinematics_report_t *report,
                           const robot_kinematics_report_t *expected)
{
    return report->scale == expected->scale &&
           report->saturated == expected->saturated;
}

static bool expect_error_without_mutation(
    robot_kinematics_error_t expected_error,
    const robot_kinematics_differential_config_t *config,
    const robot_kinematics_motor_config_t motor_configs[],
    size_t motor_count,
    const robot_kinematics_body_velocity_t *velocity,
    size_t output_capacity)
{
    robot_kinematics_motor_command_t commands[4] = {
        {.actuator_id = 101U, .motor_rpm = 11.0},
        {.actuator_id = 102U, .motor_rpm = 12.0},
        {.actuator_id = 103U, .motor_rpm = 13.0},
        {.actuator_id = 104U, .motor_rpm = 14.0},
    };
    robot_kinematics_motor_command_t original_commands[4];
    memcpy(original_commands, commands, sizeof(commands));
    robot_kinematics_report_t report = {.scale = 0.25, .saturated = true};
    robot_kinematics_report_t original_report = report;

    robot_kinematics_error_t error = robot_kinematics_differential_inverse(
        config,
        motor_configs,
        motor_count,
        velocity,
        commands,
        output_capacity,
        &report);

    HOST_TEST_CHECK(error == expected_error);
    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(commands); index++) {
        HOST_TEST_CHECK(command_matches(&commands[index], &original_commands[index]));
    }
    HOST_TEST_CHECK(report_matches(&report, &original_report));
    return true;
}

static bool test_basic_two_wheel_drive(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.6,
        .vy_epsilon_mps = 1.0e-6,
    };
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 7U,
         .side = ROBOT_KINEMATICS_SIDE_LEFT,
         .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1,
         .max_abs_rpm = 1000.0},
        {.actuator_id = 9U,
         .side = ROBOT_KINEMATICS_SIDE_RIGHT,
         .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1,
         .max_abs_rpm = 1000.0},
    };
    const robot_kinematics_body_velocity_t velocity = {
        .vx_mps = 1.0,
        .vy_mps = 0.0,
        .wz_radps = 0.0,
    };
    robot_kinematics_motor_command_t commands[2];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config,
                        motors,
                        HOST_TEST_ARRAY_COUNT(motors),
                        &velocity,
                        commands,
                        HOST_TEST_ARRAY_COUNT(commands),
                        &report) == ROBOT_KINEMATICS_OK);

    double rpm = expected_rpm(1.0, 0.1, 1.0, 1);
    HOST_TEST_CHECK(commands[0].actuator_id == 7U);
    HOST_TEST_CHECK(commands[1].actuator_id == 9U);
    HOST_TEST_CHECK(nearly_equal(commands[0].motor_rpm, rpm));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm, rpm));
    HOST_TEST_CHECK(nearly_equal(report.scale, 1.0));
    HOST_TEST_CHECK(!report.saturated);
    return true;
}

static bool test_four_motors_are_selected_by_side(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.4,
        .vy_epsilon_mps = 0.0,
    };
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 20U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
        {.actuator_id = 10U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
        {.actuator_id = 21U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
        {.actuator_id = 11U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
    };
    const robot_kinematics_body_velocity_t velocity = {
        .vx_mps = 0.5,
        .vy_mps = 0.0,
        .wz_radps = 1.0,
    };
    robot_kinematics_motor_command_t commands[4];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config,
                        motors,
                        HOST_TEST_ARRAY_COUNT(motors),
                        &velocity,
                        commands,
                        HOST_TEST_ARRAY_COUNT(commands),
                        &report) == ROBOT_KINEMATICS_OK);

    double left_rpm = expected_rpm(0.3, 0.1, 1.0, 1);
    double right_rpm = expected_rpm(0.7, 0.1, 1.0, 1);
    HOST_TEST_CHECK(commands[0].actuator_id == 20U);
    HOST_TEST_CHECK(commands[1].actuator_id == 10U);
    HOST_TEST_CHECK(commands[2].actuator_id == 21U);
    HOST_TEST_CHECK(commands[3].actuator_id == 11U);
    HOST_TEST_CHECK(nearly_equal(commands[0].motor_rpm, right_rpm));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm, left_rpm));
    HOST_TEST_CHECK(nearly_equal(commands[2].motor_rpm, right_rpm));
    HOST_TEST_CHECK(nearly_equal(commands[3].motor_rpm, left_rpm));
    HOST_TEST_CHECK(!report.saturated);
    return true;
}

static bool test_pure_rotation(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.5,
        .vy_epsilon_mps = 0.0,
    };
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
    };
    const robot_kinematics_body_velocity_t velocity = {
        .vx_mps = 0.0,
        .vy_mps = 0.0,
        .wz_radps = 2.0,
    };
    robot_kinematics_motor_command_t commands[2];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config, motors, 2U, &velocity, commands, 2U, &report) ==
                    ROBOT_KINEMATICS_OK);
    HOST_TEST_CHECK(nearly_equal(commands[0].motor_rpm,
                                 expected_rpm(-0.5, 0.1, 1.0, 1)));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm,
                                 expected_rpm(0.5, 0.1, 1.0, 1)));
    return true;
}

static bool test_individual_radius_ratio_and_direction(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.5,
        .vy_epsilon_mps = 0.0,
    };
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 30U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 2.0, .direction_sign = -1, .max_abs_rpm = 1000.0},
        {.actuator_id = 40U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.2,
         .motor_to_wheel_ratio = 3.0, .direction_sign = 1, .max_abs_rpm = 1000.0},
    };
    const robot_kinematics_body_velocity_t velocity = {
        .vx_mps = 0.8,
        .vy_mps = 0.0,
        .wz_radps = 0.0,
    };
    robot_kinematics_motor_command_t commands[2];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config, motors, 2U, &velocity, commands, 2U, &report) ==
                    ROBOT_KINEMATICS_OK);
    HOST_TEST_CHECK(nearly_equal(commands[0].motor_rpm,
                                 expected_rpm(0.8, 0.1, 2.0, -1)));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm,
                                 expected_rpm(0.8, 0.2, 3.0, 1)));
    return true;
}

static bool test_rejects_lateral_and_nonfinite_velocity(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.5,
        .vy_epsilon_mps = 1.0e-5,
    };
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
    };
    robot_kinematics_body_velocity_t velocity = {
        .vx_mps = 0.5,
        .vy_mps = 2.0e-5,
        .wz_radps = 0.0,
    };

    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_UNSUPPORTED_LATERAL_VELOCITY,
        &config, motors, 2U, &velocity, 2U));

    velocity.vy_mps = 0.0;
    velocity.vx_mps = NAN;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY,
        &config, motors, 2U, &velocity, 2U));

    velocity.vx_mps = 0.0;
    velocity.wz_radps = INFINITY;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY,
        &config, motors, 2U, &velocity, 2U));
    return true;
}

static bool test_rejects_duplicate_and_missing_sides(void)
{
    const robot_kinematics_differential_config_t config = {
        .track_width_m = 0.5,
        .vy_epsilon_mps = 0.0,
    };
    const robot_kinematics_body_velocity_t velocity = {0.0, 0.0, 0.0};
    robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
    };

    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_DUPLICATE_ACTUATOR_ID,
        &config, motors, 2U, &velocity, 2U));

    motors[1].actuator_id = 2U;
    motors[1].side = ROBOT_KINEMATICS_SIDE_LEFT;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_MISSING_RIGHT_MOTOR,
        &config, motors, 2U, &velocity, 2U));

    motors[0].side = ROBOT_KINEMATICS_SIDE_RIGHT;
    motors[1].side = ROBOT_KINEMATICS_SIDE_RIGHT;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_MISSING_LEFT_MOTOR,
        &config, motors, 2U, &velocity, 2U));
    return true;
}

static bool test_rejects_short_output(void)
{
    const robot_kinematics_differential_config_t config = {0.5, 0.0};
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
    };
    const robot_kinematics_body_velocity_t velocity = {0.5, 0.0, 0.0};

    return expect_error_without_mutation(ROBOT_KINEMATICS_ERROR_OUTPUT_CAPACITY,
                                         &config, motors, 2U, &velocity, 1U);
}

static bool test_rejects_invalid_parameters(void)
{
    robot_kinematics_differential_config_t config = {0.5, 0.0};
    const robot_kinematics_body_velocity_t velocity = {0.0, 0.0, 0.0};
    robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
    };

    config.track_width_m = 0.0;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_TRACK_WIDTH,
        &config, motors, 2U, &velocity, 2U));
    config.track_width_m = 0.5;
    config.vy_epsilon_mps = -1.0;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_VY_EPSILON,
        &config, motors, 2U, &velocity, 2U));
    config.vy_epsilon_mps = 0.0;

    motors[0].side = (robot_kinematics_side_t)99;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_SIDE,
        &config, motors, 2U, &velocity, 2U));
    motors[0].side = ROBOT_KINEMATICS_SIDE_LEFT;
    motors[0].wheel_radius_m = 0.0;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_WHEEL_RADIUS,
        &config, motors, 2U, &velocity, 2U));
    motors[0].wheel_radius_m = 0.1;
    motors[0].motor_to_wheel_ratio = NAN;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_TO_WHEEL_RATIO,
        &config, motors, 2U, &velocity, 2U));
    motors[0].motor_to_wheel_ratio = 1.0;
    motors[0].direction_sign = 0;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_DIRECTION_SIGN,
        &config, motors, 2U, &velocity, 2U));
    motors[0].direction_sign = 1;
    motors[0].max_abs_rpm = INFINITY;
    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_INVALID_MAX_ABS_RPM,
        &config, motors, 2U, &velocity, 2U));
    motors[0].max_abs_rpm = 100.0;

    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_MOTOR_COUNT_ZERO,
        &config, motors, 0U, &velocity, 2U));
    return true;
}

static bool test_global_proportional_saturation(void)
{
    const robot_kinematics_differential_config_t config = {0.5, 0.0};
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 100.0},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT, .wheel_radius_m = 0.1,
         .motor_to_wheel_ratio = 1.0, .direction_sign = 1, .max_abs_rpm = 60.0},
    };
    const robot_kinematics_body_velocity_t velocity = {1.0, 0.0, 2.0};
    robot_kinematics_motor_command_t commands[2];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config, motors, 2U, &velocity, commands, 2U, &report) ==
                    ROBOT_KINEMATICS_OK);

    double raw_left = expected_rpm(0.5, 0.1, 1.0, 1);
    double raw_right = expected_rpm(1.5, 0.1, 1.0, 1);
    double expected_scale = 60.0 / raw_right;
    HOST_TEST_CHECK(report.saturated);
    HOST_TEST_CHECK(nearly_equal(report.scale, expected_scale));
    HOST_TEST_CHECK(nearly_equal(commands[0].motor_rpm, raw_left * expected_scale));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm, 60.0));
    HOST_TEST_CHECK(nearly_equal(commands[1].motor_rpm / commands[0].motor_rpm,
                                 raw_right / raw_left));
    return true;
}

static bool test_extreme_scale_fails_without_output_underflow(void)
{
    const robot_kinematics_differential_config_t config = {0.5, 0.0};
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT,
         .wheel_radius_m = 1.0, .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1, .max_abs_rpm = 1e-300},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT,
         .wheel_radius_m = 1.0, .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1, .max_abs_rpm = 1e-300},
    };
    const robot_kinematics_body_velocity_t velocity = {1e100, 0.0, 0.0};

    HOST_TEST_CHECK(expect_error_without_mutation(
        ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW,
        &config, motors, 2U, &velocity, 2U));
    return true;
}

static bool test_saturated_output_never_exceeds_motor_limit(void)
{
    const robot_kinematics_differential_config_t config = {0.5, 0.0};
    const robot_kinematics_motor_config_t motors[] = {
        {.actuator_id = 1U, .side = ROBOT_KINEMATICS_SIDE_LEFT,
         .wheel_radius_m = 1.0, .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1, .max_abs_rpm = 2.1916609841169163e-69},
        {.actuator_id = 2U, .side = ROBOT_KINEMATICS_SIDE_RIGHT,
         .wheel_radius_m = 1.0, .motor_to_wheel_ratio = 1.0,
         .direction_sign = 1, .max_abs_rpm = 2.1916609841169163e-69},
    };
    const robot_kinematics_body_velocity_t velocity = {
        4.3319120399067333e-66, 0.0, 0.0};
    robot_kinematics_motor_command_t commands[2];
    robot_kinematics_report_t report;

    HOST_TEST_CHECK(robot_kinematics_differential_inverse(
                        &config, motors, 2U, &velocity, commands, 2U, &report) ==
                    ROBOT_KINEMATICS_OK);
    HOST_TEST_CHECK(report.saturated);
    HOST_TEST_CHECK(fabs(commands[0].motor_rpm) <= motors[0].max_abs_rpm);
    HOST_TEST_CHECK(fabs(commands[1].motor_rpm) <= motors[1].max_abs_rpm);
    return true;
}

static bool test_error_names(void)
{
    const struct {
        robot_kinematics_error_t error;
        const char *name;
    } cases[] = {
        {ROBOT_KINEMATICS_OK, "OK"},
        {ROBOT_KINEMATICS_ERROR_NULL_ARGUMENT, "NULL_ARGUMENT"},
        {ROBOT_KINEMATICS_ERROR_MOTOR_COUNT_ZERO, "MOTOR_COUNT_ZERO"},
        {ROBOT_KINEMATICS_ERROR_OUTPUT_CAPACITY, "OUTPUT_CAPACITY"},
        {ROBOT_KINEMATICS_ERROR_INVALID_TRACK_WIDTH, "INVALID_TRACK_WIDTH"},
        {ROBOT_KINEMATICS_ERROR_INVALID_VY_EPSILON, "INVALID_VY_EPSILON"},
        {ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY, "NONFINITE_BODY_VELOCITY"},
        {ROBOT_KINEMATICS_ERROR_UNSUPPORTED_LATERAL_VELOCITY,
         "UNSUPPORTED_LATERAL_VELOCITY"},
        {ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_SIDE, "INVALID_MOTOR_SIDE"},
        {ROBOT_KINEMATICS_ERROR_INVALID_WHEEL_RADIUS, "INVALID_WHEEL_RADIUS"},
        {ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_TO_WHEEL_RATIO,
         "INVALID_MOTOR_TO_WHEEL_RATIO"},
        {ROBOT_KINEMATICS_ERROR_INVALID_DIRECTION_SIGN, "INVALID_DIRECTION_SIGN"},
        {ROBOT_KINEMATICS_ERROR_INVALID_MAX_ABS_RPM, "INVALID_MAX_ABS_RPM"},
        {ROBOT_KINEMATICS_ERROR_DUPLICATE_ACTUATOR_ID, "DUPLICATE_ACTUATOR_ID"},
        {ROBOT_KINEMATICS_ERROR_MISSING_LEFT_MOTOR, "MISSING_LEFT_MOTOR"},
        {ROBOT_KINEMATICS_ERROR_MISSING_RIGHT_MOTOR, "MISSING_RIGHT_MOTOR"},
        {ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW, "NUMERIC_OVERFLOW"},
    };

    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(cases); index++) {
        HOST_TEST_CHECK(strcmp(robot_kinematics_error_name(cases[index].error),
                               cases[index].name) == 0);
    }
    HOST_TEST_CHECK(strcmp(robot_kinematics_error_name(
                               (robot_kinematics_error_t)999),
                           "UNKNOWN") == 0);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_basic_two_wheel_drive),
        HOST_TEST_CASE(test_four_motors_are_selected_by_side),
        HOST_TEST_CASE(test_pure_rotation),
        HOST_TEST_CASE(test_individual_radius_ratio_and_direction),
        HOST_TEST_CASE(test_rejects_lateral_and_nonfinite_velocity),
        HOST_TEST_CASE(test_rejects_duplicate_and_missing_sides),
        HOST_TEST_CASE(test_rejects_short_output),
        HOST_TEST_CASE(test_rejects_invalid_parameters),
        HOST_TEST_CASE(test_global_proportional_saturation),
        HOST_TEST_CASE(test_extreme_scale_fails_without_output_underflow),
        HOST_TEST_CASE(test_saturated_output_never_exceeds_motor_limit),
        HOST_TEST_CASE(test_error_names),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
