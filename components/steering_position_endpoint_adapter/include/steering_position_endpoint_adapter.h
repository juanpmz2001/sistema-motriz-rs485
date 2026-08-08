#ifndef STEERING_POSITION_ENDPOINT_ADAPTER_H
#define STEERING_POSITION_ENDPOINT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "as5600_device.h"
#include "robot_capabilities.h"
#include "steering_position_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serializes the controller's tick, target, stop and explicit-reference
 * operations.  The callback must either provide both acquire/release or
 * neither; an omitted lock is allowed only for a single-threaded composition.
 */
typedef struct {
    bool (*acquire)(void *context);
    void (*release)(void *context);
    void *context;
} steering_position_endpoint_adapter_lock_t;

typedef struct {
    as5600_device_t *as5600;
    /* Must be the same validated LUT configured on `as5600`. */
    const as5600_calibration_lut_t *approved_calibration;

    robot_endpoint_id_t endpoint_id;
    const char *endpoint_name;
    robot_endpoint_criticality_t criticality;

    steering_position_controller_config_t controller_config;
    steering_position_controller_clock_ms_fn clock_ms;
    void *clock_context;
    steering_position_controller_pwm_output_fn pwm_output;
    void *pwm_output_context;

    /* Applied to every generic POSITION command through this endpoint. */
    uint32_t position_command_ttl_ms;

    /* Reject a single fresh raw sample that moves farther than this shortest
     * circular AS5600-code delta.  It must be below half a turn. */
    uint16_t max_raw_circular_step_counts;

    /* A deliberately narrow development-only exception for AS5600 STATUS.ML.
     * It does not authorize STATUS.MH, missing MD, partial reads, or transport
     * failures. It must agree with controller_config's degraded-health policy
     * so the generic controller cannot accidentally broaden this exception. */
    bool allow_magnet_too_weak_for_development;
    steering_position_endpoint_adapter_lock_t lock;
} steering_position_endpoint_adapter_config_t;

/*
 * This combines an actuator controller with its input only for local feedback
 * control. The public endpoint has POSITION, STOPPABLE and an explicit
 * POSITION_REFERENCE maintenance capability; it does not expose a
 * PositionObservation capability. A separate AS5600 observation endpoint
 * remains the independent E3 path for generic tests.
 */
typedef struct {
    as5600_device_t *as5600;
    const as5600_calibration_lut_t *approved_calibration;
    steering_position_endpoint_adapter_lock_t lock;
    uint16_t max_raw_circular_step_counts;
    uint32_t position_command_ttl_ms;
    bool allow_magnet_too_weak_for_development;
    bool initialized;
    bool calibration_approved;
    bool have_last_raw_angle;
    uint16_t last_raw_angle;

    steering_position_controller_t controller;
    robot_position_port_t position;
    robot_position_reference_port_t position_reference;
    robot_stoppable_port_t stoppable;
    robot_endpoint_t endpoint;
} steering_position_endpoint_adapter_t;

bool steering_position_endpoint_adapter_init(
    steering_position_endpoint_adapter_t *adapter,
    const steering_position_endpoint_adapter_config_t *config);
void steering_position_endpoint_adapter_deinit(
    steering_position_endpoint_adapter_t *adapter);

/*
 * Normal application position request.  It is bounded by the endpoint and
 * always uses the profile-configured TTL; it cannot select an unbounded lease.
 */
robot_capability_error_t steering_position_endpoint_adapter_set_position_degrees(
    steering_position_endpoint_adapter_t *adapter,
    float degrees);

/* Stop clears the controller target and commands its configured neutral PWM. */
robot_capability_error_t steering_position_endpoint_adapter_stop(
    steering_position_endpoint_adapter_t *adapter);

/*
 * Explicit maintenance/homing operation.  It is intentionally not called by
 * init(), tick(), or a generic position request: a cyclic AS5600 phase is not
 * an implicit steering zero.
 */
robot_capability_error_t steering_position_endpoint_adapter_set_reference(
    steering_position_endpoint_adapter_t *adapter,
    float logical_degrees);

/*
 * Project one already-corrected AS5600 cyclic phase into the controller's
 * logical frame without feeding or unwrapping it again.  The phase must be
 * the same accepted, fresh controller sample (matching its uint32_t sensor
 * timestamp and cyclic phase).  It is therefore suitable for a separate
 * sensor-observation adapter after it releases the sensor's own lock.
 */
robot_capability_error_t steering_position_endpoint_adapter_project_cyclic_phase(
    steering_position_endpoint_adapter_t *adapter,
    float corrected_cyclic_degrees,
    uint32_t timestamp_ms,
    float *logical_degrees);

/*
 * Feed the latest read-only AS5600 snapshot into the controller.  Tick never
 * polls I2C and never exposes the controller's feedback as a generic physical
 * observation.  `report` is optional and is diagnostic controller state only.
 */
robot_capability_error_t steering_position_endpoint_adapter_tick(
    steering_position_endpoint_adapter_t *adapter,
    steering_position_controller_report_t *report);

#ifdef __cplusplus
}
#endif

#endif
