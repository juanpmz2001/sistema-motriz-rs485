#ifndef PPM_MOTION_MODEL_H
#define PPM_MOTION_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define PPM_MOTION_MODEL_MAX_CHANNELS 14U

typedef enum {
    PPM_MOTION_ACTION_NONE = 0,
    PPM_MOTION_ACTION_STOP,
    PPM_MOTION_ACTION_ARM,
    PPM_MOTION_ACTION_COMMAND,
} ppm_motion_action_t;

typedef enum {
    PPM_MOTION_PHASE_INACTIVE = 0,
    PPM_MOTION_PHASE_WAIT_NEUTRAL,
    PPM_MOTION_PHASE_ARM_PENDING,
    PPM_MOTION_PHASE_ARMED,
} ppm_motion_phase_t;

typedef struct {
    uint8_t throttle_channel;
    uint8_t steering_channel;
    uint8_t enable_channel;
    uint16_t enable_active_max_us;
    uint16_t neutral_deadband_us;
    uint16_t valid_min_us;
    uint16_t valid_max_us;
    uint16_t throttle_min_us;
    uint16_t throttle_center_us;
    uint16_t throttle_max_us;
    uint16_t steering_min_us;
    uint16_t steering_center_us;
    uint16_t steering_max_us;
    int8_t throttle_sign;
    int8_t steering_sign;
    uint8_t speed_scale_channel;
    uint16_t speed_scale_min_us;
    uint16_t speed_scale_max_us;
    float speed_scale_min;
    float speed_scale_max;
} ppm_motion_model_config_t;

typedef struct {
    bool signal_valid;
    uint32_t valid_frame_sequence;
    uint8_t channel_count;
    uint16_t channels[PPM_MOTION_MODEL_MAX_CHANNELS];
} ppm_motion_input_t;

typedef struct {
    ppm_motion_action_t action;
    uint64_t stream_id;
    uint64_t sequence;
    float normalized_vx;
    float normalized_wz;
    float speed_scale;
    bool deadman;
} ppm_motion_output_t;

typedef struct {
    ppm_motion_model_config_t config;
    ppm_motion_phase_t phase;
    bool priority_active;
    bool have_frame_sequence;
    uint32_t last_frame_sequence;
    uint8_t arm_pending_frames;
    uint64_t stream_nonce;
    uint64_t stream_id;
    uint64_t sequence;
    bool initialized;
} ppm_motion_model_t;

bool ppm_motion_model_init(ppm_motion_model_t *model,
                           const ppm_motion_model_config_t *config);

/* Each call is observational. Only a new valid receiver frame can produce an
 * ARM or COMMAND. Entering/leaving PPM priority produces one terminal STOP.
 * application_rc_active is true only after motion_application accepted the RC
 * source; an external stop forces a fresh neutral-arm handshake. */
bool ppm_motion_model_step(ppm_motion_model_t *model,
                           const ppm_motion_input_t *input,
                           bool application_rc_active,
                           ppm_motion_output_t *output);

const char *ppm_motion_phase_name(ppm_motion_phase_t phase);

#endif
