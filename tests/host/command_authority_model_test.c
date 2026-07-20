#include <math.h>
#include <stdint.h>
#include <string.h>

#include "command_authority_model.h"
#include "host_test.h"

static command_authority_config_t standard_config(void)
{
    const command_authority_config_t config = {
        .velocity_limit = {2.0f, 2.0f, 3.0f},
        .moving_epsilon = {0.01f, 0.01f, 0.02f},
        .max_ttl_ms = 1000,
    };
    return config;
}

static command_authority_command_t make_command(uint64_t stream_id,
                                                 uint64_t sequence,
                                                 uint64_t received_at_ms,
                                                 uint64_t ttl_ms,
                                                 bool valid,
                                                 bool deadman,
                                                 float vx,
                                                 float vy,
                                                 float wz)
{
    const command_authority_command_t command = {
        .stream_id = stream_id,
        .sequence = sequence,
        .received_at_ms = received_at_ms,
        .ttl_ms = ttl_ms,
        .valid = valid,
        .deadman = deadman,
        .body = {vx, vy, wz},
    };
    return command;
}

static bool velocity_is_zero(command_authority_velocity_t velocity)
{
    return velocity.vx == 0.0f && velocity.vy == 0.0f && velocity.wz == 0.0f;
}

static bool init_model(command_authority_model_t *model)
{
    command_authority_config_t config = standard_config();
    HOST_TEST_CHECK(command_authority_model_init(model, &config) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(model->selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(model->epoch == 0);
    HOST_TEST_CHECK(model->publication_revision == 0);
    return true;
}

static bool establish_initial_authority(command_authority_model_t *model,
                                        command_authority_source_t source,
                                        uint64_t stream_id,
                                        uint64_t first_received_at_ms,
                                        uint64_t ttl_ms,
                                        command_authority_velocity_t velocity,
                                        bool deadman)
{
    command_authority_cycle_result_t cycle;
    command_authority_command_t command =
        make_command(stream_id,
                     1,
                     first_received_at_ms,
                     ttl_ms,
                     true,
                     deadman,
                     velocity.vx,
                     velocity.vy,
                     velocity.wz);

    HOST_TEST_CHECK(command_authority_model_publish(
                        model, source, &command, first_received_at_ms) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(
                        model, first_received_at_ms, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_SOURCE_SWITCH);
    HOST_TEST_CHECK(cycle.selected_source == source);
    HOST_TEST_CHECK(!cycle.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    command.sequence = 2;
    command.received_at_ms = first_received_at_ms + 1;
    HOST_TEST_CHECK(command_authority_model_publish(
                        model, source, &command, command.received_at_ms) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(
                        model, command.received_at_ms, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.selected_source == source);
    HOST_TEST_CHECK(cycle.authority_granted);
    return true;
}

static bool test_three_source_priority_and_initial_epoch_barrier(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_snapshot_t snapshot;
    command_authority_command_t bluetooth =
        make_command(10, 1, 10, 100, true, true, 0.1f, 0.0f, 0.0f);
    command_authority_command_t lan =
        make_command(20, 1, 10, 100, true, true, 0.2f, 0.0f, 0.0f);
    command_authority_command_t rc =
        make_command(30, 1, 10, 100, true, true, 0.3f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(cycle.epoch == 0);

    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 10) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 10) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 10) ==
                    COMMAND_AUTHORITY_RESULT_OK);

    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 10, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.previous_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_RC);
    HOST_TEST_CHECK(cycle.epoch == 1);
    HOST_TEST_CHECK(cycle.barrier_revision == 3);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 10, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH);

    rc.sequence = 2;
    rc.received_at_ms = 11;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 11) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.command_revision == 4);
    HOST_TEST_CHECK(cycle.output.vx == 0.3f);

    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 11, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_BLUETOOTH].fresh);
    HOST_TEST_CHECK(snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_LAN].fresh);
    HOST_TEST_CHECK(snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].fresh);
    HOST_TEST_CHECK(snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].revision == 4);
    HOST_TEST_CHECK(snapshot.authority_granted);
    HOST_TEST_CHECK(snapshot.last_output_moving);
    HOST_TEST_CHECK(strcmp(command_authority_source_name(snapshot.selected_source),
                           "RC") == 0);
    return true;
}

