#ifndef MOTION_APPLICATION_SERVICE_H
#define MOTION_APPLICATION_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actuation_application_port.h"
#include "esp_err.h"
#include "motion_control_port.h"
#include "motion_application_model.h"
#include "motion_status_port.h"
#include "robot_profile.h"

#define MOTION_APPLICATION_DEFAULT_PERIOD_MS 20U
#define MOTION_APPLICATION_DEFAULT_TASK_PRIORITY 9U
#define MOTION_APPLICATION_TASK_STACK_SIZE 6144U

typedef struct motion_application_service_t *motion_application_service_handle_t;

typedef bool (*motion_application_safety_gate_fn_t)(void *context,
                                                     char *detail,
                                                     size_t detail_size);

typedef struct {
    const robot_profile_t *profile;
    actuation_application_port_t *actuation;
    uint32_t period_ms;
    uint32_t task_priority;
    motion_application_safety_gate_fn_t safety_gate;
    void *safety_context;
} motion_application_service_config_t;

typedef struct {
    bool accepted;
    char detail[MOTION_STATUS_DETAIL_MAX];
} motion_application_submit_result_t;

esp_err_t motion_application_service_init(
    const motion_application_service_config_t *config,
    motion_application_service_handle_t *out_handle);
esp_err_t motion_application_service_start(
    motion_application_service_handle_t handle);
void motion_application_service_deinit(
    motion_application_service_handle_t handle);

motion_application_submit_result_t motion_application_service_submit(
    motion_application_service_handle_t handle,
    const motion_application_event_t *event);

/* Non-blocking ingress for transport tasks. Events are copied into semantic
 * mailboxes; STOP/DISARM evict older ARM/COMMAND intent and are consumed first. */
motion_application_submit_result_t motion_application_service_publish(
    motion_application_service_handle_t handle,
    const motion_application_event_t *event);

motion_status_port_t *motion_application_service_status_port(
    motion_application_service_handle_t handle);

motion_control_port_t *motion_application_service_control_port(
    motion_application_service_handle_t handle);

bool motion_application_service_limits(
    motion_application_service_handle_t handle,
    float *max_vx_mps,
    float *max_vy_mps,
    float *max_wz_radps);

#endif
