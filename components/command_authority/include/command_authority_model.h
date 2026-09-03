#ifndef COMMAND_AUTHORITY_MODEL_H
#define COMMAND_AUTHORITY_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMMAND_AUTHORITY_RETIRED_STREAM_CAPACITY 16U

typedef enum {
    COMMAND_AUTHORITY_SOURCE_NONE = 0,
    COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
    COMMAND_AUTHORITY_SOURCE_LAN,
    COMMAND_AUTHORITY_SOURCE_RC,
    /* Build-selected direct browser control. It is still semantic motion
     * intent, never a transport path to a motor controller. */
    COMMAND_AUTHORITY_SOURCE_WEB_DIRECT,
    COMMAND_AUTHORITY_SOURCE_COUNT,
} command_authority_source_t;

typedef struct {
    float vx;
    float vy;
    float wz;
} command_authority_velocity_t;

typedef struct {
    uint64_t stream_id;
    uint64_t sequence;
    uint64_t received_at_ms;
    uint64_t ttl_ms;
    bool valid;
    bool deadman;
    command_authority_velocity_t body;
} command_authority_command_t;

typedef struct {
    command_authority_velocity_t velocity_limit;
    command_authority_velocity_t moving_epsilon;
    uint64_t max_ttl_ms;
} command_authority_config_t;

typedef enum {
    COMMAND_AUTHORITY_RESULT_OK = 0,
    COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT,
    COMMAND_AUTHORITY_RESULT_INVALID_CONFIG,
    COMMAND_AUTHORITY_RESULT_NOT_INITIALIZED,
    COMMAND_AUTHORITY_RESULT_INVALID_SOURCE,
    COMMAND_AUTHORITY_RESULT_STREAM_ID_ZERO,
    COMMAND_AUTHORITY_RESULT_STREAM_RETIRED,
    COMMAND_AUTHORITY_RESULT_STREAM_HISTORY_FULL,
    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING,
    COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION,
    COMMAND_AUTHORITY_RESULT_TIMESTAMP_IN_FUTURE,
    COMMAND_AUTHORITY_RESULT_TIMESTAMP_REGRESSION,
    COMMAND_AUTHORITY_RESULT_TTL_INVALID,
    COMMAND_AUTHORITY_RESULT_EXPIRED_ON_ARRIVAL,
    COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY,
    COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT,
    COMMAND_AUTHORITY_RESULT_COUNTER_EXHAUSTED,
} command_authority_result_t;

typedef enum {
    COMMAND_AUTHORITY_DECISION_INVALID = 0,
    COMMAND_AUTHORITY_DECISION_STOP,
    COMMAND_AUTHORITY_DECISION_APPLY,
    COMMAND_AUTHORITY_DECISION_FAULT_STOP,
} command_authority_decision_t;

typedef enum {
    COMMAND_AUTHORITY_REASON_NONE = 0,
    COMMAND_AUTHORITY_REASON_NO_FRESH_SOURCE,
    COMMAND_AUTHORITY_REASON_SOURCE_SWITCH,
    COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH,
    COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED,
    COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_INVALIDATED,
    COMMAND_AUTHORITY_REASON_COMMAND_APPLIED,
    COMMAND_AUTHORITY_REASON_EXPLICIT_STOP,
    COMMAND_AUTHORITY_REASON_CLOCK_REGRESSION,
    COMMAND_AUTHORITY_REASON_COUNTER_EXHAUSTED,
    COMMAND_AUTHORITY_REASON_INVALID_ARGUMENT,
    COMMAND_AUTHORITY_REASON_NOT_INITIALIZED,
} command_authority_reason_t;

typedef struct {
    command_authority_command_t command;
    uint64_t revision;
    uint64_t retired_stream_ids[COMMAND_AUTHORITY_RETIRED_STREAM_CAPACITY];
    uint8_t retired_stream_count;
    bool initialized;
} command_authority_mailbox_t;

typedef struct {
    command_authority_decision_t decision;
    command_authority_reason_t reason;
    command_authority_source_t previous_source;
    command_authority_source_t selected_source;
    bool authority_granted;
    uint64_t epoch;
    uint64_t publication_revision;
    uint64_t barrier_revision;
    uint64_t barrier_time_ms;
    uint64_t command_revision;
    command_authority_velocity_t output;
} command_authority_cycle_result_t;

typedef struct {
    command_authority_command_t command;
    uint64_t revision;
    uint64_t age_ms;
    bool initialized;
    bool fresh;
} command_authority_mailbox_snapshot_t;

typedef struct {
    command_authority_source_t selected_source;
    bool authority_granted;
    bool last_output_moving;
    bool counter_exhausted;
    uint64_t epoch;
    uint64_t publication_revision;
    uint64_t barrier_revision;
    uint64_t barrier_time_ms;
    uint64_t applied_revision;
    uint64_t last_now_ms;
    bool has_observed_time;
    command_authority_velocity_t output;
    command_authority_decision_t last_decision;
    command_authority_reason_t last_reason;
    command_authority_result_t last_publish_result;
    command_authority_mailbox_snapshot_t mailboxes[COMMAND_AUTHORITY_SOURCE_COUNT];
} command_authority_snapshot_t;

/*
 * The model owns no resources. Callers allocate it and must not modify its fields.
 * A velocity is moving when abs(axis) is greater than that axis' configured epsilon.
 * An epsilon of zero therefore means exact non-zero detection.
 *
 * Time is a non-wrapping uint64_t monotonic domain. Publish and arbitrate reject
 * now_ms below the last accepted/processed time. Explicit stop always executes and
 * never moves the observed time backward. A command is fresh while age_ms < ttl_ms.
 */
typedef struct {
    command_authority_config_t config;
    command_authority_mailbox_t mailboxes[COMMAND_AUTHORITY_SOURCE_COUNT];
    command_authority_source_t selected_source;
    bool authority_granted;
    bool last_output_moving;
    bool counter_exhausted;
    uint64_t epoch;
    uint64_t publication_revision;
    uint64_t barrier_revision;
    uint64_t barrier_time_ms;
    uint64_t applied_revision;
    uint64_t last_now_ms;
    bool has_observed_time;
    command_authority_velocity_t output;
    command_authority_decision_t last_decision;
    command_authority_reason_t last_reason;
    command_authority_result_t last_publish_result;
    bool initialized;
} command_authority_model_t;

command_authority_result_t command_authority_model_init(
    command_authority_model_t *model,
    const command_authority_config_t *config);

command_authority_result_t command_authority_model_publish(
    command_authority_model_t *model,
    command_authority_source_t source,
    const command_authority_command_t *command,
    uint64_t now_ms);

command_authority_decision_t command_authority_model_arbitrate(
    command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_cycle_result_t *result);

command_authority_decision_t command_authority_model_stop(
    command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_cycle_result_t *result);

/* Snapshot is observational: now_ms is checked but does not advance model time. */
command_authority_result_t command_authority_model_snapshot(
    const command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_snapshot_t *snapshot);

bool command_authority_velocity_is_moving(
    const command_authority_config_t *config,
    command_authority_velocity_t velocity);

const char *command_authority_source_name(command_authority_source_t source);
const char *command_authority_result_name(command_authority_result_t result);
const char *command_authority_decision_name(command_authority_decision_t decision);
const char *command_authority_reason_name(command_authority_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif
