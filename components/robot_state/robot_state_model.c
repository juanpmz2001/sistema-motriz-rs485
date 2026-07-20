#include "robot_state_model.h"

#include <stddef.h>

#define ROBOT_STATE_ARM_BLOCKERS                                                        \
    (ROBOT_STATE_INHIBIT_RC_INVALID | ROBOT_STATE_INHIBIT_DEADMAN_INACTIVE |            \
     ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE | ROBOT_STATE_INHIBIT_CONTROLLER_STALE |    \
     ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |               \
     ROBOT_STATE_INHIBIT_PROFILE_INVALID | ROBOT_STATE_INHIBIT_AUTHORITY_MISSING |       \
     ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED | ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED | \
     ROBOT_STATE_INHIBIT_MOTION_DETECTED | ROBOT_STATE_INHIBIT_PARTIAL_APPLY |           \
     ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN | ROBOT_STATE_INHIBIT_SELF_TEST_FAILED)

#define ROBOT_STATE_MAINTENANCE_ENTRY_BLOCKERS                                          \
    (ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |               \
     ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED | ROBOT_STATE_INHIBIT_MOTION_DETECTED |        \
     ROBOT_STATE_INHIBIT_PARTIAL_APPLY | ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN |   \
     ROBOT_STATE_INHIBIT_SELF_TEST_FAILED)

#define ROBOT_STATE_MAINTENANCE_WRITE_BLOCKERS                                          \
    (ROBOT_STATE_MAINTENANCE_ENTRY_BLOCKERS | ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE |  \
     ROBOT_STATE_INHIBIT_CONTROLLER_STALE)

#define ROBOT_STATE_MAINTENANCE_TRIP_INHIBITS                                           \
    (ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |               \
     ROBOT_STATE_INHIBIT_MOTION_DETECTED | ROBOT_STATE_INHIBIT_PARTIAL_APPLY |          \
     ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN)

#define ROBOT_STATE_OTA_ENTRY_BLOCKERS                                                  \
    (ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE | ROBOT_STATE_INHIBIT_CONTROLLER_STALE |     \
     ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |                \
     ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED | ROBOT_STATE_INHIBIT_MOTION_DETECTED |        \
     ROBOT_STATE_INHIBIT_PARTIAL_APPLY | ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN |   \
     ROBOT_STATE_INHIBIT_SELF_TEST_FAILED)

#define ROBOT_STATE_OTA_TRIP_INHIBITS                                                   \
    (ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |               \
     ROBOT_STATE_INHIBIT_MOTION_DETECTED)

#define ROBOT_STATE_FAULT_ACK_BLOCKERS                                                   \
    (ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE | ROBOT_STATE_INHIBIT_CONTROLLER_STALE |     \
     ROBOT_STATE_INHIBIT_MOTOR_FAULT | ROBOT_STATE_INHIBIT_ESTOP_ACTIVE |                \
     ROBOT_STATE_INHIBIT_PROFILE_INVALID | ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED |         \
     ROBOT_STATE_INHIBIT_MOTION_DETECTED | ROBOT_STATE_INHIBIT_PARTIAL_APPLY |           \
     ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN | ROBOT_STATE_INHIBIT_SELF_TEST_FAILED)

static bool has_only_known_inhibits(robot_state_inhibit_mask_t inhibits)
{
    return (inhibits & ~ROBOT_STATE_KNOWN_INHIBITS) == 0;
}

static void set_transition(const robot_state_model_t *model,
                           robot_operational_state_t previous_state,
                           robot_state_outcome_t outcome,
                           robot_state_inhibit_mask_t blockers,
                           uint32_t actions,
                           robot_state_transition_t *transition)
{
    if (!transition) {
        return;
    }

    transition->outcome = outcome;
    transition->previous_state = previous_state;
    transition->current_state = model ? model->state : ROBOT_OPERATIONAL_STATE_BOOTING;
    transition->blockers = blockers;
    transition->actions = actions;
    transition->revision = model ? model->revision : 0;
}

static robot_state_outcome_t reject_invalid(robot_state_transition_t *transition)
{
    set_transition(NULL,
                   ROBOT_OPERATIONAL_STATE_BOOTING,
                   ROBOT_STATE_OUTCOME_INVALID_ARGUMENT,
                   0,
                   ROBOT_STATE_ACTION_NONE,
                   transition);
    return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
}

static robot_state_outcome_t finish(robot_state_model_t *model,
                                    robot_operational_state_t previous_state,
                                    bool changed,
                                    robot_state_inhibit_mask_t blockers,
                                    uint32_t actions,
                                    robot_state_transition_t *transition)
{
    robot_state_outcome_t outcome = changed ? ROBOT_STATE_OUTCOME_APPLIED
                                            : ROBOT_STATE_OUTCOME_NO_CHANGE;
    if (changed) {
        model->revision++;
    }
    set_transition(model, previous_state, outcome, blockers, actions, transition);
    return outcome;
}

