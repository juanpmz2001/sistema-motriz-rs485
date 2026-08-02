#ifndef ROBOT_COMPOSITION_H
#define ROBOT_COMPOSITION_H
#include "actuation_coordinator.h"
#include "robot_control.h"
#include "robot_profile.h"
typedef struct { robot_endpoint_registry_t registry; actuation_coordinator_t coordinator; struct robot_control_endpoint_adapter_storage *storage; } robot_composition_t;
esp_err_t robot_composition_init(robot_composition_t*,const robot_profile_t*,robot_control_handle_t);
void robot_composition_deinit(robot_composition_t*);
robot_endpoint_id_t robot_composition_motor_endpoint_id(const robot_profile_t*,uint8_t);
#endif
