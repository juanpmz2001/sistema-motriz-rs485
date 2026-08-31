#include "host_test.h"

#include "robot_safety_rc_lan_interlock_model.h"

static bool update(robot_safety_rc_lan_interlock_model_t *model,
                   const uint16_t channels[5],
                   uint32_t sequence,
                   robot_safety_rc_lan_interlock_snapshot_t *snapshot)
{
    const robot_safety_rc_lan_observation_t observation = {
        .receiver_available = true,
        .signal_valid = true,
        .valid_frame_sequence = sequence,
        .channel_count = 5U,
        .channels = channels,
    };
    return robot_safety_rc_lan_interlock_model_update(model, &observation, snapshot);
}

static bool rafa_rc_lan_interlock_contract(void)
{
    const robot_safety_rc_lan_interlock_config_t config = {
        .enabled = true,
        .channel = 5U,
        .active_max_us = 1500U,
        .transition_confirm_good_frames = 3U,
    };
    robot_safety_rc_lan_interlock_model_t model;
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_init(&model, &config));
    robot_safety_rc_lan_interlock_snapshot_t snapshot;
    const uint16_t failsafe[5] = {1500U, 1500U, 1500U, 1500U, 2000U};
    const uint16_t ppm_active[5] = {1500U, 1500U, 1500U, 1500U, 1500U};

    const robot_safety_rc_lan_observation_t no_signal = {
        .receiver_available = true,
        .signal_valid = false,
    };
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &no_signal,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_NO_SIGNAL);
    HOST_TEST_CHECK(snapshot.lan_allowed);

    /* Establish the CH5 failsafe baseline with three distinct accepted frames. */
    HOST_TEST_CHECK(update(&model, failsafe, 1U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE);
    HOST_TEST_CHECK(snapshot.lan_allowed);
    HOST_TEST_CHECK(update(&model, failsafe, 2U, &snapshot));
    HOST_TEST_CHECK(update(&model, failsafe, 3U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE);
    HOST_TEST_CHECK(snapshot.lan_allowed);

    /* One malformed/corrupt candidate cannot revoke LAN or advance epoch. */
    HOST_TEST_CHECK(update(&model, ppm_active, 4U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE);
    HOST_TEST_CHECK(snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.priority_epoch == 0U);
    /* A reverse valid frame cancels the candidate; a cached sequence does not
     * add a synthetic confirmation. */
    HOST_TEST_CHECK(update(&model, failsafe, 5U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE);
    HOST_TEST_CHECK(update(&model, ppm_active, 5U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE);

    HOST_TEST_CHECK(update(&model, ppm_active, 6U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE);
    HOST_TEST_CHECK(update(&model, ppm_active, 7U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY_CANDIDATE);
    HOST_TEST_CHECK(update(&model, ppm_active, 8U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY);
    HOST_TEST_CHECK(!snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.priority_epoch == 1U);

    /* A transient CH5 failsafe candidate retains the already committed PPM
     * authority. Consumers that gate PPM motion must use lan_allowed, not
     * the presentation candidate enum, or a single glitch becomes a STOP. */
    HOST_TEST_CHECK(update(&model, failsafe, 9U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE);
    HOST_TEST_CHECK(!snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.priority_epoch == 1U);
    HOST_TEST_CHECK(update(&model, failsafe, 10U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE_CANDIDATE);
    HOST_TEST_CHECK(!snapshot.lan_allowed);
    HOST_TEST_CHECK(update(&model, failsafe, 11U, &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE);
    HOST_TEST_CHECK(snapshot.lan_allowed);

    /* Stale/lost signal remains immediate relative to the existing receiver
     * timeout; confirmation never stretches its safety deadline. */
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &no_signal,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_LOST);
    HOST_TEST_CHECK(snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.priority_epoch == 1U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(rafa_rc_lan_interlock_contract),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
