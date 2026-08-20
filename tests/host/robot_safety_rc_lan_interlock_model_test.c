#include "host_test.h"

#include "robot_safety_rc_lan_interlock_model.h"

static bool rafa_rc_lan_interlock_contract(void)
{
    const robot_safety_rc_lan_interlock_config_t config = {
        .enabled = true,
        .channel = 5U,
        .active_max_us = 1500U,
    };
    robot_safety_rc_lan_interlock_model_t model;
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_init(&model, &config));

    const uint16_t channels[5] = {1500U, 1500U, 1500U, 1500U, 2000U};
    robot_safety_rc_lan_interlock_snapshot_t snapshot;
    robot_safety_rc_lan_observation_t no_signal = {
        .receiver_available = true,
        .signal_valid = false,
    };
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &no_signal,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_NO_SIGNAL);
    HOST_TEST_CHECK(snapshot.lan_allowed);

    robot_safety_rc_lan_observation_t failsafe = {
        .receiver_available = true,
        .signal_valid = true,
        .channel_count = 5U,
        .channels = channels,
    };
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &failsafe,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_FAILSAFE);
    HOST_TEST_CHECK(snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.channel_us == 2000U);

    uint16_t active_channels[5] = {1500U, 1500U, 1500U, 1500U, 1500U};
    robot_safety_rc_lan_observation_t ppm_active = failsafe;
    ppm_active.channels = active_channels;
    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &ppm_active,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_PRIORITY);
    HOST_TEST_CHECK(!snapshot.lan_allowed);
    HOST_TEST_CHECK(snapshot.priority_epoch == 1U);

    HOST_TEST_CHECK(robot_safety_rc_lan_interlock_model_update(&model,
                                                                 &no_signal,
                                                                 &snapshot));
    HOST_TEST_CHECK(snapshot.state == ROBOT_SAFETY_RC_LAN_PPM_LOST);
    /* The old LAN stream was revoked by the epoch.  A fresh explicit ARM is
     * allowed only now that valid PPM priority is absent. */
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
