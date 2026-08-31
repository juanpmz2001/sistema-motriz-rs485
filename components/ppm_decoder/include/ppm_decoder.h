#ifndef PPM_DECODER_H
#define PPM_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPM_MAX_CHANNELS 14

typedef struct ppm_decoder_t *ppm_decoder_handle_t;

typedef struct {
    int ppm_pin;
    /* A published frame has exactly this many valid channel pulses.  The
     * decoder deliberately does not truncate extra pulses: PPM has no
     * checksum, so a structural mismatch is not safe to reinterpret. */
    uint8_t channel_count;
    /* Legacy spelling retained for source compatibility. It must equal
     * channel_count; partial frames are never published. */
    uint8_t min_frame_channels;
    uint32_t sync_threshold_us;
    uint16_t min_pulse_us;
    uint16_t max_pulse_us;
    uint32_t stale_timeout_ms;
} ppm_decoder_config_t;

typedef struct {
    bool signal_valid;
    uint32_t last_frame_age_ms;
    uint32_t edges_seen;
    uint32_t sync_gaps;
    uint32_t valid_frames;
    uint32_t incomplete_frames;
    uint32_t invalid_pulses;
    uint32_t overflow_pulses;
    uint32_t rejected_frames;
    uint8_t channel_count;
    uint16_t channels[PPM_MAX_CHANNELS];
} ppm_decoder_status_t;

ppm_decoder_handle_t ppm_decoder_init(const ppm_decoder_config_t *config);
void ppm_decoder_deinit(ppm_decoder_handle_t handle);
bool ppm_decoder_get_channel(ppm_decoder_handle_t handle, uint8_t channel, uint16_t *value);
bool ppm_decoder_get_all_channels(ppm_decoder_handle_t handle,
                                  uint16_t *values,
                                  uint8_t channel_count);
bool ppm_decoder_get_status(ppm_decoder_handle_t handle, ppm_decoder_status_t *status);
bool ppm_decoder_is_signal_valid(ppm_decoder_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // PPM_DECODER_H
