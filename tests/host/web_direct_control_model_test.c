#include <math.h>

#include "host_test.h"
#include "web_direct_control_model.h"

static bool init(web_direct_control_model_t *model)
{
    return web_direct_control_model_init(
        model,
        &(web_direct_control_model_config_t){
            .ttl_ms = WEB_DIRECT_CONTROL_DEFAULT_TTL_MS,
            .deadzone = WEB_DIRECT_CONTROL_DEFAULT_DEADZONE,
        });
}

static bool test_arm_never_creates_motion(void)
{
    web_direct_control_model_t model;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 10U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(model.state == WEB_DIRECT_MODEL_ARMED);
    return true;
}

static bool test_command_requires_armed_owner(void)
{
    web_direct_control_model_t model;
    web_direct_control_command_t command;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 1U, 1U, 0.4f, 0.0f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_REJECTED_NOT_ARMED);
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 2U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 2U, 3U, 0.4f, 0.0f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_REJECTED_SESSION);
    return true;
}

static bool test_second_client_cannot_claim_or_revive_current_session(void)
{
    web_direct_control_model_t model;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(!web_direct_control_model_claim_session(&model, 2U));
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 0U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    web_direct_control_model_reject_command(&model);
    HOST_TEST_CHECK(!model.lease_seen);
    return true;
}

static bool test_deadzone_and_release_are_zero_deadman_false(void)
{
    web_direct_control_model_t model;
    web_direct_control_command_t command;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 0U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 1U, 10U, 0.09f, -0.05f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(command.zero_intent && !command.deadman);
    HOST_TEST_CHECK(command.forward == 0.0f && command.turn == 0.0f);
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 1U, 20U, 0.0f, 0.0f,
                                                      false, &command) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(command.zero_intent && !command.deadman);
    return true;
}

static bool test_valid_command_renews_then_expires(void)
{
    web_direct_control_model_t model;
    web_direct_control_command_t command;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 0U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 1U, 250U, 0.4f, 0.0f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(!web_direct_control_model_expire(&model, 550U));
    HOST_TEST_CHECK(web_direct_control_model_expire(&model, 551U));
    HOST_TEST_CHECK(model.state == WEB_DIRECT_MODEL_EXPIRED && !model.armed);
    return true;
}

static bool test_disarm_fault_and_old_session_do_not_revive(void)
{
    web_direct_control_model_t model;
    web_direct_control_command_t command;
    HOST_TEST_CHECK(init(&model));
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 1U));
    HOST_TEST_CHECK(web_direct_control_model_arm(&model, 1U, 0U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(web_direct_control_model_disarm(&model, 1U) ==
                    WEB_DIRECT_MODEL_ACCEPTED);
    HOST_TEST_CHECK(!model.armed);
    web_direct_control_model_release_session(&model, 1U);
    HOST_TEST_CHECK(web_direct_control_model_claim_session(&model, 2U));
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 1U, 1U, 0.5f, 0.0f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_REJECTED_SESSION);
    web_direct_control_model_fault(&model);
    HOST_TEST_CHECK(model.state == WEB_DIRECT_MODEL_FAULT && !model.armed);
    HOST_TEST_CHECK(web_direct_control_model_command(&model, 2U, 2U, NAN, 0.0f,
                                                      true, &command) ==
                    WEB_DIRECT_MODEL_REJECTED_ARGUMENT);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_arm_never_creates_motion),
        HOST_TEST_CASE(test_command_requires_armed_owner),
        HOST_TEST_CASE(test_second_client_cannot_claim_or_revive_current_session),
        HOST_TEST_CASE(test_deadzone_and_release_are_zero_deadman_false),
        HOST_TEST_CASE(test_valid_command_renews_then_expires),
        HOST_TEST_CASE(test_disarm_fault_and_old_session_do_not_revive),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, sizeof(cases) / sizeof(cases[0]), stdout));
}
