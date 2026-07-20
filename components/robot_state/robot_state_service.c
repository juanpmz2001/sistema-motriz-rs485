#include "robot_state_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ROBOT_STATE_GATE_INVALIDATING_ACTIONS                                      \
    (ROBOT_STATE_ACTION_REQUEST_STOP | ROBOT_STATE_ACTION_EMERGENCY_STOP |          \
     ROBOT_STATE_ACTION_REVOKE_AUTHORITY)

struct robot_state_service_t {
    SemaphoreHandle_t mutex;
    robot_state_model_t model;
    robot_state_inhibit_mask_t source_inhibits[ROBOT_STATE_INHIBIT_SOURCE_COUNT];
    uint64_t gate_epoch;
};

typedef robot_state_outcome_t (*model_operation_t)(robot_state_model_t *model,
                                                   robot_state_transition_t *transition);

static bool valid_source(robot_state_inhibit_source_t source)
{
    return (unsigned int)source < (unsigned int)ROBOT_STATE_INHIBIT_SOURCE_COUNT;
}

static bool valid_inhibits(robot_state_inhibit_mask_t inhibits)
{
    return (inhibits & ~ROBOT_STATE_KNOWN_INHIBITS) == 0;
}

static bool take_mutex(robot_state_service_handle_t handle)
{
    return xSemaphoreTake(handle->mutex, portMAX_DELAY) == pdTRUE;
}

static void copy_snapshot_locked(robot_state_service_handle_t handle,
                                 robot_state_snapshot_t *snapshot)
{
    snapshot->model = handle->model;
    memcpy(snapshot->source_inhibits,
           handle->source_inhibits,
           sizeof(snapshot->source_inhibits));
    snapshot->gate_epoch = handle->gate_epoch;
}

static robot_state_outcome_t complete_operation(
    robot_state_service_handle_t handle,
    robot_state_outcome_t outcome,
    const robot_state_transition_t *local_transition,
    bool invalidate_on_no_change,
    robot_state_transition_t *transition)
{
    if ((local_transition->actions & ROBOT_STATE_GATE_INVALIDATING_ACTIONS) != 0 ||
        (invalidate_on_no_change && outcome == ROBOT_STATE_OUTCOME_NO_CHANGE)) {
        handle->gate_epoch++;
    }

    if (transition) {
        *transition = *local_transition;
    }
    (void)xSemaphoreGive(handle->mutex);
    return outcome;
}

static robot_state_outcome_t run_operation(robot_state_service_handle_t handle,
                                           model_operation_t operation,
                                           bool invalidate_on_no_change,
                                           robot_state_transition_t *transition)
{
    if (!handle || !operation) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (!take_mutex(handle)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }

    robot_state_transition_t local_transition;
    robot_state_outcome_t outcome = operation(&handle->model, &local_transition);
    return complete_operation(handle,
                              outcome,
                              &local_transition,
                              invalidate_on_no_change,
                              transition);
}

static robot_state_inhibit_mask_t aggregate_inhibits(
    robot_state_service_handle_t handle,
    robot_state_inhibit_source_t updated_source,
    robot_state_inhibit_mask_t updated_inhibits)
{
    robot_state_inhibit_mask_t aggregate = 0;
    for (size_t index = 0; index < ROBOT_STATE_INHIBIT_SOURCE_COUNT; ++index) {
        aggregate |= index == (size_t)updated_source ? updated_inhibits
                                                     : handle->source_inhibits[index];
    }
    return aggregate;
}

esp_err_t robot_state_service_init(robot_state_service_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    robot_state_service_handle_t handle = calloc(1, sizeof(struct robot_state_service_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    robot_state_model_init(&handle->model);
    *out_handle = handle;
    return ESP_OK;
}

void robot_state_service_deinit(robot_state_service_handle_t handle)
{
    if (!handle) {
        return;
    }

    vSemaphoreDelete(handle->mutex);
    free(handle);
}

robot_state_outcome_t robot_state_service_get_snapshot(robot_state_service_handle_t handle,
                                                       robot_state_snapshot_t *snapshot)
{
    if (!handle || !snapshot) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (!take_mutex(handle)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }

    copy_snapshot_locked(handle, snapshot);
    (void)xSemaphoreGive(handle->mutex);
    return ROBOT_STATE_OUTCOME_APPLIED;
}

robot_state_outcome_t robot_state_service_boot_complete(
    robot_state_service_handle_t handle,
    robot_state_inhibit_mask_t boot_failure_inhibits,
    robot_state_transition_t *transition)
{
    if (!handle || !valid_inhibits(boot_failure_inhibits)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (!take_mutex(handle)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }

    robot_state_transition_t local_transition;
    robot_state_outcome_t outcome = robot_state_model_boot_complete(
        &handle->model, boot_failure_inhibits, &local_transition);
    if (outcome == ROBOT_STATE_OUTCOME_APPLIED) {
        handle->source_inhibits[ROBOT_STATE_INHIBIT_SOURCE_BOOT] |=
            boot_failure_inhibits;
    }
    return complete_operation(handle, outcome, &local_transition, false, transition);
}

robot_state_outcome_t robot_state_service_publish_inhibits(
    robot_state_service_handle_t handle,
    robot_state_inhibit_source_t source,
    robot_state_inhibit_mask_t inhibits,
    robot_state_transition_t *transition)
{
    if (!handle || !valid_source(source) || !valid_inhibits(inhibits)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (!take_mutex(handle)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }

    robot_state_inhibit_mask_t active_inhibits =
        aggregate_inhibits(handle, source, inhibits);
    robot_state_transition_t local_transition;
    robot_state_outcome_t outcome = robot_state_model_set_inhibits(
        &handle->model, active_inhibits, &local_transition);
    if (outcome == ROBOT_STATE_OUTCOME_APPLIED ||
        outcome == ROBOT_STATE_OUTCOME_NO_CHANGE) {
        handle->source_inhibits[source] = inhibits;
    }
    return complete_operation(handle, outcome, &local_transition, false, transition);
}

robot_state_outcome_t robot_state_service_report_fault(
    robot_state_service_handle_t handle,
    robot_state_inhibit_source_t source,
    robot_state_inhibit_mask_t causes,
    robot_state_transition_t *transition)
{
    if (!handle || !valid_source(source) || causes == 0 || !valid_inhibits(causes)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (!take_mutex(handle)) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }

    robot_state_transition_t local_transition;
    robot_state_outcome_t outcome =
        robot_state_model_report_fault(&handle->model, causes, &local_transition);
    if (outcome == ROBOT_STATE_OUTCOME_APPLIED ||
        outcome == ROBOT_STATE_OUTCOME_NO_CHANGE) {
        handle->source_inhibits[source] |= causes;
    }
    return complete_operation(handle, outcome, &local_transition, false, transition);
}

robot_state_outcome_t robot_state_service_request_arm(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_request_arm, false, transition);
}

robot_state_outcome_t robot_state_service_request_disarm(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_request_disarm, true, transition);
}

robot_state_outcome_t robot_state_service_request_maintenance(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_request_maintenance, false, transition);
}

robot_state_outcome_t robot_state_service_close_maintenance(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_close_maintenance, false, transition);
}

robot_state_outcome_t robot_state_service_request_ota(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_request_ota, false, transition);
}

robot_state_outcome_t robot_state_service_acknowledge_fault(
    robot_state_service_handle_t handle,
    robot_state_transition_t *transition)
{
    return run_operation(handle, robot_state_model_acknowledge_fault, false, transition);
}
