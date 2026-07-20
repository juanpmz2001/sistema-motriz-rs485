#include <string.h>

#include "host_test.h"
#include "robot_state_model.h"

static bool boot_to_disarmed(robot_state_model_t *model)
{
    robot_state_transition_t transition;
    robot_state_model_init(model);
    HOST_TEST_CHECK(model->state == ROBOT_OPERATIONAL_STATE_BOOTING);
    HOST_TEST_CHECK(robot_state_model_boot_complete(model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(transition.previous_state == ROBOT_OPERATIONAL_STATE_BOOTING);
    HOST_TEST_CHECK(transition.current_state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK(model->revision == 1);
    return true;
}

static bool test_boot_and_policy_defaults(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    robot_state_inhibit_mask_t blockers = UINT32_MAX;

    robot_state_model_init(&model);
    HOST_TEST_CHECK(robot_state_model_authorize_motion(&model, &blockers) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(blockers == 0);

    HOST_TEST_CHECK(robot_state_model_boot_complete(&model,
                                                    ROBOT_STATE_INHIBIT_SELF_TEST_FAILED,
                                                    &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_FAULTED);
    HOST_TEST_CHECK(model.latched_faults == ROBOT_STATE_INHIBIT_SELF_TEST_FAILED);
    HOST_TEST_CHECK((transition.actions & ROBOT_STATE_ACTION_EMERGENCY_STOP) != 0);
    HOST_TEST_CHECK(robot_state_model_boot_complete(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    return true;
}

static bool test_arm_guards_and_fault_latch(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    robot_state_inhibit_mask_t blockers = 0;
    HOST_TEST_CHECK(boot_to_disarmed(&model));

    robot_state_inhibit_mask_t arm_blockers = ROBOT_STATE_INHIBIT_RC_INVALID |
                                              ROBOT_STATE_INHIBIT_DEADMAN_INACTIVE;
    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, arm_blockers, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED);
    HOST_TEST_CHECK(transition.blockers == arm_blockers);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_ARMED);
    HOST_TEST_CHECK(robot_state_model_authorize_motion(&model, &blockers) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_maintenance(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_ARMED);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model,
                                                   ROBOT_STATE_INHIBIT_CONTROLLER_STALE,
                                                   &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_FAULTED);
    HOST_TEST_CHECK(model.latched_faults == ROBOT_STATE_INHIBIT_CONTROLLER_STALE);
    HOST_TEST_CHECK((transition.actions & ROBOT_STATE_ACTION_EMERGENCY_STOP) != 0);
    HOST_TEST_CHECK(robot_state_model_authorize_motion(&model, &blockers) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);

    HOST_TEST_CHECK(robot_state_model_acknowledge_fault(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED);
    HOST_TEST_CHECK(transition.blockers == ROBOT_STATE_INHIBIT_CONTROLLER_STALE);
    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_FAULTED);
    HOST_TEST_CHECK(model.latched_faults == ROBOT_STATE_INHIBIT_CONTROLLER_STALE);
    HOST_TEST_CHECK(robot_state_model_acknowledge_fault(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK(model.latched_faults == 0);
    return true;
}

static bool test_explicit_disarm(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    HOST_TEST_CHECK(boot_to_disarmed(&model));
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_disarm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK((transition.actions & ROBOT_STATE_ACTION_REQUEST_STOP) != 0);
    HOST_TEST_CHECK((transition.actions & ROBOT_STATE_ACTION_REVOKE_AUTHORITY) != 0);
    HOST_TEST_CHECK(robot_state_model_request_disarm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_NO_CHANGE);
    return true;
}

static bool test_expired_authority_requires_new_lease_after_ack(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    HOST_TEST_CHECK(boot_to_disarmed(&model));
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);

    HOST_TEST_CHECK(robot_state_model_report_fault(
                        &model,
                        ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED,
                        &transition) == ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_FAULTED);
    HOST_TEST_CHECK(robot_state_model_acknowledge_fault(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED);
    HOST_TEST_CHECK(transition.blockers == ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    return true;
}

static bool test_maintenance_write_policy(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    robot_state_inhibit_mask_t blockers = 0;
    HOST_TEST_CHECK(boot_to_disarmed(&model));

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model,
                                                   ROBOT_STATE_INHIBIT_CONTROLLER_STALE,
                                                   &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_maintenance(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_MAINTENANCE);
    HOST_TEST_CHECK(robot_state_model_request_ota(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(robot_state_model_authorize_motion(&model, NULL) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(robot_state_model_authorize_configuration_write(&model, &blockers) ==
                    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED);
    HOST_TEST_CHECK(blockers == ROBOT_STATE_INHIBIT_CONTROLLER_STALE);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_authorize_configuration_write(&model, &blockers) ==
                    ROBOT_STATE_OUTCOME_APPLIED);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model,
                                                   ROBOT_STATE_INHIBIT_MOTION_DETECTED,
                                                   &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_FAULTED);
    HOST_TEST_CHECK(model.latched_faults == ROBOT_STATE_INHIBIT_MOTION_DETECTED);
    return true;
}

static bool test_exclusive_states_and_validation(void)
{
    robot_state_model_t model;
    robot_state_transition_t transition;
    HOST_TEST_CHECK(boot_to_disarmed(&model));

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model,
                                                   ROBOT_STATE_INHIBIT_CONTROLLER_STALE,
                                                   &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_ota(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_DISARMED);
    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model, 0, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(robot_state_model_request_ota(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_APPLIED);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_OTA);
    HOST_TEST_CHECK(robot_state_model_request_maintenance(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(robot_state_model_request_arm(&model, &transition) ==
                    ROBOT_STATE_OUTCOME_REJECTED_STATE);
    HOST_TEST_CHECK(robot_state_model_authorize_ota_install(&model, NULL) ==
                    ROBOT_STATE_OUTCOME_APPLIED);

    HOST_TEST_CHECK(robot_state_model_set_inhibits(&model,
                                                   UINT32_C(1) << 31,
                                                   &transition) ==
                    ROBOT_STATE_OUTCOME_INVALID_ARGUMENT);
    HOST_TEST_CHECK(model.state == ROBOT_OPERATIONAL_STATE_OTA);
    HOST_TEST_CHECK(strcmp(robot_operational_state_name(model.state), "OTA") == 0);
    HOST_TEST_CHECK(strcmp(robot_state_inhibit_name(ROBOT_STATE_INHIBIT_MOTOR_FAULT),
                           "MOTOR_FAULT") == 0);
    HOST_TEST_CHECK(strcmp(robot_state_outcome_name(ROBOT_STATE_OUTCOME_REJECTED_STATE),
                           "REJECTED_STATE") == 0);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_boot_and_policy_defaults),
        HOST_TEST_CASE(test_arm_guards_and_fault_latch),
        HOST_TEST_CASE(test_explicit_disarm),
        HOST_TEST_CASE(test_expired_authority_requires_new_lease_after_ack),
        HOST_TEST_CASE(test_maintenance_write_policy),
        HOST_TEST_CASE(test_exclusive_states_and_validation),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