static bool test_rc_neutral_and_deadman_block_lower_sources(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_snapshot_t snapshot;
    const command_authority_velocity_t bluetooth_velocity = {0.4f, 0.0f, 0.0f};
    command_authority_command_t rc =
        make_command(200, 1, 2, 100, true, true, 0.0f, 0.0f, 0.0f);
    command_authority_command_t lan =
        make_command(300, 1, 4, 100, true, true, 1.0f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                                                100,
                                                0,
                                                100,
                                                bluetooth_velocity,
                                                true));
    HOST_TEST_CHECK(model.last_output_moving);

    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 2, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_RC);
    HOST_TEST_CHECK(cycle.epoch == 2);

    rc.sequence = 2;
    rc.received_at_ms = 3;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 3) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 4) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    rc.sequence = 3;
    rc.received_at_ms = 4;
    rc.deadman = false;
    rc.body.vx = 1.5f;
    rc.body.vy = -1.0f;
    rc.body.wz = 2.0f;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 4) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 4, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_RC);
    HOST_TEST_CHECK(cycle.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));
    HOST_TEST_CHECK(!model.last_output_moving);

    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 4, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(velocity_is_zero(
        snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].command.body));
    HOST_TEST_CHECK(!snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].command.deadman);
    HOST_TEST_CHECK(snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_LAN].fresh);
    return true;
}

static bool test_lan_preempts_bluetooth_and_requires_own_new_publication(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    const command_authority_velocity_t bluetooth_velocity = {0.2f, 0.0f, 0.0f};
    command_authority_command_t lan =
        make_command(22, 1, 2, 100, true, true, 0.8f, 0.1f, 0.0f);
    command_authority_command_t bluetooth =
        make_command(11, 3, 3, 100, true, true, 1.2f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                                                11,
                                                0,
                                                100,
                                                bluetooth_velocity,
                                                true));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 2, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_LAN);
    HOST_TEST_CHECK(cycle.epoch == 2);
    HOST_TEST_CHECK(cycle.barrier_revision == 3);

    HOST_TEST_CHECK(command_authority_model_publish(
                        &model,
                        COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                        &bluetooth,
                        3) == COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH);

    lan.sequence = 2;
    lan.received_at_ms = 4;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 4) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 4, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_LAN);
    HOST_TEST_CHECK(cycle.output.vx == 0.8f);
    HOST_TEST_CHECK(cycle.command_revision == 5);
    return true;
}

static bool test_moving_ttl_expiry_fault_stops_without_stale_fallback(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_command_t bluetooth =
        make_command(1, 1, 0, 100, true, true, 0.2f, 0.0f, 0.0f);
    command_authority_command_t lan =
        make_command(2, 1, 0, 10, true, true, 0.9f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);

    lan.sequence = 2;
    lan.received_at_ms = 1;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 1) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 1, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(model.last_output_moving);

    bluetooth.sequence = 2;
    bluetooth.received_at_ms = 2;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_FAULT_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED);
    HOST_TEST_CHECK(cycle.previous_source == COMMAND_AUTHORITY_SOURCE_LAN);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_BLUETOOTH);
    HOST_TEST_CHECK(!cycle.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH);
    bluetooth.sequence = 3;
    bluetooth.received_at_ms = 12;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 12) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 12, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.output.vx == 0.2f);
    return true;
}

static bool test_zero_ttl_expiry_stops_and_requires_fresh_fallback(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_command_t lan =
        make_command(5, 1, 0, 100, true, true, 0.5f, 0.0f, 0.0f);
    command_authority_command_t rc =
        make_command(6, 1, 0, 10, true, true, 0.0f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    rc.sequence = 2;
    rc.received_at_ms = 1;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 1) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 1, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_LAN);
    HOST_TEST_CHECK(cycle.barrier_revision == 3);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);

    lan.sequence = 2;
    lan.received_at_ms = 12;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 12) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 12, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.output.vx == 0.5f);
    return true;
}