static robot_state_outcome_t reject(robot_state_model_t *model,
                                    robot_state_outcome_t outcome,
                                    robot_state_inhibit_mask_t blockers,
                                    robot_state_transition_t *transition)
{
    set_transition(model,
                   model->state,
                   outcome,
                   blockers,
                   ROBOT_STATE_ACTION_NONE,
                   transition);
    return outcome;
}

static robot_state_inhibit_mask_t trip_inhibits_for_state(robot_operational_state_t state)
{
    switch (state) {
    case ROBOT_OPERATIONAL_STATE_ARMED:
        return ROBOT_STATE_ARM_BLOCKERS;
    case ROBOT_OPERATIONAL_STATE_MAINTENANCE:
        return ROBOT_STATE_MAINTENANCE_TRIP_INHIBITS;
    case ROBOT_OPERATIONAL_STATE_OTA:
        return ROBOT_STATE_OTA_TRIP_INHIBITS;
    default:
        return 0;
    }
}

void robot_state_model_init(robot_state_model_t *model)
{
    if (!model) {
        return;
    }

    model->state = ROBOT_OPERATIONAL_STATE_BOOTING;
    model->active_inhibits = 0;
    model->latched_faults = 0;
    model->revision = 0;
}

robot_state_outcome_t robot_state_model_boot_complete(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t boot_failure_inhibits,
    robot_state_transition_t *transition)
{
    if (!model || !has_only_known_inhibits(boot_failure_inhibits)) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_BOOTING) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }

    robot_operational_state_t previous_state = model->state;
    uint32_t actions = ROBOT_STATE_ACTION_NONE;
    robot_state_inhibit_mask_t failure_causes =
        boot_failure_inhibits |
        (model->active_inhibits & ROBOT_STATE_INHIBIT_SELF_TEST_FAILED);
    if (failure_causes == 0) {
        model->state = ROBOT_OPERATIONAL_STATE_DISARMED;
    } else {
        model->active_inhibits |= failure_causes;
        model->latched_faults |= failure_causes;
        model->state = ROBOT_OPERATIONAL_STATE_FAULTED;
        actions = ROBOT_STATE_ACTION_EMERGENCY_STOP |
                  ROBOT_STATE_ACTION_REVOKE_AUTHORITY;
    }

    return finish(model,
                  previous_state,
                  true,
                  failure_causes,
                  actions,
                  transition);
}

robot_state_outcome_t robot_state_model_set_inhibits(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t active_inhibits,
    robot_state_transition_t *transition)
{
    if (!model || !has_only_known_inhibits(active_inhibits)) {
        return reject_invalid(transition);
    }

    robot_operational_state_t previous_state = model->state;
    bool changed = model->active_inhibits != active_inhibits;
    uint32_t actions = ROBOT_STATE_ACTION_NONE;
    model->active_inhibits = active_inhibits;

    robot_state_inhibit_mask_t trip_causes =
        active_inhibits & trip_inhibits_for_state(model->state);
    if (trip_causes != 0) {
        robot_state_inhibit_mask_t previous_latched = model->latched_faults;
        model->latched_faults |= trip_causes;
        model->state = ROBOT_OPERATIONAL_STATE_FAULTED;
        changed = changed || previous_state != model->state ||
                  previous_latched != model->latched_faults;
        actions = ROBOT_STATE_ACTION_EMERGENCY_STOP |
                  ROBOT_STATE_ACTION_REVOKE_AUTHORITY;
    }

    return finish(model, previous_state, changed, trip_causes, actions, transition);
}

robot_state_outcome_t robot_state_model_report_fault(
    robot_state_model_t *model,
    robot_state_inhibit_mask_t causes,
    robot_state_transition_t *transition)
{
    if (!model || causes == 0 || !has_only_known_inhibits(causes)) {
        return reject_invalid(transition);
    }

    robot_operational_state_t previous_state = model->state;
    robot_state_inhibit_mask_t previous_active = model->active_inhibits;
    robot_state_inhibit_mask_t previous_latched = model->latched_faults;
    model->active_inhibits |= causes;
    model->latched_faults |= causes;
    model->state = ROBOT_OPERATIONAL_STATE_FAULTED;

    bool changed = previous_state != model->state || previous_active != model->active_inhibits ||
                   previous_latched != model->latched_faults;
    return finish(model,
                  previous_state,
                  changed,
                  causes,
                  ROBOT_STATE_ACTION_EMERGENCY_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

robot_state_outcome_t robot_state_model_request_arm(robot_state_model_t *model,
                                                    robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_DISARMED) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }
    if (model->latched_faults != 0) {
        return reject(model,
                      ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED,
                      model->latched_faults,
                      transition);
    }

    robot_state_inhibit_mask_t blockers = model->active_inhibits & ROBOT_STATE_ARM_BLOCKERS;
    if (blockers != 0) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_INHIBITED, blockers, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->state = ROBOT_OPERATIONAL_STATE_ARMED;
    return finish(model, previous_state, true, 0, ROBOT_STATE_ACTION_NONE, transition);
}

