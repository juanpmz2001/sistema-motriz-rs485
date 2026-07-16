#ifndef ROBOT_SAFETY_H
#define ROBOT_SAFETY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ibus_receiver.h"
#include "robot_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOT_SAFETY_REASON_MAX 32

typedef struct robot_safety_t *robot_safety_handle_t;

typedef struct {
    robot_control_handle_t robot;
    ibus_receiver_handle_t ibus_receiver;
    uint32_t period_ms;
    uint32_t rc_loss_timeout_ms;
    uint32_t stop_repeat_ms;
    bool stop_on_rc_loss;
    bool stop_on_motor_fault;
} robot_safety_config_t;

typedef struct {
    bool task_running;
    bool rc_available;
    bool rc_signal_seen;
    bool rc_signal_valid;
    bool rc_loss_active;
    bool motor_fault_active;
    uint32_t rc_last_frame_age_ms;
    uint32_t loop_count;
    uint32_t stop_requests;
    esp_err_t last_stop_error;
    char last_stop_reason[ROBOT_SAFETY_REASON_MAX];
} robot_safety_status_t;

esp_err_t robot_safety_init(const robot_safety_config_t *config, robot_safety_handle_t *out_handle);
void robot_safety_deinit(robot_safety_handle_t handle);
esp_err_t robot_safety_start(robot_safety_handle_t handle);
esp_err_t robot_safety_get_status(robot_safety_handle_t handle, robot_safety_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // ROBOT_SAFETY_H
