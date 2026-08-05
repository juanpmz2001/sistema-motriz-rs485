#ifndef ACTUATION_APPLICATION_PORT_H
#define ACTUATION_APPLICATION_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ACTUATION_APPLICATION_OK = 0,
    ACTUATION_APPLICATION_INVALID_ARGUMENT,
    ACTUATION_APPLICATION_FAILED,
    ACTUATION_APPLICATION_PARTIAL,
    ACTUATION_APPLICATION_TIMEOUT,
} actuation_application_result_t;

typedef struct actuation_application_port actuation_application_port_t;

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

#endif
