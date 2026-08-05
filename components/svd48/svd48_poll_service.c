#include "svd48_poll_service.h"

#include <string.h>

#define SVD48_DEFAULT_POLL_PERIOD_MS 30U
#define SVD48_BACKOFF_BASE_MS 250U
#define SVD48_BACKOFF_MAX_MS 1500U

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return deadline == 0U || (int32_t)(now - deadline) >= 0;
}

static uint32_t failure_backoff_ms(uint8_t failures)
{
    if (failures == 0U) {
        return 0U;
    }
    uint8_t shift = failures > 4U ? 3U : (uint8_t)(failures - 1U);
    uint32_t backoff = SVD48_BACKOFF_BASE_MS << shift;
    return backoff > SVD48_BACKOFF_MAX_MS ? SVD48_BACKOFF_MAX_MS : backoff;
}

bool svd48_poll_service_init(svd48_poll_service_t *service,
                             svd48_device_clock_ms_fn clock_ms,
                             void *clock_context)
{
    if (!service || !clock_ms) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->clock_ms = clock_ms;
    service->clock_context = clock_context;
    service->initialized = true;
    return true;
}

bool svd48_poll_service_add_device(svd48_poll_service_t *service,
                                   svd48_device_t *device,
                                   uint32_t period_ms)
{
    if (!service || !service->initialized || !device || !device->initialized ||
        service->count >= SVD48_POLL_SERVICE_MAX_DEVICES) {
        return false;
    }
    for (size_t index = 0; index < service->count; ++index) {
        if (service->entries[index].device == device) {
            return false;
        }
    }
    svd48_poll_entry_t *entry = &service->entries[service->count++];
    entry->device = device;
    entry->period_ms = period_ms == 0U ? SVD48_DEFAULT_POLL_PERIOD_MS : period_ms;
    entry->last_result = SVD48_DEVICE_TIMEOUT;
    return true;
}

svd48_device_result_t svd48_poll_service_run_once(svd48_poll_service_t *service)
{
    if (!service || !service->initialized || service->count == 0U) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    uint32_t now = service->clock_ms(service->clock_context);
    svd48_device_result_t first_error = SVD48_DEVICE_OK;
    bool attempted = false;
    for (size_t index = 0; index < service->count; ++index) {
        svd48_poll_entry_t *entry = &service->entries[index];
        if (!time_reached(now, entry->next_poll_ms)) {
            continue;
        }
        attempted = true;
        entry->last_result = svd48_device_poll(entry->device);
        if (entry->last_result == SVD48_DEVICE_OK) {
            entry->consecutive_failures = 0U;
            entry->next_poll_ms = now + entry->period_ms;
        } else {
            if (entry->consecutive_failures < UINT8_MAX) {
                entry->consecutive_failures++;
            }
            entry->next_poll_ms = now + failure_backoff_ms(entry->consecutive_failures);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = entry->last_result;
            }
        }
    }
    return attempted ? first_error : SVD48_DEVICE_OK;
}

uint32_t svd48_poll_service_next_delay_ms(const svd48_poll_service_t *service,
                                          uint32_t maximum_delay_ms)
{
    if (!service || !service->initialized || service->count == 0U ||
        maximum_delay_ms == 0U) {
        return 1U;
    }
    uint32_t now = service->clock_ms(service->clock_context);
    uint32_t delay = maximum_delay_ms;
    for (size_t index = 0; index < service->count; ++index) {
        uint32_t deadline = service->entries[index].next_poll_ms;
        if (time_reached(now, deadline)) {
            return 1U;
        }
        uint32_t candidate = deadline - now;
        if (candidate < delay) {
            delay = candidate;
        }
    }
    return delay == 0U ? 1U : delay;
}

void svd48_poll_service_reset(svd48_poll_service_t *service)
{
    if (service) {
        memset(service, 0, sizeof(*service));
    }
}
