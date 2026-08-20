#include "motion_application_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MOTION_APPLICATION_MODEL_LOCK_MS 10U

struct motion_application_service_t {
    motion_application_service_config_t config;
    motion_application_model_t model;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t task_done;
    TaskHandle_t task;
    bool stop_requested;
    bool task_running;
    bool pending_stop;
    motion_application_plan_t stop_plan;
    bool pending_apply;
    motion_application_plan_t apply_plan;
    bool pending_stop_event;
    motion_application_event_t stop_event;
    bool pending_arm_event;
    motion_application_event_t arm_event;
    bool pending_command_event;
    motion_application_event_t command_event;
    motion_status_port_t status_port;
    motion_control_port_t control_port;
};

static uint64_t now_ms(void)
{
    int64_t timestamp_us = esp_timer_get_time();
    return timestamp_us > 0 ? (uint64_t)timestamp_us / 1000U : 0U;
}

static void copy_detail(char *destination, size_t size, const char *detail)
{
    snprintf(destination, size, "%s", detail ? detail : "UNKNOWN");
}

static motion_control_source_t status_source(
    command_authority_source_t source)
{
    switch (source) {
    case COMMAND_AUTHORITY_SOURCE_LAN:
        return MOTION_CONTROL_SOURCE_LAN;
    case COMMAND_AUTHORITY_SOURCE_RC:
        return MOTION_CONTROL_SOURCE_RC;
    default:
        return MOTION_CONTROL_SOURCE_NONE;
    }
}

static bool take_lock(motion_application_service_handle_t handle,
                      TickType_t ticks)
{
    return xSemaphoreTake(handle->lock, ticks) == pdTRUE;
}

static void release_lock(motion_application_service_handle_t handle)
{
    (void)xSemaphoreGive(handle->lock);
}

static bool endpoint_gate_open(motion_application_service_handle_t handle,
                               char *detail,
                               size_t detail_size)
{
    if (handle->config.safety_gate &&
        !handle->config.safety_gate(handle->config.safety_context,
                                    detail,
                                    detail_size)) {
        return false;
    }
    for (size_t index = 0U; index < handle->model.config.endpoint_count; ++index) {
        robot_velocity_observation_t observation;
        robot_endpoint_id_t endpoint_id =
            (robot_endpoint_id_t)handle->model.config.endpoints[index].endpoint_id;
        if (!actuation_application_get_endpoint_velocity_observation(
                handle->config.actuation,
                endpoint_id,
                &observation) ||
            !observation.valid || !observation.online || observation.stale ||
            observation.health != ROBOT_ENDPOINT_HEALTH_HEALTHY) {
            snprintf(detail,
                     detail_size,
                     "ENDPOINT_%u_UNHEALTHY",
                     (unsigned)endpoint_id);
            return false;
        }
    }
    copy_detail(detail, detail_size, "SAFE");
    return true;
}

static void queue_plan_locked(motion_application_service_handle_t handle,
                              const motion_application_plan_t *plan)
{
    if (plan->action == MOTION_APPLICATION_PLAN_STOP) {
        handle->stop_plan = *plan;
        handle->pending_stop = true;
        handle->pending_apply = false;
    } else if (plan->action == MOTION_APPLICATION_PLAN_APPLY) {
        handle->apply_plan = *plan;
        handle->pending_apply = true;
    }
}

static bool take_pending_plan_locked(
    motion_application_service_handle_t handle,
    motion_application_plan_t *plan)
{
    if (handle->pending_stop) {
        *plan = handle->stop_plan;
        handle->pending_stop = false;
        return true;
    }
    if (handle->pending_apply) {
        *plan = handle->apply_plan;
        handle->pending_apply = false;
        return true;
    }
    return false;
}

static bool take_pending_event_locked(
    motion_application_service_handle_t handle,
    motion_application_event_t *event)
{
    if (handle->pending_stop_event) {
        *event = handle->stop_event;
        handle->pending_stop_event = false;
        return true;
    }
    if (handle->pending_arm_event) {
        *event = handle->arm_event;
        handle->pending_arm_event = false;
        return true;
    }
    if (handle->pending_command_event) {
        *event = handle->command_event;
        handle->pending_command_event = false;
        return true;
    }
    return false;
}

