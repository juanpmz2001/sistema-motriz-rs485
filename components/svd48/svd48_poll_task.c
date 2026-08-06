#include "svd48_poll_task.h"

#include <string.h>

#define SVD48_POLL_TASK_STACK_SIZE 4096U
#define SVD48_POLL_TASK_PRIORITY 8U
#define SVD48_POLL_TASK_MAX_SLEEP_MS 50U

static void polling_task(void *argument)
{
    svd48_poll_task_t *task = argument;
    task->running = true;
    while (!task->stop_requested) {
        (void)svd48_poll_service_run_once(task->service);
        uint32_t delay_ms = svd48_poll_service_next_delay_ms(
            task->service, SVD48_POLL_TASK_MAX_SLEEP_MS);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    task->running = false;
    task->task = NULL;
    xSemaphoreGive(task->stopped);
    vTaskDelete(NULL);
}

esp_err_t svd48_poll_task_start(svd48_poll_task_t *task,
                                svd48_poll_service_t *service)
{
    if (!task || !service || !service->initialized || service->count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A timed-out stop must be collected before this storage can be reused. */
    if (task->service || task->task || task->stopped || task->stop_requested ||
        task->running) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(task, 0, sizeof(*task));
    task->service = service;
    task->stopped = xSemaphoreCreateBinaryStatic(&task->stopped_storage);
    if (!task->stopped) {
        memset(task, 0, sizeof(*task));
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(polling_task,
                                     "svd48_poll",
                                     SVD48_POLL_TASK_STACK_SIZE,
                                     task,
                                     SVD48_POLL_TASK_PRIORITY,
                                     &task->task);
    if (created != pdPASS) {
        vSemaphoreDelete(task->stopped);
        memset(task, 0, sizeof(*task));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t svd48_poll_task_stop(svd48_poll_task_t *task, uint32_t timeout_ms)
{
    if (!task) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!task->stopped) {
        if (task->task || task->running) {
            return ESP_ERR_INVALID_STATE;
        }
        memset(task, 0, sizeof(*task));
        return ESP_OK;
    }
    /* A previous timeout may leave only the completion semaphore to collect. */
    task->stop_requested = true;
    if (xSemaphoreTake(task->stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(task->stopped);
    memset(task, 0, sizeof(*task));
    return ESP_OK;
}
