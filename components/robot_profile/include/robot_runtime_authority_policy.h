#ifndef ROBOT_RUNTIME_AUTHORITY_POLICY_H
#define ROBOT_RUNTIME_AUTHORITY_POLICY_H

#include <stdbool.h>

#include "robot_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A build-selected diagnostic mode may alter source participation without
 * mutating the immutable hardware profile. This projection owns only authority
 * participation; TTL, endpoint limits, SVD safety and STOP keep their owners. */
typedef struct {
    bool lan_only_diagnostic_active;
    bool ppm_motion_active;
    bool rc_lan_interlock_active;
    bool stop_on_rc_loss;
} robot_runtime_authority_policy_t;

robot_runtime_authority_policy_t robot_runtime_authority_policy_for(
    const robot_profile_t *profile,
    bool request_rafa_lan_only_diagnostic);

const char *robot_runtime_authority_profile_name(
    const robot_profile_t *profile,
    const robot_runtime_authority_policy_t *policy);

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_RUNTIME_AUTHORITY_POLICY_H */