static bool execute_plan(motion_application_service_handle_t handle,
                         const motion_application_plan_t *plan)
{
    if (plan->action == MOTION_APPLICATION_PLAN_STOP) {
        return actuation_application_stop_all(handle->config.actuation) ==
               ACTUATION_APPLICATION_OK;
    }
    if (plan->action != MOTION_APPLICATION_PLAN_APPLY ||
        plan->target_count == 0U) {
        return true;
    }
    actuation_application_velocity_request_t
        requests[MOTION_APPLICATION_MAX_ENDPOINTS];
    for (size_t index = 0U; index < plan->target_count; ++index) {
        requests[index].endpoint_id =
            (robot_endpoint_id_t)plan->targets[index].endpoint_id;
        requests[index].rpm = plan->targets[index].rpm;
    }
    return actuation_application_apply_endpoint_speeds_rpm(
               handle->config.actuation,
               requests,
               plan->target_count) == ACTUATION_APPLICATION_OK;
}

static void record_plan_result(motion_application_service_handle_t handle,
                               const motion_application_plan_t *plan,
                               bool success)
{
    if (!take_lock(handle, portMAX_DELAY)) {
        return;
    }
    motion_application_model_record_actuation(&handle->model, plan, success);
    release_lock(handle);
}

static void process_one_cycle(motion_application_service_handle_t handle)
{
    motion_application_plan_t plan = {0};
    bool have_plan = false;
    char safety_detail[MOTION_STATUS_DETAIL_MAX] = {0};
    bool safe = endpoint_gate_open(handle,
                                   safety_detail,
                                   sizeof(safety_detail));

    if (take_lock(handle, portMAX_DELAY)) {
        motion_application_event_t event;
        if (take_pending_event_locked(handle, &event)) {
            motion_application_result_t result =
                motion_application_model_submit(&handle->model,
                                                &event,
                                                safe,
                                                now_ms(),
                                                &plan);
            queue_plan_locked(handle, &plan);
            if (result != MOTION_APPLICATION_RESULT_OK &&
                plan.action == MOTION_APPLICATION_PLAN_NONE) {
                copy_detail(handle->model.last_detail,
                            sizeof(handle->model.last_detail),
                            motion_application_result_name(result));
            }
        }
        have_plan = take_pending_plan_locked(handle, &plan);
        if (!have_plan) {
            motion_application_result_t result = motion_application_model_tick(
                &handle->model,
                safe,
                now_ms(),
                &plan);
            have_plan = plan.action != MOTION_APPLICATION_PLAN_NONE;
            if (result != MOTION_APPLICATION_RESULT_OK && !have_plan) {
                copy_detail(handle->model.last_detail,
                            sizeof(handle->model.last_detail),
                            motion_application_result_name(result));
            }
        }
        release_lock(handle);
    }
    if (!have_plan) {
        return;
    }

    bool success = execute_plan(handle, &plan);
    record_plan_result(handle, &plan, success);
    if (!success && plan.action == MOTION_APPLICATION_PLAN_APPLY) {
        motion_application_plan_t stop = {
            .action = MOTION_APPLICATION_PLAN_STOP,
        };
        copy_detail(stop.detail, sizeof(stop.detail), "APPLY_FAILURE_STOP");
        bool stopped = execute_plan(handle, &stop);
        record_plan_result(handle, &stop, stopped);
    }
}

static void motion_application_task(void *argument)
{
    motion_application_service_handle_t handle = argument;
    if (take_lock(handle, portMAX_DELAY)) {
        handle->task_running = true;
        release_lock(handle);
    }

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(handle->config.period_ms));
        if (take_lock(handle, portMAX_DELAY)) {
            bool stop_requested = handle->stop_requested;
            release_lock(handle);
            if (stop_requested) {
                break;
            }
        }
        /* STOP is always removed before APPLY. If STOP arrives while a bounded
         * endpoint transaction is in progress, it is the next service action. */
        process_one_cycle(handle);
    }

    motion_application_plan_t stop = {
        .action = MOTION_APPLICATION_PLAN_STOP,
    };
    copy_detail(stop.detail, sizeof(stop.detail), "SERVICE_SHUTDOWN");
    bool stopped = execute_plan(handle, &stop);
    record_plan_result(handle, &stop, stopped);

    if (take_lock(handle, portMAX_DELAY)) {
        handle->task_running = false;
        handle->task = NULL;
        release_lock(handle);
    }
    (void)xSemaphoreGive(handle->task_done);
    vTaskDelete(NULL);
}

