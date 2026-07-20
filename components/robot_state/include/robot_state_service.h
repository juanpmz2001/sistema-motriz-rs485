#ifndef ROBOT_STATE_SERVICE_H
#define ROBOT_STATE_SERVICE_H

#include <stdint.h>

#include "esp_err.h"
#include "robot_state_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct robot_state_service_t *robot_state_service_handle_t;

typedef enum {
    ROBOT_STATE_INHIBIT_SOURCE_BOOT = 0,
    ROBOT_STATE_INHIBIT_SOURCE_PROFILE,
    ROBOT_STATE_INHIBIT_SOURCE_CONTROLLER,
    ROBOT_STATE_INHIBIT_SOURCE_RC,
    ROBOT_STATE_INHIBIT_SOURCE_AUTHORITY,
    ROBOT_STATE_INHIBIT_SOURCE_ESTOP,
    ROBOT_STATE_INHIBIT_SOURCE_CONFIG,
    ROBOT_STATE_INHIBIT_SOURCE_COUNT,
} robot_state_inhibit_source_t;

typedef struct {
    robot_state_model_t model;
    robot_state_inhibit_mask_t source_inhibits[ROBOT_STATE_INHIBIT_SOURCE_COUNT];
    uint64_t gate_epoch;
} robot_state_snapshot_t;

esp_err_t robot_state_service_init(robot_state_service_handle_t *out_handle);

/* The caller must quiesce service users before deinitializing the handle. */
void robot_state_service_deinit(robot_state_service_handle_t handle);

robot_state_outcome_t robot_state_service_get_snapshot(robot_state_service_handle_t handle,
                                                       robot_state_snapshot_t *snapshot);

robot_state_outcome_t robot_state_service_boot_complete(
    robot_state_service_handle_t handle,
    robot_state_inhibit_mask_t boot_failure_inhibits,
    robot_state_transition_t *transition);

/*
 * Replaces the complete inhibit mask for one single-owner source slot. Multiple
 * producers must aggregate externally before publishing through the slot.
 */
robot_state_outcome_t robot_state_service_publish_inhibits(
    robot_state_service_handle_t handle,
    robot_state_inhibit_source_t source,
    robot_state_inhibit_mask_t inhibits,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_report_fault(
    robot_state_service_handle_t handle,
    robot_state_inhibit_source_t source,
    robot_state_inhibit_mask_t causes,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_request_arm(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_request_disarm(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_request_maintenance(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_close_maintenance(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_request_ota(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_service_acknowledge_fault(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition);

#ifdef __cplusplus
}
#endif

#endif
