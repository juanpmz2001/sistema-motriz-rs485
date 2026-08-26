#include "ppm_motion_source.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ppm_motion_model.h"

static const char *TAG = "ppm_motion";
#define PPM_MOTION_SOURCE_TASK_STACK_SIZE 4096U

struct ppm_motion_source_t {
    ppm_motion_source_config_t config;
    ppm_motion_model_t model;
    SemaphoreHandle_t task_done;
    TaskHandle_t task;
    bool stop_requested;
};

static uint64_t now_ms(void)
{
    int64_t timestamp_us = esp_timer_get_time();
    return timestamp_us > 0 ? (uint64_t)timestamp_us / 1000U : 0U;
}

static ppm_motion_model_config_t model_config_from_profile(
    const robot_ppm_motion_profile_t *source)
{
    return (ppm_motion_model_config_t){
        .throttle_channel = source->throttle_channel,
        .steering_channel = source->steering_channel,
        .enable_channel = source->enable_channel,
        .enable_active_max_us = source->enable_active_max_us,
        .neutral_deadband_us = source->neutral_deadband_us,
        .valid_min_us = source->valid_min_us,
        .valid_max_us = source->valid_max_us,
        .throttle_min_us = source->throttle_min_us,
        .throttle_center_us = source->throttle_center_us,
        .throttle_max_us = source->throttle_max_us,
        .steering_min_us = source->steering_min_us,
        .steering_center_us = source->steering_center_us,
        .steering_max_us = source->steering_max_us,
        .throttle_sign = source->throttle_sign,
        .steering_sign = source->steering_sign,
        .speed_scale_channel = source->speed_scale_channel,
        .speed_scale_min_us = source->speed_scale_min_us,
        .speed_scale_max_us = source->speed_scale_max_us,
        .speed_scale_min = source->speed_scale_min,
        .speed_scale_max = source->speed_scale_max,
    };
}

static bool application_rc_active(ppm_motion_source_handle_t handle)
{
    motion_status_snapshot_t status = {0};
    return motion_status_snapshot(
               motion_application_service_status_port(
                   handle->config.motion_application),
               &status) &&
           status.available && status.source == MOTION_CONTROL_SOURCE_RC &&
           (status.state == MOTION_CONTROL_ARMED ||
            status.state == MOTION_CONTROL_ACTIVE);
}

static void publish_output(ppm_motion_source_handle_t handle,
                           const ppm_motion_output_t *output)
{
    if (output->action == PPM_MOTION_ACTION_NONE) {
        return;
    }
    motion_application_event_t event = {
        .source = COMMAND_AUTHORITY_SOURCE_RC,
        .received_at_ms = now_ms(),
    };
    switch (output->action) {
    case PPM_MOTION_ACTION_STOP:
        event.action = MOTION_APPLICATION_EVENT_STOP;
        event.source = COMMAND_AUTHORITY_SOURCE_NONE;
        break;
    case PPM_MOTION_ACTION_ARM:
        event.action = MOTION_APPLICATION_EVENT_ARM;
        event.stream_id = output->stream_id;
        event.sequence = output->sequence;
        break;
    case PPM_MOTION_ACTION_COMMAND:
        event.action = MOTION_APPLICATION_EVENT_COMMAND;
        event.stream_id = output->stream_id;
        event.sequence = output->sequence;
        event.vx_mps = output->normalized_vx * output->speed_scale *
                       handle->config.profile->application.max_vx_mps;
        event.vy_mps = 0.0f;
        event.wz_radps = output->normalized_wz * output->speed_scale *
                         handle->config.profile->application.max_wz_radps;
        event.deadman = output->deadman;
        break;
    default:
        return;
    }
    motion_application_submit_result_t result =
        motion_application_service_publish(handle->config.motion_application,
                                           &event);
    if (!result.accepted) {
        ESP_LOGW(TAG,
                 "PPM %u was not queued: %s",
                 (unsigned)output->action,
                 result.detail);
    }
}

static void ppm_motion_task(void *argument)
{
    ppm_motion_source_handle_t handle = argument;
    TickType_t last_wake = xTaskGetTickCount();
    while (!handle->stop_requested) {
        ibus_receiver_status_t receiver = {0};
        ppm_motion_input_t input = {0};
        if (ibus_receiver_get_status(handle->config.receiver, &receiver) ==
            ESP_OK) {
            input.signal_valid = receiver.signal_valid;
            input.valid_frame_sequence = receiver.valid_frames;
            input.channel_count = receiver.frame_channel_count;
            memcpy(input.channels,
                   receiver.channels,
                   sizeof(input.channels));
        }
        ppm_motion_output_t output;
        if (ppm_motion_model_step(&handle->model,
                                  &input,
                                  application_rc_active(handle),
                                  &output)) {
            publish_output(handle, &output);
        }
        vTaskDelayUntil(&last_wake,
                        pdMS_TO_TICKS(handle->config.period_ms));
    }
    (void)xSemaphoreGive(handle->task_done);
    vTaskDelete(NULL);
}

esp_err_t ppm_motion_source_init(const ppm_motion_source_config_t *config,
                                 ppm_motion_source_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || !config->profile || !config->profile->ppm_motion.enabled ||
        !config->receiver || !config->motion_application ||
        config->profile->application.kind !=
            ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY) {
        return ESP_ERR_INVALID_ARG;
    }
    ppm_motion_source_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    if (handle->config.period_ms == 0U) {
        handle->config.period_ms = PPM_MOTION_SOURCE_DEFAULT_PERIOD_MS;
    }
    if (handle->config.task_priority == 0U) {
        handle->config.task_priority = PPM_MOTION_SOURCE_DEFAULT_TASK_PRIORITY;
    }
    const ppm_motion_model_config_t model_config =
        model_config_from_profile(&config->profile->ppm_motion);
    if (handle->config.task_priority >= configMAX_PRIORITIES ||
        !ppm_motion_model_init(&handle->model, &model_config)) {
        free(handle);
        return ESP_ERR_INVALID_ARG;
    }
    handle->task_done = xSemaphoreCreateBinary();
    if (!handle->task_done) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t ppm_motion_source_start(ppm_motion_source_handle_t handle)
{
    if (!handle || handle->task) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->stop_requested = false;
    return xTaskCreate(ppm_motion_task,
                       "ppm_motion",
                       PPM_MOTION_SOURCE_TASK_STACK_SIZE,
                       handle,
                       handle->config.task_priority,
                       &handle->task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void ppm_motion_source_deinit(ppm_motion_source_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->stop_requested = true;
    if (handle->task) {
        (void)xSemaphoreTake(handle->task_done, portMAX_DELAY);
    }
    vSemaphoreDelete(handle->task_done);
    free(handle);
}
