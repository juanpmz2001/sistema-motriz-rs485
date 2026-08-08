#ifndef ACTUATION_COORDINATOR_H
#define ACTUATION_COORDINATOR_H

#include "robot_capabilities.h"

#define ACTUATION_COORDINATOR_MAX_SETPOINTS 16

typedef enum {
    ACTUATION_RESULT_SUCCESS = 0,
    ACTUATION_RESULT_FAILURE,
    ACTUATION_RESULT_PARTIAL,
    ACTUATION_RESULT_LOCK_TIMEOUT,
} actuation_result_t;

typedef struct {
    bool (*acquire)(void *context);
    void (*release)(void *context);
    void *context;
} actuation_lock_port_t;

typedef struct {
    robot_endpoint_id_t endpoint_id;
    int16_t rpm;
} actuation_velocity_request_t;

typedef struct {
    robot_endpoint_id_t endpoint_id;
    float degrees;
} actuation_position_request_t;

typedef struct {
    robot_endpoint_id_t endpoint_id;
    robot_capability_error_t error;
    bool applied;
    bool rollback_stop_attempted;
    robot_capability_error_t rollback_stop_error;
} actuation_endpoint_result_t;

typedef struct {
    actuation_result_t outcome;
    size_t requested;
    size_t applied;
    actuation_endpoint_result_t endpoints[ACTUATION_COORDINATOR_MAX_SETPOINTS];
} actuation_report_t;

typedef struct {
    robot_endpoint_registry_t *registry;
    actuation_lock_port_t lock;
} actuation_coordinator_t;

bool actuation_coordinator_init(actuation_coordinator_t *coordinator,
                                robot_endpoint_registry_t *registry,
                                const actuation_lock_port_t *lock);
actuation_result_t actuation_coordinator_set_velocity_rpm(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    int16_t rpm,
    actuation_report_t *report);
actuation_result_t actuation_coordinator_apply_velocity_rpm(
    actuation_coordinator_t *coordinator,
    const actuation_velocity_request_t *requests,
    size_t request_count,
    actuation_report_t *report);
actuation_result_t actuation_coordinator_set_position_degrees(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    float degrees,
    actuation_report_t *report);
/* Maintenance operation: stop first, then explicitly establish the logical
 * reference while holding the same authority lock as normal setpoints. */
actuation_result_t actuation_coordinator_set_position_reference_degrees(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    float degrees,
    actuation_report_t *report);
actuation_result_t actuation_coordinator_apply_position_degrees(
    actuation_coordinator_t *coordinator,
    const actuation_position_request_t *requests,
    size_t request_count,
    actuation_report_t *report);
actuation_result_t actuation_coordinator_stop_endpoint(
    actuation_coordinator_t *coordinator,
    robot_endpoint_id_t endpoint_id,
    actuation_report_t *report);
actuation_result_t actuation_coordinator_stop_all(actuation_coordinator_t *coordinator,
                                                  actuation_report_t *report);

#endif
