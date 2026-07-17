#include "robot_safety.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "svd48.h"

static const char *TAG = "robot_safety";

#define ROBOT_SAFETY_DEFAULT_PERIOD_MS 20
#define ROBOT_SAFETY_DEFAULT_RC_LOSS_TIMEOUT_MS 150
#define ROBOT_SAFETY_DEFAULT_STOP_REPEAT_MS 500
#define ROBOT_SAFETY_TASK_STACK 4096
#define ROBOT_SAFETY_TASK_PRIORITY 9
#define ROBOT_SAFETY_LOCK_TIMEOUT_MS 10

struct robot_safety_t {
    robot_safety_config_t config;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool stop_task;
    robot_safety_status_t status;
};

static esp_err_t take_lock(robot_safety_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(ROBOT_SAFETY_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    snprintf(dest, dest_size, "%s", src ? src : "UNKNOWN");
}

static void record_status(robot_safety_handle_t handle,
                          bool rc_available,
                          bool rc_signal_seen,
                          bool rc_signal_valid,
                          bool rc_loss_active,
                          bool motor_fault_active,
                          uint32_t rc_last_frame_age_ms)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->status.task_running = true;
    handle->status.rc_available = rc_available;
    handle->status.rc_signal_seen = rc_signal_seen;
    handle->status.rc_signal_valid = rc_signal_valid;
    handle->status.rc_loss_active = rc_loss_active;
    handle->status.motor_fault_active = motor_fault_active;
    handle->status.rc_last_frame_age_ms = rc_last_frame_age_ms;
    handle->status.loop_count++;
    xSemaphoreGive(handle->lock);
}

static void record_stop(robot_safety_handle_t handle, const char *reason, esp_err_t err)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->status.stop_requests++;
    handle->status.last_stop_error = err;
    copy_text(handle->status.last_stop_reason, sizeof(handle->status.last_stop_reason), reason);
    xSemaphoreGive(handle->lock);
}

static bool motor_fault_detected(robot_safety_handle_t handle)
{
    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        svd48_motor_telemetry_t telemetry;
        if (!robot_control_get_motor(handle->config.robot, motor, &telemetry)) {
            continue;
        }
        if (!telemetry.online || telemetry.stale) {
            continue;
        }
        if (telemetry.error_code != 0) {
            return true;
        }
    }
    return false;
}

static void safety_task(void *arg)
{
    robot_safety_handle_t handle = (robot_safety_handle_t)arg;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_stop_tick = 0;
    bool rc_signal_seen = false;

    if (take_lock(handle) == ESP_OK) {
        handle->status.task_running = true;
        copy_text(handle->status.last_stop_reason,
                  sizeof(handle->status.last_stop_reason),
                  "NONE");
        xSemaphoreGive(handle->lock);
    }

    while (!handle->stop_task) {
        bool rc_available = handle->config.ibus_receiver != NULL;
        bool rc_signal_valid = false;
        uint32_t rc_last_frame_age_ms = 0;
        bool rc_loss_active = false;

        if (rc_available) {
            ibus_receiver_status_t ibus_status;
            if (ibus_receiver_get_status(handle->config.ibus_receiver, &ibus_status) == ESP_OK) {
                rc_signal_valid = ibus_status.signal_valid;
                rc_last_frame_age_ms = ibus_status.last_frame_age_ms;
                if (rc_signal_valid) {
                    rc_signal_seen = true;
                }
                rc_loss_active = rc_signal_seen &&
                                 !rc_signal_valid &&
                                 rc_last_frame_age_ms >= handle->config.rc_loss_timeout_ms;
            }
        }

        bool motor_fault_active = handle->config.stop_on_motor_fault && motor_fault_detected(handle);
        record_status(handle,
                      rc_available,
                      rc_signal_seen,
                      rc_signal_valid,
                      rc_loss_active,
                      motor_fault_active,
                      rc_last_frame_age_ms);

        const bool stop_for_rc = handle->config.stop_on_rc_loss && rc_loss_active;
        const bool stop_for_fault = motor_fault_active;
        if (stop_for_rc || stop_for_fault) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = (uint32_t)((now - last_stop_tick) * portTICK_PERIOD_MS);
            if (last_stop_tick == 0 || elapsed_ms >= handle->config.stop_repeat_ms) {
                const char *reason = stop_for_fault ? "MOTOR_FAULT" : "RC_LOSS";
                esp_err_t err = robot_control_stop_all(handle->config.robot);
                record_stop(handle, reason, err);
                last_stop_tick = now;
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(handle->config.period_ms));
    }

    if (take_lock(handle) == ESP_OK) {
        handle->status.task_running = false;
        handle->task = NULL;
        xSemaphoreGive(handle->lock);
    }
    vTaskDelete(NULL);
}

esp_err_t robot_safety_init(const robot_safety_config_t *config, robot_safety_handle_t *out_handle)
{
    if (!config || !config->robot || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    robot_safety_handle_t handle = calloc(1, sizeof(struct robot_safety_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    if (handle->config.period_ms == 0) {
        handle->config.period_ms = ROBOT_SAFETY_DEFAULT_PERIOD_MS;
    }
    if (handle->config.rc_loss_timeout_ms == 0) {
        handle->config.rc_loss_timeout_ms = ROBOT_SAFETY_DEFAULT_RC_LOSS_TIMEOUT_MS;
    }
    if (handle->config.stop_repeat_ms == 0) {
        handle->config.stop_repeat_ms = ROBOT_SAFETY_DEFAULT_STOP_REPEAT_MS;
    }

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    copy_text(handle->status.last_stop_reason, sizeof(handle->status.last_stop_reason), "NONE");

    *out_handle = handle;
    return ESP_OK;
}

void robot_safety_deinit(robot_safety_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->stop_task = true;
    if (handle->task) {
        for (int i = 0; i < 80 && handle->status.task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t robot_safety_start(robot_safety_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->task) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->stop_task = false;
    BaseType_t ok = xTaskCreate(safety_task,
                                "robot_safety",
                                ROBOT_SAFETY_TASK_STACK,
                                handle,
                                ROBOT_SAFETY_TASK_PRIORITY,
                                &handle->task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "robot safety task started period=%lums rc_loss=%lums stop_repeat=%lums",
             (unsigned long)handle->config.period_ms,
             (unsigned long)handle->config.rc_loss_timeout_ms,
             (unsigned long)handle->config.stop_repeat_ms);
    return ESP_OK;
}

esp_err_t robot_safety_get_status(robot_safety_handle_t handle, robot_safety_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    *status = handle->status;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}
