#ifndef ROBOT_CONTROL_H
#define ROBOT_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "svd48.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct robot_control_t *robot_control_handle_t;

typedef struct {
    svd48_handle_t svd48;
    float wheelbase_m;
    float track_width_m;
    float wheel_radius_m;
    float max_wheel_rpm;

    bool enable_steering_servos;
    int steering_servo_pins[SVD48_MOTOR_COUNT];
    int servo_min_us;
    int servo_center_us;
    int servo_max_us;
    float servo_min_deg;
    float servo_max_deg;
} robot_control_config_t;

typedef struct {
    float vx_mps;
    float vy_mps;
    float wz_radps;
    int16_t wheel_rpm[SVD48_MOTOR_COUNT];
    float steering_deg[SVD48_MOTOR_COUNT];
} robot_motion_command_t;

robot_control_handle_t robot_control_init(const robot_control_config_t *config);
void robot_control_deinit(robot_control_handle_t handle);

esp_err_t robot_control_enable_all(robot_control_handle_t handle);
esp_err_t robot_control_stop_all(robot_control_handle_t handle);
esp_err_t robot_control_stop_motor(robot_control_handle_t handle, uint8_t motor);
esp_err_t robot_control_clear_motor_alarm(robot_control_handle_t handle, uint8_t motor);
esp_err_t robot_control_set_motor_speed(robot_control_handle_t handle, uint8_t motor, int16_t rpm);
esp_err_t robot_control_move_vel(robot_control_handle_t handle, float vx_mps, float vy_mps, float wz_radps);
esp_err_t robot_control_poll_once(robot_control_handle_t handle);
void robot_control_set_trace_enabled(robot_control_handle_t handle, bool enabled);
bool robot_control_get_trace_enabled(robot_control_handle_t handle);

bool robot_control_get_motor(robot_control_handle_t handle, uint8_t motor, svd48_motor_telemetry_t *telemetry);
bool robot_control_get_last_motion(robot_control_handle_t handle, robot_motion_command_t *command);
bool robot_control_is_safe_for_ota(robot_control_handle_t handle, char *reason, size_t reason_size);
esp_err_t robot_control_prepare_for_ota(robot_control_handle_t handle);
esp_err_t robot_control_read_svd48_registers(robot_control_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t quantity, uint16_t *out_regs);
esp_err_t robot_control_write_svd48_register(robot_control_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif // ROBOT_CONTROL_H
