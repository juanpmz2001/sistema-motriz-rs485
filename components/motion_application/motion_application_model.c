#include "motion_application_model.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static command_authority_velocity_t zero_velocity(void)
{
    const command_authority_velocity_t zero = {0.0f, 0.0f, 0.0f};
    return zero;
}

static void copy_detail(char *destination, const char *detail)
{
    snprintf(destination, MOTION_STATUS_DETAIL_MAX, "%s",
             detail ? detail : "UNKNOWN");
}

static void reset_plan(motion_application_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->action = MOTION_APPLICATION_PLAN_NONE;
    copy_detail(plan->detail, "NO_CHANGE");
}

static void stop_plan(motion_application_plan_t *plan, const char *detail)
{
    reset_plan(plan);
    plan->action = MOTION_APPLICATION_PLAN_STOP;
    copy_detail(plan->detail, detail);
}

static bool stream_is_retired(const motion_application_model_t *model,
                              uint64_t stream_id)
{
    for (uint8_t index = 0U; index < model->retired_stream_count; ++index) {
        if (model->retired_streams[index] == stream_id) {
            return true;
        }
    }
    return false;
}

static bool retire_active_stream(motion_application_model_t *model)
{
    if (model->active_stream_id == 0U) {
        return true;
    }
    if (!stream_is_retired(model, model->active_stream_id)) {
        if (model->retired_stream_count == MOTION_APPLICATION_RETIRED_STREAMS) {
            memmove(&model->retired_streams[0],
                    &model->retired_streams[1],
                    sizeof(model->retired_streams[0]) *
                        (MOTION_APPLICATION_RETIRED_STREAMS - 1U));
            model->retired_stream_count--;
        }
        model->retired_streams[model->retired_stream_count++] =
            model->active_stream_id;
    }
    model->active_stream_id = 0U;
    model->active_source = COMMAND_AUTHORITY_SOURCE_NONE;
    model->sequence = 0U;
    model->deadman = false;
    model->hold_zero_when_deadman_released = false;
    model->requested = zero_velocity();
    return true;
}

static bool event_source_supported(command_authority_source_t source)
{
    return source == COMMAND_AUTHORITY_SOURCE_LAN ||
           source == COMMAND_AUTHORITY_SOURCE_RC ||
           source == COMMAND_AUTHORITY_SOURCE_WEB_DIRECT;
}

static bool config_valid(const motion_application_model_config_t *config)
{
    if (!config || config->command_ttl_ms == 0U ||
        config->endpoint_count < 2U ||
        config->endpoint_count > MOTION_APPLICATION_MAX_ENDPOINTS ||
        !isfinite(config->velocity_limit.vx) ||
        !isfinite(config->velocity_limit.vy) ||
        !isfinite(config->velocity_limit.wz) ||
        config->velocity_limit.vx <= 0.0f ||
        config->velocity_limit.vy <= 0.0f ||
        config->velocity_limit.wz <= 0.0f) {
        return false;
    }
    for (size_t index = 0U; index < config->endpoint_count; ++index) {
        const motion_application_endpoint_config_t *endpoint =
            &config->endpoints[index];
        if (endpoint->endpoint_id == 0U || !endpoint->name ||
            endpoint->name[0] == '\0') {
            return false;
        }
    }
    robot_kinematics_motor_config_t motors[MOTION_APPLICATION_MAX_ENDPOINTS];
    for (size_t index = 0U; index < config->endpoint_count; ++index) {
        const motion_application_endpoint_config_t *source =
            &config->endpoints[index];
        motors[index] = (robot_kinematics_motor_config_t){
            .actuator_id = source->endpoint_id,
            .side = source->side,
            .wheel_radius_m = source->wheel_radius_m,
            .motor_to_wheel_ratio = source->motor_to_wheel_ratio,
            .direction_sign = source->direction_sign,
            .max_abs_rpm = source->max_abs_rpm,
        };
    }
    const robot_kinematics_body_velocity_t zero = {0.0, 0.0, 0.0};
    robot_kinematics_motor_command_t commands[MOTION_APPLICATION_MAX_ENDPOINTS];
    robot_kinematics_report_t report;
    return robot_kinematics_differential_inverse(
               &config->differential,
               motors,
               config->endpoint_count,
               &zero,
               commands,
               MOTION_APPLICATION_MAX_ENDPOINTS,
               &report) == ROBOT_KINEMATICS_OK;
}

