#include "robot_runtime_authority_policy.h"

#include <string.h>

robot_runtime_authority_policy_t robot_runtime_authority_policy_for(
    const robot_profile_t *profile,
    bool request_rafa_lan_only_diagnostic,
    bool request_rafa_web_joystick_experimental,
    bool request_rafa_softap_web_joystick_experimental)
{
    const bool is_rafa = profile && profile->name &&
                         strcmp(profile->name, "rafa") == 0;
    const bool lan_only = request_rafa_lan_only_diagnostic && is_rafa;
    const bool web_joystick = request_rafa_web_joystick_experimental && is_rafa;
    const bool softap_web_joystick =
        request_rafa_softap_web_joystick_experimental && is_rafa;
    const bool web_direct = web_joystick || softap_web_joystick;
    const bool profile_interlock = profile && profile->rc_lan_interlock.enabled;

    return (robot_runtime_authority_policy_t){
        .lan_only_diagnostic_active = lan_only,
        .web_joystick_experimental_active = web_joystick,
        .softap_web_joystick_experimental_active = softap_web_joystick,
        .web_direct_control_active = web_direct,
        .ppm_motion_active = profile && profile->ppm_motion.enabled && !lan_only && !web_direct,
        .control_lan_active = !web_direct,
        .rc_lan_interlock_active = profile_interlock && !lan_only && !web_direct,
        /* The diagnostic must not turn observed PPM loss into a LAN STOP. */
        .stop_on_rc_loss = !lan_only && !web_direct && !profile_interlock,
    };
}

const char *robot_runtime_authority_profile_name(
    const robot_profile_t *profile,
    const robot_runtime_authority_policy_t *policy)
{
    if (policy && policy->lan_only_diagnostic_active) {
        return "rafa_lan_only_diagnostic";
    }
    if (policy && policy->softap_web_joystick_experimental_active) {
        return "rafa_softap_web_joystick_experimental";
    }
    if (policy && policy->web_joystick_experimental_active) {
        return "rafa_web_joystick_experimental";
    }
    return profile && profile->name ? profile->name : "UNKNOWN";
}
