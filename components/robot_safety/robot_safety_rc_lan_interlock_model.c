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
    case ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE:
        return "PPM_PRIORITY_CANDIDATE";
    case ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE:
        return "RC_FAILSAFE_CANDIDATE";
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
    if (config->enabled && config->transition_confirm_good_frames == 0U) {
        return false;
    }
    model->state = config->enabled ? ROBOT_SAFETY_RC_LAN_NO_SIGNAL
                                   : ROBOT_SAFETY_RC_LAN_DISABLED;
    model->committed_state = model->state;
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

    robot_safety_rc_lan_interlock_state_t next = ROBOT_SAFETY_RC_LAN_DISABLED;
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

    const bool is_valid_new_frame = observation->signal_valid &&
                                    observation->valid_frame_sequence != 0U &&
                                    observation->valid_frame_sequence !=
                                        model->last_observed_frame_sequence;
    if (is_valid_new_frame) {
        model->last_observed_frame_sequence = observation->valid_frame_sequence;
    }

    if (!model->config.enabled) {
        model->committed_state = next;
        model->candidate_state = next;
        model->candidate_valid_frames = 0U;
        model->state = next;
    } else if (next == ROBOT_SAFETY_RC_LAN_NO_SIGNAL ||
               next == ROBOT_SAFETY_RC_LAN_PPM_LOST ||
               next == ROBOT_SAFETY_RC_LAN_CHANNEL_UNAVAILABLE) {
        /* Absence is governed by the existing age deadline, not by a frame
         * confirmation counter. It also cancels any noisy CH5 candidate. */
        model->candidate_valid_frames = 0U;
        model->candidate_state = model->committed_state;
        model->state = next;
    } else if (next == model->committed_state) {
        model->candidate_valid_frames = 0U;
        model->candidate_state = next;
        model->state = next;
    } else if (is_valid_new_frame) {
        if (model->candidate_state == next) {
            model->candidate_valid_frames++;
        } else {
            model->candidate_state = next;
            model->candidate_valid_frames = 1U;
        }
        if (model->candidate_valid_frames >=
            model->config.transition_confirm_good_frames) {
            if (next == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY &&
                model->committed_state != ROBOT_SAFETY_RC_LAN_PPM_PRIORITY) {
                model->priority_epoch++;
            }
            model->committed_state = next;
            model->candidate_valid_frames = 0U;
            model->state = next;
        } else {
            model->state = next == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY
                               ? ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE
                               : ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE;
        }
    } else if (model->candidate_valid_frames == 0U) {
        /* A cached accepted frame may report state, but cannot begin a
         * transition or fabricate confirmation. */
        model->state = model->committed_state;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = model->state;
    const robot_safety_rc_lan_interlock_state_t authority_state =
        (model->state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE ||
         model->state == ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE)
            ? model->committed_state
            : model->state;
    snapshot->lan_allowed =
        authority_state == ROBOT_SAFETY_RC_LAN_DISABLED ||
        authority_state == ROBOT_SAFETY_RC_LAN_NO_SIGNAL ||
        authority_state == ROBOT_SAFETY_RC_LAN_FAILSAFE ||
        authority_state == ROBOT_SAFETY_RC_LAN_PPM_LOST;
    snapshot->channel_us = channel_us;
    snapshot->priority_epoch = model->priority_epoch;
    snapshot->candidate_valid_frames = model->candidate_valid_frames;
    snapshot->transition_confirm_good_frames =
        model->config.transition_confirm_good_frames;
    snprintf(snapshot->detail,
             sizeof(snapshot->detail),
             "%s",
             robot_safety_rc_lan_interlock_state_name(model->state));
    return true;
}
