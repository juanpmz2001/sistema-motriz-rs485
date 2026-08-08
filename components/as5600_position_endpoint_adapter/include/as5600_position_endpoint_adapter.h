#ifndef AS5600_POSITION_ENDPOINT_ADAPTER_H
#define AS5600_POSITION_ENDPOINT_ADAPTER_H

#include <stdbool.h>

#include "as5600_device.h"
#include "robot_capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maps one corrected cyclic AS5600 phase into the logical coordinate system
 * of a composed actuator. The AS5600 adapter deliberately knows no actuator,
 * PWM or controller type; a composition binds this opaque frame only after
 * every endpoint exists. Returning non-OK leaves the public observation
 * unreferenced/invalid rather than inventing a mechanical zero.
 */
typedef robot_capability_error_t (*as5600_position_coordinate_frame_fn)(
    void *context,
    float corrected_cyclic_degrees,
    uint32_t sample_timestamp_ms,
    float *logical_degrees);

typedef struct {
    as5600_position_coordinate_frame_fn project_cyclic_phase;
    void *context;
} as5600_position_coordinate_frame_t;

/*
 * Read-only bridge from one AS5600 device to one independent physical-position
 * observation endpoint. It deliberately exposes no position actuation port:
 * a steering actuator and its encoder are separate devices.
 */
typedef struct {
    as5600_device_t *device;
    /*
     * The profile explicitly approves this LUT for this endpoint's physical
     * coordinate system. NULL permits device qualification, but the exposed
     * application observation remains uncalibrated and invalid.
     */
    const as5600_calibration_lut_t *approved_calibration;
    bool calibration_approved;
    as5600_position_coordinate_frame_t coordinate_frame;
    robot_position_observation_port_t position_observation;
    robot_endpoint_t endpoint;
} as5600_position_endpoint_adapter_t;

bool as5600_position_endpoint_adapter_init(
    as5600_position_endpoint_adapter_t *adapter,
    as5600_device_t *device,
    robot_endpoint_id_t endpoint_id,
    const char *name,
    robot_endpoint_criticality_t criticality,
    const as5600_calibration_lut_t *approved_calibration);
/* Bind an opaque logical coordinate frame during composition, before tasks
 * start. Passing NULL removes the binding and leaves generic observations
 * unreferenced. */
bool as5600_position_endpoint_adapter_set_coordinate_frame(
    as5600_position_endpoint_adapter_t *adapter,
    const as5600_position_coordinate_frame_t *coordinate_frame);
void as5600_position_endpoint_adapter_deinit(
    as5600_position_endpoint_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
