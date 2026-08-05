#ifndef SVD48_POLL_TASK_H
#define SVD48_POLL_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "svd48_poll_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    svd48_poll_service_t *service;
    TaskHandle_t task;
    StaticSemaphore_t stopped_storage;
    SemaphoreHandle_t stopped;
    volatile bool stop_requested;
    volatile bool running;
} svd48_poll_task_t;

esp_err_t svd48_poll_task_start(svd48_poll_task_t *task,
                                svd48_poll_service_t *service);
esp_err_t svd48_poll_task_stop(svd48_poll_task_t *task, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
