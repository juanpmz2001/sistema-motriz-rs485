#include "web_direct_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WEB_DIRECT_CONTROL_TELEMETRY_PERIOD_MS 200U
#define WEB_DIRECT_CONTROL_TASK_STACK_SIZE 6144U
#define WEB_DIRECT_CONTROL_TASK_PRIORITY 5U
#define WEB_DIRECT_CONTROL_MAX_FRAME_BYTES 256U

extern const unsigned char web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const unsigned char web_ui_html_end[] asm("_binary_web_ui_html_end");

static const char *TAG = "web_direct";

struct web_direct_control_t {
    web_direct_control_config_t config;
    web_direct_control_model_t model;
    httpd_handle_t server;
    TaskHandle_t telemetry_task;
    SemaphoreHandle_t lock;
    bool stop_requested;
    int active_fd;
    uint64_t sequence;
    float peak_rpm_m1;
    float peak_rpm_m2;
    float peak_current_m1;
    float peak_current_m2;
};

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void lock(web_direct_control_handle_t handle)
{
    (void)xSemaphoreTake(handle->lock, portMAX_DELAY);
}

static void unlock(web_direct_control_handle_t handle)
{
    (void)xSemaphoreGive(handle->lock);
}

static bool admitted(web_direct_control_handle_t handle, char *detail, size_t size)
{
    if (!handle->config.admission_gate) return true;
    return handle->config.admission_gate(handle->config.admission_context, detail, size);
}

static bool publish_event(web_direct_control_handle_t handle,
                          motion_application_event_action_t action,
                          float vx_mps,
                          float wz_radps,
                          bool deadman,
                          char *detail,
                          size_t detail_size)
{
    motion_application_event_t event = {
        .action = action,
        .source = COMMAND_AUTHORITY_SOURCE_WEB_DIRECT,
        .stream_id = handle->model.session_id,
        .sequence = ++handle->sequence,
        .received_at_ms = now_ms(),
        .vx_mps = vx_mps,
        .vy_mps = 0.0f,
        .wz_radps = wz_radps,
        .deadman = deadman,
        .hold_zero_when_deadman_released =
            action == MOTION_APPLICATION_EVENT_COMMAND && !deadman,
    };
    motion_application_submit_result_t result =
        motion_application_service_publish(handle->config.motion_application, &event);
    snprintf(detail, detail_size, "%s", result.detail);
    return result.accepted;
}

static bool telemetry_for_channel(web_direct_control_handle_t handle,
                                  svd48_workspace_channel_id_t channel,
                                  int16_t *rpm,
                                  float *current_a)
{
    svd48_workspace_controller_info_t controller;
    if (!svd48_workspace_controller_at(handle->config.svd48_workspace, 0U, &controller)) {
        return false;
    }
    svd48_workspace_channel_telemetry_t telemetry;
    if (!svd48_workspace_get_channel_telemetry(handle->config.svd48_workspace,
                                                controller.device_id,
                                                channel,
                                                &telemetry)) {
        return false;
    }
    *rpm = telemetry.observed_speed_rpm;
    *current_a = (float)telemetry.current_deciamp / 10.0f;
    return telemetry.online && !telemetry.stale;
}

static const char *state_name_locked(web_direct_control_handle_t handle)
{
    motion_status_snapshot_t status;
    if (motion_status_snapshot(handle->config.motion_status, &status) &&
        status.state == MOTION_CONTROL_FAULT) {
        web_direct_control_model_fault(&handle->model);
    }
    return web_direct_control_model_state_name(handle->model.state);
}

static void server_global_context_not_owned(void *context)
{
    (void)context;
}

static void server_close_handler(httpd_handle_t server, int fd)
{
    web_direct_control_handle_t handle = httpd_get_global_user_ctx(server);
    bool websocket_closed = false;
    if (handle) {
        lock(handle);
        if (handle->active_fd == fd) {
            handle->active_fd = -1;
            websocket_closed = true;
        }
        unlock(handle);
    }
    if (websocket_closed) {
        ESP_LOGI(TAG, "WEB_SOCKET_DISCONNECTED");
    }
    (void)close(fd);
}