static bool test_sequence_stream_timestamp_and_revision_policy(void)
{
    command_authority_model_t model;
    command_authority_snapshot_t snapshot;
    command_authority_command_t command =
        make_command(0, 9, 10, 100, true, true, 0.0f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    command.stream_id = 7;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_NONE, &command, 10) ==
                    COMMAND_AUTHORITY_RESULT_INVALID_SOURCE);
    command.stream_id = 0;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 10) ==
                    COMMAND_AUTHORITY_RESULT_STREAM_ID_ZERO);
    HOST_TEST_CHECK(model.publication_revision == 0);

    command.stream_id = 7;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 10) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(model.publication_revision == 1);
    command.received_at_ms = 11;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 11) ==
                    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);
    command.sequence = 8;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 11) ==
                    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);

    command.stream_id = 8;
    command.sequence = 0;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 11) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(model.publication_revision == 2);
    command.stream_id = 9;
    command.received_at_ms = 10;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 11) ==
                    COMMAND_AUTHORITY_RESULT_TIMESTAMP_REGRESSION);

    command.stream_id = 8;
    command.sequence = UINT64_MAX;
    command.received_at_ms = 12;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 12) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    command.sequence = 0;
    command.received_at_ms = 13;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 13) ==
                    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);

    command.stream_id = 9;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 13) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(model.publication_revision == 4);
    HOST_TEST_CHECK(model.mailboxes[COMMAND_AUTHORITY_SOURCE_LAN].revision == 4);

    command.stream_id = 10;
    command.received_at_ms = 12;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 12) ==
                    COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION);
    command.received_at_ms = 15;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &command, 14) ==
                    COMMAND_AUTHORITY_RESULT_TIMESTAMP_IN_FUTURE);
    command.received_at_ms = 4;
    command.ttl_ms = 10;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &command, 14) ==
                    COMMAND_AUTHORITY_RESULT_EXPIRED_ON_ARRIVAL);

    command.received_at_ms = 14;
    command.ttl_ms = 0;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &command, 14) ==
                    COMMAND_AUTHORITY_RESULT_TTL_INVALID);
    command.ttl_ms = 1001;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &command, 14) ==
                    COMMAND_AUTHORITY_RESULT_TTL_INVALID);
    command.ttl_ms = 100;
    command.valid = false;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &command, 14) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 14, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(snapshot.publication_revision == 5);
    HOST_TEST_CHECK(
        snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_BLUETOOTH].initialized);
    HOST_TEST_CHECK(!snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_BLUETOOTH].fresh);
    return true;
}

static bool test_nonfinite_limits_and_moving_epsilon(void)
{
    command_authority_model_t model;
    command_authority_config_t config = standard_config();
    command_authority_config_t bad_config = config;
    command_authority_command_t command =
        make_command(1, 1, 0, 100, true, true, 0.0f, 0.0f, 0.0f);
    command_authority_velocity_t velocity;

    bad_config.max_ttl_ms = 0;
    HOST_TEST_CHECK(command_authority_model_init(&model, &bad_config) ==
                    COMMAND_AUTHORITY_RESULT_INVALID_CONFIG);
    bad_config = config;
    bad_config.moving_epsilon.vx = -0.1f;
    HOST_TEST_CHECK(command_authority_model_init(&model, &bad_config) ==
                    COMMAND_AUTHORITY_RESULT_INVALID_CONFIG);
    bad_config = config;
    bad_config.velocity_limit.wz = INFINITY;
    HOST_TEST_CHECK(command_authority_model_init(&model, &bad_config) ==
                    COMMAND_AUTHORITY_RESULT_INVALID_CONFIG);
    HOST_TEST_CHECK(command_authority_model_init(&model, &config) ==
                    COMMAND_AUTHORITY_RESULT_OK);

    command.body.vx = NAN;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY);
    command.body.vx = 0.0f;
    command.body.vy = INFINITY;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY);
    command.body.vy = 0.0f;
    command.body.wz = -INFINITY;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_NONFINITE_VELOCITY);

    command.body.wz = 0.0f;
    command.body.vx = 2.01f;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT);
    command.body.vx = 0.0f;
    command.body.vy = -2.01f;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT);
    command.body.vy = 0.0f;
    command.body.wz = 3.01f;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT);

    command.body.vx = -2.0f;
    command.body.vy = 2.0f;
    command.body.wz = -3.0f;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);

    velocity.vx = config.moving_epsilon.vx;
    velocity.vy = -config.moving_epsilon.vy;
    velocity.wz = config.moving_epsilon.wz;
    HOST_TEST_CHECK(!command_authority_velocity_is_moving(&config, velocity));
    velocity.wz = config.moving_epsilon.wz + 0.001f;
    HOST_TEST_CHECK(command_authority_velocity_is_moving(&config, velocity));
    config.moving_epsilon = (command_authority_velocity_t){0.0f, 0.0f, 0.0f};
    HOST_TEST_CHECK(!command_authority_velocity_is_moving(
        &config, (command_authority_velocity_t){0.0f, 0.0f, 0.0f}));
    HOST_TEST_CHECK(command_authority_velocity_is_moving(
        &config, (command_authority_velocity_t){0.0f, -0.0001f, 0.0f}));
    return true;
}

