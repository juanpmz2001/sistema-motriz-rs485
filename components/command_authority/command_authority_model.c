#include "command_authority_model.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static command_authority_velocity_t zero_velocity(void)
{
    const command_authority_velocity_t velocity = {0.0f, 0.0f, 0.0f};
    return velocity;
}

static bool source_is_valid(command_authority_source_t source)
{
    return source >= COMMAND_AUTHORITY_SOURCE_BLUETOOTH &&
           source <= COMMAND_AUTHORITY_SOURCE_RC;
}

static bool scalar_is_finite(float value)
{
    return isfinite(value) != 0;
}

static float scalar_abs(float value)
{
    return value < 0.0f ? -value : value;
}

static bool velocity_is_finite(command_authority_velocity_t velocity)
{
    return scalar_is_finite(velocity.vx) && scalar_is_finite(velocity.vy) &&
           scalar_is_finite(velocity.wz);
}

static bool config_is_valid(const command_authority_config_t *config)
{
    if (!config || config->max_ttl_ms == 0 ||
        !velocity_is_finite(config->velocity_limit) ||
        !velocity_is_finite(config->moving_epsilon)) {
        return false;
    }

    if (config->velocity_limit.vx <= 0.0f ||
        config->velocity_limit.vy <= 0.0f ||
        config->velocity_limit.wz <= 0.0f) {
        return false;
    }

    if (config->moving_epsilon.vx < 0.0f ||
        config->moving_epsilon.vy < 0.0f ||
        config->moving_epsilon.wz < 0.0f) {
        return false;
    }

    return config->moving_epsilon.vx <= config->velocity_limit.vx &&
           config->moving_epsilon.vy <= config->velocity_limit.vy &&
           config->moving_epsilon.wz <= config->velocity_limit.wz;
}

bool command_authority_velocity_is_moving(
    const command_authority_config_t *config,
    command_authority_velocity_t velocity)
{
    if (!config || !velocity_is_finite(velocity)) {
        return false;
    }

    return scalar_abs(velocity.vx) > config->moving_epsilon.vx ||
           scalar_abs(velocity.vy) > config->moving_epsilon.vy ||
           scalar_abs(velocity.wz) > config->moving_epsilon.wz;
}

static bool velocity_is_within_limits(const command_authority_model_t *model,
                                      command_authority_velocity_t velocity)
{
    return scalar_abs(velocity.vx) <= model->config.velocity_limit.vx &&
           scalar_abs(velocity.vy) <= model->config.velocity_limit.vy &&
           scalar_abs(velocity.wz) <= model->config.velocity_limit.wz;
}

static bool mailbox_is_fresh(const command_authority_mailbox_t *mailbox,
                             uint64_t now_ms)
{
    if (!mailbox->initialized || !mailbox->command.valid ||
        now_ms < mailbox->command.received_at_ms) {
        return false;
    }

    return now_ms - mailbox->command.received_at_ms < mailbox->command.ttl_ms;
}

static bool mailbox_is_expired(const command_authority_mailbox_t *mailbox,
                               uint64_t now_ms)
{
    if (!mailbox->initialized || !mailbox->command.valid ||
        now_ms < mailbox->command.received_at_ms) {
        return false;
    }

    return now_ms - mailbox->command.received_at_ms >= mailbox->command.ttl_ms;
}

static command_authority_source_t highest_fresh_source(
    const command_authority_model_t *model,
    uint64_t now_ms)
{
    int source;
    for (source = (int)COMMAND_AUTHORITY_SOURCE_RC;
         source >= (int)COMMAND_AUTHORITY_SOURCE_BLUETOOTH;
         source--) {
        if (mailbox_is_fresh(&model->mailboxes[source], now_ms)) {
            return (command_authority_source_t)source;
        }
    }

    return COMMAND_AUTHORITY_SOURCE_NONE;
}

static void revoke_authority(command_authority_model_t *model)
{
    model->authority_granted = false;
    model->last_output_moving = false;
    model->applied_revision = 0;
    model->output = zero_velocity();
}

