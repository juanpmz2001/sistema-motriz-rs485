#include "communication_quality_model.h"

#include <limits.h>
#include <string.h>

static bool policy_valid(const communication_quality_policy_t *policy)
{
    return policy && policy->warning_age_ms > 0U &&
           policy->warning_age_ms <= policy->stale_age_ms &&
           policy->stale_age_ms < policy->offline_age_ms &&
           policy->suspect_failure_count > 0U &&
           policy->suspect_failure_count <= policy->degraded_failure_count &&
           policy->recovery_confirm_count > 0U;
}

static communication_health_t state_at(const communication_quality_model_t *model,
                                       uint32_t now_ms)
{
    if (!model->has_last_good) {
        return COMMUNICATION_HEALTH_UNKNOWN;
    }
    const uint32_t age_ms = now_ms - model->last_good_ms;
    if (age_ms > model->policy.offline_age_ms) {
        return COMMUNICATION_HEALTH_OFFLINE;
    }
    if (age_ms > model->policy.stale_age_ms) {
        return COMMUNICATION_HEALTH_STALE;
    }
    if (model->recovery_pending) {
        return COMMUNICATION_HEALTH_SUSPECT;
    }
    if (model->consecutive_failures >= model->policy.degraded_failure_count) {
        return COMMUNICATION_HEALTH_DEGRADED;
    }
    if (model->consecutive_failures >= model->policy.suspect_failure_count ||
        age_ms > model->policy.warning_age_ms) {
        return COMMUNICATION_HEALTH_SUSPECT;
    }
    return COMMUNICATION_HEALTH_HEALTHY;
}

bool communication_quality_model_init(
    communication_quality_model_t *model,
    const communication_quality_policy_t *policy)
{
    if (!model || !policy_valid(policy)) {
        return false;
    }
    memset(model, 0, sizeof(*model));
    model->policy = *policy;
    model->initialized = true;
    return true;
}

void communication_quality_model_record_good(communication_quality_model_t *model,
                                             uint32_t now_ms)
{
    if (!model || !model->initialized) {
        return;
    }
    const communication_health_t previous = state_at(model, now_ms);
    if (previous == COMMUNICATION_HEALTH_DEGRADED ||
        previous == COMMUNICATION_HEALTH_STALE ||
        previous == COMMUNICATION_HEALTH_OFFLINE) {
        model->recovery_pending = true;
        model->consecutive_good = 0U;
    }
    model->has_last_good = true;
    model->last_good_ms = now_ms;
    model->consecutive_failures = 0U;
    model->consecutive_good++;
    model->total_good++;
    if (model->recovery_pending &&
        model->consecutive_good >= model->policy.recovery_confirm_count) {
        model->recovery_pending = false;
    }
}

void communication_quality_model_record_failure(communication_quality_model_t *model,
                                                uint32_t now_ms,
                                                uint32_t raw_failure_reason)
{
    if (!model || !model->initialized) {
        return;
    }
    model->last_failure_ms = now_ms;
    model->last_failure_reason = raw_failure_reason;
    model->consecutive_good = 0U;
    if (model->consecutive_failures < UINT32_MAX) {
        model->consecutive_failures++;
    }
    model->total_failures++;
}

bool communication_quality_model_snapshot(const communication_quality_model_t *model,
                                          uint32_t now_ms,
                                          communication_quality_snapshot_t *snapshot)
{
    if (!model || !model->initialized || !snapshot) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->has_last_good = model->has_last_good;
    snapshot->last_good_age_ms = model->has_last_good
                                     ? now_ms - model->last_good_ms
                                     : UINT32_MAX;
    snapshot->last_failure_age_ms = model->total_failures > 0U
                                        ? now_ms - model->last_failure_ms
                                        : UINT32_MAX;
    snapshot->last_failure_reason = model->last_failure_reason;
    snapshot->consecutive_good = model->consecutive_good;
    snapshot->consecutive_failures = model->consecutive_failures;
    snapshot->total_good = model->total_good;
    snapshot->total_failures = model->total_failures;
    snapshot->raw_state = !model->has_last_good
                              ? COMMUNICATION_HEALTH_UNKNOWN
                              : model->consecutive_failures == 0U
                                    ? COMMUNICATION_HEALTH_HEALTHY
                                    : COMMUNICATION_HEALTH_SUSPECT;
    snapshot->effective_state = state_at(model, now_ms);
    if (!model->has_last_good) {
        snapshot->quality = COMMUNICATION_QUALITY_UNKNOWN;
    } else if (model->recovery_pending) {
        snapshot->quality = COMMUNICATION_QUALITY_RECOVERING;
    } else if (model->consecutive_failures >= model->policy.degraded_failure_count) {
        snapshot->quality = COMMUNICATION_QUALITY_DEGRADED;
    } else if (model->consecutive_failures > 0U) {
        snapshot->quality = COMMUNICATION_QUALITY_TRANSIENT_FAILURE;
    } else {
        snapshot->quality = COMMUNICATION_QUALITY_GOOD;
    }
    return true;
}

const char *communication_health_name(communication_health_t state)
{
    switch (state) {
    case COMMUNICATION_HEALTH_HEALTHY: return "HEALTHY";
    case COMMUNICATION_HEALTH_SUSPECT: return "SUSPECT";
    case COMMUNICATION_HEALTH_DEGRADED: return "DEGRADED";
    case COMMUNICATION_HEALTH_STALE: return "STALE";
    case COMMUNICATION_HEALTH_OFFLINE: return "OFFLINE";
    default: return "UNKNOWN";
    }
}

const char *communication_quality_name(communication_quality_t quality)
{
    switch (quality) {
    case COMMUNICATION_QUALITY_GOOD: return "GOOD";
    case COMMUNICATION_QUALITY_TRANSIENT_FAILURE: return "TRANSIENT_FAILURE";
    case COMMUNICATION_QUALITY_DEGRADED: return "DEGRADED";
    case COMMUNICATION_QUALITY_RECOVERING: return "RECOVERING";
    default: return "UNKNOWN";
    }
}
