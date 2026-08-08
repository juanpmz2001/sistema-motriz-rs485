#ifndef ACTUATION_APPLICATION_PORT_H
#define ACTUATION_APPLICATION_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_capabilities.h"

typedef enum {
    ACTUATION_APPLICATION_OK = 0,
    ACTUATION_APPLICATION_INVALID_ARGUMENT,
    ACTUATION_APPLICATION_FAILED,
    ACTUATION_APPLICATION_PARTIAL,
    ACTUATION_APPLICATION_TIMEOUT,
} actuation_application_result_t;

typedef struct actuation_application_port actuation_application_port_t;

/* Internal in-firmware DTO, not a wire format or stable binary ABI. Components
 * that consume it are rebuilt together and should use named fields. */
typedef struct {
    robot_endpoint_id_t id;
    const char *name;
    uint32_t capabilities;
    robot_endpoint_criticality_t criticality;
    bool available;
    bool velocity_observation_supported;
    int16_t min_rpm;
    int16_t max_rpm;
    bool position_observation_supported;
    float min_position_degrees;
    float max_position_degrees;
} actuation_application_endpoint_info_t;

/* Internal source-level application vtable; it is not a stable binary ABI. */
typedef struct {
    actuation_application_result_t (*set_legacy_motor_speed_rpm)(
        actuation_application_port_t *port, uint8_t motor_index, int16_t rpm);
    actuation_application_result_t (*stop_legacy_motor)(
        actuation_application_port_t *port, uint8_t motor_index);
    actuation_application_result_t (*stop_all)(actuation_application_port_t *port);
    size_t (*legacy_motor_count)(const actuation_application_port_t *port);
    bool (*legacy_motor_limits_rpm)(const actuation_application_port_t *port,
                                    uint8_t motor_index,
                                    int16_t *min_rpm,
                                    int16_t *max_rpm);
    size_t (*endpoint_count)(const actuation_application_port_t *port);
    bool (*endpoint_at)(const actuation_application_port_t *port,
                        size_t index,
                        actuation_application_endpoint_info_t *endpoint);
    actuation_application_result_t (*set_endpoint_speed_rpm)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id,
        int16_t rpm);
    actuation_application_result_t (*stop_endpoint)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id);
    bool (*get_endpoint_velocity_observation)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id,
        robot_velocity_observation_t *observation);
    actuation_application_result_t (*set_endpoint_position_degrees)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id,
        float degrees);
    actuation_application_result_t (*set_endpoint_position_reference_degrees)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id,
        float degrees);
    bool (*get_endpoint_position_observation)(
        actuation_application_port_t *port,
        robot_endpoint_id_t endpoint_id,
        robot_position_observation_t *observation);
} actuation_application_ops_t;

struct actuation_application_port {
    const actuation_application_ops_t *ops;
    void *context;
};

static inline actuation_application_result_t actuation_application_set_legacy_motor_speed_rpm(
    actuation_application_port_t *port, uint8_t motor_index, int16_t rpm)
{
    return port && port->ops && port->ops->set_legacy_motor_speed_rpm
               ? port->ops->set_legacy_motor_speed_rpm(port, motor_index, rpm)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline actuation_application_result_t actuation_application_stop_legacy_motor(
    actuation_application_port_t *port, uint8_t motor_index)
{
    return port && port->ops && port->ops->stop_legacy_motor
               ? port->ops->stop_legacy_motor(port, motor_index)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline actuation_application_result_t actuation_application_stop_all(
    actuation_application_port_t *port)
{
    return port && port->ops && port->ops->stop_all
               ? port->ops->stop_all(port)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline size_t actuation_application_legacy_motor_count(
    const actuation_application_port_t *port)
{
    return port && port->ops && port->ops->legacy_motor_count
               ? port->ops->legacy_motor_count(port)
               : 0U;
}

static inline bool actuation_application_legacy_motor_limits_rpm(
    const actuation_application_port_t *port,
    uint8_t motor_index,
    int16_t *min_rpm,
    int16_t *max_rpm)
{
    return port && port->ops && port->ops->legacy_motor_limits_rpm
               ? port->ops->legacy_motor_limits_rpm(port,
                                                    motor_index,
                                                    min_rpm,
                                                    max_rpm)
               : false;
}

static inline size_t actuation_application_endpoint_count(
    const actuation_application_port_t *port)
{
    return port && port->ops && port->ops->endpoint_count
               ? port->ops->endpoint_count(port)
               : 0U;
}

static inline bool actuation_application_endpoint_at(
    const actuation_application_port_t *port,
    size_t index,
    actuation_application_endpoint_info_t *endpoint)
{
    return port && port->ops && port->ops->endpoint_at
               ? port->ops->endpoint_at(port, index, endpoint)
               : false;
}

static inline bool actuation_application_find_endpoint(
    const actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    actuation_application_endpoint_info_t *endpoint)
{
    if (!endpoint || endpoint_id == 0U) {
        return false;
    }
    size_t count = actuation_application_endpoint_count(port);
    for (size_t index = 0; index < count; ++index) {
        actuation_application_endpoint_info_t candidate;
        if (actuation_application_endpoint_at(port, index, &candidate) &&
            candidate.id == endpoint_id) {
            *endpoint = candidate;
            return true;
        }
    }
    return false;
}

static inline actuation_application_result_t
actuation_application_set_endpoint_speed_rpm(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    int16_t rpm)
{
    return port && port->ops && port->ops->set_endpoint_speed_rpm
               ? port->ops->set_endpoint_speed_rpm(port, endpoint_id, rpm)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline actuation_application_result_t actuation_application_stop_endpoint(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id)
{
    return port && port->ops && port->ops->stop_endpoint
               ? port->ops->stop_endpoint(port, endpoint_id)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline bool actuation_application_get_endpoint_velocity_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_velocity_observation_t *observation)
{
    return port && port->ops && port->ops->get_endpoint_velocity_observation
               ? port->ops->get_endpoint_velocity_observation(port,
                                                              endpoint_id,
                                                              observation)
               : false;
}

static inline actuation_application_result_t
actuation_application_set_endpoint_position_degrees(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    return port && port->ops && port->ops->set_endpoint_position_degrees
               ? port->ops->set_endpoint_position_degrees(port,
                                                          endpoint_id,
                                                          degrees)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline actuation_application_result_t
actuation_application_set_endpoint_position_reference_degrees(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    return port && port->ops &&
                   port->ops->set_endpoint_position_reference_degrees
               ? port->ops->set_endpoint_position_reference_degrees(
                     port, endpoint_id, degrees)
               : ACTUATION_APPLICATION_INVALID_ARGUMENT;
}

static inline bool actuation_application_get_endpoint_position_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_position_observation_t *observation)
{
    return port && port->ops && port->ops->get_endpoint_position_observation
               ? port->ops->get_endpoint_position_observation(port,
                                                              endpoint_id,
                                                              observation)
               : false;
}

#endif