static void fill_cycle_result(const command_authority_model_t *model,
                              command_authority_source_t previous_source,
                              command_authority_decision_t decision,
                              command_authority_reason_t reason,
                              uint64_t command_revision,
                              command_authority_cycle_result_t *result)
{
    if (!result) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->decision = decision;
    result->reason = reason;
    result->previous_source = previous_source;
    if (!model || !model->initialized) {
        result->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
        return;
    }

    result->selected_source = model->selected_source;
    result->authority_granted = model->authority_granted;
    result->epoch = model->epoch;
    result->publication_revision = model->publication_revision;
    result->barrier_revision = model->barrier_revision;
    result->barrier_time_ms = model->barrier_time_ms;
    result->command_revision = command_revision;
    result->output = model->output;
}

static command_authority_decision_t record_decision(
    command_authority_model_t *model,
    command_authority_source_t previous_source,
    command_authority_decision_t decision,
    command_authority_reason_t reason,
    uint64_t command_revision,
    command_authority_cycle_result_t *result)
{
    model->last_decision = decision;
    model->last_reason = reason;
    fill_cycle_result(model,
                      previous_source,
                      decision,
                      reason,
                      command_revision,
                      result);
    return decision;
}

static command_authority_result_t remember_publish_result(
    command_authority_model_t *model,
    command_authority_result_t result)
{
    if (model && model->initialized) {
        model->last_publish_result = result;
    }
    return result;
}

static command_authority_result_t reject_order_with_fail_safe_stop(
    command_authority_model_t *model,
    const command_authority_command_t *command,
    uint64_t now_ms,
    command_authority_result_t result)
{
    if (!command->valid || !command->deadman) {
        (void)command_authority_model_stop(model, now_ms, NULL);
    }
    return remember_publish_result(model, result);
}

command_authority_result_t command_authority_model_init(
    command_authority_model_t *model,
    const command_authority_config_t *config)
{
    if (!model) {
        return COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT;
    }

    memset(model, 0, sizeof(*model));
    model->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
    model->last_decision = COMMAND_AUTHORITY_DECISION_STOP;
    model->last_reason = COMMAND_AUTHORITY_REASON_NO_FRESH_SOURCE;
    model->last_publish_result = COMMAND_AUTHORITY_RESULT_OK;

    if (!config) {
        return COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT;
    }
    if (!config_is_valid(config)) {
        return COMMAND_AUTHORITY_RESULT_INVALID_CONFIG;
    }

    model->config = *config;
    model->initialized = true;
    return COMMAND_AUTHORITY_RESULT_OK;
}

