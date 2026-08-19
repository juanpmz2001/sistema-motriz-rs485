#ifndef MOTION_CONTROL_PORT_H
#define MOTION_CONTROL_PORT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct motion_control_port motion_control_port_t;

typedef struct {
    bool (*stop_all)(motion_control_port_t *port,
                     char *detail,
                     size_t detail_size);
} motion_control_ops_t;

struct motion_control_port {
    const motion_control_ops_t *ops;
    void *context;
};

static inline bool motion_control_stop_all(motion_control_port_t *port,
                                           char *detail,
                                           size_t detail_size)
{
    return port && port->ops && port->ops->stop_all
               ? port->ops->stop_all(port, detail, detail_size)
               : false;
}

#endif
