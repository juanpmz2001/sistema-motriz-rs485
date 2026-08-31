#ifndef COMMUNICATION_QUALITY_MODEL_H
#define COMMUNICATION_QUALITY_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is intentionally protocol-neutral temporal semantics. CRC, framing,
 * address and device-fault validation remain owned by their respective
 * drivers. A failure code is opaque raw evidence from that driver. */
typedef enum {
    COMMUNICATION_HEALTH_UNKNOWN = 0,
    COMMUNICATION_HEALTH_HEALTHY,
    COMMUNICATION_HEALTH_SUSPECT,
    COMMUNICATION_HEALTH_DEGRADED,
    COMMUNICATION_HEALTH_STALE,
    COMMUNICATION_HEALTH_OFFLINE,
} communication_health_t;

typedef enum {
    COMMUNICATION_QUALITY_UNKNOWN = 0,
    COMMUNICATION_QUALITY_GOOD,
    COMMUNICATION_QUALITY_TRANSIENT_FAILURE,
    COMMUNICATION_QUALITY_DEGRADED,
    COMMUNICATION_QUALITY_RECOVERING,
} communication_quality_t;

typedef struct {
    uint32_t warning_age_ms;
    uint32_t stale_age_ms;
    uint32_t offline_age_ms;
    uint8_t suspect_failure_count;
    uint8_t degraded_failure_count;
    uint8_t recovery_confirm_count;
} communication_quality_policy_t;

typedef struct {
    communication_quality_policy_t policy;
    bool initialized;
    bool has_last_good;
    bool recovery_pending;
    uint32_t last_good_ms;
    uint32_t last_failure_ms;
    uint32_t last_failure_reason;
    uint32_t consecutive_good;
    uint32_t consecutive_failures;
    uint32_t total_good;
    uint32_t total_failures;
} communication_quality_model_t;

typedef struct {
    communication_health_t raw_state;
    communication_health_t effective_state;
    communication_quality_t quality;
    bool has_last_good;
    uint32_t last_good_age_ms;
    uint32_t last_failure_age_ms;
    uint32_t last_failure_reason;
    uint32_t consecutive_good;
    uint32_t consecutive_failures;
    uint32_t total_good;
    uint32_t total_failures;
} communication_quality_snapshot_t;

bool communication_quality_model_init(
    communication_quality_model_t *model,
    const communication_quality_policy_t *policy);
void communication_quality_model_record_good(communication_quality_model_t *model,
                                             uint32_t now_ms);
void communication_quality_model_record_failure(communication_quality_model_t *model,
                                                uint32_t now_ms,
                                                uint32_t raw_failure_reason);
bool communication_quality_model_snapshot(const communication_quality_model_t *model,
                                          uint32_t now_ms,
                                          communication_quality_snapshot_t *snapshot);
const char *communication_health_name(communication_health_t state);
const char *communication_quality_name(communication_quality_t quality);

#ifdef __cplusplus
}
#endif

#endif /* COMMUNICATION_QUALITY_MODEL_H */
