#ifndef WEB_DIRECT_CONTROL_MODEL_H
#define WEB_DIRECT_CONTROL_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_DIRECT_CONTROL_DEFAULT_TTL_MS 300U
#define WEB_DIRECT_CONTROL_DEFAULT_DEADZONE 0.10f

typedef enum {
    WEB_DIRECT_MODEL_DISARMED = 0,
    WEB_DIRECT_MODEL_ARMED,
    WEB_DIRECT_MODEL_ACTIVE,
    WEB_DIRECT_MODEL_EXPIRED,
    WEB_DIRECT_MODEL_FAULT,
} web_direct_control_model_state_t;

typedef enum {
    WEB_DIRECT_MODEL_ACCEPTED = 0,
    WEB_DIRECT_MODEL_REJECTED_SESSION,
    WEB_DIRECT_MODEL_REJECTED_NOT_ARMED,
    WEB_DIRECT_MODEL_REJECTED_ARGUMENT,
} web_direct_control_model_result_t;

typedef struct {
    bool accepted;
    bool renews_lease;
    bool zero_intent;
    float forward;
    float turn;
    bool deadman;
} web_direct_control_command_t;

typedef struct {
    uint32_t ttl_ms;
    float deadzone;
} web_direct_control_model_config_t;

typedef struct {
    web_direct_control_model_config_t config;
    bool session_claimed;
    uint64_t session_id;
    bool armed;
    bool lease_seen;
    uint64_t last_valid_ms;
    web_direct_control_model_state_t state;
} web_direct_control_model_t;

bool web_direct_control_model_init(web_direct_control_model_t *model,
                                   const web_direct_control_model_config_t *config);
bool web_direct_control_model_claim_session(web_direct_control_model_t *model,
                                            uint64_t session_id);
void web_direct_control_model_release_session(web_direct_control_model_t *model,
                                              uint64_t session_id);
/* Releases a disconnected owner only when no armed lease remains.  An armed
 * owner deliberately stays claimed until the normal TTL expiry path withdraws
 * its motion, so a reconnect cannot take over a live stream early. */
bool web_direct_control_model_release_disarmed_session(web_direct_control_model_t *model,
                                                        uint64_t session_id);
web_direct_control_model_result_t web_direct_control_model_arm(
    web_direct_control_model_t *model, uint64_t session_id, uint64_t now_ms);
web_direct_control_model_result_t web_direct_control_model_disarm(
    web_direct_control_model_t *model, uint64_t session_id);
web_direct_control_model_result_t web_direct_control_model_command(
    web_direct_control_model_t *model,
    uint64_t session_id,
    uint64_t now_ms,
    float forward,
    float turn,
    bool deadman,
    web_direct_control_command_t *out_command);
bool web_direct_control_model_expire(web_direct_control_model_t *model,
                                     uint64_t now_ms);
void web_direct_control_model_fault(web_direct_control_model_t *model);
void web_direct_control_model_reject_command(web_direct_control_model_t *model);
const char *web_direct_control_model_state_name(web_direct_control_model_state_t state);

#ifdef __cplusplus
}
#endif

#endif