static void send_status_async(web_direct_control_handle_t handle, int fd)
{
    int16_t m1_rpm = 0;
    int16_t m2_rpm = 0;
    float m1_current = 0.0f;
    float m2_current = 0.0f;
    const bool m1_valid = telemetry_for_channel(handle, SVD48_WORKSPACE_CHANNEL_M1,
                                                &m1_rpm, &m1_current);
    const bool m2_valid = telemetry_for_channel(handle, SVD48_WORKSPACE_CHANNEL_M2,
                                                &m2_rpm, &m2_current);
    wifi_manager_status_t wifi_status = {0};
    const bool wifi_valid = handle->config.wifi_manager &&
                            wifi_manager_get_status(handle->config.wifi_manager,
                                                    &wifi_status) == ESP_OK;
    char network[192] = "null";
    if (wifi_valid && wifi_status.mode == WIFI_MANAGER_MODE_SOFTAP) {
        snprintf(network, sizeof(network),
                 "{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"clients\":%u,\"dhcp\":%s}",
                 wifi_manager_mode_to_string(wifi_status.mode),
                 wifi_status.ssid,
                 wifi_status.ip_addr,
                 (unsigned)wifi_status.connected_clients,
                 wifi_status.dhcp_server_running ? "true" : "false");
    }
    char payload[896];
    lock(handle);
    if (m1_valid && fabsf((float)m1_rpm) > handle->peak_rpm_m1)
        handle->peak_rpm_m1 = fabsf((float)m1_rpm);
    if (m2_valid && fabsf((float)m2_rpm) > handle->peak_rpm_m2)
        handle->peak_rpm_m2 = fabsf((float)m2_rpm);
    if (m1_valid && fabsf(m1_current) > handle->peak_current_m1)
        handle->peak_current_m1 = fabsf(m1_current);
    if (m2_valid && fabsf(m2_current) > handle->peak_current_m2)
        handle->peak_current_m2 = fabsf(m2_current);
    const uint64_t age = handle->model.lease_seen ? now_ms() - handle->model.last_valid_ms : 0U;
    const char *state = state_name_locked(handle);
    const char *link = handle->active_fd >= 0 ? "CONNECTED" : "DISCONNECTED";
    snprintf(payload, sizeof(payload),
             "{\"type\":\"status\",\"state\":\"%s\",\"link\":\"%s\",\"last_command_ms\":%llu,"
             "\"network\":%s,"
             "\"m1\":{\"rpm\":%d,\"current_a\":%.1f,\"valid\":%s},"
             "\"m2\":{\"rpm\":%d,\"current_a\":%.1f,\"valid\":%s},"
             "\"peaks\":{\"rpm_m1\":%.0f,\"rpm_m2\":%.0f,\"current_m1\":%.1f,\"current_m2\":%.1f,\"torque\":null}}",
             state, link, (unsigned long long)age, network,
             (int)m1_rpm, m1_current, m1_valid ? "true" : "false",
             (int)m2_rpm, m2_current, m2_valid ? "true" : "false",
             handle->peak_rpm_m1, handle->peak_rpm_m2,
             handle->peak_current_m1, handle->peak_current_m2);
    unlock(handle);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    (void)httpd_ws_send_frame_async(handle->server, fd, &frame);
}

static void telemetry_task(void *arg)
{
    web_direct_control_handle_t handle = arg;
    while (!handle->stop_requested) {
        int fd = -1;
        uint64_t expired_session = 0U;
        lock(handle);
        if (web_direct_control_model_expire(&handle->model, now_ms())) {
            expired_session = handle->model.session_id;
            handle->active_fd = -1;
            ESP_LOGW(TAG, "WEB_TTL_EXPIRED session=%llu", (unsigned long long)expired_session);
            web_direct_control_model_release_session(&handle->model, expired_session);
        }
        fd = handle->active_fd;
        unlock(handle);
        if (fd >= 0) send_status_async(handle, fd);
        vTaskDelay(pdMS_TO_TICKS(WEB_DIRECT_CONTROL_TELEMETRY_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    const size_t length = (size_t)(web_ui_html_end - web_ui_html_start);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)web_ui_html_start, length);
}

static bool json_text_equals(cJSON *root, const char *key, const char *expected)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, expected) == 0;
}