static esp_err_t build_model_config(
    const robot_profile_t *profile,
    motion_application_model_config_t *config)
{
    if (!profile || !config ||
        profile->application.kind != ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(config, 0, sizeof(*config));
    config->command_ttl_ms = profile->application.control_ttl_ms;
    config->velocity_limit = (command_authority_velocity_t){
        profile->application.max_vx_mps,
        profile->application.max_vy_mps,
        profile->application.max_wz_radps,
    };
    config->moving_epsilon = (command_authority_velocity_t){
        0.0001f,
        0.0001f,
        0.0001f,
    };
    config->differential = (robot_kinematics_differential_config_t){
        .track_width_m = profile->application.track_width_m,
        .vy_epsilon_mps = 0.0001,
    };
    for (size_t index = 0U; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *source = &profile->endpoints[index];
        if (source->motion_side == ROBOT_PROFILE_MOTION_SIDE_NONE) {
            continue;
        }
        if (config->endpoint_count >= MOTION_APPLICATION_MAX_ENDPOINTS) {
            return ESP_ERR_INVALID_SIZE;
        }
        int32_t negative = source->min_rpm < 0 ? -(int32_t)source->min_rpm
                                               : (int32_t)source->min_rpm;
        int32_t positive = source->max_rpm < 0 ? -(int32_t)source->max_rpm
                                               : (int32_t)source->max_rpm;
        double symmetric_limit = (double)(negative < positive ? negative
                                                               : positive);
        motion_application_endpoint_config_t *target =
            &config->endpoints[config->endpoint_count++];
        target->endpoint_id = source->id;
        target->name = source->name;
        target->side = source->motion_side == ROBOT_PROFILE_MOTION_SIDE_LEFT
                           ? ROBOT_KINEMATICS_SIDE_LEFT
                           : ROBOT_KINEMATICS_SIDE_RIGHT;
        target->wheel_radius_m = profile->application.wheel_radius_m;
        target->motor_to_wheel_ratio = source->motor_to_wheel_ratio;
        target->direction_sign = source->motion_direction_sign;
        target->max_abs_rpm = symmetric_limit;
    }
    return config->endpoint_count >= 2U ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool status_snapshot(motion_status_port_t *port,
                            motion_status_snapshot_t *snapshot)
{
    motion_application_service_handle_t handle = port ? port->context : NULL;
    if (!handle || !snapshot) {
        return false;
    }
    motion_application_model_snapshot_t model_snapshot;
    bool task_running;
    if (!take_lock(handle,
                   pdMS_TO_TICKS(MOTION_APPLICATION_MODEL_LOCK_MS))) {
        return false;
    }
    bool copied = motion_application_model_snapshot(&handle->model,
                                                    &model_snapshot);
    task_running = handle->task_running;
    release_lock(handle);
    if (!copied) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->available = true;
    snapshot->task_running = task_running;
    snapshot->state = model_snapshot.state;
    snapshot->source = status_source(model_snapshot.source);
    snapshot->command_ttl_ms = model_snapshot.command_ttl_ms;
    snapshot->deadman = model_snapshot.deadman;
    snapshot->stream_id_hash = model_snapshot.stream_id;
    snapshot->sequence = model_snapshot.sequence;
    snapshot->max_vx_mps = handle->model.config.velocity_limit.vx;
    snapshot->max_vy_mps = handle->model.config.velocity_limit.vy;
    snapshot->max_wz_radps = handle->model.config.velocity_limit.wz;
    snapshot->requested_vx_mps = model_snapshot.requested.vx;
    snapshot->requested_vy_mps = model_snapshot.requested.vy;
    snapshot->requested_wz_radps = model_snapshot.requested.wz;
    snapshot->endpoint_count = model_snapshot.endpoint_count;
    copy_detail(snapshot->last_detail,
                sizeof(snapshot->last_detail),
                model_snapshot.last_detail);
    uint64_t current_ms = now_ms();
    if (model_snapshot.stream_id != 0U &&
        current_ms >= model_snapshot.last_received_ms) {
        uint64_t age = current_ms - model_snapshot.last_received_ms;
        snapshot->lease_age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        snapshot->lease_fresh = age < model_snapshot.command_ttl_ms;
        snapshot->lease_remaining_ms = snapshot->lease_fresh
                                           ? model_snapshot.command_ttl_ms -
                                                 snapshot->lease_age_ms
                                           : 0U;
    }
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        motion_status_endpoint_t *target = &snapshot->endpoints[index];
        target->endpoint_id =
            (robot_endpoint_id_t)model_snapshot.targets[index].endpoint_id;
        target->name = handle->model.config.endpoints[index].name;
        target->target_rpm = model_snapshot.targets[index].rpm;
        robot_velocity_observation_t observation;
        if (actuation_application_get_endpoint_velocity_observation(
                handle->config.actuation,
                target->endpoint_id,
                &observation)) {
            target->observed_valid = observation.valid;
            target->observed_rpm = observation.rpm;
            target->observation_timestamp_ms = observation.timestamp_ms;
            target->online = observation.online;
            target->stale = observation.stale;
            target->health = observation.health;
        }
    }
    return true;
}

static bool emergency_stop(motion_control_port_t *port,
                           char *detail,
                           size_t detail_size)
{
    motion_application_service_handle_t handle = port ? port->context : NULL;
    motion_application_event_t event = {
        .action = MOTION_APPLICATION_EVENT_STOP,
        .source = COMMAND_AUTHORITY_SOURCE_NONE,
        .received_at_ms = now_ms(),
    };
    motion_application_submit_result_t result =
        motion_application_service_submit(handle, &event);
    copy_detail(detail, detail_size, result.detail);
    return result.accepted;
}

esp_err_t motion_application_service_init(
    const motion_application_service_config_t *config,
    motion_application_service_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || !config->profile || !config->actuation) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t priority = config->task_priority == 0U
                            ? MOTION_APPLICATION_DEFAULT_TASK_PRIORITY
                            : config->task_priority;
    if (priority >= configMAX_PRIORITIES) {
        return ESP_ERR_INVALID_ARG;
    }
    motion_application_model_config_t model_config;
    esp_err_t error = build_model_config(config->profile, &model_config);
    if (error != ESP_OK) {
        return error;
    }

    motion_application_service_handle_t handle =
        calloc(1, sizeof(struct motion_application_service_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    handle->config.period_ms = config->period_ms == 0U
                                   ? MOTION_APPLICATION_DEFAULT_PERIOD_MS
                                   : config->period_ms;
    handle->config.task_priority = priority;
    if (motion_application_model_init(&handle->model, &model_config) !=
        MOTION_APPLICATION_RESULT_OK) {
        free(handle);
        return ESP_ERR_INVALID_ARG;
    }
    handle->lock = xSemaphoreCreateMutex();
    handle->task_done = xSemaphoreCreateBinary();
    if (!handle->lock || !handle->task_done) {
        if (handle->task_done) {
            vSemaphoreDelete(handle->task_done);
        }
        if (handle->lock) {
            vSemaphoreDelete(handle->lock);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    static const motion_status_ops_t status_ops = {
        .snapshot = status_snapshot,
    };
    static const motion_control_ops_t control_ops = {
        .stop_all = emergency_stop,
    };
    handle->status_port.ops = &status_ops;
    handle->status_port.context = handle;
    handle->control_port.ops = &control_ops;
    handle->control_port.context = handle;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t motion_application_service_start(
    motion_application_service_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(handle, portMAX_DELAY)) {
        return ESP_FAIL;
    }
    if (handle->task) {
        release_lock(handle);
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(handle->task_done, 0U) == pdTRUE) {
    }
    handle->stop_requested = false;
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(motion_application_task,
                                     "motion_app",
                                     MOTION_APPLICATION_TASK_STACK_SIZE,
                                     handle,
                                     handle->config.task_priority,
                                     &task);
    if (created != pdPASS) {
        release_lock(handle);
        return ESP_ERR_NO_MEM;
    }
    handle->task = task;
    release_lock(handle);
    return ESP_OK;
}

void motion_application_service_deinit(
    motion_application_service_handle_t handle)
{
    if (!handle) {
        return;
    }
    if (take_lock(handle, portMAX_DELAY)) {
        handle->stop_requested = true;
        TaskHandle_t task = handle->task;
        release_lock(handle);
        if (task) {
            xTaskNotifyGive(task);
            (void)xSemaphoreTake(handle->task_done, portMAX_DELAY);
        }
    }
    vSemaphoreDelete(handle->task_done);
    vSemaphoreDelete(handle->lock);
    free(handle);
}

motion_application_submit_result_t motion_application_service_submit(
    motion_application_service_handle_t handle,
    const motion_application_event_t *event)
{
    motion_application_submit_result_t response = {0};
    copy_detail(response.detail, sizeof(response.detail), "INVALID_ARGUMENT");
    if (!handle || !event) {
        return response;
    }
    char safety_detail[MOTION_STATUS_DETAIL_MAX] = {0};
    bool safe = endpoint_gate_open(handle,
                                   safety_detail,
                                   sizeof(safety_detail));
    if (!take_lock(handle,
                   pdMS_TO_TICKS(MOTION_APPLICATION_MODEL_LOCK_MS))) {
        copy_detail(response.detail, sizeof(response.detail), "CONTROL_BUSY");
        return response;
    }
    motion_application_plan_t plan = {0};
    motion_application_result_t result = motion_application_model_submit(
        &handle->model,
        event,
        safe,
        now_ms(),
        &plan);
    queue_plan_locked(handle, &plan);
    response.accepted = result == MOTION_APPLICATION_RESULT_OK;
    copy_detail(response.detail,
                sizeof(response.detail),
                response.accepted ? plan.detail
                                  : motion_application_result_name(result));
    TaskHandle_t task = handle->task;
    release_lock(handle);
    if (task && plan.action != MOTION_APPLICATION_PLAN_NONE) {
        xTaskNotifyGive(task);
    }
    return response;
}

motion_application_submit_result_t motion_application_service_publish(
    motion_application_service_handle_t handle,
    const motion_application_event_t *event)
{
    motion_application_submit_result_t response = {0};
    copy_detail(response.detail, sizeof(response.detail), "INVALID_ARGUMENT");
    if (!handle || !event) {
        return response;
    }
    if (!take_lock(handle, 0U)) {
        copy_detail(response.detail, sizeof(response.detail), "CONTROL_BUSY");
        return response;
    }
    switch (event->action) {
    case MOTION_APPLICATION_EVENT_STOP:
    case MOTION_APPLICATION_EVENT_DISARM:
        handle->stop_event = *event;
        handle->pending_stop_event = true;
        handle->pending_arm_event = false;
        handle->pending_command_event = false;
        handle->pending_apply = false;
        break;
    case MOTION_APPLICATION_EVENT_ARM:
        handle->arm_event = *event;
        handle->pending_arm_event = true;
        break;
    case MOTION_APPLICATION_EVENT_COMMAND:
        handle->command_event = *event;
        handle->pending_command_event = true;
        break;
    default:
        release_lock(handle);
        return response;
    }
    TaskHandle_t task = handle->task;
    response.accepted = task != NULL;
    copy_detail(response.detail,
                sizeof(response.detail),
                response.accepted ? "QUEUED" : "CONTROL_NOT_RUNNING");
    release_lock(handle);
    if (task) {
        xTaskNotifyGive(task);
    }
    return response;
}

motion_status_port_t *motion_application_service_status_port(
    motion_application_service_handle_t handle)
{
    return handle ? &handle->status_port : NULL;
}

motion_control_port_t *motion_application_service_control_port(
    motion_application_service_handle_t handle)
{
    return handle ? &handle->control_port : NULL;
}

bool motion_application_service_limits(
    motion_application_service_handle_t handle,
    float *max_vx_mps,
    float *max_vy_mps,
    float *max_wz_radps)
{
    if (!handle || !max_vx_mps || !max_vy_mps || !max_wz_radps) {
        return false;
    }
    *max_vx_mps = handle->model.config.velocity_limit.vx;
    *max_vy_mps = handle->model.config.velocity_limit.vy;
    *max_wz_radps = handle->model.config.velocity_limit.wz;
    return true;
}
