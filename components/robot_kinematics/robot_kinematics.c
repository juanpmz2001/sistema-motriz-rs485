#include "robot_kinematics.h"

#include <float.h>
#include <math.h>

static const long double RPM_PER_RADIAN_PER_SECOND =
    60.0L / 6.283185307179586476925286766559005768L;

static bool is_valid_side(robot_kinematics_side_t side)
{
    return side == ROBOT_KINEMATICS_SIDE_LEFT ||
           side == ROBOT_KINEMATICS_SIDE_RIGHT;
}

static long double unscaled_motor_rpm(
    const robot_kinematics_motor_config_t *motor,
    long double left_velocity_mps,
    long double right_velocity_mps)
{
    long double wheel_velocity_mps = motor->side == ROBOT_KINEMATICS_SIDE_LEFT
                                         ? left_velocity_mps
                                         : right_velocity_mps;
    long double wheel_radps = wheel_velocity_mps / (long double)motor->wheel_radius_m;
    return wheel_radps * RPM_PER_RADIAN_PER_SECOND *
           (long double)motor->motor_to_wheel_ratio *
           (long double)motor->direction_sign;
}

static robot_kinematics_error_t validate_motor_configs(
    const robot_kinematics_motor_config_t motor_configs[],
    size_t motor_count)
{
    size_t left_count = 0U;
    size_t right_count = 0U;

    for (size_t index = 0U; index < motor_count; index++) {
        const robot_kinematics_motor_config_t *motor = &motor_configs[index];

        if (!is_valid_side(motor->side)) {
            return ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_SIDE;
        }
        if (!isfinite(motor->wheel_radius_m) || motor->wheel_radius_m <= 0.0) {
            return ROBOT_KINEMATICS_ERROR_INVALID_WHEEL_RADIUS;
        }
        if (!isfinite(motor->motor_to_wheel_ratio) ||
            motor->motor_to_wheel_ratio <= 0.0) {
            return ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_TO_WHEEL_RATIO;
        }
        if (motor->direction_sign != -1 && motor->direction_sign != 1) {
            return ROBOT_KINEMATICS_ERROR_INVALID_DIRECTION_SIGN;
        }
        if (!isfinite(motor->max_abs_rpm) || motor->max_abs_rpm <= 0.0) {
            return ROBOT_KINEMATICS_ERROR_INVALID_MAX_ABS_RPM;
        }

        for (size_t previous = 0U; previous < index; previous++) {
            if (motor_configs[previous].actuator_id == motor->actuator_id) {
                return ROBOT_KINEMATICS_ERROR_DUPLICATE_ACTUATOR_ID;
            }
        }

        if (motor->side == ROBOT_KINEMATICS_SIDE_LEFT) {
            left_count++;
        } else {
            right_count++;
        }
    }

    if (left_count == 0U) {
        return ROBOT_KINEMATICS_ERROR_MISSING_LEFT_MOTOR;
    }
    if (right_count == 0U) {
        return ROBOT_KINEMATICS_ERROR_MISSING_RIGHT_MOTOR;
    }
    return ROBOT_KINEMATICS_OK;
}