static bool json_number(cJSON *root, const char *key, float *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = (float)item->valuedouble;
    return true;
}

static void send_result(web_direct_control_handle_t handle, int fd, bool accepted, const char *detail)
{
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"type\":\"result\",\"accepted\":%s,\"detail\":\"%s\"}",
             accepted ? "true" : "false", detail ? detail : "UNKNOWN");
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)payload, .len = strlen(payload)};
    (void)httpd_ws_send_frame_async(handle->server, fd, &frame);
}

static esp_err_t control_handler(httpd_req_t *request)
{
    web_direct_control_handle_t handle = request->user_ctx;
    const int fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        const uint64_t session = ((uint64_t)esp_random() << 32U) | esp_random();
        lock(handle);
        const bool claimed = web_direct_control_model_claim_session(&handle->model, session);
        if (claimed) handle->active_fd = fd;
        unlock(handle);
        send_result(handle, fd, claimed, claimed ? "WEB_SOCKET_CONNECTED" : "SESSION_BUSY");
        if (claimed) ESP_LOGI(TAG, "WEB_SOCKET_CONNECTED");
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(request, &frame, 0U);
    if (err != ESP_OK || frame.len == 0U || frame.len > WEB_DIRECT_CONTROL_MAX_FRAME_BYTES ||
        frame.type != HTTPD_WS_TYPE_TEXT) return ESP_FAIL;
    uint8_t data[WEB_DIRECT_CONTROL_MAX_FRAME_BYTES + 1U] = {0};
    frame.payload = data;
    err = httpd_ws_recv_frame(request, &frame, frame.len);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_ParseWithLength((const char *)data, frame.len);
    if (!root) { send_result(handle, fd, false, "BAD_JSON"); return ESP_OK; }
    bool accepted = false;
    char detail[48] = "REJECTED";
    lock(handle);
    if (fd != handle->active_fd) {
        snprintf(detail, sizeof(detail), "%s", "SESSION_BUSY");
        unlock(handle);
        cJSON_Delete(root);
        send_result(handle, fd, false, detail);
        return ESP_OK;
    }
    const uint64_t session = handle->model.session_id;
    if (json_text_equals(root, "type", "arm")) {
        if (!admitted(handle, detail, sizeof(detail))) {
            accepted = false;
        } else if (web_direct_control_model_arm(&handle->model, session, now_ms()) == WEB_DIRECT_MODEL_ACCEPTED) {
            accepted = publish_event(handle, MOTION_APPLICATION_EVENT_ARM, 0.0f, 0.0f, false, detail, sizeof(detail));
            if (accepted) {
                handle->peak_rpm_m1 = handle->peak_rpm_m2 = 0.0f;
                handle->peak_current_m1 = handle->peak_current_m2 = 0.0f;
                ESP_LOGI(TAG, "WEB_ARM");
            } else {
                (void)web_direct_control_model_disarm(&handle->model, session);
            }
        }
    } else if (json_text_equals(root, "type", "disarm")) {
        if (web_direct_control_model_disarm(&handle->model, session) == WEB_DIRECT_MODEL_ACCEPTED) {
            accepted = publish_event(handle, MOTION_APPLICATION_EVENT_DISARM, 0.0f, 0.0f, false, detail, sizeof(detail));
            if (accepted) ESP_LOGI(TAG, "WEB_DISARM");
        }
    } else if (json_text_equals(root, "type", "stop")) {
        if (web_direct_control_model_disarm(&handle->model, session) == WEB_DIRECT_MODEL_ACCEPTED) {
            accepted = publish_event(handle, MOTION_APPLICATION_EVENT_STOP, 0.0f, 0.0f, false, detail, sizeof(detail));
            if (accepted) ESP_LOGW(TAG, "WEB_STOP");
        }
    } else if (json_text_equals(root, "type", "command")) {
        float forward, turn;
        cJSON *deadman = cJSON_GetObjectItemCaseSensitive(root, "deadman");
        web_direct_control_command_t command;
        if (json_number(root, "forward", &forward) && json_number(root, "turn", &turn) && cJSON_IsBool(deadman) &&
            web_direct_control_model_command(&handle->model, session, now_ms(), forward, turn,
                                             cJSON_IsTrue(deadman), &command) == WEB_DIRECT_MODEL_ACCEPTED) {
            accepted = publish_event(handle, MOTION_APPLICATION_EVENT_COMMAND,
                                     command.forward * handle->config.max_vx_mps,
                                     command.turn * handle->config.max_wz_radps,
                                     command.deadman, detail, sizeof(detail));
            if (!accepted) web_direct_control_model_reject_command(&handle->model);
        }
    } else {
        snprintf(detail, sizeof(detail), "%s", "BAD_COMMAND");
    }
    unlock(handle);
    cJSON_Delete(root);
    if (!accepted) ESP_LOGW(TAG, "WEB_CONTROL_REJECTED %s", detail);
    send_result(handle, fd, accepted, detail);
    return ESP_OK;
}

