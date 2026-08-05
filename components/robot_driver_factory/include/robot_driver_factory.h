#ifndef ROBOT_DRIVER_FACTORY_H
#define ROBOT_DRIVER_FACTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_FACTORY_OK = 0,
    ROBOT_FACTORY_INVALID_CONFIGURATION,
    ROBOT_FACTORY_NO_STORAGE,
    ROBOT_FACTORY_CONSTRUCTION_FAILED,
    ROBOT_FACTORY_ENDPOINT_FAILED,
    ROBOT_FACTORY_START_FAILED,
    ROBOT_FACTORY_STOP_FAILED,
} robot_factory_result_t;

typedef struct robot_driver_factory robot_driver_factory_t;

typedef struct {
    robot_factory_result_t (*validate)(const robot_driver_factory_t *factory,
                                       const robot_profile_t *profile,
                                       const robot_bus_profile_t *bus,
                                       const robot_device_profile_t *device);
    size_t (*storage_required)(const robot_driver_factory_t *factory,
                               const robot_device_profile_t *device);
    robot_factory_result_t (*construct)(const robot_driver_factory_t *factory,
                                        void *runtime_context,
                                        const robot_profile_t *profile,
                                        const robot_bus_profile_t *bus,
                                        const robot_device_profile_t *device);
    robot_factory_result_t (*create_endpoint)(
        const robot_driver_factory_t *factory,
        void *runtime_context,
        const robot_profile_t *profile,
        const robot_device_profile_t *device,
        const robot_endpoint_profile_t *endpoint);
    robot_factory_result_t (*start)(const robot_driver_factory_t *factory,
                                    void *runtime_context,
                                    const robot_device_profile_t *device);
    robot_factory_result_t (*stop)(const robot_driver_factory_t *factory,
                                   void *runtime_context,
                                   const robot_device_profile_t *device);
    void (*destroy)(const robot_driver_factory_t *factory,
                    void *runtime_context,
                    const robot_device_profile_t *device);
} robot_driver_factory_ops_t;

struct robot_driver_factory {
    robot_driver_id_t driver_id;
    robot_bus_type_t bus_type;
    uint32_t capabilities;
    uint8_t max_channels;
    const robot_driver_factory_ops_t *ops;
};

typedef struct {
    const robot_driver_factory_t *items;
    size_t count;
} robot_executable_factory_registry_t;

typedef enum {
    ROBOT_COMPOSITION_STAGE_NONE = 0,
    ROBOT_COMPOSITION_STAGE_SCHEMA,
    ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP,
    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE,
    ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT,
    ROBOT_COMPOSITION_STAGE_DEVICE_CONSTRUCT,
    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT,
    ROBOT_COMPOSITION_STAGE_SERVICE_START,
} robot_composition_stage_t;

typedef enum {
    ROBOT_COMPOSITION_DIAGNOSTIC_OK = 0,
    ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID,
    ROBOT_COMPOSITION_DIAGNOSTIC_FACTORY_MISSING,
    ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE,
    ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID,
    ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED,
    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED,
    ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED,
} robot_composition_diagnostic_code_t;

typedef struct {
    bool schema_valid;
    bool composition_supported;
    bool runtime_ready;
    robot_profile_error_t schema_error;
    robot_composition_diagnostic_code_t code;
    robot_composition_stage_t stage;
    robot_driver_id_t driver_id;
    uint16_t bus_id;
    uint16_t device_id;
    robot_endpoint_id_t endpoint_id;
    robot_factory_result_t factory_result;
} robot_composition_diagnostics_t;

const robot_driver_factory_t *robot_executable_factory_find(
    const robot_executable_factory_registry_t *registry,
    robot_driver_id_t driver_id);
bool robot_composition_preflight(
    const robot_profile_t *profile,
    const robot_executable_factory_registry_t *registry,
    robot_composition_diagnostics_t *diagnostics);
const char *robot_composition_diagnostic_code_name(
    robot_composition_diagnostic_code_t code);
const char *robot_composition_stage_name(robot_composition_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif
