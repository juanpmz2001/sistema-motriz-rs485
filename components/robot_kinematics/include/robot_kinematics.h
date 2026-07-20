#ifndef ROBOT_KINEMATICS_H
#define ROBOT_KINEMATICS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_KINEMATICS_SIDE_LEFT = 0,
    ROBOT_KINEMATICS_SIDE_RIGHT,
} robot_kinematics_side_t;

typedef struct {
    double track_width_m;
    double vy_epsilon_mps;
} robot_kinematics_differential_config_t;

typedef struct {
    size_t actuator_id;
    robot_kinematics_side_t side;
    double wheel_radius_m;
    double motor_to_wheel_ratio;
    int direction_sign;
    double max_abs_rpm;
} robot_kinematics_motor_config_t;

typedef struct {
    double vx_mps;
    double vy_mps;
    double wz_radps;
} robot_kinematics_body_velocity_t;

typedef struct {
    size_t actuator_id;
    double motor_rpm;
} robot_kinematics_motor_command_t;

typedef struct {
    double scale;
    bool saturated;
} robot_kinematics_report_t;

typedef enum {
    ROBOT_KINEMATICS_OK = 0,
    ROBOT_KINEMATICS_ERROR_NULL_ARGUMENT,
    ROBOT_KINEMATICS_ERROR_MOTOR_COUNT_ZERO,
    ROBOT_KINEMATICS_ERROR_OUTPUT_CAPACITY,
    ROBOT_KINEMATICS_ERROR_INVALID_TRACK_WIDTH,
    ROBOT_KINEMATICS_ERROR_INVALID_VY_EPSILON,
    ROBOT_KINEMATICS_ERROR_NONFINITE_BODY_VELOCITY,
    ROBOT_KINEMATICS_ERROR_UNSUPPORTED_LATERAL_VELOCITY,
    ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_SIDE,
    ROBOT_KINEMATICS_ERROR_INVALID_WHEEL_RADIUS,
    ROBOT_KINEMATICS_ERROR_INVALID_MOTOR_TO_WHEEL_RATIO,
    ROBOT_KINEMATICS_ERROR_INVALID_DIRECTION_SIGN,
    ROBOT_KINEMATICS_ERROR_INVALID_MAX_ABS_RPM,
    ROBOT_KINEMATICS_ERROR_DUPLICATE_ACTUATOR_ID,
    ROBOT_KINEMATICS_ERROR_MISSING_LEFT_MOTOR,
    ROBOT_KINEMATICS_ERROR_MISSING_RIGHT_MOTOR,
    ROBOT_KINEMATICS_ERROR_NUMERIC_OVERFLOW,
} robot_kinematics_error_t;

/*
 * Commands are emitted in motor_configs order. On any error, motor_commands
 * and report remain unchanged.
 */
robot_kinematics_error_t robot_kinematics_differential_inverse(
    const robot_kinematics_differential_config_t *config,
    const robot_kinematics_motor_config_t motor_configs[],
    size_t motor_count,
    const robot_kinematics_body_velocity_t *body_velocity,
    robot_kinematics_motor_command_t motor_commands[],
    size_t motor_command_capacity,
    robot_kinematics_report_t *report);

const char *robot_kinematics_error_name(robot_kinematics_error_t error);

#ifdef __cplusplus
}
#endif

#endif