command_authority_result_t command_authority_model_publish(
    command_authority_model_t *model,
    command_authority_source_t source,
    const command_authority_command_t *command,
    uint64_t now_ms)
{
    command_authority_mailbox_t *mailbox;
    command_authority_command_t normalized_command;
    bool stream_changed;

    if (!model || !command) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT);
    }
    if (!model->initialized) {
        return COMMAND_AUTHORITY_RESULT_NOT_INITIALIZED;
    }
    if (!source_is_valid(source)) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_INVALID_SOURCE);
    }
    if (command->stream_id == 0) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_STREAM_ID_ZERO);
    }
    if (model->has_observed_time && now_ms < model->last_now_ms) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION);
    }
    if (command->received_at_ms > now_ms) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_TIMESTAMP_IN_FUTURE);
    }
    if (command->ttl_ms == 0 || command->ttl_ms > model->config.max_ttl_ms ||
        command->received_at_ms > UINT64_MAX - command->ttl_ms) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_TTL_INVALID);
    }
    if (now_ms - command->received_at_ms >= command->ttl_ms) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_EXPIRED_ON_ARRIVAL);
    }
    if (command->valid && command->deadman && !velocity_is_finite(command->body)) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY);
    }
    if (command->valid && command->deadman &&
        !velocity_is_within_limits(model, command->body)) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT);
    }

    mailbox = &model->mailboxes[source];
    stream_changed = mailbox->initialized &&
                     command->stream_id != mailbox->command.stream_id;
    if (mailbox->initialized &&
        command->received_at_ms < mailbox->command.received_at_ms) {
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_TIMESTAMP_REGRESSION);
    }
    if (mailbox->initialized && !stream_changed &&
        command->sequence <= mailbox->command.sequence) {
        return reject_order_with_fail_safe_stop(
            model, command, now_ms,
            COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);
    }
    if (stream_changed) {
        for (uint8_t index = 0; index < mailbox->retired_stream_count; index++) {
            if (mailbox->retired_stream_ids[index] == command->stream_id) {
                return reject_order_with_fail_safe_stop(
                    model, command, now_ms,
                    COMMAND_AUTHORITY_RESULT_STREAM_RETIRED);
            }
        }
        if (mailbox->retired_stream_count >=
            COMMAND_AUTHORITY_RETIRED_STREAM_CAPACITY) {
            return reject_order_with_fail_safe_stop(
                model, command, now_ms,
                COMMAND_AUTHORITY_RESULT_STREAM_HISTORY_FULL);
        }
    }
    if (model->publication_revision == UINT64_MAX) {
        model->counter_exhausted = true;
        return remember_publish_result(model,
                                       COMMAND_AUTHORITY_RESULT_COUNTER_EXHAUSTED);
    }

    normalized_command = *command;
    if (!normalized_command.valid || !normalized_command.deadman) {
        normalized_command.body = zero_velocity();
    }

    model->publication_revision++;
    if (stream_changed) {
        mailbox->retired_stream_ids[mailbox->retired_stream_count++] =
            mailbox->command.stream_id;
    }
    mailbox->command = normalized_command;
    mailbox->revision = model->publication_revision;
    mailbox->initialized = true;
    if (stream_changed && model->selected_source == source) {
        model->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
        revoke_authority(model);
    }
    model->last_now_ms = now_ms;
    model->has_observed_time = true;
    model->last_publish_result = COMMAND_AUTHORITY_RESULT_OK;
    return COMMAND_AUTHORITY_RESULT_OK;
}

static command_authority_decision_t switch_source(
    command_authority_model_t *model,
    command_authority_source_t desired_source,
    uint64_t now_ms,
    command_authority_cycle_result_t *result)
{
    command_authority_source_t previous_source = model->selected_source;
    bool previous_was_moving = model->last_output_moving;
    bool previous_is_fresh = false;
    bool previous_is_expired = false;
    command_authority_decision_t decision = COMMAND_AUTHORITY_DECISION_STOP;
    command_authority_reason_t reason = COMMAND_AUTHORITY_REASON_SOURCE_SWITCH;

    if (source_is_valid(previous_source)) {
        previous_is_fresh =
            mailbox_is_fresh(&model->mailboxes[previous_source], now_ms);
        previous_is_expired =
            mailbox_is_expired(&model->mailboxes[previous_source], now_ms);
    }

    if (model->epoch == UINT64_MAX) {
        model->counter_exhausted = true;
        model->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
        model->barrier_revision = model->publication_revision;
        model->barrier_time_ms = now_ms;
        revoke_authority(model);
        return record_decision(model,
                               previous_source,
                               COMMAND_AUTHORITY_DECISION_FAULT_STOP,
                               COMMAND_AUTHORITY_REASON_COUNTER_EXHAUSTED,
                               0,
                               result);
    }

    model->epoch++;
    model->selected_source = desired_source;
    model->barrier_revision = model->publication_revision;
    model->barrier_time_ms = now_ms;
    revoke_authority(model);

    if (previous_source != COMMAND_AUTHORITY_SOURCE_NONE && !previous_is_fresh) {
        reason = previous_is_expired
                     ? COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED
                     : COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_INVALIDATED;
        if (previous_was_moving) {
            decision = COMMAND_AUTHORITY_DECISION_FAULT_STOP;
        }
    }

    if (desired_source != COMMAND_AUTHORITY_SOURCE_NONE &&
        model->publication_revision == UINT64_MAX) {
        model->counter_exhausted = true;
        model->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
        decision = COMMAND_AUTHORITY_DECISION_FAULT_STOP;
        reason = COMMAND_AUTHORITY_REASON_COUNTER_EXHAUSTED;
    }

    return record_decision(model,
                           previous_source,
                           decision,
                           reason,
                           0,
                           result);
}

