#ifndef ROBOT_CONTROL_ENDPOINT_ADAPTER_H
#define ROBOT_CONTROL_ENDPOINT_ADAPTER_H

#include "robot_capabilities.h"

/* Transitional strangler seam. Replace with device-specific endpoint adapters. */
typedef int (*robot_control_legacy_speed_fn)(void *, uint8_t, int16_t);
typedef int (*robot_control_legacy_stop_fn)(void *, uint8_t);

typedef struct {
    void *legacy;
    uint8_t legacy_motor_index;
    robot_control_legacy_speed_fn set_speed;
    robot_control_legacy_stop_fn stop;
    robot_velocity_rpm_port_t velocity;
    robot_stoppable_port_t stoppable;
    robot_endpoint_t endpoint;
} robot_control_endpoint_adapter_t;

bool robot_control_endpoint_adapter_init(
    robot_control_endpoint_adapter_t *adapter, void *legacy, uint8_t legacy_motor_index,
    robot_endpoint_id_t endpoint_id, const char *name,
    robot_endpoint_criticality_t criticality, int16_t min_rpm, int16_t max_rpm,
    robot_control_legacy_speed_fn set_speed, robot_control_legacy_stop_fn stop);

#endif
