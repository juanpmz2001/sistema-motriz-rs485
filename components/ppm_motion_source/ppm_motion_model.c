#include "ppm_motion_model.h"

#include <string.h>

#define PPM_MOTION_ARM_PENDING_MAX_NEW_FRAMES 2U
#define PPM_MOTION_STREAM_PREFIX UINT64_C(0x5243000000000000)

static bool axis_config_valid(uint16_t minimum_us,
                              uint16_t center_us,
                              uint16_t maximum_us,
                              uint16_t deadband_us,
                              uint16_t valid_min_us,
                              uint16_t valid_max_us)
{
    return minimum_us >= valid_min_us && maximum_us <= valid_max_us &&
           minimum_us + deadband_us < center_us &&
           center_us + deadband_us < maximum_us;
}

static bool config_valid(const ppm_motion_model_config_t *config)
{
    return config && config->throttle_channel > 0U &&
           config->throttle_channel <= PPM_MOTION_MODEL_MAX_CHANNELS &&
           config->steering_channel > 0U &&
           config->steering_channel <= PPM_MOTION_MODEL_MAX_CHANNELS &&
           config->enable_channel > 0U &&
           config->enable_channel <= PPM_MOTION_MODEL_MAX_CHANNELS &&
           config->speed_scale_channel > 0U &&
           config->speed_scale_channel <= PPM_MOTION_MODEL_MAX_CHANNELS &&
           config->throttle_channel != config->steering_channel &&
           config->throttle_channel != config->enable_channel &&
           config->steering_channel != config->enable_channel &&
           config->speed_scale_channel != config->throttle_channel &&
           config->speed_scale_channel != config->steering_channel &&
           config->speed_scale_channel != config->enable_channel &&
           config->enable_active_max_us > 0U &&
           config->neutral_deadband_us > 0U &&
           config->valid_min_us < config->valid_max_us &&
           config->enable_active_max_us >= config->valid_min_us &&
           config->enable_active_max_us <= config->valid_max_us &&
           axis_config_valid(config->throttle_min_us,
                             config->throttle_center_us,
                             config->throttle_max_us,
                             config->neutral_deadband_us,
                             config->valid_min_us,
                             config->valid_max_us) &&
           axis_config_valid(config->steering_min_us,
                             config->steering_center_us,
                             config->steering_max_us,
                             config->neutral_deadband_us,
                             config->valid_min_us,
                             config->valid_max_us) &&
           config->speed_scale_min_us >= config->valid_min_us &&
           config->speed_scale_min_us < config->speed_scale_max_us &&
           config->speed_scale_max_us <= config->valid_max_us &&
           config->speed_scale_min > 0.0f &&
           config->speed_scale_min <= config->speed_scale_max &&
           config->speed_scale_max <= 1.0f &&
           (config->throttle_sign == -1 || config->throttle_sign == 1) &&
           (config->steering_sign == -1 || config->steering_sign == 1);
}

static bool pulse_is_valid(const ppm_motion_model_t *model, uint16_t pulse_us)
{
    return pulse_us >= model->config.valid_min_us &&
           pulse_us <= model->config.valid_max_us;
}

static bool input_has_channels(const ppm_motion_model_t *model,
                               const ppm_motion_input_t *input)
{
    return input && input->signal_valid &&
           input->channel_count >= model->config.enable_channel &&
           input->channel_count >= model->config.throttle_channel &&
           input->channel_count >= model->config.steering_channel &&
           input->channel_count >= model->config.speed_scale_channel &&
           pulse_is_valid(model,
                          input->channels[model->config.throttle_channel - 1U]) &&
           pulse_is_valid(model,
                          input->channels[model->config.steering_channel - 1U]) &&
           pulse_is_valid(model,
                          input->channels[model->config.enable_channel - 1U]) &&
           pulse_is_valid(model,
                          input->channels[model->config.speed_scale_channel - 1U]);
}

static bool input_is_priority(const ppm_motion_model_t *model,
                              const ppm_motion_input_t *input)
{
    return input_has_channels(model, input) &&
           input->channels[model->config.enable_channel - 1U] <=
               model->config.enable_active_max_us;
}

static bool is_neutral(uint16_t pulse_us,
                       uint16_t center_us,
                       uint16_t deadband_us)
{
    int32_t difference = (int32_t)pulse_us - center_us;
    if (difference < 0) {
        difference = -difference;
    }
    return difference <= deadband_us;
}

static float normalize_axis(uint16_t pulse_us,
                            uint16_t minimum_us,
                            uint16_t center_us,
                            uint16_t maximum_us,
                            uint16_t deadband_us,
                            int8_t sign)
{
    const int32_t center = center_us;
    const int32_t deadband = deadband_us;
    int32_t delta = (int32_t)pulse_us - center;
    if (delta >= -deadband && delta <= deadband) {
        return 0.0f;
    }

    float normalized;
    if (delta > 0) {
        const int32_t span = (int32_t)maximum_us - center - deadband;
        normalized = span > 0 ? (float)(delta - deadband) / (float)span : 0.0f;
    } else {
        const int32_t span = center - deadband - (int32_t)minimum_us;
        normalized = span > 0 ? (float)(delta + deadband) / (float)span : 0.0f;
    }
    if (normalized > 1.0f) {
        normalized = 1.0f;
    } else if (normalized < -1.0f) {
        normalized = -1.0f;
    }
    return sign < 0 ? -normalized : normalized;
}

