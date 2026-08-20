#include "robot_safety_rc_lan_interlock_model.h"

#include <stdio.h>
#include <string.h>

static bool config_valid(const robot_safety_rc_lan_interlock_config_t *config)
{
    return config && (!config->enabled ||
                      (config->channel > 0U && config->channel <= 14U &&
                       config->active_max_us > 0U));
}

const char *robot_safety_rc_lan_interlock_state_name(
    robot_safety_rc_lan_interlock_state_t state)
{
    switch (state) {
    case ROBOT_SAFETY_RC_LAN_DISABLED:
        return "DISABLED";
    case ROBOT_SAFETY_RC_LAN_NO_SIGNAL:
        return "RC_NO_SIGNAL";
    case ROBOT_SAFETY_RC_LAN_FAILSAFE:
        return "RC_FAILSAFE";
    case ROBOT_SAFETY_RC_LAN_PPM_PRIORITY:
        return "PPM_PRIORITY";
    case ROBOT_SAFETY_RC_LAN_PPM_LOST:
        return "PPM_LOST";
    case ROBOT_SAFETY_RC_LAN_CHANNEL_UNAVAILABLE:
        return "RC_CHANNEL_UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

bool robot_safety_rc_lan_interlock_model_init(
    robot_safety_rc_lan_interlock_model_t *model,
    const robot_safety_rc_lan_interlock_config_t *config)
{
    if (!model || !config_valid(config)) {
        return false;
    }
    memset(model, 0, sizeof(*model));
    model->config = *config;
    model->state = config->enabled ? ROBOT_SAFETY_RC_LAN_NO_SIGNAL
                                   : ROBOT_SAFETY_RC_LAN_DISABLED;
    model->initialized = true;
    return true;
}

bool robot_safety_rc_lan_interlock_model_update(
    robot_safety_rc_lan_interlock_model_t *model,
    const robot_safety_rc_lan_observation_t *observation,
    robot_safety_rc_lan_interlock_snapshot_t *snapshot)
{
    if (!model || !model->initialized || !observation || !snapshot) {
        return false;
    }

    robot_safety_rc_lan_interlock_state_t next =
        ROBOT_SAFETY_RC_LAN_DISABLED;
    uint16_t channel_us = 0U;
    if (model->config.enabled) {
        if (!observation->receiver_available || !observation->signal_valid) {
            next = model->priority_epoch > 0U
                       ? ROBOT_SAFETY_RC_LAN_PPM_LOST
                       : ROBOT_SAFETY_RC_LAN_NO_SIGNAL;
        } else if (!observation->channels ||
                   observation->channel_count < model->config.channel) {
            next = ROBOT_SAFETY_RC_LAN_CHANNEL_UNAVAILABLE;
        } else {
            channel_us = observation->channels[model->config.channel - 1U];
            next = channel_us <= model->config.active_max_us
                       ? ROBOT_SAFETY_RC_LAN_PPM_PRIORITY
                       : ROBOT_SAFETY_RC_LAN_FAILSAFE;
        }
    }

    if (next == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY &&
        model->state != ROBOT_SAFETY_RC_LAN_PPM_PRIORITY) {
        model->priority_epoch++;
    }
    model->state = next;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = next;
    snapshot->lan_allowed =
        next == ROBOT_SAFETY_RC_LAN_DISABLED ||
        next == ROBOT_SAFETY_RC_LAN_NO_SIGNAL ||
        next == ROBOT_SAFETY_RC_LAN_FAILSAFE ||
        next == ROBOT_SAFETY_RC_LAN_PPM_LOST;
    snapshot->channel_us = channel_us;
    snapshot->priority_epoch = model->priority_epoch;
    snprintf(snapshot->detail,
             sizeof(snapshot->detail),
             "%s",
             robot_safety_rc_lan_interlock_state_name(next));
    return true;
}