command_authority_decision_t command_authority_model_arbitrate(
    command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_cycle_result_t *result)
{
    command_authority_source_t desired_source;
    command_authority_source_t selected_source;
    command_authority_mailbox_t *mailbox;

    if (!model) {
        fill_cycle_result(NULL,
                          COMMAND_AUTHORITY_SOURCE_NONE,
                          COMMAND_AUTHORITY_DECISION_INVALID,
                          COMMAND_AUTHORITY_REASON_INVALID_ARGUMENT,
                          0,
                          result);
        return COMMAND_AUTHORITY_DECISION_INVALID;
    }
    if (!model->initialized) {
        fill_cycle_result(NULL,
                          COMMAND_AUTHORITY_SOURCE_NONE,
                          COMMAND_AUTHORITY_DECISION_INVALID,
                          COMMAND_AUTHORITY_REASON_NOT_INITIALIZED,
                          0,
                          result);
        return COMMAND_AUTHORITY_DECISION_INVALID;
    }
    if (model->has_observed_time && now_ms < model->last_now_ms) {
        fill_cycle_result(model,
                          model->selected_source,
                          COMMAND_AUTHORITY_DECISION_INVALID,
                          COMMAND_AUTHORITY_REASON_CLOCK_REGRESSION,
                          0,
                          result);
        return COMMAND_AUTHORITY_DECISION_INVALID;
    }

    model->last_now_ms = now_ms;
    model->has_observed_time = true;
    desired_source = highest_fresh_source(model, now_ms);
    if (desired_source != model->selected_source) {
        return switch_source(model, desired_source, now_ms, result);
    }

    selected_source = model->selected_source;
    if (selected_source == COMMAND_AUTHORITY_SOURCE_NONE) {
        revoke_authority(model);
        return record_decision(model,
                               selected_source,
                               COMMAND_AUTHORITY_DECISION_STOP,
                               COMMAND_AUTHORITY_REASON_NO_FRESH_SOURCE,
                               0,
                               result);
    }

    mailbox = &model->mailboxes[selected_source];
    if (mailbox->revision <= model->barrier_revision ||
        mailbox->command.received_at_ms <= model->barrier_time_ms) {
        revoke_authority(model);
        return record_decision(
            model,
            selected_source,
            COMMAND_AUTHORITY_DECISION_STOP,
            COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH,
            0,
            result);
    }

    model->authority_granted = true;
    model->applied_revision = mailbox->revision;
    model->output = mailbox->command.deadman ? mailbox->command.body
                                             : zero_velocity();
    model->last_output_moving =
        command_authority_velocity_is_moving(&model->config, model->output);
    return record_decision(model,
                           selected_source,
                           COMMAND_AUTHORITY_DECISION_APPLY,
                           COMMAND_AUTHORITY_REASON_COMMAND_APPLIED,
                           mailbox->revision,
                           result);
}