static bool test_nonmoving_publication_normalizes_unused_velocity(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_snapshot_t snapshot;
    const command_authority_velocity_t moving = {1.0f, 0.0f, 0.0f};
    command_authority_command_t command;

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_RC,
                                                10,
                                                0,
                                                100,
                                                moving,
                                                true));

    command = make_command(10, 3, 2, 100, true, false, NAN, INFINITY, -INFINITY);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 2, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));
    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 2, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(velocity_is_zero(
        snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].command.body));

    command.sequence = 4;
    command.received_at_ms = 3;
    command.valid = false;
    command.deadman = true;
    command.body = (command_authority_velocity_t){NAN, NAN, NAN};
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 3) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));
    return true;
}

static bool test_retired_stream_cannot_resume_selected_source(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    const command_authority_velocity_t moving = {0.5f, 0.0f, 0.0f};
    command_authority_command_t replacement;
    command_authority_command_t retired;
    uint64_t epoch_after_switch;

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_LAN,
                                                10,
                                                0,
                                                100,
                                                moving,
                                                true));

    replacement = make_command(20, 1, 2, 100, true, false, 0.0f, 0.0f, 0.0f);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &replacement, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(!model.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(model.output));
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 2, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_SOURCE_SWITCH);
    epoch_after_switch = cycle.epoch;

    replacement.sequence = 2;
    replacement.received_at_ms = 3;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &replacement, 3) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    retired = make_command(10, 99, 4, 100, true, true, 1.5f, 0.0f, 0.0f);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &retired, 4) ==
                    COMMAND_AUTHORITY_RESULT_STREAM_RETIRED);
    HOST_TEST_CHECK(model.epoch == epoch_after_switch);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 4, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));
    return true;
}

static bool test_fresh_after_switch_requires_later_receive_time(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_command_t bluetooth =
        make_command(10, 1, 0, 100, true, true, 0.4f, 0.0f, 0.0f);
    command_authority_command_t lan =
        make_command(20, 1, 0, 10, true, true, 0.8f, 0.0f, 0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    lan.sequence = 2;
    lan.received_at_ms = 1;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 1) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 1, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);

    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_FAULT_STOP);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_BLUETOOTH);
    HOST_TEST_CHECK(cycle.barrier_time_ms == 11);

    bluetooth.sequence = 2;
    bluetooth.received_at_ms = 10;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 11) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 11, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_WAITING_FRESH_AFTER_SWITCH);

    bluetooth.sequence = 3;
    bluetooth.received_at_ms = 12;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_BLUETOOTH, &bluetooth, 12) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 12, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(cycle.output.vx == 0.4f);
    return true;
}

