#ifndef ROBOT_COMPOSITION_H
#define ROBOT_COMPOSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actuation_application_port.h"
#include "actuation_coordinator.h"
#include "as5600_device.h"
#include "as5600_diagnostics_port.h"
#include "as5600_position_endpoint_adapter.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bitbang_transport.h"
#include "motor_mode_pwm.h"
#include "robot_control.h"
#include "robot_driver_factory.h"
#include "robot_profile.h"
#include "rs485_transport.h"
#include "svd48.h"
#include "svd48_channel_endpoint_adapter.h"
#include "svd48_device.h"
#include "svd48_poll_service.h"
#include "svd48_poll_task.h"
#include "svd48_workspace_port.h"
#include "steering_position_endpoint_adapter.h"

typedef struct {
    bool used;
    uint16_t profile_bus_id;
    rs485_transport_t rs485;
} robot_composition_bus_slot_t;

typedef struct {
    bool used;
    uint16_t profile_bus_id;
    i2c_bitbang_transport_t i2c;
} robot_composition_i2c_bus_slot_t;

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
    bool used;
    bool started;
    uint16_t profile_device_id;
    uint16_t profile_bus_id;
    robot_endpoint_criticality_t criticality;
    motor_mode_pwm_t pwm;
} robot_composition_pwm_motor_slot_t;

typedef struct {
    bool used;
    bool started;
    uint16_t profile_device_id;
    uint16_t profile_bus_id;
    robot_endpoint_criticality_t criticality;
    uint32_t telemetry_period_ms;
    uint32_t next_poll_ms;
    StaticSemaphore_t state_lock_storage;
    SemaphoreHandle_t state_lock;
    as5600_calibration_lut_t calibration;
    bool calibration_configured;
    as5600_device_t as5600;
} robot_composition_as5600_slot_t;

typedef struct {
    bool used;
    bool started;
    uint16_t profile_device_id;
    robot_endpoint_criticality_t criticality;
    StaticSemaphore_t state_lock_storage;
    SemaphoreHandle_t state_lock;
    steering_position_endpoint_adapter_t adapter;
} robot_composition_steering_slot_t;

typedef struct {
    TaskHandle_t task;
    StaticSemaphore_t stopped_storage;
    SemaphoreHandle_t stopped;
    volatile bool stop_requested;
    volatile bool running;
} robot_composition_steering_task_t;

typedef struct {
    TaskHandle_t task;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    svd48_workspace_stop_diagnostic_result_t report;
} robot_composition_stop_diagnostic_t;

typedef struct {
    robot_endpoint_registry_t registry;
    actuation_coordinator_t coordinator;
    svd48_channel_endpoint_adapter_t adapters[ROBOT_PROFILE_MAX_ENDPOINTS];
    size_t adapter_count;
    as5600_position_endpoint_adapter_t
        as5600_observation_adapters[ROBOT_PROFILE_MAX_ENDPOINTS];
    size_t as5600_observation_adapter_count;
    robot_endpoint_id_t steering_endpoint_ids[ROBOT_PROFILE_MAX_ENDPOINTS];
    size_t steering_endpoint_count;
    robot_composition_bus_slot_t buses[ROBOT_PROFILE_MAX_BUSES];
    size_t bus_count;
    robot_composition_i2c_bus_slot_t i2c_buses[ROBOT_PROFILE_MAX_BUSES];
    size_t i2c_bus_count;
    robot_composition_device_slot_t devices[ROBOT_PROFILE_MAX_DEVICES];
    size_t device_count;
    robot_composition_pwm_motor_slot_t
        pwm_motors[ROBOT_PROFILE_MAX_DEVICES];
    size_t pwm_motor_count;
    robot_composition_as5600_slot_t as5600_devices[ROBOT_PROFILE_MAX_DEVICES];
    size_t as5600_device_count;
    robot_composition_steering_slot_t
        steering_controllers[ROBOT_PROFILE_MAX_DEVICES];
    size_t steering_controller_count;
    svd48_device_t *svd48_device_views[ROBOT_PROFILE_MAX_DEVICES];
    svd48_legacy_channel_binding_t legacy_bindings[SVD48_MOTOR_COUNT];
    robot_endpoint_id_t legacy_endpoint_ids[SVD48_MOTOR_COUNT];
    size_t legacy_binding_count;
    svd48_poll_service_t polling_service;
    svd48_poll_task_t polling_task;
    robot_composition_steering_task_t steering_task;
    robot_composition_stop_diagnostic_t stop_diagnostic;
    svd48_handle_t legacy_svd48;
    StaticSemaphore_t coordinator_lock_storage;
    SemaphoreHandle_t coordinator_lock;
    robot_control_handle_t legacy_robot;
    const robot_profile_t *profile;
    actuation_application_port_t application_port;
    svd48_workspace_port_t svd48_workspace_port;
    as5600_diagnostics_port_t as5600_diagnostics_port;
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
/* Device-qualified inventory and cached diagnostics for the SVD48 maintenance
 * workspace. Actuation still flows through the application/coordinator port. */
svd48_workspace_port_t *robot_composition_svd48_workspace_port(
    robot_composition_t *composition);
/* Read-only device-qualified diagnostics. This is intentionally separate from
 * the generic endpoint/application observation boundary. */
as5600_diagnostics_port_t *robot_composition_as5600_diagnostics_port(
    robot_composition_t *composition);
const robot_composition_diagnostics_t *robot_composition_get_diagnostics(
    const robot_composition_t *composition);
const robot_executable_factory_registry_t *robot_composition_factory_registry(void);

#endif
