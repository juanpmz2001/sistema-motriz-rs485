#ifndef SVD48_POLL_SERVICE_H
#define SVD48_POLL_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "svd48_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_POLL_SERVICE_MAX_DEVICES 4U

typedef struct {
    svd48_device_t *device;
    uint32_t period_ms;
    uint32_t next_poll_ms;
    uint8_t consecutive_failures;
    svd48_device_result_t last_result;
} svd48_poll_entry_t;

typedef struct {
    svd48_poll_entry_t entries[SVD48_POLL_SERVICE_MAX_DEVICES];
    size_t count;
    svd48_device_clock_ms_fn clock_ms;
    void *clock_context;
    bool initialized;
} svd48_poll_service_t;

bool svd48_poll_service_init(svd48_poll_service_t *service,
                             svd48_device_clock_ms_fn clock_ms,
                             void *clock_context);
bool svd48_poll_service_add_device(svd48_poll_service_t *service,
                                   svd48_device_t *device,
                                   uint32_t period_ms);
svd48_device_result_t svd48_poll_service_run_once(svd48_poll_service_t *service);
uint32_t svd48_poll_service_next_delay_ms(const svd48_poll_service_t *service,
                                          uint32_t maximum_delay_ms);
void svd48_poll_service_reset(svd48_poll_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
