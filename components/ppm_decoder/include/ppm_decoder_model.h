#ifndef PPM_DECODER_MODEL_H
#define PPM_DECODER_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include "ppm_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t channel_count;
    uint8_t min_frame_channels;
    uint32_t sync_threshold_us;
    uint16_t min_pulse_us;
    uint16_t max_pulse_us;
} ppm_decoder_model_config_t;

typedef struct {
    ppm_decoder_model_config_t config;
    bool initialized;
    bool has_last_edge;
    uint32_t last_edge_us;
    uint32_t last_valid_frame_us;
    uint16_t working_channels[PPM_MAX_CHANNELS];
    uint16_t channels[PPM_MAX_CHANNELS];
    uint8_t working_channel_count;
    uint8_t published_channel_count;
    uint32_t edges_seen;
    uint32_t sync_gaps;
    uint32_t valid_frames;
    uint32_t incomplete_frames;
    uint32_t invalid_pulses;
    uint32_t overflow_pulses;
    uint32_t rejected_frames;
} ppm_decoder_model_t;

typedef struct {
    uint32_t last_frame_age_us;
    uint32_t edges_seen;
    uint32_t sync_gaps;
    uint32_t valid_frames;
    uint32_t incomplete_frames;
    uint32_t invalid_pulses;
    uint32_t overflow_pulses;
    uint32_t rejected_frames;
    uint8_t channel_count;
    uint16_t channels[PPM_MAX_CHANNELS];
} ppm_decoder_model_status_t;

bool ppm_decoder_model_init(ppm_decoder_model_t *model,
                            const ppm_decoder_model_config_t *config);
bool ppm_decoder_model_feed_rising_edge(ppm_decoder_model_t *model, uint32_t now_us);
bool ppm_decoder_model_snapshot(const ppm_decoder_model_t *model,
                                uint32_t now_us,
                                ppm_decoder_model_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // PPM_DECODER_MODEL_H
