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
    case SVD48_DEVICE_PARTIAL:
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

static robot_endpoint_health_t map_health(svd48_channel_health_t health)
{
    switch (health) {
    case SVD48_CHANNEL_HEALTH_HEALTHY:
        return ROBOT_ENDPOINT_HEALTH_HEALTHY;
    case SVD48_CHANNEL_HEALTH_DEGRADED:
        return ROBOT_ENDPOINT_HEALTH_DEGRADED;
    case SVD48_CHANNEL_HEALTH_OFFLINE:
        return ROBOT_ENDPOINT_HEALTH_OFFLINE;
    case SVD48_CHANNEL_HEALTH_FAULT:
        return ROBOT_ENDPOINT_HEALTH_FAULT;
    case SVD48_CHANNEL_HEALTH_STALE:
        return ROBOT_ENDPOINT_HEALTH_STALE;
    case SVD48_CHANNEL_HEALTH_UNKNOWN:
    default:
        return ROBOT_ENDPOINT_HEALTH_UNKNOWN;
    }
}

static robot_capability_error_t read_observation(
    robot_velocity_observation_port_t *port,
    robot_velocity_observation_t *observation)
{
    svd48_channel_endpoint_adapter_t *adapter = port ? port->context : NULL;
    svd48_channel_snapshot_t snapshot;
    if (!adapter || !adapter->channel || !observation ||
        !svd48_channel_get_snapshot(adapter->channel, &snapshot)) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    memset(observation, 0, sizeof(*observation));
    observation->valid =
        (snapshot.valid_observations & SVD48_OBSERVATION_SPEED) != 0U;
    observation->rpm = snapshot.observed_speed_rpm;
    observation->timestamp_ms =
        snapshot.observation_update_ms[SVD48_OBSERVATION_INDEX_SPEED];
    observation->source = observation->valid
                              ? ROBOT_VELOCITY_OBSERVATION_SOURCE_DEVICE_FEEDBACK
                              : ROBOT_VELOCITY_OBSERVATION_SOURCE_UNKNOWN;
    observation->online = snapshot.online;
    observation->stale =
        (snapshot.stale_observations & SVD48_OBSERVATION_SPEED) != 0U;
    observation->health = map_health(
        svd48_channel_health_from_snapshot(&snapshot));
    return ROBOT_CAP_OK;
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
    static const robot_velocity_observation_ops_t observation_ops = {
        .read = read_observation,
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
    adapter->velocity_observation.ops = &observation_ops;
    adapter->velocity_observation.context = adapter;
    adapter->endpoint.velocity_observation = &adapter->velocity_observation;
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
    diagnostics->health = svd48_channel_health_from_snapshot(
        &diagnostics->observation);
    return true;
}
