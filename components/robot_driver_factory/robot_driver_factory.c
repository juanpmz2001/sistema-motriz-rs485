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
        const robot_bus_profile_t *bus = find_bus(profile, device->bus_id);
        if (!bus || factory->bus_type != bus->type) {
            diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
            return false;
        }
        diagnostics->stage = ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE;
        if (!factory->ops || !factory->ops->validate ||
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
    }
    diagnostics->composition_supported = true;
    diagnostics->code = ROBOT_COMPOSITION_DIAGNOSTIC_OK;
    diagnostics->stage = ROBOT_COMPOSITION_STAGE_NONE;
    diagnostics->driver_id = 0;
    diagnostics->device_id = 0;
    diagnostics->bus_id = 0;
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