robot_kinematics_error_t robot_kinematics_differential_inverse(
    const robot_kinematics_differential_config_t *config,
    const robot_kinematics_motor_config_t motor_configs[],
    size_t motor_count,
    const robot_kinematics_body_velocity_t *body_velocity,
    robot_kinematics_motor_command_t motor_commands[],
    size_t motor_command_capacity,
    robot_kinematics_report_t *report)
{
    if (config == NULL || motor_configs == NULL || body_velocity == NULL ||
        motor_commands == NULL || report == NULL) {
        return ROBOT_KINEMATICS_ERROR_NULL_ARGUMENT;
    }
    if (motor_count == 0U) {
        return ROBOT_KINEMATICS_ERROR_MOTOR_COUNT_ZERO;
    }
    if (motor_command_capacity < motor_count) {
        return ROBOT_KINEMATICS_ERROR_OUTPUT_CAPACITY;
    }
    if (!isfinite(config->track_width_m) || config->track_width_m <= 0.0) {
        return ROBOT_KINEMATICS_ERROR_INVALID_TRACK_WIDTH;
    }
    if (!isfinite(config->vy_epsilon_mps) || config->vy_epsilon_mps < 0.0) {
        return ROBOT_KINEMATICS_ERROR_INVALID_VY_EPSILON;
    }
    if (!isfinite(body_velocity->vx_mps) || !isfinite(body_velocity->vy_mps) ||
        !isfinite(body_velocity->wz_radps)) {
        return ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY;
    }
    if (fabs(body_velocity->vy_mps) > config->vy_epsilon_mps) {
        return ROBOT_KINEMATICS_ERROR_UNSUPPORTED_LATERAL_VELOCITY;
    }

    robot_kinematics_error_t validation =
        validate_motor_configs(motor_configs, motor_count);
    if (validation != ROBOT_KINEMATICS_OK) {
        return validation;
    }

    long double yaw_velocity_mps =
        (long double)body_velocity->wz_radps *
        ((long double)config->track_width_m / 2.0L);
    long double left_velocity_mps =
        (long double)body_velocity->vx_mps - yaw_velocity_mps;
    long double right_velocity_mps =
        (long double)body_velocity->vx_mps + yaw_velocity_mps;

    if (!isfinite(yaw_velocity_mps) || !isfinite(left_velocity_mps) ||
        !isfinite(right_velocity_mps)) {
        return ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW;
    }

    long double scale = 1.0L;
    for (size_t index = 0U; index < motor_count; index++) {
        long double raw_rpm = unscaled_motor_rpm(&motor_configs[index],
                                                 left_velocity_mps,
                                                 right_velocity_mps);
        if (!isfinite(raw_rpm) || fabsl(raw_rpm) > (long double)DBL_MAX) {
            return ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW;
        }

        long double abs_rpm = fabsl(raw_rpm);
        if (abs_rpm > (long double)motor_configs[index].max_abs_rpm) {
            long double motor_scale =
                (long double)motor_configs[index].max_abs_rpm / abs_rpm;
            if (motor_scale < scale) {
                scale = motor_scale;
            }
        }
    }

    if (scale < (long double)DBL_TRUE_MIN) {
        return ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW;
    }

    robot_kinematics_report_t completed_report = {
        .scale = (double)scale,
        .saturated = scale < 1.0L,
    };

    for (size_t index = 0U; index < motor_count; index++) {
        long double raw_rpm = unscaled_motor_rpm(&motor_configs[index],
                                                 left_velocity_mps,
                                                 right_velocity_mps);
        double motor_rpm = (double)(raw_rpm * scale);
        if (fabs(motor_rpm) > motor_configs[index].max_abs_rpm) {
            motor_rpm = copysign(motor_configs[index].max_abs_rpm, motor_rpm);
        }
        robot_kinematics_motor_command_t completed_command = {
            .actuator_id = motor_configs[index].actuator_id,
            .motor_rpm = motor_rpm,
        };
        motor_commands[index] = completed_command;
    }
    *report = completed_report;
    return ROBOT_KINEMATICS_OK;
}

const char *robot_kinematics_error_name(robot_kinematics_error_t error)
{
    switch (error) {
    case ROBOT_KINEMATICS_OK:
        return "OK";
    case ROBOT_KINEMATICS_ERROR_NULL_ARGUMENT:
        return "NULL_ARGUMENT";
    case ROBOT_KINEMATICS_ERROR_MOTOR_COUNT_ZERO:
        return "MOTOR_COUNT_ZERO";
    case ROBOT_KINEMATICS_ERROR_OUTPUT_CAPACITY:
        return "OUTPUT_CAPACITY";
    case ROBOT_KINEMATICS_ERROR_INVALID_TRACK_WIDTH:
        return "INVALID_TRACK_WIDTH";
    case ROBOT_KINEMATICS_ERROR_INVALID_VY_EPSILON:
        return "INVALID_VY_EPSILON";
    case ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY:
        return "NONFINITE_BODY_VELOCITY";
    case ROBOT_KINEMATICS_ERROR_UNSUPPORTED_LATERAL_VELOCITY:
        return "UNSUPPORTED_LATERAL_VELOCITY";
    case ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_SIDE:
        return "INVALID_MOTOR_SIDE";
    case ROBOT_KINEMATICS_ERROR_INVALID_WHEEL_RADIUS:
        return "INVALID_WHEEL_RADIUS";
    case ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_TO_WHEEL_RATIO:
        return "INVALID_MOTOR_TO_WHEEL_RATIO";
    case ROBOT_KINEMATICS_ERROR_INVALID_DIRECTION_SIGN:
        return "INVALID_DIRECTION_SIGN";
    case ROBOT_KINEMATICS_ERROR_INVALID_MAX_ABS_RPM:
        return "INVALID_MAX_ABS_RPM";
    case ROBOT_KINEMATICS_ERROR_DUPLICATE_ACTUATOR_ID:
        return "DUPLICATE_ACTUATOR_ID";
    case ROBOT_KINEMATICS_ERROR_MISSING_LEFT_MOTOR:
        return "MISSING_LEFT_MOTOR";
    case ROBOT_KINEMATICS_ERROR_MISSING_RIGHT_MOTOR:
        return "MISSING_RIGHT_MOTOR";
    case ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW:
        return "NUMERIC_OVERFLOW";
    default:
        return "UNKNOWN";
    }
}