static bool reset_authority(motion_application_model_t *model)
{
    const command_authority_config_t authority_config = {
        .velocity_limit = model->config.velocity_limit,
        .moving_epsilon = model->config.moving_epsilon,
        .max_ttl_ms = model->config.command_ttl_ms,
    };
    return command_authority_model_init(&model->authority,
                                        &authority_config) ==
           COMMAND_AUTHORITY_RESULT_OK;
}

motion_application_result_t motion_application_model_init(
    motion_application_model_t *model,
    const motion_application_model_config_t *config)
{
    if (!model || !config) {
        return MOTION_APPLICATION_RESULT_INVALID_ARGUMENT;
    }
    memset(model, 0, sizeof(*model));
    if (!config_valid(config)) {
        return MOTION_APPLICATION_RESULT_INVALID_CONFIG;
    }
    model->config = *config;
    if (!reset_authority(model)) {
        return MOTION_APPLICATION_RESULT_INVALID_CONFIG;
    }
    model->state = MOTION_CONTROL_DISARMED;
    model->requested = zero_velocity();
    for (size_t index = 0U; index < config->endpoint_count; ++index) {
        model->targets[index].endpoint_id = config->endpoints[index].endpoint_id;
    }
    copy_detail(model->last_detail, "READY");
    model->initialized = true;
    return MOTION_APPLICATION_RESULT_OK;
}

static motion_application_result_t fault_stop(
    motion_application_model_t *model,
    uint64_t now_ms,
    const char *detail,
    motion_application_plan_t *plan,
    motion_application_result_t result)
{
    (void)command_authority_model_stop(&model->authority, now_ms, NULL);
    if (!retire_active_stream(model)) {
        result = MOTION_APPLICATION_RESULT_STREAM_HISTORY_FULL;
    }
    if (!reset_authority(model)) {
        result = MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }
    model->state = MOTION_CONTROL_FAULT;
    copy_detail(model->last_detail, detail);
    stop_plan(plan, detail);
    return result;
}