static bool test_rejected_nonmoving_order_still_fails_safe(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    const command_authority_velocity_t moving = {1.0f, 0.0f, 0.0f};
    command_authority_command_t release;
    command_authority_mailbox_t *mailbox;

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_LAN,
                                                10,
                                                0,
                                                100,
                                                moving,
                                                true));
    release = make_command(10, 1, 2, 100, true, false, NAN, NAN, NAN);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &release, 2) ==
                    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);
    HOST_TEST_CHECK(model.selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(!model.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(model.output));

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_LAN,
                                                10,
                                                0,
                                                100,
                                                moving,
                                                true));
    mailbox = &model.mailboxes[COMMAND_AUTHORITY_SOURCE_LAN];
    mailbox->retired_stream_count = COMMAND_AUTHORITY_RETIRED_STREAM_CAPACITY;
    for (uint8_t index = 0U;
         index < COMMAND_AUTHORITY_RETIRED_STREAM_CAPACITY;
         index++) {
        mailbox->retired_stream_ids[index] = 100U + index;
    }
    release = make_command(20, 1, 2, 100, true, false, NAN, NAN, NAN);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &release, 2) ==
                    COMMAND_AUTHORITY_RESULT_STREAM_HISTORY_FULL);
    HOST_TEST_CHECK(model.selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(!model.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(model.output));
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 2, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    return true;
}

static bool test_explicit_stop_revokes_and_invalidates_pending_commands(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_snapshot_t snapshot;
    const command_authority_velocity_t rc_velocity = {1.0f, 0.0f, 0.0f};
    command_authority_command_t lan =
        make_command(20, 1, 2, 100, true, true, 0.5f, 0.0f, 0.0f);
    command_authority_command_t bluetooth =
        make_command(30, 1, 2, 100, true, true, 0.4f, 0.0f, 0.0f);
    command_authority_command_t rc =
        make_command(10, 2, 4, 100, true, true, 1.0f, 0.0f, 0.0f);
    uint64_t revision_before_stop;

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(establish_initial_authority(&model,
                                                COMMAND_AUTHORITY_SOURCE_RC,
                                                10,
                                                0,
                                                100,
                                                rc_velocity,
                                                true));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_LAN, &lan, 2) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model,
                        COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                        &bluetooth,
                        2) == COMMAND_AUTHORITY_RESULT_OK);
    revision_before_stop = model.publication_revision;

    HOST_TEST_CHECK(command_authority_model_stop(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_EXPLICIT_STOP);
    HOST_TEST_CHECK(cycle.previous_source == COMMAND_AUTHORITY_SOURCE_RC);
    HOST_TEST_CHECK(cycle.selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(cycle.epoch == 2);
    HOST_TEST_CHECK(cycle.publication_revision == revision_before_stop);
    HOST_TEST_CHECK(!cycle.authority_granted);
    HOST_TEST_CHECK(velocity_is_zero(cycle.output));

    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 3, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(!snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_BLUETOOTH]
                         .command.valid);
    HOST_TEST_CHECK(
        !snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_LAN].command.valid);
    HOST_TEST_CHECK(!snapshot.mailboxes[COMMAND_AUTHORITY_SOURCE_RC].command.valid);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 3, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.epoch == 2);

    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 4) ==
                    COMMAND_AUTHORITY_RESULT_SEQUENCE_NOT_INCREASING);
    rc.sequence = 3;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 4) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 4, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.epoch == 3);
    rc.sequence = 4;
    rc.received_at_ms = 5;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &rc, 5) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 5, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);

    HOST_TEST_CHECK(command_authority_model_stop(&model, 4, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.epoch == 4);
    HOST_TEST_CHECK(model.last_now_ms == 5);
    HOST_TEST_CHECK(model.selected_source == COMMAND_AUTHORITY_SOURCE_NONE);
    HOST_TEST_CHECK(!model.authority_granted);
    return true;
}