static float speed_scale(const ppm_motion_model_t *model, uint16_t pulse_us)
{
    const uint16_t minimum_us = model->config.speed_scale_min_us;
    const uint16_t maximum_us = model->config.speed_scale_max_us;
    float normalized = ((float)pulse_us - minimum_us) /
                       (float)(maximum_us - minimum_us);
    if (normalized < 0.0f) {
        normalized = 0.0f;
    } else if (normalized > 1.0f) {
        normalized = 1.0f;
    }
    return model->config.speed_scale_min +
           normalized * (model->config.speed_scale_max -
                         model->config.speed_scale_min);
}

static void reset_output(ppm_motion_output_t *output)
{
    memset(output, 0, sizeof(*output));
    output->action = PPM_MOTION_ACTION_NONE;
}

bool ppm_motion_model_init(ppm_motion_model_t *model,
                           const ppm_motion_model_config_t *config)
{
    if (!model || !config_valid(config)) {
        return false;
    }
    memset(model, 0, sizeof(*model));
    model->config = *config;
    model->phase = PPM_MOTION_PHASE_INACTIVE;
    model->stream_nonce = PPM_MOTION_STREAM_PREFIX;
    model->initialized = true;
    return true;
}

bool ppm_motion_model_step(ppm_motion_model_t *model,
                           const ppm_motion_input_t *input,
                           bool application_rc_active,
                           ppm_motion_output_t *output)
{
    if (!model || !model->initialized || !input || !output) {
        return false;
    }
    reset_output(output);
    const bool has_input = input_has_channels(model, input);
    const bool priority = input_is_priority(model, input);
    bool new_frame = false;
    if (input->signal_valid) {
        new_frame = !model->have_frame_sequence ||
                    input->valid_frame_sequence != model->last_frame_sequence;
        model->have_frame_sequence = true;
        model->last_frame_sequence = input->valid_frame_sequence;
    }

    if (!priority) {
        if (model->priority_active) {
            output->action = PPM_MOTION_ACTION_STOP;
        }
        model->priority_active = false;
        model->phase = PPM_MOTION_PHASE_INACTIVE;
        model->arm_pending_frames = 0U;
        model->stream_id = 0U;
        model->sequence = 0U;
        return true;
    }

    if (!model->priority_active) {
        /* A PPM takeover always retires a previous LAN stream before PPM can
         * arm. The next new, neutral PPM frame performs the source handshake. */
        model->priority_active = true;
        model->phase = PPM_MOTION_PHASE_WAIT_NEUTRAL;
        model->arm_pending_frames = 0U;
        output->action = PPM_MOTION_ACTION_STOP;
        return true;
    }

    if (model->phase == PPM_MOTION_PHASE_ARM_PENDING) {
        if (application_rc_active) {
            model->phase = PPM_MOTION_PHASE_ARMED;
            model->arm_pending_frames = 0U;
        } else if (new_frame) {
            model->arm_pending_frames++;
            if (model->arm_pending_frames >=
                PPM_MOTION_ARM_PENDING_MAX_NEW_FRAMES) {
                model->phase = PPM_MOTION_PHASE_WAIT_NEUTRAL;
                model->arm_pending_frames = 0U;
            }
        }
        return true;
    }

    if (model->phase == PPM_MOTION_PHASE_ARMED && !application_rc_active) {
        /* A higher-priority external STOP cannot be bypassed by held sticks. */
        model->phase = PPM_MOTION_PHASE_WAIT_NEUTRAL;
        model->arm_pending_frames = 0U;
        return true;
    }

    if (!new_frame || !has_input) {
        return true;
    }
    const uint16_t throttle = input->channels[model->config.throttle_channel - 1U];
    const uint16_t steering = input->channels[model->config.steering_channel - 1U];
    if (model->phase == PPM_MOTION_PHASE_WAIT_NEUTRAL) {
        if (!is_neutral(throttle,
                        model->config.throttle_center_us,
                        model->config.neutral_deadband_us) ||
            !is_neutral(steering,
                        model->config.steering_center_us,
                        model->config.neutral_deadband_us)) {
            return true;
        }
        model->stream_nonce++;
        if (model->stream_nonce == 0U) {
            model->stream_nonce++;
        }
        model->stream_id = model->stream_nonce;
        model->sequence = 1U;
        model->phase = PPM_MOTION_PHASE_ARM_PENDING;
        model->arm_pending_frames = 0U;
        output->action = PPM_MOTION_ACTION_ARM;
        output->stream_id = model->stream_id;
        output->sequence = model->sequence;
        return true;
    }

    if (model->phase == PPM_MOTION_PHASE_ARMED) {
        model->sequence++;
        output->action = PPM_MOTION_ACTION_COMMAND;
        output->stream_id = model->stream_id;
        output->sequence = model->sequence;
        output->normalized_vx = normalize_axis(
            throttle,
            model->config.throttle_min_us,
            model->config.throttle_center_us,
            model->config.throttle_max_us,
            model->config.neutral_deadband_us,
            model->config.throttle_sign);
        output->normalized_wz = normalize_axis(
            steering,
            model->config.steering_min_us,
            model->config.steering_center_us,
            model->config.steering_max_us,
            model->config.neutral_deadband_us,
            model->config.steering_sign);
        output->speed_scale = speed_scale(
            model,
            input->channels[model->config.speed_scale_channel - 1U]);
        output->deadman = output->normalized_vx != 0.0f ||
                          output->normalized_wz != 0.0f;
    }
    return true;
}

const char *ppm_motion_phase_name(ppm_motion_phase_t phase)
{
    switch (phase) {
    case PPM_MOTION_PHASE_INACTIVE:
        return "INACTIVE";
    case PPM_MOTION_PHASE_WAIT_NEUTRAL:
        return "WAIT_NEUTRAL";
    case PPM_MOTION_PHASE_ARM_PENDING:
        return "ARM_PENDING";
    case PPM_MOTION_PHASE_ARMED:
        return "ARMED";
    default:
        return "UNKNOWN";
    }
}
