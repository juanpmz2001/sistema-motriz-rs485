#ifndef MOTION_APPLICATION_MODEL_H
#define MOTION_APPLICATION_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "command_authority_model.h"
#include "motion_status_port.h"
#include "robot_kinematics.h"

#define MOTION_APPLICATION_MAX_ENDPOINTS MOTION_STATUS_MAX_ENDPOINTS
#define MOTION_APPLICATION_RETIRED_STREAMS 16U

typedef enum {
    MOTION_APPLICATION_EVENT_ARM = 0,
    MOTION_APPLICATION_EVENT_COMMAND,
    MOTION_APPLICATION_EVENT_DISARM,
    MOTION_APPLICATION_EVENT_STOP,
} motion_application_event_action_t;

typedef struct {
    motion_application_event_action_t action;
    command_authority_source_t source;
    uint64_t stream_id;
    uint64_t sequence;
    uint64_t received_at_ms;
    float vx_mps;
    float vy_mps;
    float wz_radps;
    bool deadman;
} motion_application_event_t;

typedef struct {
    size_t endpoint_id;
    const char *name;
    robot_kinematics_side_t side;
    double wheel_radius_m;
    double motor_to_wheel_ratio;
    int direction_sign;
    double max_abs_rpm;
} motion_application_endpoint_config_t;

typedef struct {
    uint32_t command_ttl_ms;
    command_authority_velocity_t velocity_limit;
    command_authority_velocity_t moving_epsilon;
    robot_kinematics_differential_config_t differential;
    size_t endpoint_count;
    motion_application_endpoint_config_t
        endpoints[MOTION_APPLICATION_MAX_ENDPOINTS];
} motion_application_model_config_t;

typedef enum {
    MOTION_APPLICATION_PLAN_NONE = 0,
    MOTION_APPLICATION_PLAN_APPLY,
    MOTION_APPLICATION_PLAN_STOP,
} motion_application_plan_action_t;

typedef struct {
    size_t endpoint_id;
    int16_t rpm;
} motion_application_target_t;

typedef struct {
    motion_application_plan_action_t action;
    uint64_t command_revision;
    size_t target_count;
    motion_application_target_t targets[MOTION_APPLICATION_MAX_ENDPOINTS];
    char detail[MOTION_STATUS_DETAIL_MAX];
} motion_application_plan_t;

typedef enum {
    MOTION_APPLICATION_RESULT_OK = 0,
    MOTION_APPLICATION_RESULT_INVALID_ARGUMENT,
    MOTION_APPLICATION_RESULT_INVALID_CONFIG,
    MOTION_APPLICATION_RESULT_NOT_ARMED,
    MOTION_APPLICATION_RESULT_ALREADY_ARMED,
    MOTION_APPLICATION_RESULT_STREAM_MISMATCH,
    MOTION_APPLICATION_RESULT_STREAM_RETIRED,
    MOTION_APPLICATION_RESULT_STREAM_HISTORY_FULL,
    MOTION_APPLICATION_RESULT_SEQUENCE_INVALID,
    MOTION_APPLICATION_RESULT_UNSAFE,
    MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED,
    MOTION_APPLICATION_RESULT_KINEMATICS_FAILED,
    MOTION_APPLICATION_RESULT_SOURCE_INVALID,
    MOTION_APPLICATION_RESULT_SOURCE_MISMATCH,
} motion_application_result_t;

typedef struct {
    motion_control_state_t state;
    command_authority_source_t source;
    uint32_t command_ttl_ms;
    bool deadman;
    uint64_t stream_id;
    uint64_t sequence;
    uint64_t last_received_ms;
    command_authority_velocity_t requested;
    size_t endpoint_count;
    motion_application_target_t targets[MOTION_APPLICATION_MAX_ENDPOINTS];
    char last_detail[MOTION_STATUS_DETAIL_MAX];
} motion_application_model_snapshot_t;

typedef struct {
    motion_application_model_config_t config;
    command_authority_model_t authority;
    motion_control_state_t state;
    command_authority_source_t active_source;
    uint64_t active_stream_id;
    uint64_t sequence;
    uint64_t last_received_ms;
    uint64_t last_dispatched_revision;
    uint64_t retired_streams[MOTION_APPLICATION_RETIRED_STREAMS];
    uint8_t retired_stream_count;
    bool deadman;
    command_authority_velocity_t requested;
    motion_application_target_t targets[MOTION_APPLICATION_MAX_ENDPOINTS];
    char last_detail[MOTION_STATUS_DETAIL_MAX];
    bool initialized;
} motion_application_model_t;

motion_application_result_t motion_application_model_init(
    motion_application_model_t *model,
    const motion_application_model_config_t *config);

motion_application_result_t motion_application_model_submit(
    motion_application_model_t *model,
    const motion_application_event_t *event,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan);

motion_application_result_t motion_application_model_tick(
    motion_application_model_t *model,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan);

void motion_application_model_record_actuation(
    motion_application_model_t *model,
    const motion_application_plan_t *plan,
    bool success);

bool motion_application_model_snapshot(
    const motion_application_model_t *model,
    motion_application_model_snapshot_t *snapshot);

const char *motion_application_result_name(motion_application_result_t result);

#endif
