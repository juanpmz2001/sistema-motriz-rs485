#ifndef CONTROL_LAN_H
#define CONTROL_LAN_H

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_LAN_DEFAULT_PORT 32322U
#define CONTROL_LAN_DEFAULT_TASK_PRIORITY 5U
#define CONTROL_LAN_TASK_STACK_SIZE 6144U
#define CONTROL_LAN_PACKET_MAX 512U
#define CONTROL_LAN_REQUEST_ID_MAX 65U
#define CONTROL_LAN_STREAM_ID_MAX 65U
#define CONTROL_LAN_ACTION_MAX 16U
#define CONTROL_LAN_DETAIL_MAX 64U
#define CONTROL_LAN_SENDER_MAX 24U
#define CONTROL_LAN_MAX_EXACT_SEQUENCE UINT64_C(9007199254740991)
#define CONTROL_LAN_REQUEST_TYPE "botfarms_control_command"
#define CONTROL_LAN_RESPONSE_TYPE "botfarms_control_response"
#define CONTROL_LAN_PROTOCOL_VERSION "1.0"

typedef struct control_lan_t *control_lan_handle_t;

typedef enum {
    CONTROL_LAN_ACTION_ARM = 0,
    CONTROL_LAN_ACTION_COMMAND,
    CONTROL_LAN_ACTION_DISARM,
    CONTROL_LAN_ACTION_STOP,
} control_lan_action_t;

typedef struct {
    control_lan_action_t action;
    uint64_t stream_id_hash;
    uint64_t sequence;
    /* Monotonic microseconds since boot, sourced from esp_timer_get_time(). */
    uint64_t timestamp_us;
    float vx_mps;
    float vy_mps;
    float wz_radps;
    bool deadman;
} control_lan_event_t;

typedef struct {
    bool accepted;
    char detail[CONTROL_LAN_DETAIL_MAX];
} control_lan_callback_result_t;

/*
 * The callback runs in the control_lan task and must not block or call the
 * control_lan lifecycle API. An arm for a different active stream is preceded
 * synchronously by a STOP event; rejecting that STOP rejects the arm. Both the
 * event and callback result are values; detail must be NUL-terminated.
 */
typedef control_lan_callback_result_t (*control_lan_event_callback_t)(control_lan_event_t event,
                                                                      void *context);

/* The transport asks this source-policy boundary whether a fresh LAN session
 * may be accepted.  revocation_epoch is monotonic: when it changes, any
 * existing stream is stopped and retired before another packet is processed.
 * This keeps source selection out of the UDP grammar and out of the motion
 * application itself. */
typedef struct {
    bool lan_allowed;
    uint32_t revocation_epoch;
    char detail[CONTROL_LAN_DETAIL_MAX];
} control_lan_authority_status_t;

typedef control_lan_authority_status_t (*control_lan_authority_status_callback_t)(
    void *context);

typedef struct {
    config_manager_handle_t config_manager;
    uint16_t listen_port;
    uint32_t task_priority;
    float max_abs_vx_mps;
    float max_abs_vy_mps;
    float max_abs_wz_radps;
    control_lan_event_callback_t event_callback;
    void *callback_context;
    control_lan_authority_status_callback_t authority_status_callback;
    void *authority_context;
} control_lan_config_t;

typedef struct {
    bool task_running;
    uint16_t listen_port;
    uint32_t packets_seen;
    uint32_t packets_accepted;
    uint32_t packets_rejected;
    uint32_t sequence_gaps;
    uint32_t duplicate_or_out_of_order;
    uint32_t invalid_schema;
    uint32_t auth_failures;
    uint32_t last_valid_command_age_ms;
    char last_sender[CONTROL_LAN_SENDER_MAX];
    char last_action[CONTROL_LAN_ACTION_MAX];
    char last_detail[CONTROL_LAN_DETAIL_MAX];
    bool lan_allowed;
    uint32_t revocation_epoch;
    char authority_detail[CONTROL_LAN_DETAIL_MAX];
} control_lan_status_t;

/*
 * Hashes the decoded stream_id bytes with FNV-1a 64-bit. A natural zero hash
 * is remapped to 1, so valid events always carry a non-zero hash. Collisions
 * are intentionally left for the upper control layer to handle.
 */
uint64_t control_lan_hash_stream_id(const char *stream_id);

esp_err_t control_lan_init(const control_lan_config_t *config, control_lan_handle_t *out_handle);
esp_err_t control_lan_start(control_lan_handle_t handle);
/* The owner must quiesce all external API callers before deinit returns. */
void control_lan_deinit(control_lan_handle_t handle);
esp_err_t control_lan_get_status(control_lan_handle_t handle, control_lan_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_LAN_H
