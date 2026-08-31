#include "host_test.h"

#include "communication_quality_model.h"

static communication_quality_policy_t tested_policy(void)
{
    return (communication_quality_policy_t){
        .warning_age_ms = 100U,
        .stale_age_ms = 300U,
        .offline_age_ms = 600U,
        .suspect_failure_count = 2U,
        .degraded_failure_count = 3U,
        .recovery_confirm_count = 2U,
    };
}

static bool snapshot(communication_quality_model_t *model,
                     uint32_t now_ms,
                     communication_health_t expected,
                     communication_quality_t expected_quality)
{
    communication_quality_snapshot_t value;
    return communication_quality_model_snapshot(model, now_ms, &value) &&
           value.effective_state == expected && value.quality == expected_quality;
}

static bool test_failures_are_evidence_until_threshold_or_age(void)
{
    communication_quality_model_t model;
    const communication_quality_policy_t policy = tested_policy();
    HOST_TEST_CHECK(communication_quality_model_init(&model, &policy));
    HOST_TEST_CHECK(snapshot(&model, 0U, COMMUNICATION_HEALTH_UNKNOWN,
                             COMMUNICATION_QUALITY_UNKNOWN));
    communication_quality_model_record_good(&model, 10U);
    HOST_TEST_CHECK(snapshot(&model, 20U, COMMUNICATION_HEALTH_HEALTHY,
                             COMMUNICATION_QUALITY_GOOD));
    communication_quality_model_record_failure(&model, 30U, 11U);
    HOST_TEST_CHECK(snapshot(&model, 30U, COMMUNICATION_HEALTH_HEALTHY,
                             COMMUNICATION_QUALITY_TRANSIENT_FAILURE));
    communication_quality_model_record_failure(&model, 40U, 12U);
    HOST_TEST_CHECK(snapshot(&model, 40U, COMMUNICATION_HEALTH_SUSPECT,
                             COMMUNICATION_QUALITY_TRANSIENT_FAILURE));
    communication_quality_model_record_failure(&model, 50U, 13U);
    HOST_TEST_CHECK(snapshot(&model, 50U, COMMUNICATION_HEALTH_DEGRADED,
                             COMMUNICATION_QUALITY_DEGRADED));
    HOST_TEST_CHECK(snapshot(&model, 311U, COMMUNICATION_HEALTH_STALE,
                             COMMUNICATION_QUALITY_DEGRADED));
    HOST_TEST_CHECK(snapshot(&model, 611U, COMMUNICATION_HEALTH_OFFLINE,
                             COMMUNICATION_QUALITY_DEGRADED));
    return true;
}

static bool test_recovery_needs_two_new_samples(void)
{
    communication_quality_model_t model;
    const communication_quality_policy_t policy = tested_policy();
    HOST_TEST_CHECK(communication_quality_model_init(&model, &policy));
    communication_quality_model_record_good(&model, 0U);
    communication_quality_model_record_failure(&model, 10U, 1U);
    communication_quality_model_record_failure(&model, 20U, 1U);
    communication_quality_model_record_failure(&model, 30U, 1U);
    communication_quality_model_record_good(&model, 40U);
    HOST_TEST_CHECK(snapshot(&model, 40U, COMMUNICATION_HEALTH_SUSPECT,
                             COMMUNICATION_QUALITY_RECOVERING));
    communication_quality_model_record_good(&model, 50U);
    HOST_TEST_CHECK(snapshot(&model, 50U, COMMUNICATION_HEALTH_HEALTHY,
                             COMMUNICATION_QUALITY_GOOD));
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_failures_are_evidence_until_threshold_or_age),
        HOST_TEST_CASE(test_recovery_needs_two_new_samples),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
