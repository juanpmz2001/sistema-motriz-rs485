#include "robot_driver_factory.h"

#include <string.h>

static const robot_bus_profile_t *find_bus(const robot_profile_t *profile,
                                           uint16_t bus_id)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0; index < profile->bus_count; ++index) {
        if (profile->buses[index].id == bus_id) {
            return &profile->buses[index];
        }
    }
    return NULL;
}

const robot_driver_factory_t *robot_executable_factory_find(
    const robot_executable_factory_registry_t *registry,
    robot_driver_id_t driver_id)
{
    if (!registry) {
        return NULL;
    }
    for (size_t index = 0; index < registry->count; ++index) {
        if (registry->items[index].driver_id == driver_id) {
            return &registry->items[index];
        }
    }
    return NULL;
}

bool robot_composition_preflight(
    const robot_profile_t *profile,
    const robot_executable_factory_registry_t *registry,
    robot_composition_diagnostics_t *diagnostics)
{
    if (!diagnostics) {
        return false;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->stage = ROBOT_COMPOSITION_STAGE_SCHEMA;
    diagnostics->schema_error = robot_profile_validate(profile);
    if (diagnostics->schema_error != ROBOT_PROFILE_VALID) {
        diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID;
        return false;
    }
    diagnostics->schema_valid = true;

    /* Every active composition needs an application endpoint, but the
     * transitional SVD48 legacy view is optional.  A position-only bench
     * profile must not be rejected merely because it has no SVD48 channels. */
    if (profile->endpoint_count == 0U) {
        diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED;
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT;
        diagnostics->factory_result = ROBOT_FACTORY_ENDPOINT_FAILED;
        return false;
    }
    if (!registry || profile->endpoint_count > registry->endpoint_capacity) {
        diagnostics->code =
            ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED;
        diagnostics->factory_result = ROBOT_FACTORY_NO_STORAGE;
        diagnostics->required_storage = profile->endpoint_count;
        diagnostics->available_storage = registry ? registry->endpoint_capacity : 0U;
        return false;
    }
    size_t legacy_binding_count = 0U;
    for (size_t index = 0U; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        const robot_device_profile_t *device = NULL;
        for (size_t device_index = 0U; device_index < profile->device_count;
             ++device_index) {
            if (profile->devices[device_index].id == endpoint->device_id) {
                device = &profile->devices[device_index];
                break;
            }
        }
        if (device && device->driver_id == ROBOT_DRIVER_SVD48) {
            ++legacy_binding_count;
        }
    }
    if (legacy_binding_count > registry->legacy_binding_capacity) {
        diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_LEGACY_BINDING_LIMIT;
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT;
        diagnostics->factory_result = ROBOT_FACTORY_ENDPOINT_FAILED;
        diagnostics->required_storage = legacy_binding_count;
        diagnostics->available_storage = registry->legacy_binding_capacity;
        return false;
    }

    size_t required_storage = 0U;
    for (size_t index = 0; index < profile->device_count; ++index) {
        const robot_device_profile_t *device = &profile->devices[index];
        diagnostics->driver_id = device->driver_id;
        diagnostics->device_id = device->id;
        diagnostics->bus_id = device->bus_id;
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP;
        const robot_driver_factory_t *factory = robot_executable_factory_find(
            registry, device->driver_id);
        if (!factory) {
            diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_FACTORY_MISSING;
            return false;
        }
        const robot_bus_profile_t *bus =
            device->bus_id == ROBOT_PROFILE_NO_BUS
                ? NULL
                : find_bus(profile, device->bus_id);
        const bool bus_matches =
            factory->bus_type == ROBOT_BUS_NONE
                ? bus == NULL && device->bus_id == ROBOT_PROFILE_NO_BUS
                : bus != NULL && factory->bus_type == bus->type;
        if (!bus_matches) {
            diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
            return false;
        }
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE;
        /* Poll deadlines use signed modular comparison and must stay below
         * INT32_MAX to remain unambiguous across uint32_t clock wraparound. */
        if ((bus && bus->telemetry_period_ms >= INT32_MAX) ||
            !factory->ops || !factory->ops->validate ||
            !factory->ops->storage_required || !factory->ops->construct ||
            !factory->ops->create_endpoint || !factory->ops->start ||
            !factory->ops->stop || !factory->ops->destroy ||
            device->channel_count > factory->max_channels ||
            factory->ops->validate(factory, profile, bus, device) !=
                ROBOT_FACTORY_OK) {
            diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID;
            diagnostics->factory_result = ROBOT_FACTORY_INVALID_CONFIGURATION;
            return false;
        }
        const size_t device_storage = factory->ops->storage_required(factory,
                                                                      device);
        if (device_storage == 0U ||
            device_storage > SIZE_MAX - required_storage ||
            required_storage + device_storage >
                registry->runtime_storage_capacity) {
            diagnostics->code =
                ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED;
            diagnostics->factory_result = ROBOT_FACTORY_NO_STORAGE;
            diagnostics->required_storage =
                device_storage > SIZE_MAX - required_storage
                    ? SIZE_MAX
                    : required_storage + device_storage;
            diagnostics->available_storage =
                registry->runtime_storage_capacity;
            return false;
        }
        required_storage += device_storage;
    }
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        const robot_device_profile_t *device = NULL;
        for (size_t device_index = 0; device_index < profile->device_count;
             ++device_index) {
            if (profile->devices[device_index].id == endpoint->device_id) {
                device = &profile->devices[device_index];
                break;
            }
        }
        const robot_driver_factory_t *factory =
            device ? robot_executable_factory_find(registry, device->driver_id)
                   : NULL;
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT;
        diagnostics->endpoint_id = endpoint->id;
        diagnostics->device_id = endpoint->device_id;
        diagnostics->driver_id = device ? device->driver_id : 0;
        diagnostics->bus_id = device ? device->bus_id : 0U;
        /* A profile capability that can move an output must carry an
         * independently invokable stop path.  This applies equally to the
         * newer position actuator boundary; otherwise a future PWM/position
         * factory could bypass the velocity-only invariant. */
        const bool motion_without_stop =
            (endpoint->capabilities &
             (ROBOT_CAPABILITY_VELOCITY_RPM | ROBOT_CAPABILITY_POSITION)) !=
                0U &&
            (endpoint->capabilities & ROBOT_CAPABILITY_STOPPABLE) == 0U;
        if (!factory || endpoint->capabilities == 0U || motion_without_stop ||
            (endpoint->capabilities & ~factory->capabilities) != 0U ||
            endpoint->min_rpm > endpoint->max_rpm) {
            diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED;
            diagnostics->factory_result = ROBOT_FACTORY_ENDPOINT_FAILED;
            return false;
        }
    }
    diagnostics->composition_supported = true;
    diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_OK;
    diagnostics->stage = ROBOT_COMPOSITION_STAGE_NONE;
    diagnostics->driver_id = 0;
    diagnostics->device_id = 0;
    diagnostics->bus_id = 0;
    diagnostics->required_storage = required_storage;
    diagnostics->available_storage = registry->runtime_storage_capacity;
    return true;
}