static motion_application_result_t convert_authority_decision(
    motion_application_model_t *model,
    const command_authority_cycle_result_t *cycle,
    uint64_t now_ms,
    motion_application_plan_t *plan)
{
    if (cycle->decision == COMMAND_AUTHORITY_DECISION_FAULT_STOP ||
        cycle->reason == COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED ||
        cycle->reason == COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_INVALIDATED ||
        cycle->reason == COMMAND_AUTHORITY_REASON_NO_FRESH_SOURCE) {
        (void)command_authority_model_stop(&model->authority, now_ms, NULL);
        bool retired = retire_active_stream(model);
        bool authority_reset = reset_authority(model);
        model->state = retired && authority_reset ? MOTION_CONTROL_EXPIRED
                                                   : MOTION_CONTROL_FAULT;
        copy_detail(model->last_detail,
                    retired ? "SOURCE_EXPIRED" : "STREAM_HISTORY_FULL");
        stop_plan(plan, model->last_detail);
        return retired && authority_reset
                   ? MOTION_APPLICATION_RESULT_OK
                   : MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }

    if (cycle->decision != COMMAND_AUTHORITY_DECISION_APPLY) {
        if (cycle->reason ==
            COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH) {
            copy_detail(model->last_detail, "SOURCE_SWITCH_BARRIER");
            return MOTION_APPLICATION_RESULT_OK;
        }
        copy_detail(model->last_detail,
                    command_authority_reason_name(cycle->reason));
        return MOTION_APPLICATION_RESULT_OK;
    }

    if (cycle->command_revision == model->last_dispatched_revision) {
        return MOTION_APPLICATION_RESULT_OK;
    }
    model->last_dispatched_revision = cycle->command_revision;
    model->requested = cycle->output;
    /* A live deadman with zero velocity is an APPLY plan: endpoints stay
     * enabled at zero. STOP is reserved for authority/safety/fault loss. */
    if (!model->deadman && !model->hold_zero_when_deadman_released) {
        model->state = MOTION_CONTROL_ARMED;
        copy_detail(model->last_detail, "DEADMAN_RELEASED");
        stop_plan(plan, model->last_detail);
        plan->command_revision = cycle->command_revision;
        return MOTION_APPLICATION_RESULT_OK;
    }

    robot_kinematics_motor_config_t motors[MOTION_APPLICATION_MAX_ENDPOINTS];
    for (size_t index = 0U; index < model->config.endpoint_count; ++index) {
        const motion_application_endpoint_config_t *source =
            &model->config.endpoints[index];
        motors[index] = (robot_kinematics_motor_config_t){
            .actuator_id = source->endpoint_id,
            .side = source->side,
            .wheel_radius_m = source->wheel_radius_m,
            .motor_to_wheel_ratio = source->motor_to_wheel_ratio,
            .direction_sign = source->direction_sign,
            .max_abs_rpm = source->max_abs_rpm,
        };
    }
    const robot_kinematics_body_velocity_t velocity = {
        .vx_mps = cycle->output.vx,
        .vy_mps = cycle->output.vy,
        .wz_radps = cycle->output.wz,
    };
    robot_kinematics_motor_command_t commands[MOTION_APPLICATION_MAX_ENDPOINTS];
    robot_kinematics_report_t report;
    robot_kinematics_error_t error = robot_kinematics_differential_inverse(
        &model->config.differential,
        motors,
        model->config.endpoint_count,
        &velocity,
        commands,
        MOTION_APPLICATION_MAX_ENDPOINTS,
        &report);
    if (error != ROBOT_KINEMATICS_OK) {
        return fault_stop(model,
                          now_ms,
                          robot_kinematics_error_name(error),
                          plan,
                          MOTION_APPLICATION_RESULT_KINEMATICS_FAILED);
    }

    reset_plan(plan);
    plan->action = MOTION_APPLICATION_PLAN_APPLY;
    plan->command_revision = cycle->command_revision;
    plan->target_count = model->config.endpoint_count;
    for (size_t index = 0U; index < plan->target_count; ++index) {
        long rounded = lround(commands[index].motor_rpm);
        if (rounded < INT16_MIN || rounded > INT16_MAX) {
            return fault_stop(model,
                              now_ms,
                              "RPM_NUMERIC_OVERFLOW",
                              plan,
                              MOTION_APPLICATION_RESULT_KINEMATICS_FAILED);
        }
        plan->targets[index].endpoint_id = commands[index].actuator_id;
        plan->targets[index].rpm = (int16_t)rounded;
    }
    copy_detail(plan->detail, report.saturated ? "APPLY_SATURATED"
                                               : "APPLY");
    copy_detail(model->last_detail, plan->detail);
    return MOTION_APPLICATION_RESULT_OK;
}

static motion_application_result_t handle_arm(
    motion_application_model_t *model,
    const motion_application_event_t *event,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan)
{
    if (!event_source_supported(event->source)) {
        return MOTION_APPLICATION_RESULT_SOURCE_INVALID;
    }
    if (!safety_gate_open) {
        copy_detail(model->last_detail, "SAFETY_GATE_CLOSED");
        return MOTION_APPLICATION_RESULT_UNSAFE;
    }
    if (event->stream_id == 0U || event->sequence == 0U) {
        return MOTION_APPLICATION_RESULT_SEQUENCE_INVALID;
    }
    if (stream_is_retired(model, event->stream_id)) {
        return MOTION_APPLICATION_RESULT_STREAM_RETIRED;
    }
    if (model->active_stream_id != 0U) {
        return model->active_stream_id == event->stream_id
                   ? MOTION_APPLICATION_RESULT_ALREADY_ARMED
                   : MOTION_APPLICATION_RESULT_STREAM_MISMATCH;
    }

    if (!reset_authority(model)) {
        return MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }
    const command_authority_command_t command = {
        .stream_id = event->stream_id,
        .sequence = event->sequence,
        .received_at_ms = event->received_at_ms,
        .ttl_ms = model->config.command_ttl_ms,
        .valid = true,
        .deadman = false,
        .body = {0.0f, 0.0f, 0.0f},
    };
    if (command_authority_model_publish(&model->authority,
                                        event->source,
                                        &command,
                                        now_ms) != COMMAND_AUTHORITY_RESULT_OK) {
        return MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }
    command_authority_cycle_result_t cycle;
    (void)command_authority_model_arbitrate(&model->authority, now_ms, &cycle);
    model->active_stream_id = event->stream_id;
    model->active_source = event->source;
    model->sequence = event->sequence;
    model->last_received_ms = event->received_at_ms;
    model->deadman = false;
    model->hold_zero_when_deadman_released = false;
    model->requested = zero_velocity();
    model->state = MOTION_CONTROL_ARMED;
    model->last_dispatched_revision = 0U;
    copy_detail(model->last_detail, "ARMED");
    stop_plan(plan, "ARM_BARRIER_STOP");
    return MOTION_APPLICATION_RESULT_OK;
}

