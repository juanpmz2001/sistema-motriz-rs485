#include <stdint.h>
#include <string.h>

#include "host_test.h"
#include "motion_application_model.h"

static motion_application_model_config_t standard_config(void)
{
    motion_application_model_config_t config = {
        .command_ttl_ms = 100U,
        .velocity_limit = {0.5f, 0.01f, 1.0f},
        .moving_epsilon = {0.0001f, 0.0001f, 0.0001f},
        .differential = {
            .track_width_m = 0.4,
            .vy_epsilon_mps = 0.0001,
        },
        .endpoint_count = 2U,
        .endpoints = {
            {
                .endpoint_id = 1U,
                .name = "left",
                .side = ROBOT_KINEMATICS_SIDE_LEFT,
                .wheel_radius_m = 0.1,
                .motor_to_wheel_ratio = 1.0,
                .direction_sign = 1,
                .max_abs_rpm = 100.0,
            },
            {
                .endpoint_id = 2U,
                .name = "right",
                .side = ROBOT_KINEMATICS_SIDE_RIGHT,
                .wheel_radius_m = 0.1,
                .motor_to_wheel_ratio = 1.0,
                .direction_sign = 1,
                .max_abs_rpm = 100.0,
            },
        },
    };
    return config;
}

static motion_application_event_t event(motion_application_event_action_t action,
                                        uint64_t stream,
                                        uint64_t sequence,
                                        uint64_t received_at_ms)
{
    motion_application_event_t value = {
        .action = action,
        .source = COMMAND_AUTHORITY_SOURCE_LAN,
        .stream_id = stream,
        .sequence = sequence,
        .received_at_ms = received_at_ms,
    };
    return value;
}

static bool init_model(motion_application_model_t *model);

static bool test_rc_source_is_explicit_and_cannot_mix_with_lan(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    motion_application_event_t arm =
        event(MOTION_APPLICATION_EVENT_ARM, 88U, 1U, 0U);
    arm.source = COMMAND_AUTHORITY_SOURCE_RC;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &arm, true, 0U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    motion_application_model_snapshot_t snapshot;
    HOST_TEST_CHECK(motion_application_model_snapshot(&model, &snapshot));
    HOST_TEST_CHECK(snapshot.source == COMMAND_AUTHORITY_SOURCE_RC);

    motion_application_event_t command =
        event(MOTION_APPLICATION_EVENT_COMMAND, 88U, 2U, 10U);
    command.source = COMMAND_AUTHORITY_SOURCE_RC;
    command.deadman = true;
    command.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    command.sequence = 3U;
    command.received_at_ms = 20U;
    command.source = COMMAND_AUTHORITY_SOURCE_LAN;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 20U, &plan) ==
                    MOTION_APPLICATION_RESULT_SOURCE_MISMATCH);
    return true;
}

