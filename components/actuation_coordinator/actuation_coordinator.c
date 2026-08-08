#include "actuation_coordinator.h"

#include <string.h>

static void reset_report(actuation_report_t *report, size_t requested)
{
    if (!report) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->requested = requested;
    report->outcome = ACTUATION_RESULT_FAILURE;
}

static bool acquire(const actuation_coordinator_t *coordinator)
{
    return coordinator->lock.acquire(coordinator->lock.context);
}

static void release(const actuation_coordinator_t *coordinator)
{
    coordinator->lock.release(coordinator->lock.context);
}

bool actuation_coordinator_init(actuation_coordinator_t *coordinator,
                                robot_endpoint_registry_t *registry,
                                const actuation_lock_port_t *lock)
{
    if (!coordinator || !registry || !lock || !lock->acquire || !lock->release) {
        return false;
    }
    coordinator->registry = registry;
    coordinator->lock = *lock;
    return true;
}

static void rollback_applied_endpoints(actuation_coordinator_t *coordinator,
                                       actuation_report_t *report,
                                       size_t applied_count)
{
    for (size_t applied = 0; applied < applied_count; ++applied) {
        actuation_endpoint_result_t *previous = &report->endpoints[applied];
        if (!previous->applied) {
            continue;
        }
        robot_endpoint_t *previous_endpoint = robot_endpoint_registry_find(
            coordinator->registry, previous->endpoint_id);
        previous->rollback_stop_attempted = true;
        previous->rollback_stop_error = robot_endpoint_stop(previous_endpoint);
    }
}

static actuation_result_t apply_velocity_locked(
    actuation_coordinator_t *coordinator,
    const actuation_velocity_request_t *requests,
    size_t request_count,
    actuation_report_t *report)
{
    for (size_t index = 0; index < request_count; ++index) {
        const actuation_velocity_request_t *request = &requests[index];
        robot_endpoint_t *endpoint = robot_endpoint_registry_find(
            coordinator->registry, request->endpoint_id);
        actuation_endpoint_result_t *result = &report->endpoints[index];
        result->endpoint_id = request->endpoint_id;
        result->error = endpoint ? robot_velocity_set_rpm(endpoint, request->rpm)
                                 : ROBOT_CAP_UNAVAILABLE;
        if (result->error == ROBOT_CAP_OK) {
            result->applied = true;
            ++report->applied;
            continue;
        }

        if (endpoint && endpoint->criticality == ROBOT_ENDPOINT_REQUIRED) {
            rollback_applied_endpoints(coordinator, report, index);
        }
        return report->applied ? ACTUATION_RESULT_PARTIAL : ACTUATION_RESULT_FAILURE;
    }
    return ACTUATION_RESULT_SUCCESS;
}

actuation_result_t actuation_coordinator_apply_velocity_rpm(
    actuation_coordinator_t *coordinator,
    const actuation_velocity_request_t *requests,
    size_t request_count,
    actuation_report_t *report)
{
    reset_report(report, request_count);
    if (!coordinator || !coordinator->registry || !requests || !report ||
        request_count == 0 || request_count > ACTUATION_COORDINATOR_MAX_SETPOINTS) {
        return ACTUATION_RESULT_FAILURE;
    }
    if (!acquire(coordinator)) {
        report->outcome = ACTUATION_RESULT_LOCK_TIMEOUT;
        return report->outcome;
    }
    report->outcome = apply_velocity_locked(coordinator, requests, request_count, report);
    release(coordinator);
    return report->outcome;
}

actuation_result_t actuation_coordinator_set_velocity_rpm(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    int16_t rpm,
    actuation_report_t *report)
{
    const actuation_velocity_request_t request = {.endpoint_id = endpoint_id, .rpm = rpm};
    return actuation_coordinator_apply_velocity_rpm(coordinator, &request, 1, report);
}

static actuation_result_t apply_position_locked(
    actuation_coordinator_t *coordinator,
    const actuation_position_request_t *requests,
    size_t request_count,
    actuation_report_t *report)
{
    for (size_t index = 0; index < request_count; ++index) {
        const actuation_position_request_t *request = &requests[index];
        robot_endpoint_t *endpoint = robot_endpoint_registry_find(
            coordinator->registry, request->endpoint_id);
        actuation_endpoint_result_t *result = &report->endpoints[index];
        result->endpoint_id = request->endpoint_id;
        result->error = endpoint
                            ? robot_position_set_degrees(endpoint, request->degrees)
                            : ROBOT_CAP_UNAVAILABLE;
        if (result->error == ROBOT_CAP_OK) {
            result->applied = true;
            ++report->applied;
            continue;
        }

        if (endpoint && endpoint->criticality == ROBOT_ENDPOINT_REQUIRED) {
            rollback_applied_endpoints(coordinator, report, index);
        }
        return report->applied ? ACTUATION_RESULT_PARTIAL : ACTUATION_RESULT_FAILURE;
    }
    return ACTUATION_RESULT_SUCCESS;
}