static bool test_no_wrap_and_counter_exhaustion_policy(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;
    command_authority_snapshot_t snapshot;
    command_authority_command_t command =
        make_command(1,
                     1,
                     UINT64_MAX - 5,
                     10,
                     true,
                     true,
                     0.0f,
                     0.0f,
                     0.0f);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model,
                        COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                        &command,
                        UINT64_MAX - 5) == COMMAND_AUTHORITY_RESULT_TTL_INVALID);

    command.received_at_ms = UINT64_MAX - 20;
    command.ttl_ms = 10;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model,
                        COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                        &command,
                        UINT64_MAX - 20) == COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(
                        &model, UINT64_MAX - 20, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    command.sequence = 2;
    command.received_at_ms = UINT64_MAX - 19;
    command.ttl_ms = 9;
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model,
                        COMMAND_AUTHORITY_SOURCE_BLUETOOTH,
                        &command,
                        UINT64_MAX - 19) == COMMAND_AUTHORITY_RESULT_OK);
    HOST_TEST_CHECK(command_authority_model_arbitrate(
                        &model, UINT64_MAX - 19, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_APPLY);
    HOST_TEST_CHECK(command_authority_model_arbitrate(
                        &model, UINT64_MAX - 10, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.reason ==
                    COMMAND_AUTHORITY_REASON_SELECTED_SOURCE_EXPIRED);
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_INVALID);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_CLOCK_REGRESSION);
    HOST_TEST_CHECK(command_authority_model_snapshot(&model, 0, &snapshot) ==
                    COMMAND_AUTHORITY_RESULT_CLOCK_REGRESSION);

    HOST_TEST_CHECK(init_model(&model));
    model.publication_revision = UINT64_MAX;
    command = make_command(2, 1, 0, 10, true, true, 0.0f, 0.0f, 0.0f);
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_COUNTER_EXHAUSTED);
    HOST_TEST_CHECK(model.publication_revision == UINT64_MAX);
    HOST_TEST_CHECK(model.counter_exhausted);

    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(command_authority_model_publish(
                        &model, COMMAND_AUTHORITY_SOURCE_RC, &command, 0) ==
                    COMMAND_AUTHORITY_RESULT_OK);
    model.epoch = UINT64_MAX;
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_FAULT_STOP);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_COUNTER_EXHAUSTED);
    HOST_TEST_CHECK(cycle.epoch == UINT64_MAX);
    HOST_TEST_CHECK(command_authority_model_stop(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_STOP);
    HOST_TEST_CHECK(cycle.epoch == UINT64_MAX);
    return true;
}

static bool test_diagnostic_names_and_invalid_calls(void)
{
    command_authority_model_t model;
    command_authority_cycle_result_t cycle;

    memset(&model, 0, sizeof(model));
    HOST_TEST_CHECK(command_authority_model_arbitrate(&model, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_INVALID);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_NOT_INITIALIZED);
    HOST_TEST_CHECK(command_authority_model_stop(NULL, 0, &cycle) ==
                    COMMAND_AUTHORITY_DECISION_INVALID);
    HOST_TEST_CHECK(cycle.reason == COMMAND_AUTHORITY_REASON_INVALID_ARGUMENT);
    HOST_TEST_CHECK(strcmp(command_authority_result_name(
                               COMMAND_AUTHORITY_RESULT_VELOCITY_LIMIT),
                           "VELOCITY_LIMIT") == 0);
    HOST_TEST_CHECK(strcmp(command_authority_decision_name(
                               COMMAND_AUTHORITY_DECISION_FAULT_STOP),
                           "FAULT_STOP") == 0);
    HOST_TEST_CHECK(strcmp(command_authority_reason_name(
                               COMMAND_AUTHORITY_REASON_EXPLICIT_STOP),
                           "EXPLICIT_STOP") == 0);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_three_source_priority_and_initial_epoch_barrier),
        HOST_TEST_CASE(test_rc_neutral_and_deadman_block_lower_sources),
        HOST_TEST_CASE(
            test_lan_preempts_bluetooth_and_requires_own_new_publication),
        HOST_TEST_CASE(
            test_moving_ttl_expiry_fault_stops_without_stale_fallback),
        HOST_TEST_CASE(
            test_zero_ttl_expiry_stops_and_requires_fresh_fallback),
        HOST_TEST_CASE(test_sequence_stream_timestamp_and_revision_policy),
        HOST_TEST_CASE(test_nonfinite_limits_and_moving_epsilon),
        HOST_TEST_CASE(test_nonmoving_publication_normalizes_unused_velocity),
        HOST_TEST_CASE(test_retired_stream_cannot_resume_selected_source),
        HOST_TEST_CASE(test_fresh_after_switch_requires_later_receive_time),
        HOST_TEST_CASE(test_rejected_nonmoving_order_still_fails_safe),
        HOST_TEST_CASE(
            test_explicit_stop_revokes_and_invalidates_pending_commands),
        HOST_TEST_CASE(test_no_wrap_and_counter_exhaustion_policy),
        HOST_TEST_CASE(test_diagnostic_names_and_invalid_calls),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