command_authority_decision_t command_authority_model_stop(
    command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_cycle_result_t *result)
{
    command_authority_source_t previous_source;
    int source;

    if (!model) {
        fill_cycle_result(NULL,
                          COMMAND_AUTHORITY_SOURCE_NONE,
                          COMMAND_AUTHORITY_DECISION_INVALID,
                          COMMAND_AUTHORITY_REASON_INVALID_ARGUMENT,
                          0,
                          result);
        return COMMAND_AUTHORITY_DECISION_INVALID;
    }
    if (!model->initialized) {
        fill_cycle_result(NULL,
                          COMMAND_AUTHORITY_SOURCE_NONE,
                          COMMAND_AUTHORITY_DECISION_INVALID,
                          COMMAND_AUTHORITY_REASON_NOT_INITIALIZED,
                          0,
                          result);
        return COMMAND_AUTHORITY_DECISION_INVALID;
    }
    previous_source = model->selected_source;
    if (!model->has_observed_time || now_ms >= model->last_now_ms) {
        model->last_now_ms = now_ms;
        model->has_observed_time = true;
    }
    for (source = (int)COMMAND_AUTHORITY_SOURCE_BLUETOOTH;
         source <= (int)COMMAND_AUTHORITY_SOURCE_RC;
         source++) {
        model->mailboxes[source].command.valid = false;
    }

    if (model->epoch == UINT64_MAX) {
        model->counter_exhausted = true;
    } else {
        model->epoch++;
    }
    model->selected_source = COMMAND_AUTHORITY_SOURCE_NONE;
    model->barrier_revision = model->publication_revision;
    model->barrier_time_ms = model->last_now_ms;
    revoke_authority(model);
    return record_decision(model,
                           previous_source,
                           COMMAND_AUTHORITY_DECISION_STOP,
                           COMMAND_AUTHORITY_REASON_EXPLICIT_STOP,
                           0,
                           result);
}

command_authority_result_t command_authority_model_snapshot(
    const command_authority_model_t *model,
    uint64_t now_ms,
    command_authority_snapshot_t *snapshot)
{
    int source;

    if (!model || !snapshot) {
        return COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!model->initialized) {
        return COMMAND_AUTHORITY_RESULT_NOT_INITIALIZED;
    }
    if (model->has_observed_time && now_ms < model->last_now_ms) {
        return COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION;
    }

    snapshot->selected_source = model->selected_source;
    snapshot->authority_granted = model->authority_granted;
    snapshot->last_output_moving = model->last_output_moving;
    snapshot->counter_exhausted = model->counter_exhausted;
    snapshot->epoch = model->epoch;
    snapshot->publication_revision = model->publication_revision;
    snapshot->barrier_revision = model->barrier_revision;
    snapshot->barrier_time_ms = model->barrier_time_ms;
    snapshot->applied_revision = model->applied_revision;
    snapshot->last_now_ms = model->last_now_ms;
    snapshot->has_observed_time = model->has_observed_time;
    snapshot->output = model->output;
    snapshot->last_decision = model->last_decision;
    snapshot->last_reason = model->last_reason;
    snapshot->last_publish_result = model->last_publish_result;

    for (source = (int)COMMAND_AUTHORITY_SOURCE_BLUETOOTH;
         source <= (int)COMMAND_AUTHORITY_SOURCE_RC;
         source++) {
        const command_authority_mailbox_t *mailbox = &model->mailboxes[source];
        command_authority_mailbox_snapshot_t *mailbox_snapshot =
            &snapshot->mailboxes[source];
        mailbox_snapshot->command = mailbox->command;
        mailbox_snapshot->revision = mailbox->revision;
        mailbox_snapshot->initialized = mailbox->initialized;
        mailbox_snapshot->fresh = mailbox_is_fresh(mailbox, now_ms);
        if (mailbox->initialized && now_ms >= mailbox->command.received_at_ms) {
            mailbox_snapshot->age_ms =
                now_ms - mailbox->command.received_at_ms;
        }
    }

    return COMMAND_AUTHORITY_RESULT_OK;
}

const char *command_authority_source_name(command_authority_source_t source)
{
    switch (source) {
    case COMMAND_AUTHORITY_SOURCE_NONE:
        return "NONE";
    case COMMAND_AUTHORITY_SOURCE_BLUETOOTH:
        return "BLUETOOTH";
    case COMMAND_AUTHORITY_SOURCE_LAN:
        return "LAN";
    case COMMAND_AUTHORITY_SOURCE_RC:
        return "RC";
    default:
        return "UNKNOWN";
    }
}

