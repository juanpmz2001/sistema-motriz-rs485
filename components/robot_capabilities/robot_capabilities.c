#include "robot_capabilities.h"

uint32_t robot_endpoint_capabilities(const robot_endpoint_t *endpoint)
{
    if (!endpoint) return 0;
    uint32_t result = 0;
    if (endpoint->velocity_rpm) result |= ROBOT_CAPABILITY_VELOCITY_RPM;
    if (endpoint->stoppable) result |= ROBOT_CAPABILITY_STOPPABLE;
    if (endpoint->position) result |= ROBOT_CAPABILITY_POSITION;
    if (endpoint->position_sensor) result |= ROBOT_CAPABILITY_POSITION_SENSOR;
    return result;
}

robot_capability_error_t robot_velocity_set_rpm(robot_endpoint_t *endpoint, int16_t rpm)
{
    if (!endpoint) return ROBOT_CAP_INVALID_ARGUMENT;
    if (!endpoint->available) return ROBOT_CAP_UNAVAILABLE;
    robot_velocity_rpm_port_t *port = endpoint->velocity_rpm;
    if (!port || !port->ops || !port->ops->set_velocity_rpm) return ROBOT_CAP_UNSUPPORTED;
    if (!port->context) return ROBOT_CAP_INVALID_ARGUMENT;
    if (rpm < port->min_rpm || rpm > port->max_rpm) return ROBOT_CAP_OUT_OF_RANGE;
    return port->ops->set_velocity_rpm(port, rpm);
}

robot_capability_error_t robot_endpoint_stop(robot_endpoint_t *endpoint)
{
    if (!endpoint) return ROBOT_CAP_INVALID_ARGUMENT;
    robot_stoppable_port_t *port = endpoint->stoppable;
    if (!port || !port->ops || !port->ops->stop) return ROBOT_CAP_UNSUPPORTED;
    if (!port->context) return ROBOT_CAP_INVALID_ARGUMENT;
    return port->ops->stop(port);
}

robot_capability_error_t robot_endpoint_read_velocity_observation(
    robot_endpoint_t *endpoint,
    robot_velocity_observation_t *observation)
{
    if (!endpoint || !observation) return ROBOT_CAP_INVALID_ARGUMENT;
    if (!endpoint->available) return ROBOT_CAP_UNAVAILABLE;
    robot_velocity_observation_port_t *port = endpoint->velocity_observation;
    if (!port || !port->ops || !port->ops->read) return ROBOT_CAP_UNSUPPORTED;
    if (!port->context) return ROBOT_CAP_INVALID_ARGUMENT;
    return port->ops->read(port, observation);
}

void robot_endpoint_registry_init(robot_endpoint_registry_t *registry)
{
    if (!registry) return;
    registry->count = 0;
    for (size_t index = 0; index < ROBOT_ENDPOINT_REGISTRY_MAX; ++index) registry->items[index] = NULL;
}

robot_endpoint_t *robot_endpoint_registry_find(const robot_endpoint_registry_t *registry,
                                               robot_endpoint_id_t id)
{
    if (!registry) return NULL;
    for (size_t index = 0; index < registry->count; ++index)
        if (registry->items[index]->id == id) return registry->items[index];
    return NULL;
}

robot_registry_error_t robot_endpoint_registry_add(robot_endpoint_registry_t *registry,
                                                   robot_endpoint_t *endpoint)
{
    if (!registry || !endpoint || !endpoint->name || robot_endpoint_capabilities(endpoint) == 0)
        return ROBOT_REGISTRY_INVALID_ARGUMENT;
    if (robot_endpoint_registry_find(registry, endpoint->id)) return ROBOT_REGISTRY_DUPLICATE_ID;
    if (registry->count >= ROBOT_ENDPOINT_REGISTRY_MAX) return ROBOT_REGISTRY_FULL;
    registry->items[registry->count++] = endpoint;
    return ROBOT_REGISTRY_OK;
}

bool robot_endpoint_has_capability(const robot_endpoint_t *endpoint, uint32_t capability)
{
    return (robot_endpoint_capabilities(endpoint) & capability) == capability;
}

const robot_endpoint_t *robot_endpoint_registry_at(const robot_endpoint_registry_t *registry,
                                                   size_t index)
{
    return registry && index < registry->count ? registry->items[index] : NULL;
}
