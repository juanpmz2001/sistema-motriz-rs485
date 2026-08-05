#ifndef ROBOT_COMPOSITION_H
#define ROBOT_COMPOSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actuation_application_port.h"
#include "actuation_coordinator.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "robot_control.h"
#include "robot_driver_factory.h"
#include "robot_profile.h"
#include "rs485_transport.h"
#include "svd48.h"
#include "svd48_channel_endpoint_adapter.h"
#include "svd48_device.h"
#include "svd48_poll_service.h"
#include "svd48_poll_task.h"

typedef struct {
    bool used;
    uint16_t profile_bus_id;
    rs485_transport_t rs485;
} robot_composition_bus_slot_t;

typedef struct {
    bool used;
    bool started;
    uint16_t profile_device_id;
    uint16_t profile_bus_id;
    robot_endpoint_criticality_t criticality;
    StaticSemaphore_t state_lock_storage;
    SemaphoreHandle_t state_lock;
    svd48_device_t svd48;
} robot_composition_device_slot_t;

typedef struct {
    robot_endpoint_registry_t registry;
    actuation_coordinator_t coordinator;
    svd48_channel_endpoint_adapter_t adapters[ROBOT_PROFILE_MAX_ENDPOINTS];
    size_t adapter_count;
    robot_composition_bus_slot_t buses[ROBOT_PROFILE_MAX_BUSES];
    size_t bus_count;
    robot_composition_device_slot_t devices[ROBOT_PROFILE_MAX_DEVICES];
    size_t device_count;
    svd48_device_t *svd48_device_views[ROBOT_PROFILE_MAX_DEVICES];
    svd48_legacy_channel_binding_t legacy_bindings[SVD48_MOTOR_COUNT];
    robot_endpoint_id_t legacy_endpoint_ids[SVD48_MOTOR_COUNT];
    size_t legacy_binding_count;
    svd48_poll_service_t polling_service;
    svd48_poll_task_t polling_task;
    svd48_handle_t legacy_svd48;
    StaticSemaphore_t coordinator_lock_storage;
    SemaphoreHandle_t coordinator_lock;
    robot_control_handle_t legacy_robot;
    const robot_profile_t *profile;
    actuation_application_port_t application_port;
    robot_composition_diagnostics_t diagnostics;
    bool constructed;
    bool started;
} robot_composition_t;

esp_err_t robot_composition_init(robot_composition_t *composition,
                                 const robot_profile_t *profile);
esp_err_t robot_composition_start(robot_composition_t *composition);
esp_err_t robot_composition_stop(robot_composition_t *composition);
void robot_composition_deinit(robot_composition_t *composition);
void robot_composition_attach_legacy_robot(robot_composition_t *composition,
                                           robot_control_handle_t legacy_robot);
svd48_handle_t robot_composition_legacy_svd48(robot_composition_t *composition);
const robot_composition_diagnostics_t *robot_composition_get_diagnostics(
    const robot_composition_t *composition);
const robot_executable_factory_registry_t *robot_composition_factory_registry(void);

#endif