const char *robot_composition_diagnostic_code_name(
    robot_composition_diagnostic_code_t code)
{
    switch (code) {
    case ROBOT_COMPOSITION_DIAGNOSTIC_OK:
        return "OK";
    case ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID:
        return "SCHEMA_INVALID";
    case ROBOT_COMPOSITION_DIAGNOSTIC_FACTORY_MISSING:
        return "FACTORY_MISSING";
    case ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE:
        return "BUS_INCOMPATIBLE";
    case ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID:
        return "DEVICE_INVALID";
    case ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED:
        return "CONSTRUCTION_FAILED";
    case ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED:
        return "ENDPOINT_FAILED";
    case ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED:
        return "START_FAILED";
    case ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED:
        return "STATIC_CAPACITY_EXCEEDED";
    case ROBOT_COMPOSITION_DIAGNOSTIC_LEGACY_BINDING_LIMIT:
        return "LEGACY_BINDING_LIMIT";
    default:
        return "UNKNOWN";
    }
}

const char *robot_composition_stage_name(robot_composition_stage_t stage)
{
    switch (stage) {
    case ROBOT_COMPOSITION_STAGE_NONE:
        return "NONE";
    case ROBOT_COMPOSITION_STAGE_SCHEMA:
        return "SCHEMA";
    case ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP:
        return "FACTORY_LOOKUP";
    case ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE:
        return "FACTORY_VALIDATE";
    case ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT:
        return "BUS_CONSTRUCT";
    case ROBOT_COMPOSITION_STAGE_DEVICE_CONSTRUCT:
        return "DEVICE_CONSTRUCT";
    case ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT:
        return "ENDPOINT_CONSTRUCT";
    case ROBOT_COMPOSITION_STAGE_SERVICE_START:
        return "SERVICE_START";
    default:
        return "UNKNOWN";
    }
}
