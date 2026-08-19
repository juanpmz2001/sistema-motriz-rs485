#include "motion_status_port.h"

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
