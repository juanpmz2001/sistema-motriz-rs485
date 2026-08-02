#ifndef ROBOT_COMPOSITION_H
#define ROBOT_COMPOSITION_H

#include "actuation_application_port.h"
#include "actuation_coordinator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "robot_control.h"
#include "robot_control_endpoint_adapter.h"
#include "robot_profile.h"

typedef struct {
    robot_endpoint_registry_t registry;
    actuation_coordinator_t coordinator;
    robot_control_endpoint_adapter_t adapters[ROBOT_PROFILE_MAX_ENDPOINTS];
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    robot_control_handle_t legacy_robot;
    const robot_profile_t *profile;
    actuation_application_port_t application_port;
} robot_composition_t;

esp_err_t robot_composition_init(robot_composition_t *composition,
                                 const robot_profile_t *profile,
                                 robot_control_handle_t legacy_robot);
void robot_composition_deinit(robot_composition_t *composition);

#endif