static motion_application_result_t handle_command(
    motion_application_model_t *model,
    const motion_application_event_t *event,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan)
{
    if (model->state != MOTION_CONTROL_ARMED &&
        model->state != MOTION_CONTROL_ACTIVE) {
        return MOTION_APPLICATION_RESULT_NOT_ARMED;
    }
    if (event->stream_id != model->active_stream_id) {
        return MOTION_APPLICATION_RESULT_STREAM_MISMATCH;
    }
    if (!event_source_supported(event->source)) {
        return MOTION_APPLICATION_RESULT_SOURCE_INVALID;
    }
    if (event->hold_zero_when_deadman_released &&
        (event->source != COMMAND_AUTHORITY_SOURCE_WEB_DIRECT || event->deadman ||
         event->vx_mps != 0.0f || event->vy_mps != 0.0f || event->wz_radps != 0.0f)) {
        return MOTION_APPLICATION_RESULT_INVALID_ARGUMENT;
    }
    if (event->source != model->active_source) {
        return MOTION_APPLICATION_RESULT_SOURCE_MISMATCH;
    }
    if (!safety_gate_open) {
        return fault_stop(model,
                          now_ms,
                          "SAFETY_GATE_CLOSED",
                          plan,
                          MOTION_APPLICATION_RESULT_UNSAFE);
    }
    const command_authority_command_t command = {
        .stream_id = event->stream_id,
        .sequence = event->sequence,
        .received_at_ms = event->received_at_ms,
        .ttl_ms = model->config.command_ttl_ms,
        .valid = true,
        .deadman = event->deadman,
        .body = {event->vx_mps, event->vy_mps, event->wz_radps},
    };
    command_authority_result_t published = command_authority_model_publish(
        &model->authority,
        event->source,
        &command,
        now_ms);
    if (published != COMMAND_AUTHORITY_RESULT_OK) {
        copy_detail(model->last_detail,
                    command_authority_result_name(published));
        if (!event->deadman) {
            stop_plan(plan, model->last_detail);
        }
        return MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }
    model->sequence = event->sequence;
    model->last_received_ms = event->received_at_ms;
    model->deadman = event->deadman;
    model->hold_zero_when_deadman_released = event->hold_zero_when_deadman_released;
    command_authority_cycle_result_t cycle;
    (void)command_authority_model_arbitrate(&model->authority, now_ms, &cycle);
    return convert_authority_decision(model, &cycle, now_ms, plan);
}

motion_application_result_t motion_application_model_submit(
    motion_application_model_t *model,
    const motion_application_event_t *event,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan)
{
    if (!model || !model->initialized || !event || !plan ||
        event->received_at_ms > now_ms) {
        return MOTION_APPLICATION_RESULT_INVALID_ARGUMENT;
    }
    reset_plan(plan);
    switch (event->action) {
    case MOTION_APPLICATION_EVENT_ARM:
        return handle_arm(model, event, safety_gate_open, now_ms, plan);
    case MOTION_APPLICATION_EVENT_COMMAND:
        return handle_command(model, event, safety_gate_open, now_ms, plan);
    case MOTION_APPLICATION_EVENT_DISARM:
    case MOTION_APPLICATION_EVENT_STOP: {
        (void)command_authority_model_stop(&model->authority, now_ms, NULL);
        bool retired = retire_active_stream(model);
        bool authority_reset = reset_authority(model);
        model->state = retired && authority_reset ? MOTION_CONTROL_DISARMED
                                                   : MOTION_CONTROL_FAULT;
        copy_detail(model->last_detail,
                    event->action == MOTION_APPLICATION_EVENT_STOP
                        ? "EXPLICIT_STOP"
                        : "DISARMED");
        stop_plan(plan, model->last_detail);
        return retired && authority_reset
                   ? MOTION_APPLICATION_RESULT_OK
                   : MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED;
    }
    default:
        return MOTION_APPLICATION_RESULT_INVALID_ARGUMENT;
    }
}

