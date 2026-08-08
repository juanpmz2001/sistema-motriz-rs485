#ifndef AS5600_DIAGNOSTICS_PORT_H
#define AS5600_DIAGNOSTICS_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "as5600_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Explicit composition-to-application port for lower-level AS5600
 * qualification.  It is intentionally separate from actuation and generic
 * position-observation ports: callers identify a profile device and receive
 * a concrete device diagnostic snapshot.
 */
typedef struct as5600_diagnostics_port as5600_diagnostics_port_t;

typedef struct {
    bool (*read)(as5600_diagnostics_port_t *port,
                 uint16_t profile_device_id,
                 as5600_device_diagnostics_t *diagnostics);
} as5600_diagnostics_port_ops_t;

struct as5600_diagnostics_port {
    const as5600_diagnostics_port_ops_t *ops;
    void *context;
};

static inline bool as5600_diagnostics_port_read(
    as5600_diagnostics_port_t *port,
    uint16_t profile_device_id,
    as5600_device_diagnostics_t *diagnostics)
{
    return port != NULL && port->ops != NULL && port->ops->read != NULL
               ? port->ops->read(port, profile_device_id, diagnostics)
               : false;
}

#ifdef __cplusplus
}
#endif

#endif
