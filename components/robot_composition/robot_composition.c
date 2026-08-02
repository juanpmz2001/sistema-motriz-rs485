#include "robot_composition.h"

#include <string.h>

#define COORDINATOR_LOCK_TIMEOUT_MS 500

static int legacy_set_speed(void *context, uint8_t motor, int16_t rpm)
{
    return (int)robot_control_set_motor_speed((robot_control_handle_t)context, motor, rpm);
}

static int legacy_stop_motor(void *context, uint8_t motor)
{
    return (int)robot_control_stop_motor((robot_control_handle_t)context, motor);
}

static bool acquire_lock(void *context)
{
    robot_composition_t *composition = context;
    return xSemaphoreTake(composition->lock, pdMS_TO_TICKS(COORDINATOR_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void release_lock(void *context)
{
    robot_composition_t *composition = context;
    xSemaphoreGive(composition->lock);
}

static uint8_t legacy_motor_index(const robot_profile_t *profile,
                                  const robot_endpoint_profile_t *endpoint)
{
    uint8_t index = endpoint->channel;
    for (size_t device_index = 0; device_index < profile->device_count; ++device_index) {
        const robot_device_profile_t *device = &profile->devices[device_index];
        if (device->id == endpoint->device_id) return index;
        if (device->driver_id == ROBOT_DRIVER_SVD48) index += device->channel_count;
    }
    return UINT8_MAX;
}

static robot_endpoint_id_t endpoint_for_legacy_motor(const robot_composition_t *composition,
                                                     uint8_t motor)
{
    for (size_t index = 0; index < composition->profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &composition->profile->endpoints[index];
        if (legacy_motor_index(composition->profile, endpoint) == motor) return endpoint->id;
    }
    return 0;
}

static actuation_application_result_t map_result(actuation_result_t result)
{
    switch (result) {
    case ACTUATION_RESULT_SUCCESS: return ACTUATION_APPLICATION_OK;
    case ACTUATION_RESULT_PARTIAL: return ACTUATION_APPLICATION_PARTIAL;
    case ACTUATION_RESULT_LOCK_TIMEOUT: return ACTUATION_APPLICATION_TIMEOUT;
    default: return ACTUATION_APPLICATION_FAILED;
    }
}

static actuation_application_result_t application_set_speed(
    actuation_application_port_t *port, uint8_t motor, int16_t rpm)
{
    robot_composition_t *composition = port->context;
    robot_endpoint_id_t endpoint_id = endpoint_for_legacy_motor(composition, motor);
    if (!endpoint_id) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    return map_result(actuation_coordinator_set_velocity_rpm(
        &composition->coordinator, endpoint_id, rpm, &report));
}

static actuation_application_result_t application_stop_motor(
    actuation_application_port_t *port, uint8_t motor)
{
    robot_composition_t *composition = port->context;
    robot_endpoint_id_t endpoint_id = endpoint_for_legacy_motor(composition, motor);
    if (!endpoint_id) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    return map_result(actuation_coordinator_stop_endpoint(
        &composition->coordinator, endpoint_id, &report));
}

static actuation_application_result_t application_stop_all(actuation_application_port_t *port)
{
    robot_composition_t *composition = port->context;
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_stop_all(&composition->coordinator, &report);
    if (result == ACTUATION_RESULT_SUCCESS) {
        robot_control_record_coordinated_stop(composition->legacy_robot);
    }
    return map_result(result);
}

esp_err_t robot_composition_init(robot_composition_t *composition,
                                 const robot_profile_t *profile,
                                 robot_control_handle_t legacy_robot)
{
    static const actuation_application_ops_t application_ops = {
        .set_legacy_motor_speed_rpm = application_set_speed,
        .stop_legacy_motor = application_stop_motor,
        .stop_all = application_stop_all,
    };
    if (!composition || !profile || !legacy_robot ||
        robot_profile_validate(profile) != ROBOT_PROFILE_VALID) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(composition, 0, sizeof(*composition));
    composition->profile = profile;
    composition->legacy_robot = legacy_robot;
    composition->lock = xSemaphoreCreateMutexStatic(&composition->lock_storage);
    if (!composition->lock) {
        return ESP_ERR_NO_MEM;
    }
    robot_endpoint_registry_init(&composition->registry);
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        if (!robot_control_endpoint_adapter_init(
                &composition->adapters[index], legacy_robot, legacy_motor_index(profile, endpoint),
                endpoint->id, endpoint->name, endpoint->criticality,
                endpoint->min_rpm, endpoint->max_rpm, legacy_set_speed, legacy_stop_motor) ||
            robot_endpoint_registry_add(&composition->registry,
                                        &composition->adapters[index].endpoint) != ROBOT_REGISTRY_OK) {
            robot_composition_deinit(composition);
            return ESP_ERR_INVALID_ARG;
        }
    }
    const actuation_lock_port_t lock = {
        .acquire = acquire_lock, .release = release_lock, .context = composition};
    if (!actuation_coordinator_init(&composition->coordinator, &composition->registry, &lock)) {
        robot_composition_deinit(composition);
        return ESP_ERR_INVALID_ARG;
    }
    composition->application_port.ops = &application_ops;
    composition->application_port.context = composition;
    return ESP_OK;
}

void robot_composition_deinit(robot_composition_t *composition)
{
    if (!composition) {
        return;
    }
    if (composition->lock) {
        vSemaphoreDelete(composition->lock);
    }
    memset(composition, 0, sizeof(*composition));
}
