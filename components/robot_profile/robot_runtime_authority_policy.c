#include "robot_runtime_authority_policy.h"

#include <string.h>

robot_runtime_authority_policy_t robot_runtime_authority_policy_for(
    const robot_profile_t *profile,
    bool request_rafa_lan_only_diagnostic)
{
    const bool is_rafa = profile && profile->name &&
                         strcmp(profile->name, "rafa") == 0;
    const bool lan_only = request_rafa_lan_only_diagnostic && is_rafa;
    const bool profile_interlock = profile && profile->rc_lan_interlock.enabled;

    return (robot_runtime_authority_policy_t){
        .lan_only_diagnostic_active = lan_only,
        .ppm_motion_active = profile && profile->ppm_motion.enabled && !lan_only,
        .rc_lan_interlock_active = profile_interlock && !lan_only,
        /* The diagnostic must not turn observed PPM loss into a LAN STOP. */
        .stop_on_rc_loss = !lan_only && !profile_interlock,
    };
}

const char *robot_runtime_authority_profile_name(
    const robot_profile_t *profile,
    const robot_runtime_authority_policy_t *policy)
{
    if (policy && policy->lan_only_diagnostic_active) {
        return "rafa_lan_only_diagnostic";
    }
    return profile && profile->name ? profile->name : "UNKNOWN";
}