esp_err_t web_direct_control_init(const web_direct_control_config_t *config,
                                  web_direct_control_handle_t *out_handle)
{
    if (!config || !out_handle || !config->motion_application || !config->motion_status ||
        !config->svd48_workspace || config->max_vx_mps <= 0.0f || config->max_wz_radps <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    web_direct_control_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) return ESP_ERR_NO_MEM;
    handle->config = *config;
    handle->active_fd = -1;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock || !web_direct_control_model_init(&handle->model,
            &(web_direct_control_model_config_t){.ttl_ms = WEB_DIRECT_CONTROL_DEFAULT_TTL_MS,
                                                  .deadzone = WEB_DIRECT_CONTROL_DEFAULT_DEADZONE})) {
        if (handle->lock) vSemaphoreDelete(handle->lock);
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t web_direct_control_start(web_direct_control_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2U;
    config.stack_size = 6144U;
    config.global_user_ctx = handle;
    config.global_user_ctx_free_fn = server_global_context_not_owned;
    config.close_fn = server_close_handler;
    esp_err_t err = httpd_start(&handle->server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = handle};
    const httpd_uri_t control = {.uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = handle, .is_websocket = true};
    if ((err = httpd_register_uri_handler(handle->server, &root)) != ESP_OK ||
        (err = httpd_register_uri_handler(handle->server, &control)) != ESP_OK ||
        xTaskCreate(telemetry_task, "web_direct_telemetry", WEB_DIRECT_CONTROL_TASK_STACK_SIZE,
                    handle, WEB_DIRECT_CONTROL_TASK_PRIORITY, &handle->telemetry_task) != pdPASS) {
        httpd_stop(handle->server);
        handle->server = NULL;
        return err == ESP_OK ? ESP_ERR_NO_MEM : err;
    }
    ESP_LOGW(TAG, "WEB_DIRECT experiment active: HTTP /, WebSocket /control, TTL=%ums", WEB_DIRECT_CONTROL_DEFAULT_TTL_MS);
    return ESP_OK;
}

void web_direct_control_deinit(web_direct_control_handle_t handle)
{
    if (!handle) return;
    handle->stop_requested = true;
    if (handle->telemetry_task) {
        vTaskDelay(pdMS_TO_TICKS(WEB_DIRECT_CONTROL_TELEMETRY_PERIOD_MS + 10U));
    }
    if (handle->server) httpd_stop(handle->server);
    if (handle->lock) vSemaphoreDelete(handle->lock);
    free(handle);
}
