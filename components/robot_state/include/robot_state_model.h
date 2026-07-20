#ifndef ROBOT_STATE_MODEL_H
#define ROBOT_STATE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_OPERATIONAL_STATE_BOOTING = 0,
    ROBOT_OPERATIONAL_STATE_DISARMED,
    ROBOT_OPERATIONAL_STATE_ARMED,
    ROBOT_OPERATIONAL_STATE_FAULTED,
    ROBOT_OPERATIONAL_STATE_MAINTENANCE,
    ROBOT_OPERATIONAL_STATE_OTA,
} robot_operational_state_t;

typedef uint32_t robot_state_inhibit_mask_t;

#define ROBOT_STATE_INHIBIT_RC_INVALID              (UINT32_C(1) << 0)
#define ROBOT_STATE_INHIBIT_DEADMAN_INACTIVE        (UINT32_C(1) << 1)
#define ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE      (UINT32_C(1) << 2)
#define ROBOT_STATE_INHIBIT_CONTROLLER_STALE        (UINT32_C(1) << 3)
#define ROBOT_STATE_INHIBIT_MOTOR_FAULT             (UINT32_C(1) << 4)
#define ROBOT_STATE_INHIBIT_ESTOP_ACTIVE            (UINT32_C(1) << 5)
#define ROBOT_STATE_INHIBIT_PROFILE_INVALID         (UINT32_C(1) << 6)
#define ROBOT_STATE_INHIBIT_AUTHORITY_MISSING       (UINT32_C(1) << 7)
#define ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED (UINT32_C(1) << 8)
#define ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED        (UINT32_C(1) << 9)
#define ROBOT_STATE_INHIBIT_MOTION_DETECTED         (UINT32_C(1) << 10)
#define ROBOT_STATE_INHIBIT_PARTIAL_APPLY           (UINT32_C(1) << 11)
#define ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN  (UINT32_C(1) << 12)
#define ROBOT_STATE_INHIBIT_SELF_TEST_FAILED        (UINT32_C(1) << 13)

#define ROBOT_STATE_KNOWN_INHIBITS                                                        \
    (ROBOT_STATE_INHIBIT_RC_INVALID | ROBOT_STATE_INHIBIT_DEADMAN_INACTIVE |             \
     ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE | ROBOT_STATE_INHIBIT_CONTROLLER_STALE |     \
     ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |                \
     ROBOT_STATE_INHIBIT_PROFILE_INVALID | ROBOT_STATE_INHIBIT_AUTHORITY_MISSING |        \
     ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED | ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED | \
     ROBOT_STATE_INHIBIT_MOTION_DETECTED | ROBOT_STATE_INHIBIT_PARTIAL_APPLY |            \
     ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN | ROBOT_STATE_INHIBIT_SELF_TEST_FAILED)

typedef enum {
    ROBOT_STATE_ACTION_NONE = 0,
    ROBOT_STATE_ACTION_REQUEST_STOP = UINT32_C(1) << 0,
    ROBOT_STATE_ACTION_EMERGENCY_STOP = UINT32_C(1) << 1,
    ROBOT_STATE_ACTION_REVOKE_AUTHORITY = UINT32_C(1) << 2,
} robot_state_action_t;

typedef enum {
    ROBOT_STATE_OUTCOME_APPLIED = 0,
    ROBOT_STATE_OUTCOME_NO_CHANGE,
    ROBOT_STATE_OUTCOME_REJECTED_STATE,
    ROBOT_STATE_OUTCOME_REJECTED_INHIBITED,
    ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED,
    ROBOT_STATE_OUTCOME_INVALID_ARGUMENT,
} robot_state_outcome_t;

typedef struct {
    robot_operational_state_t state;
    robot_state_inhibit_mask_t active_inhibits;
    robot_state_inhibit_mask_t latched_faults;
    uint32_t revision;
} robot_state_model_t;

typedef struct {
    robot_state_outcome_t outcome;
    robot_operational_state_t previous_state;
    robot_operational_state_t current_state;
    robot_state_inhibit_mask_t blockers;
    uint32_t actions;
    uint32_t revision;
} robot_state_transition_t;

void robot_state_model_init(robot_state_model_t *model);

robot_state_outcome_t robot_state_model_boot_complete(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t boot_failure_inhibits,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_set_inhibits(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t active_inhibits,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_report_fault(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t causes,
    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_request_arm(robot_state_model_t *model,
                                                    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_request_disarm(robot_state_model_t *model,
                                                       robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_request_maintenance(robot_state_model_t *model,
                                                            robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_close_maintenance(robot_state_model_t *model,
                                                          robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_request_ota(robot_state_model_t *model,
                                                    robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_acknowledge_fault(robot_state_model_t *model,
                                                          robot_state_transition_t *transition);

robot_state_outcome_t robot_state_model_authorize_motion(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers);

robot_state_outcome_t robot_state_model_authorize_configuration_write(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers);

robot_state_outcome_t robot_state_model_authorize_ota_install(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers);

const char *robot_operational_state_name(robot_operational_state_t state);
const char *robot_state_outcome_name(robot_state_outcome_t outcome);
const char *robot_state_inhibit_name(robot_state_inhibit_mask_t single_inhibit);

#ifdef __cplusplus
}
#endif

#endif
