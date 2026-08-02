#include "robot_control_endpoint_adapter.h"

#include <string.h>

static robot_capability_error_t set_speed(robot_velocity_rpm_port_t *port, int16_t rpm)
{
    robot_control_endpoint_adapter_t *adapter = port ? port->context : NULL;
    if (!adapter || !adapter->legacy || !adapter->set_speed) return ROBOT_CAP_INVALID_ARGUMENT;
    return adapter->set_speed(adapter->legacy, adapter->legacy_motor_index, rpm) == 0
               ? ROBOT_CAP_OK : ROBOT_CAP_IO_ERROR;
}

static robot_capability_error_t stop(robot_stoppable_port_t *port)
{
    robot_control_endpoint_adapter_t *adapter = port ? port->context : NULL;
    if (!adapter || !adapter->legacy || !adapter->stop) return ROBOT_CAP_INVALID_ARGUMENT;
    return adapter->stop(adapter->legacy, adapter->legacy_motor_index) == 0
               ? ROBOT_CAP_OK : ROBOT_CAP_IO_ERROR;
}

bool robot_control_endpoint_adapter_init(
    robot_control_endpoint_adapter_t *adapter, void *legacy, uint8_t index,
    robot_endpoint_id_t id, const char *name, robot_endpoint_criticality_t criticality,
    int16_t min_rpm, int16_t max_rpm, robot_control_legacy_speed_fn speed_fn,
    robot_control_legacy_stop_fn stop_fn)
{
    static const robot_velocity_rpm_ops_t velocity_ops = {.set_velocity_rpm = set_speed};
    static const robot_stoppable_ops_t stoppable_ops = {.stop = stop};
    if (!adapter || !legacy || !name || !speed_fn || !stop_fn || min_rpm > max_rpm) return false;
    memset(adapter, 0, sizeof(*adapter));
    adapter->legacy = legacy;
    adapter->legacy_motor_index = index;
    adapter->set_speed = speed_fn;
    adapter->stop = stop_fn;
    adapter->velocity = (robot_velocity_rpm_port_t){&velocity_ops, adapter, min_rpm, max_rpm};
    adapter->stoppable = (robot_stoppable_port_t){&stoppable_ops, adapter};
    adapter->endpoint.id = id;
    adapter->endpoint.name = name;
    adapter->endpoint.criticality = criticality;
    adapter->endpoint.available = true;
    adapter->endpoint.velocity_rpm = &adapter->velocity;
    adapter->endpoint.stoppable = &adapter->stoppable;
    return true;
}
