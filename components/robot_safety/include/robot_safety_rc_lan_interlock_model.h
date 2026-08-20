#ifndef ROBOT_SAFETY_RC_LAN_INTERLOCK_MODEL_H
#define ROBOT_SAFETY_RC_LAN_INTERLOCK_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOT_SAFETY_RC_LAN_DETAIL_MAX 32U

typedef enum {
    ROBOT_SAFETY_RC_LAN_DISABLED = 0,
    ROBOT_SAFETY_RC_LAN_NO_SIGNAL,
    ROBOT_SAFETY_RC_LAN_FAILSAFE,
    ROBOT_SAFETY_RC_LAN_PPM_PRIORITY,
    ROBOT_SAFETY_RC_LAN_PPM_LOST,
    ROBOT_SAFETY_RC_LAN_CHANNEL_UNAVAILABLE,
} robot_safety_rc_lan_interlock_state_t;

typedef struct {
    bool enabled;
    /* One-based physical receiver channel. */
    uint8_t channel;
    /* A valid pulse at or below this value means PPM has priority. */
    uint16_t active_max_us;
} robot_safety_rc_lan_interlock_config_t;

typedef struct {
    bool receiver_available;
    bool signal_valid;
    uint8_t channel_count;
    const uint16_t *channels;
} robot_safety_rc_lan_observation_t;

typedef struct {
    robot_safety_rc_lan_interlock_config_t config;
    robot_safety_rc_lan_interlock_state_t state;
    uint32_t priority_epoch;
    bool initialized;
} robot_safety_rc_lan_interlock_model_t;

typedef struct {
    robot_safety_rc_lan_interlock_state_t state;
    bool lan_allowed;
    uint16_t channel_us;
    uint32_t priority_epoch;
    char detail[ROBOT_SAFETY_RC_LAN_DETAIL_MAX];
} robot_safety_rc_lan_interlock_snapshot_t;

bool robot_safety_rc_lan_interlock_model_init(
    robot_safety_rc_lan_interlock_model_t *model,
    const robot_safety_rc_lan_interlock_config_t *config);

bool robot_safety_rc_lan_interlock_model_update(
    robot_safety_rc_lan_interlock_model_t *model,
    const robot_safety_rc_lan_observation_t *observation,
    robot_safety_rc_lan_interlock_snapshot_t *snapshot);

const char *robot_safety_rc_lan_interlock_state_name(
    robot_safety_rc_lan_interlock_state_t state);

#ifdef __cplusplus
}
#endif

#endif // ROBOT_SAFETY_RC_LAN_INTERLOCK_MODEL_H