static bool test_web_direct_released_deadman_holds_zero_only_for_that_source(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    motion_application_event_t arm = event(MOTION_APPLICATION_EVENT_ARM, 99U, 1U, 0U);
    arm.source = COMMAND_AUTHORITY_SOURCE_WEB_DIRECT;
    HOST_TEST_CHECK(motion_application_model_submit(&model, &arm, true, 0U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    motion_application_event_t released =
        event(MOTION_APPLICATION_EVENT_COMMAND, 99U, 2U, 10U);
    released.source = COMMAND_AUTHORITY_SOURCE_WEB_DIRECT;
    released.deadman = false;
    released.hold_zero_when_deadman_released = true;
    HOST_TEST_CHECK(motion_application_model_submit(&model, &released, true, 10U,
                                                    &plan) == MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    HOST_TEST_CHECK(plan.target_count == 2U && plan.targets[0].rpm == 0 &&
                    plan.targets[1].rpm == 0);

    motion_application_event_t invalid = released;
    invalid.source = COMMAND_AUTHORITY_SOURCE_LAN;
    invalid.sequence = 3U;
    HOST_TEST_CHECK(motion_application_model_submit(&model, &invalid, true, 20U,
                                                    &plan) ==
                    MOTION_APPLICATION_RESULT_INVALID_ARGUMENT);
    return true;
}

static bool init_model(motion_application_model_t *model)
{
    motion_application_model_config_t config = standard_config();
    HOST_TEST_CHECK(motion_application_model_init(model, &config) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(model->state == MOTION_CONTROL_DISARMED);
    return true;
}

static bool arm_model(motion_application_model_t *model,
                      uint64_t stream,
                      uint64_t now_ms)
{
    motion_application_plan_t plan;
    motion_application_event_t arm =
        event(MOTION_APPLICATION_EVENT_ARM, stream, 1U, now_ms);
    HOST_TEST_CHECK(motion_application_model_submit(
                        model, &arm, true, now_ms, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    HOST_TEST_CHECK(strcmp(plan.detail, "ARM_BARRIER_STOP") == 0);
    HOST_TEST_CHECK(model->state == MOTION_CONTROL_ARMED);
    return true;
}

static bool test_arm_command_deadman_and_disarm(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(arm_model(&model, 11U, 0U));

    motion_application_event_t command =
        event(MOTION_APPLICATION_EVENT_COMMAND, 11U, 2U, 10U);
    command.deadman = true;
    command.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    HOST_TEST_CHECK(plan.target_count == 2U);
    HOST_TEST_CHECK(plan.targets[0].rpm == 10);
    HOST_TEST_CHECK(plan.targets[1].rpm == 10);
    motion_application_model_record_actuation(&model, &plan, true);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_ACTIVE);

    command.sequence = 3U;
    command.received_at_ms = 20U;
    command.deadman = false;
    command.vx_mps = 0.4f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 20U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    motion_application_model_record_actuation(&model, &plan, true);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_ARMED);

    motion_application_event_t disarm =
        event(MOTION_APPLICATION_EVENT_DISARM, 11U, 4U, 21U);
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &disarm, true, 21U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_DISARMED);
    return true;
}

static bool test_live_deadman_zero_velocity_applies_hold_zero(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(arm_model(&model, 12U, 0U));

    motion_application_event_t moving =
        event(MOTION_APPLICATION_EVENT_COMMAND, 12U, 2U, 10U);
    moving.deadman = true;
    moving.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &moving, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    motion_application_model_record_actuation(&model, &plan, true);

    motion_application_event_t neutral =
        event(MOTION_APPLICATION_EVENT_COMMAND, 12U, 3U, 20U);
    neutral.deadman = true;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &neutral, true, 20U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    HOST_TEST_CHECK(plan.target_count == 2U);
    HOST_TEST_CHECK(plan.targets[0].rpm == 0);
    HOST_TEST_CHECK(plan.targets[1].rpm == 0);
    motion_application_model_record_actuation(&model, &plan, true);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_ACTIVE);

    neutral.sequence = 4U;
    neutral.received_at_ms = 30U;
    neutral.deadman = false;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &neutral, true, 30U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    return true;
}

static bool test_expiry_stops_and_retires_stream(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(arm_model(&model, 22U, 0U));

    motion_application_event_t command =
        event(MOTION_APPLICATION_EVENT_COMMAND, 22U, 2U, 10U);
    command.deadman = true;
    command.wz_radps = 0.5f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    motion_application_model_record_actuation(&model, &plan, true);

    HOST_TEST_CHECK(motion_application_model_tick(&model, true, 109U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_NONE);
    HOST_TEST_CHECK(motion_application_model_tick(&model, true, 110U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_EXPIRED);

    motion_application_event_t stale_arm =
        event(MOTION_APPLICATION_EVENT_ARM, 22U, 3U, 111U);
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &stale_arm, true, 111U, &plan) ==
                    MOTION_APPLICATION_RESULT_STREAM_RETIRED);
    motion_application_event_t stale_command =
        event(MOTION_APPLICATION_EVENT_COMMAND, 22U, 4U, 112U);
    stale_command.deadman = true;
    stale_command.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &stale_command, true, 112U, &plan) ==
                    MOTION_APPLICATION_RESULT_NOT_ARMED);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_NONE);
    HOST_TEST_CHECK(arm_model(&model, 23U, 112U));
    return true;
}

static bool test_replay_does_not_refresh_lease_and_safety_faults(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));
    HOST_TEST_CHECK(arm_model(&model, 31U, 0U));

    motion_application_event_t command =
        event(MOTION_APPLICATION_EVENT_COMMAND, 31U, 2U, 10U);
    command.deadman = true;
    command.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    command.received_at_ms = 50U;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, true, 50U, &plan) ==
                    MOTION_APPLICATION_RESULT_AUTHORITY_REJECTED);
    HOST_TEST_CHECK(model.last_received_ms == 10U);
    HOST_TEST_CHECK(motion_application_model_tick(&model, true, 110U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);

    HOST_TEST_CHECK(arm_model(&model, 32U, 111U));
    command = event(MOTION_APPLICATION_EVENT_COMMAND, 32U, 2U, 112U);
    command.deadman = true;
    command.vx_mps = 0.1f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &command, false, 112U, &plan) ==
                    MOTION_APPLICATION_RESULT_UNSAFE);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    HOST_TEST_CHECK(model.state == MOTION_CONTROL_FAULT);
    return true;
}

