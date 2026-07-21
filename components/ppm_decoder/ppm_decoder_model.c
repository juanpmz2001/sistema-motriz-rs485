#include "ppm_decoder_model.h"

#include <limits.h>
#include <string.h>

bool ppm_decoder_model_init(ppm_decoder_model_t *model,
                            const ppm_decoder_model_config_t *config)
{
    if (!model || !config || config->channel_count == 0 ||
        config->channel_count > PPM_MAX_CHANNELS ||
        config->min_frame_channels == 0 ||
        config->min_frame_channels > config->channel_count ||
        config->sync_threshold_us == 0 ||
        config->min_pulse_us == 0 ||
        config->max_pulse_us < config->min_pulse_us ||
        config->sync_threshold_us <= config->max_pulse_us) {
        return false;
    }

    memset(model, 0, sizeof(*model));
    model->config = *config;
    model->initialized = true;
    return true;
}

bool ppm_decoder_model_feed_rising_edge(ppm_decoder_model_t *model, uint32_t now_us)
{
    if (!model || !model->initialized) {
        return false;
    }

    model->edges_seen++;
    if (!model->has_last_edge) {
        model->has_last_edge = true;
        model->last_edge_us = now_us;
        return false;
    }

    const uint32_t pulse_us = now_us - model->last_edge_us;
    model->last_edge_us = now_us;

    if (pulse_us >= model->config.sync_threshold_us) {
        model->sync_gaps++;
        bool published = false;
        if (model->working_channel_count >= model->config.min_frame_channels) {
            const uint8_t count = model->working_channel_count > model->config.channel_count
                                      ? model->config.channel_count
                                      : model->working_channel_count;
            memcpy(model->channels,
                   model->working_channels,
                   (size_t)count * sizeof(model->channels[0]));
            model->published_channel_count = count;
            model->last_valid_frame_us = now_us;
            model->valid_frames++;
            published = true;
        } else if (model->working_channel_count > 0) {
            model->incomplete_frames++;
        }
        model->working_channel_count = 0;
        return published;
    }

    if (pulse_us < model->config.min_pulse_us || pulse_us > model->config.max_pulse_us) {
        model->invalid_pulses++;
        return false;
    }

    if (model->working_channel_count < model->config.channel_count) {
        model->working_channels[model->working_channel_count] = (uint16_t)pulse_us;
        model->working_channel_count++;
    } else {
        model->overflow_pulses++;
        if (model->working_channel_count < UINT8_MAX) {
            model->working_channel_count++;
        }
    }
    return false;
}

bool ppm_decoder_model_snapshot(const ppm_decoder_model_t *model,
                                uint32_t now_us,
                                ppm_decoder_model_status_t *status)
{
    if (!model || !model->initialized || !status) {
        return false;
    }

    memset(status, 0, sizeof(*status));
    status->last_frame_age_us = model->last_valid_frame_us == 0
                                  ? UINT32_MAX
                                  : now_us - model->last_valid_frame_us;
    status->edges_seen = model->edges_seen;
    status->sync_gaps = model->sync_gaps;
    status->valid_frames = model->valid_frames;
    status->incomplete_frames = model->incomplete_frames;
    status->invalid_pulses = model->invalid_pulses;
    status->overflow_pulses = model->overflow_pulses;
    status->channel_count = model->published_channel_count;
    memcpy(status->channels, model->channels, sizeof(status->channels));
    return true;
}
