#ifndef MOTION_STATUS_PORT_H
#define MOTION_STATUS_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_capabilities.h"

#define MOTION_STATUS_MAX_ENDPOINTS 8U
#define MOTION_STATUS_DETAIL_MAX 48U

typedef enum {
    MOTION_CONTROL_UNAVAILABLE = 0,
    MOTION_CONTROL_DISARMED,
    MOTION_CONTROL_ARMED,
    MOTION_CONTROL_ACTIVE,
    MOTION_CONTROL_EXPIRED,
    MOTION_CONTROL_FAULT,
} motion_control_state_t;

typedef struct {
    robot_endpoint_id_t endpoint_id;
    const char *name;
    int16_t target_rpm;
    bool observed_valid;
    int16_t observed_rpm;
    uint32_t observation_timestamp_ms;
    bool online;
    bool stale;
    robot_endpoint_health_t health;
} motion_status_endpoint_t;

typedef struct {
    bool available;
    bool task_running;
    motion_control_state_t state;
    uint32_t command_ttl_ms;
    uint32_t lease_age_ms;
    uint32_t lease_remaining_ms;
    bool lease_fresh;
    bool deadman;
    uint64_t stream_id_hash;
    uint64_t sequence;
    float max_vx_mps;
    float max_vy_mps;
    float max_wz_radps;
    float requested_vx_mps;
    float requested_vy_mps;
    float requested_wz_radps;
    size_t endpoint_count;
    motion_status_endpoint_t endpoints[MOTION_STATUS_MAX_ENDPOINTS];
    char last_detail[MOTION_STATUS_DETAIL_MAX];
} motion_status_snapshot_t;

typedef struct motion_status_port motion_status_port_t;

typedef struct {
    bool (*snapshot)(motion_status_port_t *port,
                     motion_status_snapshot_t *snapshot);
} motion_status_ops_t;

struct motion_status_port {
    const motion_status_ops_t *ops;
    void *context;
};

static inline bool motion_status_snapshot(motion_status_port_t *port,
                                          motion_status_snapshot_t *snapshot)
{
    return port && port->ops && port->ops->snapshot
               ? port->ops->snapshot(port, snapshot)
               : false;
}

const char *motion_control_state_name(motion_control_state_t state);

/* Maintenance writes and bench motion must not compete with a prepared or active
 * continuous-control session. Profiles without this capability intentionally
 * report UNAVAILABLE and remain eligible for bounded maintenance operations. */
bool motion_status_blocks_maintenance_changes(motion_status_port_t *port,
                                              motion_control_state_t *state);

#endif
