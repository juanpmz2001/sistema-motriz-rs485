#ifndef ROBOT_SAFETY_H
#define ROBOT_SAFETY_H

#include <stdbool.h>
#include <stdint.h>
#include "actuation_application_port.h"
#include "esp_err.h"
#include "ibus_receiver.h"
#include "robot_control.h"
#include "robot_safety_rc_lan_interlock_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOT_SAFETY_REASON_MAX 32

typedef struct robot_safety_t *robot_safety_handle_t;

typedef struct {
    robot_control_handle_t robot; /* telemetry-only legacy dependency */
    actuation_application_port_t *stop_port;
    ibus_receiver_handle_t ibus_receiver;
    uint32_t period_ms;
    uint32_t rc_loss_timeout_ms;
    uint32_t stop_repeat_ms;
    bool stop_on_rc_loss;
    bool stop_on_motor_fault;
    robot_safety_rc_lan_interlock_config_t rc_lan_interlock;
} robot_safety_config_t;

typedef struct {
    bool task_running;
    bool rc_available;
    bool rc_signal_seen;
    bool rc_signal_valid;
    bool rc_loss_active;
    bool motor_fault_active;
    bool lan_control_allowed;
    uint16_t rc_lan_channel_us;
    uint32_t rc_lan_priority_epoch;
    robot_safety_rc_lan_interlock_state_t rc_lan_interlock_state;
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