actuation_result_t actuation_coordinator_apply_position_degrees(
    actuation_coordinator_t *coordinator,
    const actuation_position_request_t *requests,
    size_t request_count,
    actuation_report_t *report)
{
    reset_report(report, request_count);
    if (!coordinator || !coordinator->registry || !requests || !report ||
        request_count == 0 || request_count > ACTUATION_COORDINATOR_MAX_SETPOINTS) {
        return ACTUATION_RESULT_FAILURE;
    }
    if (!acquire(coordinator)) {
        report->outcome = ACTUATION_RESULT_LOCK_TIMEOUT;
        return report->outcome;
    }
    report->outcome = apply_position_locked(coordinator, requests, request_count, report);
    release(coordinator);
    return report->outcome;
}

actuation_result_t actuation_coordinator_set_position_degrees(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    float degrees,
    actuation_report_t *report)
{
    const actuation_position_request_t request = {
        .endpoint_id = endpoint_id,
        .degrees = degrees,
    };
    return actuation_coordinator_apply_position_degrees(coordinator, &request, 1,
                                                         report);
}

actuation_result_t actuation_coordinator_set_position_reference_degrees(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    float degrees,
    actuation_report_t *report)
{
    reset_report(report, 1U);
    if (!coordinator || !coordinator->registry || !report) {
        return ACTUATION_RESULT_FAILURE;
    }
    if (!acquire(coordinator)) {
        report->outcome = ACTUATION_RESULT_LOCK_TIMEOUT;
        return report->outcome;
    }

    robot_endpoint_t *endpoint = robot_endpoint_registry_find(
        coordinator->registry, endpoint_id);
    report->endpoints[0].endpoint_id = endpoint_id;
    if (!endpoint ||
        !robot_endpoint_has_capability(endpoint,
                                       ROBOT_CAPABILITY_POSITION_REFERENCE) ||
        !robot_endpoint_has_capability(endpoint, ROBOT_CAPABILITY_STOPPABLE)) {
        report->endpoints[0].error = endpoint ? ROBOT_CAP_UNSUPPORTED
                                               : ROBOT_CAP_UNAVAILABLE;
    } else {
        /* A reference must not preserve or race an old motion target. */
        report->endpoints[0].rollback_stop_attempted = true;
        report->endpoints[0].rollback_stop_error = robot_endpoint_stop(endpoint);
        if (report->endpoints[0].rollback_stop_error != ROBOT_CAP_OK) {
            report->endpoints[0].error = report->endpoints[0].rollback_stop_error;
        } else {
            report->endpoints[0].error =
                robot_position_set_reference_degrees(endpoint, degrees);
        }
    }
    if (report->endpoints[0].error == ROBOT_CAP_OK) {
        report->endpoints[0].applied = true;
        report->applied = 1U;
        report->outcome = ACTUATION_RESULT_SUCCESS;
    }
    release(coordinator);
    return report->outcome;
}

actuation_result_t actuation_coordinator_stop_endpoint(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    actuation_report_t *report)
{
    reset_report(report, 1);
    if (!coordinator || !coordinator->registry || !report) {
        return ACTUATION_RESULT_FAILURE;
    }
    if (!acquire(coordinator)) {
        report->outcome = ACTUATION_RESULT_LOCK_TIMEOUT;
        return report->outcome;
    }
    robot_endpoint_t *endpoint = robot_endpoint_registry_find(coordinator->registry, endpoint_id);
    report->endpoints[0].endpoint_id = endpoint_id;
    report->endpoints[0].error = endpoint ? robot_endpoint_stop(endpoint) : ROBOT_CAP_UNAVAILABLE;
    if (report->endpoints[0].error == ROBOT_CAP_OK) {
        report->endpoints[0].applied = true;
        report->applied = 1;
        report->outcome = ACTUATION_RESULT_SUCCESS;
    }
    release(coordinator);
    return report->outcome;
}

actuation_result_t actuation_coordinator_stop_all(actuation_coordinator_t *coordinator,
                                                  actuation_report_t *report)
{
    reset_report(report, 0);
    if (!coordinator || !coordinator->registry || !report) {
        return ACTUATION_RESULT_FAILURE;
    }
    if (!acquire(coordinator)) {
        report->outcome = ACTUATION_RESULT_LOCK_TIMEOUT;
        return report->outcome;
    }
    for (size_t index = 0; index < coordinator->registry->count; ++index) {
        robot_endpoint_t *endpoint = coordinator->registry->items[index];
        if (!robot_endpoint_has_capability(endpoint, ROBOT_CAPABILITY_STOPPABLE)) {
            continue;
        }
        actuation_endpoint_result_t *result = &report->endpoints[report->requested++];
        result->endpoint_id = endpoint->id;
        result->error = robot_endpoint_stop(endpoint);
        if (result->error == ROBOT_CAP_OK) {
            result->applied = true;
            ++report->applied;
        }
    }
    if (report->requested == 0) {
        report->outcome = ACTUATION_RESULT_FAILURE;
    } else if (report->applied == report->requested) {
        report->outcome = ACTUATION_RESULT_SUCCESS;
    } else if (report->applied > 0) {
        report->outcome = ACTUATION_RESULT_PARTIAL;
    }
    release(coordinator);
    return report->outcome;
}