const char *command_authority_result_name(command_authority_result_t result)
{
    switch (result) {
    case COMMAND_AUTHORITY_RESULT_OK:
        return "OK";
    case COMMAND_AUTHORITY_RESULT_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case COMMAND_AUTHORITY_RESULT_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case COMMAND_AUTHORITY_RESULT_NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    case COMMAND_AUTHORITY_RESULT_INVALID_SOURCE:
        return "INVALID_SOURCE";
    case COMMAND_AUTHORITY_RESULT_STREAM_ID_ZERO:
        return "STREAM_ID_ZERO";
    case COMMAND_AUTHORITY_RESULT_STREAM_RETIRED:
        return "STREAM_RETIRED";
    case COMMAND_AUTHORITY_RESULT_STREAM_HISTORY_FULL:
        return "STREAM_HISTORY_FULL";
    case COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING:
        return "SEQUENCE_NOT_INCREASING";
    case COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION:
        return "CLOCK_REGRESSION";
    case COMMAND_AUTHORITY_RESULT_TIMESTAMP_IN_FUTURE:
        return "TIMESTAMP_IN_FUTURE";
    case COMMAND_AUTHORITY_RESULT_TIMESTAMP_REGRESSION:
        return "TIMESTAMP_REGRESSION";
    case COMMAND_AUTHORITY_RESULT_TTL_INVALID:
        return "TTL_INVALID";
    case COMMAND_AUTHORITY_RESULT_EXPIRED_ON_ARRIVAL:
        return "EXPIRED_ON_ARRIVAL";
    case COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY:
        return "NONFINITE_VELOCITY";
    case COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT:
        return "VELOCITY_LIMIT";
    case COMMAND_AUTHORITY_RESULT_COUNTER_EXHAUSTED:
        return "COUNTER_EXHAUSTED";
    default:
        return "UNKNOWN";
    }
}

const char *command_authority_decision_name(command_authority_decision_t decision)
{
    switch (decision) {
    case COMMAND_AUTHORITY_DECISION_INVALID:
        return "INVALID";
    case COMMAND_AUTHORITY_DECISION_STOP:
        return "STOP";
    case COMMAND_AUTHORITY_DECISION_APPLY:
        return "APPLY";
    case COMMAND_AUTHORITY_DECISION_FAULT_STOP:
        return "FAULT_STOP";
    default:
        return "UNKNOWN";
    }
}

const char *command_authority_reason_name(command_authority_reason_t reason)
{
    switch (reason) {
    case COMMAND_AUTHORITY_REASON_NONE:
        return "NONE";
    case COMMAND_AUTHORITY_REASON_NO_FRESH_SOURCE:
        return "NO_FRESH_SOURCE";
    case COMMAND_AUTHORITY_REASON_SOURCE_SWITCH:
        return "SOURCE_SWITCH";
    case COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH:
        return "WAITING_FRESH_AFTER_SWITCH";
    case COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED:
        return "SELECTED_SOURCE_EXPIRED";
    case COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_INVALIDATED:
        return "SELECTED_SOURCE_INVALIDATED";
    case COMMAND_AUTHORITY_REASON_COMMAND_APPLIED:
        return "COMMAND_APPLIED";
    case COMMAND_AUTHORITY_REASON_EXPLICIT_STOP:
        return "EXPLICIT_STOP";
    case COMMAND_AUTHORITY_REASON_CLOCK_REGRESSION:
        return "CLOCK_REGRESSION";
    case COMMAND_AUTHORITY_REASON_COUNTER_EXHAUSTED:
        return "COUNTER_EXHAUSTED";
    case COMMAND_AUTHORITY_REASON_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case COMMAND_AUTHORITY_REASON_NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    default:
        return "UNKNOWN";
    }
}