robot_state_outcome_t robot_state_model_request_disarm(robot_state_model_t *model,
                                                       robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state == ROBOT_OPERATIONAL_STATE_DISARMED) {
        return finish(model,
                      model->state,
                      false,
                      0,
                      ROBOT_STATE_ACTION_NONE,
                      transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_ARMED) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->state = ROBOT_OPERATIONAL_STATE_DISARMED;
    return finish(model,
                  previous_state,
                  true,
                  0,
                  ROBOT_STATE_ACTION_REQUEST_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

robot_state_outcome_t robot_state_model_request_maintenance(robot_state_model_t *model,
                                                            robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_DISARMED) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }
    if (model->latched_faults != 0) {
        return reject(model,
                      ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED,
                      model->latched_faults,
                      transition);
    }

    robot_state_inhibit_mask_t blockers =
        model->active_inhibits & ROBOT_STATE_MAINTENANCE_ENTRY_BLOCKERS;
    if (blockers != 0) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_INHIBITED, blockers, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->state = ROBOT_OPERATIONAL_STATE_MAINTENANCE;
    return finish(model,
                  previous_state,
                  true,
                  0,
                  ROBOT_STATE_ACTION_REQUEST_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

robot_state_outcome_t robot_state_model_close_maintenance(robot_state_model_t *model,
                                                          robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_MAINTENANCE) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->state = ROBOT_OPERATIONAL_STATE_DISARMED;
    return finish(model,
                  previous_state,
                  true,
                  0,
                  ROBOT_STATE_ACTION_REQUEST_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

robot_state_outcome_t robot_state_model_request_ota(robot_state_model_t *model,
                                                    robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_DISARMED) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }
    if (model->latched_faults != 0) {
        return reject(model,
                      ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED,
                      model->latched_faults,
                      transition);
    }

    robot_state_inhibit_mask_t blockers = model->active_inhibits & ROBOT_STATE_OTA_ENTRY_BLOCKERS;
    if (blockers != 0) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_INHIBITED, blockers, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->state = ROBOT_OPERATIONAL_STATE_OTA;
    return finish(model,
                  previous_state,
                  true,
                  0,
                  ROBOT_STATE_ACTION_REQUEST_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

robot_state_outcome_t robot_state_model_acknowledge_fault(robot_state_model_t *model,
                                                          robot_state_transition_t *transition)
{
    if (!model) {
        return reject_invalid(transition);
    }
    if (model->state != ROBOT_OPERATIONAL_STATE_FAULTED) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_STATE, 0, transition);
    }

    robot_state_inhibit_mask_t blockers =
        model->active_inhibits & ROBOT_STATE_FAULT_ACK_BLOCKERS;
    if (blockers != 0) {
        return reject(model, ROBOT_STATE_OUTCOME_REJECTED_INHIBITED, blockers, transition);
    }

    robot_operational_state_t previous_state = model->state;
    model->latched_faults = 0;
    model->state = ROBOT_OPERATIONAL_STATE_DISARMED;
    return finish(model,
                  previous_state,
                  true,
                  0,
                  ROBOT_STATE_ACTION_REQUEST_STOP |
                      ROBOT_STATE_ACTION_REVOKE_AUTHORITY,
                  transition);
}

static robot_state_outcome_t authorize(const robot_state_model_t *model,
                                       robot_operational_state_t required_state,
                                       robot_state_inhibit_mask_t inhibit_mask,
                                       robot_state_inhibit_mask_t *blockers)
{
    if (blockers) {
        *blockers = 0;
    }
    if (!model) {
        return ROBOT_STATE_OUTCOME_INVALID_ARGUMENT;
    }
    if (model->state != required_state) {
        return ROBOT_STATE_OUTCOME_REJECTED_STATE;
    }
    if (model->latched_faults != 0) {
        if (blockers) {
            *blockers = model->latched_faults;
        }
        return ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED;
    }

    robot_state_inhibit_mask_t active_blockers = model->active_inhibits & inhibit_mask;
    if (active_blockers != 0) {
        if (blockers) {
            *blockers = active_blockers;
        }
        return ROBOT_STATE_OUTCOME_REJECTED_INHIBITED;
    }
    return ROBOT_STATE_OUTCOME_APPLIED;
}

robot_state_outcome_t robot_state_model_authorize_motion(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers)
{
    return authorize(model,
                     ROBOT_OPERATIONAL_STATE_ARMED,
                     ROBOT_STATE_ARM_BLOCKERS,
                     blockers);
}

robot_state_outcome_t robot_state_model_authorize_configuration_write(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers)
{
    return authorize(model,
                     ROBOT_OPERATIONAL_STATE_MAINTENANCE,
                     ROBOT_STATE_MAINTENANCE_WRITE_BLOCKERS,
                     blockers);
}

robot_state_outcome_t robot_state_model_authorize_ota_install(
    const robot_state_model_t *model,
    robot_state_inhibit_mask_t *blockers)
{
    return authorize(model,
                     ROBOT_OPERATIONAL_STATE_OTA,
                     ROBOT_STATE_OTA_ENTRY_BLOCKERS,
                     blockers);
}

const char *robot_operational_state_name(robot_operational_state_t state)
{
    switch (state) {
    case ROBOT_OPERATIONAL_STATE_BOOTING:
        return "BOOTING";
    case ROBOT_OPERATIONAL_STATE_DISARMED:
        return "DISARMED";
    case ROBOT_OPERATIONAL_STATE_ARMED:
        return "ARMED";
    case ROBOT_OPERATIONAL_STATE_FAULTED:
        return "FAULTED";
    case ROBOT_OPERATIONAL_STATE_MAINTENANCE:
        return "MAINTENANCE";
    case ROBOT_OPERATIONAL_STATE_OTA:
        return "OTA";
    default:
        return "UNKNOWN";
    }
}

const char *robot_state_outcome_name(robot_state_outcome_t outcome)
{
    switch (outcome) {
    case ROBOT_STATE_OUTCOME_APPLIED:
        return "APPLIED";
    case ROBOT_STATE_OUTCOME_NO_CHANGE:
        return "NO_CHANGE";
    case ROBOT_STATE_OUTCOME_REJECTED_STATE:
        return "REJECTED_STATE";
    case ROBOT_STATE_OUTCOME_REJECTED_INHIBITED:
        return "REJECTED_INHIBITED";
    case ROBOT_STATE_OUTCOME_REJECTED_FAULT_LATCHED:
        return "REJECTED_FAULT_LATCHED";
    case ROBOT_STATE_OUTCOME_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    default:
        return "UNKNOWN";
    }
}

const char *robot_state_inhibit_name(robot_state_inhibit_mask_t single_inhibit)
{
    switch (single_inhibit) {
    case ROBOT_STATE_INHIBIT_RC_INVALID:
        return "RC_INVALID";
    case ROBOT_STATE_INHIBIT_DEADMAN_INACTIVE:
        return "DEADMAN_INACTIVE";
    case ROBOT_STATE_INHIBIT_CONTROLLER_OFFLINE:
        return "CONTROLLER_OFFLINE";
    case ROBOT_STATE_INHIBIT_CONTROLLER_STALE:
        return "CONTROLLER_STALE";
    case ROBOT_STATE_INHIBIT_MOTOR_FAULT:
        return "MOTOR_FAULT";
    case ROBOT_STATE_INHIBIT_ESTOP_ACTIVE:
        return "ESTOP_ACTIVE";
    case ROBOT_STATE_INHIBIT_PROFILE_INVALID:
        return "PROFILE_INVALID";
    case ROBOT_STATE_INHIBIT_AUTHORITY_MISSING:
        return "AUTHORITY_MISSING";
    case ROBOT_STATE_INHIBIT_AUTHORITY_LEASE_EXPIRED:
        return "AUTHORITY_LEASE_EXPIRED";
    case ROBOT_STATE_INHIBIT_STOP_UNCONFIRMED:
        return "STOP_UNCONFIRMED";
    case ROBOT_STATE_INHIBIT_MOTION_DETECTED:
        return "MOTION_DETECTED";
    case ROBOT_STATE_INHIBIT_PARTIAL_APPLY:
        return "PARTIAL_APPLY";
    case ROBOT_STATE_INHIBIT_CONFIG_OUTCOME_UNKNOWN:
        return "CONFIG_OUTCOME_UNKNOWN";
    case ROBOT_STATE_INHIBIT_SELF_TEST_FAILED:
        return "SELF_TEST_FAILED";
    default:
        return "UNKNOWN";
    }
}