motion_application_result_t motion_application_model_tick(
    motion_application_model_t *model,
    bool safety_gate_open,
    uint64_t now_ms,
    motion_application_plan_t *plan)
{
    if (!model || !model->initialized || !plan) {
        return MOTION_APPLICATION_RESULT_INVALID_ARGUMENT;
    }
    reset_plan(plan);
    if (model->state != MOTION_CONTROL_ARMED &&
        model->state != MOTION_CONTROL_ACTIVE) {
        return MOTION_APPLICATION_RESULT_OK;
    }
    if (!safety_gate_open) {
        return fault_stop(model,
                          now_ms,
                          "SAFETY_GATE_CLOSED",
                          plan,
                          MOTION_APPLICATION_RESULT_UNSAFE);
    }
    command_authority_cycle_result_t cycle;
    command_authority_decision_t decision =
        command_authority_model_arbitrate(&model->authority, now_ms, &cycle);
    if (decision == COMMAND_AUTHORITY_DECISION_INVALID) {
        return fault_stop(model,
                          now_ms,
                          "AUTHORITY_INVALID",
                          plan,
                          MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED);
    }
    return convert_authority_decision(model, &cycle, now_ms, plan);
}

void motion_application_model_record_actuation(
    motion_application_model_t *model,
    const motion_application_plan_t *plan,
    bool success)
{
    if (!model || !model->initialized || !plan ||
        plan->action == MOTION_APPLICATION_PLAN_NONE) {
        return;
    }
    if (!success) {
        model->state = MOTION_CONTROL_FAULT;
        copy_detail(model->last_detail, "ACTUATION_FAILED");
        return;
    }
    if (plan->action == MOTION_APPLICATION_PLAN_STOP) {
        for (size_t index = 0U; index < model->config.endpoint_count; ++index) {
            model->targets[index].rpm = 0;
        }
        copy_detail(model->last_detail, plan->detail);
        return;
    }
    for (size_t index = 0U; index < plan->target_count; ++index) {
        model->targets[index] = plan->targets[index];
    }
    model->state = MOTION_CONTROL_ACTIVE;
    copy_detail(model->last_detail, plan->detail);
}

bool motion_application_model_snapshot(
    const motion_application_model_t *model,
    motion_application_model_snapshot_t *snapshot)
{
    if (!model || !model->initialized || !snapshot) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = model->state;
    snapshot->source = model->active_source;
    snapshot->command_ttl_ms = model->config.command_ttl_ms;
    snapshot->deadman = model->deadman;
    snapshot->hold_zero_when_deadman_released =
        model->hold_zero_when_deadman_released;
    snapshot->stream_id = model->active_stream_id;
    snapshot->sequence = model->sequence;
    snapshot->last_received_ms = model->last_received_ms;
    snapshot->requested = model->requested;
    snapshot->endpoint_count = model->config.endpoint_count;
    memcpy(snapshot->targets,
           model->targets,
           sizeof(model->targets));
    copy_detail(snapshot->last_detail, model->last_detail);
    return true;
}

const char *motion_application_result_name(motion_application_result_t result)
{
    switch (result) {
    case MOTION_APPLICATION_RESULT_OK:
        return "OK";
    case MOTION_APPLICATION_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case MOTION_APPLICATION_RESULT_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case MOTION_APPLICATION_RESULT_NOT_ARMED:
        return "NOT_ARMED";
    case MOTION_APPLICATION_RESULT_ALREADY_ARMED:
        return "ALREADY_ARMED";
    case MOTION_APPLICATION_RESULT_STREAM_MISMATCH:
        return "STREAM_MISMATCH";
    case MOTION_APPLICATION_RESULT_STREAM_RETIRED:
        return "STREAM_RETIRED";
    case MOTION_APPLICATION_RESULT_STREAM_HISTORY_FULL:
        return "STREAM_HISTORY_FULL";
    case MOTION_APPLICATION_RESULT_SEQUENCE_INVALID:
        return "SEQUENCE_INVALID";
    case MOTION_APPLICATION_RESULT_UNSAFE:
        return "UNSAFE";
    case MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED:
        return "AUTHORITY_REJECTED";
    case MOTION_APPLICATION_RESULT_KINEMATICS_FAILED:
        return "KINEMATICS_FAILED";
    case MOTION_APPLICATION_RESULT_SOURCE_INVALID:
        return "SOURCE_INVALID";
    case MOTION_APPLICATION_RESULT_SOURCE_MISMATCH:
        return "SOURCE_MISMATCH";
    default:
        return "UNKNOWN";
    }
}
