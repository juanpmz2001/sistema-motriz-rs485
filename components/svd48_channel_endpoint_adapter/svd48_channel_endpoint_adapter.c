#include "svd48_channel_endpoint_adapter.h"

#include <string.h>

static robot_capability_error_t map_result(svd48_device_result_t result)
{
    switch (result) {
    case SVD48_DEVICE_OK:
        return ROBOT_CAP_OK;
    case SVD48_DEVICE_INVALID_ARGUMENT:
        return ROBOT_CAP_INVALID_ARGUMENT;
    case SVD48_DEVICE_UNSUPPORTED:
        return ROBOT_CAP_UNSUPPORTED;
    case SVD48_DEVICE_TIMEOUT:
    case SVD48_DEVICE_BUS_BUSY:
    case SVD48_DEVICE_IO_ERROR:
    case SVD48_DEVICE_INCOMPLETE_FRAME:
    case SVD48_DEVICE_CANCELLED:
    case SVD48_DEVICE_CRC_ERROR:
    case SVD48_DEVICE_EXCEPTION:
    case SVD48_DEVICE_BAD_RESPONSE:
    default:
        return ROBOT_CAP_IO_ERROR;
    }
}

static robot_capability_error_t set_velocity(robot_velocity_rpm_port_t *port,
                                             int16_t rpm)
{
    svd48_channel_endpoint_adapter_t *adapter = port ? port->context : NULL;
    if (!adapter || !adapter->channel) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    svd48_device_result_t result = svd48_channel_set_target_rpm(adapter->channel,
                                                               rpm);
    if (result != SVD48_DEVICE_OK) {
        (void)svd48_channel_stop(adapter->channel);
        return map_result(result);
    }
    result = svd48_channel_enable(adapter->channel);
    if (result != SVD48_DEVICE_OK) {
        (void)svd48_channel_stop(adapter->channel);
    }
    return map_result(result);
}

static robot_capability_error_t stop_channel(robot_stoppable_port_t *port)
{
    svd48_channel_endpoint_adapter_t *adapter = port ? port->context : NULL;
    return adapter && adapter->channel
               ? map_result(svd48_channel_stop(adapter->channel))
               : ROBOT_CAP_INVALID_ARGUMENT;
}

bool svd48_channel_endpoint_adapter_init(
    svd48_channel_endpoint_adapter_t *adapter,
    svd48_channel_t *channel,
    robot_endpoint_id_t endpoint_id,
    const char *name,
    robot_endpoint_criticality_t criticality,
    uint32_t capabilities,
    int16_t min_rpm,
    int16_t max_rpm)
{
    static const robot_velocity_rpm_ops_t velocity_ops = {
        .set_velocity_rpm = set_velocity,
    };
    static const robot_stoppable_ops_t stoppable_ops = {
        .stop = stop_channel,
    };
    const uint32_t supported = ROBOT_CAPABILITY_VELOCITY_RPM |
                               ROBOT_CAPABILITY_STOPPABLE;
    if (!adapter || !channel || !channel->device || !name || name[0] == '\0' ||
        endpoint_id == 0U || capabilities == 0U ||
        (capabilities & ~supported) != 0U || min_rpm > max_rpm) {
        return false;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->channel = channel;
    adapter->configured_capabilities = capabilities;
    if (capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) {
        adapter->velocity.ops = &velocity_ops;
        adapter->velocity.context = adapter;
        adapter->velocity.min_rpm = min_rpm;
        adapter->velocity.max_rpm = max_rpm;
        adapter->endpoint.velocity_rpm = &adapter->velocity;
    }
    if (capabilities & ROBOT_CAPABILITY_STOPPABLE) {
        adapter->stoppable.ops = &stoppable_ops;
        adapter->stoppable.context = adapter;
        adapter->endpoint.stoppable = &adapter->stoppable;
    }
    adapter->endpoint.id = endpoint_id;
    adapter->endpoint.name = name;
    adapter->endpoint.criticality = criticality;
    /* Communication health never suppresses a configured stop attempt. */
    adapter->endpoint.available = true;
    return true;
}

void svd48_channel_endpoint_adapter_deinit(
    svd48_channel_endpoint_adapter_t *adapter)
{
    if (adapter) {
        memset(adapter, 0, sizeof(*adapter));
    }
}

bool svd48_channel_endpoint_adapter_get_diagnostics(
    svd48_channel_endpoint_adapter_t *adapter,
    svd48_endpoint_diagnostics_t *diagnostics)
{
    if (!adapter || !adapter->channel || !adapter->channel->device ||
        !diagnostics ||
        !svd48_channel_get_snapshot(adapter->channel, &diagnostics->observation)) {
        return false;
    }
    diagnostics->device_id = svd48_device_id(adapter->channel->device);
    diagnostics->device_address = svd48_device_address(adapter->channel->device);
    diagnostics->channel = adapter->channel->id;
    diagnostics->health = svd48_channel_get_health(adapter->channel);
    return true;
}
