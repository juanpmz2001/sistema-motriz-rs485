#ifndef ROBOT_CAPABILITIES_H
#define ROBOT_CAPABILITIES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ROBOT_ENDPOINT_NAME_MAX 40
#define ROBOT_ENDPOINT_REGISTRY_MAX 16
#define ROBOT_CAPABILITY_VELOCITY_RPM (UINT32_C(1)<<0)
#define ROBOT_CAPABILITY_STOPPABLE (UINT32_C(1)<<1)
typedef uint16_t robot_endpoint_id_t;
typedef enum { ROBOT_CAP_OK=0, ROBOT_CAP_INVALID_ARGUMENT, ROBOT_CAP_UNAVAILABLE, ROBOT_CAP_UNSUPPORTED, ROBOT_CAP_OUT_OF_RANGE, ROBOT_CAP_IO_ERROR } robot_capability_error_t;
typedef enum { ROBOT_ENDPOINT_DEVELOPMENT=0, ROBOT_ENDPOINT_OPTIONAL, ROBOT_ENDPOINT_REQUIRED } robot_endpoint_criticality_t;
typedef struct robot_velocity_rpm_port robot_velocity_rpm_port_t;
typedef struct { robot_capability_error_t (*set_velocity_rpm)(robot_velocity_rpm_port_t *, int16_t); } robot_velocity_rpm_ops_t;
struct robot_velocity_rpm_port { const robot_velocity_rpm_ops_t *ops; void *context; int16_t min_rpm; int16_t max_rpm; };
typedef struct robot_stoppable_port robot_stoppable_port_t;
typedef struct { robot_capability_error_t (*stop)(robot_stoppable_port_t *); } robot_stoppable_ops_t;
struct robot_stoppable_port { const robot_stoppable_ops_t *ops; void *context; };
typedef struct { robot_endpoint_id_t id; const char *name; uint32_t capabilities; robot_endpoint_criticality_t criticality; bool available; robot_velocity_rpm_port_t *velocity_rpm; robot_stoppable_port_t *stoppable; } robot_endpoint_t;
typedef struct { robot_endpoint_t *items[ROBOT_ENDPOINT_REGISTRY_MAX]; size_t count; } robot_endpoint_registry_t;
typedef enum { ROBOT_REGISTRY_OK=0, ROBOT_REGISTRY_INVALID_ARGUMENT, ROBOT_REGISTRY_DUPLICATE_ID, ROBOT_REGISTRY_FULL, ROBOT_REGISTRY_CAPABILITY_MISMATCH } robot_registry_error_t;
robot_capability_error_t robot_velocity_set_rpm(robot_endpoint_t *endpoint, int16_t rpm);
robot_capability_error_t robot_endpoint_stop(robot_endpoint_t *endpoint);
void robot_endpoint_registry_init(robot_endpoint_registry_t *registry);
robot_registry_error_t robot_endpoint_registry_add(robot_endpoint_registry_t *registry, robot_endpoint_t *endpoint);
robot_endpoint_t *robot_endpoint_registry_find(const robot_endpoint_registry_t *registry, robot_endpoint_id_t id);
bool robot_endpoint_has_capability(const robot_endpoint_t *endpoint, uint32_t capability);
const robot_endpoint_t *robot_endpoint_registry_at(const robot_endpoint_registry_t *registry, size_t index);
#endif
