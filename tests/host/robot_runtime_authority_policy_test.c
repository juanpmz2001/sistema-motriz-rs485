#include "host_test.h"

#include <string.h>

#include "robot_runtime_authority_policy.h"

static robot_profile_t rafa_profile(void)
{
    robot_profile_t profile = {0};
    profile.name = "rafa";
    profile.ppm_motion.enabled = true;
    profile.rc_lan_interlock.enabled = true;
    return profile;
}

static bool normal_rafa_keeps_ppm_authority(void)
{
    robot_profile_t profile = rafa_profile();
    const robot_runtime_authority_policy_t policy =
        robot_runtime_authority_policy_for(&profile, false, false);
    HOST_TEST_CHECK(!policy.lan_only_diagnostic_active);
    HOST_TEST_CHECK(!policy.web_joystick_experimental_active);
    HOST_TEST_CHECK(policy.ppm_motion_active);
    HOST_TEST_CHECK(policy.control_lan_active);
    HOST_TEST_CHECK(policy.rc_lan_interlock_active);
    HOST_TEST_CHECK(!policy.stop_on_rc_loss);
    HOST_TEST_CHECK(strcmp(robot_runtime_authority_profile_name(&profile, &policy),
                           "rafa") == 0);
    return true;
}

static bool lan_only_rafa_observes_ppm_without_authority(void)
{
    robot_profile_t profile = rafa_profile();
    const robot_runtime_authority_policy_t policy =
        robot_runtime_authority_policy_for(&profile, true, false);
    HOST_TEST_CHECK(policy.lan_only_diagnostic_active);
    HOST_TEST_CHECK(!policy.web_joystick_experimental_active);
    HOST_TEST_CHECK(!policy.ppm_motion_active);
    HOST_TEST_CHECK(policy.control_lan_active);
    HOST_TEST_CHECK(!policy.rc_lan_interlock_active);
    HOST_TEST_CHECK(!policy.stop_on_rc_loss);
    HOST_TEST_CHECK(strcmp(robot_runtime_authority_profile_name(&profile, &policy),
                           "rafa_lan_only_diagnostic") == 0);
    return true;
}

static bool request_does_not_mutate_other_profiles(void)
{
    robot_profile_t profile = {0};
    profile.name = "current_robot";
    const robot_runtime_authority_policy_t policy =
        robot_runtime_authority_policy_for(&profile, true, false);
    HOST_TEST_CHECK(!policy.lan_only_diagnostic_active);
    HOST_TEST_CHECK(!policy.web_joystick_experimental_active);
    HOST_TEST_CHECK(!policy.ppm_motion_active);
    HOST_TEST_CHECK(policy.control_lan_active);
    HOST_TEST_CHECK(!policy.rc_lan_interlock_active);
    HOST_TEST_CHECK(policy.stop_on_rc_loss);
    HOST_TEST_CHECK(strcmp(robot_runtime_authority_profile_name(&profile, &policy),
                           "current_robot") == 0);
    return true;
}

static bool web_experiment_isolated_from_ppm_and_udp_lan(void)
{
    robot_profile_t profile = rafa_profile();
    const robot_runtime_authority_policy_t policy =
        robot_runtime_authority_policy_for(&profile, false, true);
    HOST_TEST_CHECK(policy.web_joystick_experimental_active);
    HOST_TEST_CHECK(!policy.lan_only_diagnostic_active);
    HOST_TEST_CHECK(!policy.ppm_motion_active);
    HOST_TEST_CHECK(!policy.control_lan_active);
    HOST_TEST_CHECK(!policy.rc_lan_interlock_active);
    HOST_TEST_CHECK(!policy.stop_on_rc_loss);
    HOST_TEST_CHECK(strcmp(robot_runtime_authority_profile_name(&profile, &policy),
                           "rafa_web_joystick_experimental") == 0);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(normal_rafa_keeps_ppm_authority),
        HOST_TEST_CASE(lan_only_rafa_observes_ppm_without_authority),
        HOST_TEST_CASE(request_does_not_mutate_other_profiles),
        HOST_TEST_CASE(web_experiment_isolated_from_ppm_and_udp_lan),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
