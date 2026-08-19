#include "motion_status_port.h"

bool motion_status_blocks_maintenance_changes(motion_status_port_t *port,
                                              motion_control_state_t *state)
{
    if (state) {
        *state = MOTION_CONTROL_UNAVAILABLE;
    }

    motion_status_snapshot_t snapshot = { 0 };
    if (!motion_status_snapshot(port, &snapshot) || !snapshot.available) {
        return false;
    }
    if (state) {
        *state = snapshot.state;
    }
    return snapshot.state == MOTION_CONTROL_ARMED ||
           snapshot.state == MOTION_CONTROL_ACTIVE;
}

const char *motion_control_state_name(motion_control_state_t state)
{
    switch (state) {
    case MOTION_CONTROL_UNAVAILABLE:
        return "UNAVAILABLE";
    case MOTION_CONTROL_DISARMED:
        return "DISARMED";
    case MOTION_CONTROL_ARMED:
        return "ARMED";
    case MOTION_CONTROL_ACTIVE:
        return "ACTIVE";
    case MOTION_CONTROL_EXPIRED:
        return "EXPIRED";
    case MOTION_CONTROL_FAULT:
        return "FAULT";
    default:
        return "UNKNOWN";
    }
}