static bool test_stop_has_global_priority_and_stream_history_is_bounded(void)
{
    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(init_model(&model));

    for (uint64_t stream = 1U;
         stream <= MOTION_APPLICATION_RETIRED_STREAMS + 2U;
         ++stream) {
        HOST_TEST_CHECK(arm_model(&model, 100U + stream, stream * 2U));
        motion_application_event_t stop = event(MOTION_APPLICATION_EVENT_STOP,
                                                9999U,
                                                1U,
                                                stream * 2U + 1U);
        HOST_TEST_CHECK(motion_application_model_submit(
                            &model,
                            &stop,
                            true,
                            stop.received_at_ms,
                            &plan) == MOTION_APPLICATION_RESULT_OK);
        HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    }
    HOST_TEST_CHECK(model.retired_stream_count ==
                    MOTION_APPLICATION_RETIRED_STREAMS);
    return true;
}

static bool test_rafa_qualified_geometry_targets_and_ttl(void)
{
    motion_application_model_config_t config = standard_config();
    config.command_ttl_ms = 300U;
    config.velocity_limit = (command_authority_velocity_t){
        .vx = 0.80f,
        .vy = 0.0001f,
        .wz = 0.5235988f,
    };
    config.differential.track_width_m = 1.52;
    config.endpoints[0] = (motion_application_endpoint_config_t){
        .endpoint_id = 1U,
        .name = "rafa_traction_m1",
        .side = ROBOT_KINEMATICS_SIDE_RIGHT,
        .wheel_radius_m = 0.20,
        .motor_to_wheel_ratio = 1.0,
        .direction_sign = 1,
        .max_abs_rpm = 40.0,
    };
    config.endpoints[1] = (motion_application_endpoint_config_t){
        .endpoint_id = 2U,
        .name = "rafa_traction_m2",
        .side = ROBOT_KINEMATICS_SIDE_LEFT,
        .wheel_radius_m = 0.20,
        .motor_to_wheel_ratio = 1.0,
        .direction_sign = -1,
        .max_abs_rpm = 40.0,
    };

    motion_application_model_t model;
    motion_application_plan_t plan;
    HOST_TEST_CHECK(motion_application_model_init(&model, &config) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(model.config.command_ttl_ms == 300U);

    motion_application_event_t arm =
        event(MOTION_APPLICATION_EVENT_ARM, 77U, 1U, 0U);
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &arm, true, 0U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);

    motion_application_event_t forward =
        event(MOTION_APPLICATION_EVENT_COMMAND, 77U, 2U, 10U);
    forward.deadman = true;
    forward.vx_mps = 0.80f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &forward, true, 10U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    HOST_TEST_CHECK(plan.target_count == 2U);
    HOST_TEST_CHECK(plan.targets[0].endpoint_id == 1U);
    HOST_TEST_CHECK(plan.targets[0].rpm == 38);
    HOST_TEST_CHECK(plan.targets[1].endpoint_id == 2U);
    HOST_TEST_CHECK(plan.targets[1].rpm == -38);
    motion_application_model_record_actuation(&model, &plan, true);

    motion_application_event_t release =
        event(MOTION_APPLICATION_EVENT_COMMAND, 77U, 3U, 20U);
    release.deadman = false;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &release, true, 20U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_STOP);
    motion_application_model_record_actuation(&model, &plan, true);

    motion_application_event_t turn =
        event(MOTION_APPLICATION_EVENT_COMMAND, 77U, 4U, 30U);
    turn.deadman = true;
    turn.wz_radps = 0.5235988f;
    HOST_TEST_CHECK(motion_application_model_submit(
                        &model, &turn, true, 30U, &plan) ==
                    MOTION_APPLICATION_RESULT_OK);
    HOST_TEST_CHECK(plan.action == MOTION_APPLICATION_PLAN_APPLY);
    /* Before the controller direction signs, a positive differential turn is
     * approximately left=-19 RPM/right=+19 RPM. Rafa's M2 sign is -1, so the
     * two controller targets are both +19 RPM. */
    HOST_TEST_CHECK(plan.targets[0].rpm == 19);
    HOST_TEST_CHECK(plan.targets[1].rpm == 19);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_arm_command_deadman_and_disarm),
        HOST_TEST_CASE(test_live_deadman_zero_velocity_applies_hold_zero),
        HOST_TEST_CASE(test_expiry_stops_and_retires_stream),
        HOST_TEST_CASE(test_replay_does_not_refresh_lease_and_safety_faults),
        HOST_TEST_CASE(
            test_stop_has_global_priority_and_stream_history_is_bounded),
        HOST_TEST_CASE(test_rafa_qualified_geometry_targets_and_ttl),
        HOST_TEST_CASE(test_rc_source_is_explicit_and_cannot_mix_with_lan),
        HOST_TEST_CASE(test_web_direct_released_deadman_holds_zero_only_for_that_source),
    };
    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
