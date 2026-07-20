#include "maintenance_lan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "serial_gateway_result.h"

static const char *TAG = "maintenance_lan";

#define MAINTENANCE_LAN_TASK_STACK 12288
#define MAINTENANCE_LAN_TASK_PRIORITY 2
#define MAINTENANCE_LAN_PACKET_MAX 768
#define MAINTENANCE_LAN_LOCK_TIMEOUT_MS 100
#define MAINTENANCE_LAN_SOCKET_TIMEOUT_SEC 1
#define MAINTENANCE_LAN_TYPE "botfarms_maintenance_request"
#define MAINTENANCE_LAN_RESPONSE_TYPE "botfarms_maintenance_response"
#define MAINTENANCE_LAN_MAX_LINES 24
#define MAINTENANCE_LAN_LINE_MAX 384

struct maintenance_lan_t {
    maintenance_lan_config_t config;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool stop_task;
    maintenance_lan_status_t status;
};

typedef struct {
    char lines[MAINTENANCE_LAN_MAX_LINES][MAINTENANCE_LAN_LINE_MAX];
    size_t line_count;
    char partial[MAINTENANCE_LAN_LINE_MAX];
    size_t partial_len;
    bool truncated;
} command_capture_t;

static esp_err_t take_lock(maintenance_lan_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(MAINTENANCE_LAN_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    snprintf(dest, dest_size, "%s", src ? src : "");
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static void record_packet(maintenance_lan_handle_t handle,
                          const char *sender,
                          const char *action,
                          const char *detail,
                          bool accepted,
                          bool command)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->status.packets_seen++;
    if (accepted) {
        handle->status.packets_accepted++;
    } else {
        handle->status.packets_rejected++;
    }
    if (command) {
        handle->status.commands++;
    }
    copy_text(handle->status.last_sender, sizeof(handle->status.last_sender), sender);
    copy_text(handle->status.last_action, sizeof(handle->status.last_action), action);
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), detail);
    xSemaphoreGive(handle->lock);
}

static void capture_flush_line(command_capture_t *capture)
{
    if (!capture || capture->partial_len == 0) {
        return;
    }
    capture->partial[capture->partial_len] = '\0';
    if (capture->line_count < MAINTENANCE_LAN_MAX_LINES) {
        copy_text(capture->lines[capture->line_count],
                  sizeof(capture->lines[capture->line_count]),
                  capture->partial);
        capture->line_count++;
    } else {
        capture->truncated = true;
    }
    capture->partial_len = 0;
}

static void capture_output(void *ctx, const char *chunk)
{
    command_capture_t *capture = (command_capture_t *)ctx;
    if (!capture || !chunk) {
        return;
    }

    for (const char *cursor = chunk; *cursor; cursor++) {
        char ch = *cursor;
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            capture_flush_line(capture);
            continue;
        }
        if (capture->partial_len + 1 < sizeof(capture->partial)) {
            capture->partial[capture->partial_len++] = ch;
        } else {
            capture->truncated = true;
        }
    }
}

static void add_lines(cJSON *root, const command_capture_t *capture)
{
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    if (!lines || !capture) {
        return;
    }
    for (size_t i = 0; i < capture->line_count; i++) {
        cJSON_AddItemToArray(lines, cJSON_CreateString(capture->lines[i]));
    }
}

static bool capture_error_detail(const command_capture_t *capture, char *detail, size_t detail_size)
{
    if (!capture || !detail || detail_size == 0) {
        return false;
    }
    for (size_t i = 0; i < capture->line_count; i++) {
        if (serial_gateway_error_code_from_line(capture->lines[i], detail, detail_size)) {
            return true;
        }
    }
    return false;
}

static cJSON *base_response(const char *request_id, const char *status, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "type", MAINTENANCE_LAN_RESPONSE_TYPE);
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "status", status ? status : "err");
    cJSON_AddStringToObject(root, "detail", detail ? detail : "UNKNOWN");
    return root;
}

static char *response_error(const char *request_id, const char *detail)
{
    cJSON *root = base_response(request_id, "err", detail);
    if (!root) {
        return NULL;
    }
    cJSON_AddArrayToObject(root, "lines");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void snapshot_status(maintenance_lan_handle_t handle, maintenance_lan_status_t *status)
{
    memset(status, 0, sizeof(*status));
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    *status = handle->status;
    xSemaphoreGive(handle->lock);
}

static char *response_status(maintenance_lan_handle_t handle, const char *request_id, const char *detail)
{
    maintenance_lan_status_t status;
    snapshot_status(handle, &status);

    cJSON *root = base_response(request_id, "ok", detail);
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "service", "maintenance_lan");
    cJSON_AddNumberToObject(root, "port", status.listen_port);
    cJSON_AddNumberToObject(root, "packets_seen", status.packets_seen);
    cJSON_AddNumberToObject(root, "packets_accepted", status.packets_accepted);
    cJSON_AddNumberToObject(root, "packets_rejected", status.packets_rejected);
    cJSON_AddNumberToObject(root, "commands", status.commands);
    cJSON_AddStringToObject(root, "last_sender", status.last_sender);
    cJSON_AddStringToObject(root, "last_action", status.last_action);

    if (handle->config.wifi_manager) {
        wifi_manager_status_t wifi_status;
        if (wifi_manager_get_status(handle->config.wifi_manager, &wifi_status) == ESP_OK) {
            cJSON_AddStringToObject(root, "wifi_state", wifi_manager_state_to_string(wifi_status.state));
            cJSON_AddStringToObject(root, "wifi_ip", wifi_status.ip_addr);
            cJSON_AddStringToObject(root, "wifi_ssid", wifi_status.ssid);
        }
    }

    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    if (lines) {
        char line[MAINTENANCE_LAN_LINE_MAX] = { 0 };
        snprintf(line,
                 sizeof(line),
                 "DATA MAINT_LAN TASK:%s PORT:%u SEEN:%lu ACCEPTED:%lu REJECTED:%lu COMMANDS:%lu LAST_SENDER:%s LAST_ACTION:%s DETAIL:%s",
                 status.task_running ? "RUNNING" : "STOPPED",
                 status.listen_port,
                 (unsigned long)status.packets_seen,
                 (unsigned long)status.packets_accepted,
                 (unsigned long)status.packets_rejected,
                 (unsigned long)status.commands,
                 status.last_sender[0] ? status.last_sender : "NONE",
                 status.last_action[0] ? status.last_action : "NONE",
                 status.last_detail[0] ? status.last_detail : "NONE");
        cJSON_AddItemToArray(lines, cJSON_CreateString(line));
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static char *response_command(maintenance_lan_handle_t handle,
                              const char *request_id,
                              const char *command,
                              const char *sender_ip,
                              const char *action)
{
    command_capture_t *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        record_packet(handle, sender_ip, action, "NO_MEM", false, false);
        return response_error(request_id, "NO_MEM");
    }

    esp_err_t err = serial_gateway_execute_command(handle->config.gateway,
                                                   command,
                                                   SERIAL_GATEWAY_POLICY_LAN_SAFE,
                                                   capture_output,
                                                   capture);
    capture_flush_line(capture);

    char command_error[64] = { 0 };
    bool semantic_error = capture_error_detail(capture, command_error, sizeof(command_error));
    bool success = err == ESP_OK && !semantic_error;
    const char *detail = err != ESP_OK ? "COMMAND_EXEC_FAILED" :
                         semantic_error ? command_error : "OK";

    // Authenticated, valid command packets are accepted even when the command reports an error.
    record_packet(handle, sender_ip, action, detail, true, true);

    cJSON *root = base_response(request_id, success ? "ok" : "err", detail);
    if (!root) {
        free(capture);
        return NULL;
    }
    cJSON_AddStringToObject(root, "command", command);
    cJSON_AddBoolToObject(root, "truncated", capture->truncated);
    add_lines(root, capture);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(capture);
    return out;
}

static char *handle_packet(maintenance_lan_handle_t handle, const char *payload, const char *sender_ip)
{
    const char *action = "unknown";
    const char *request_id = "";

    cJSON *root = cJSON_Parse(payload);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        record_packet(handle, sender_ip, action, "JSON_PARSE", false, false);
        return response_error(request_id, "JSON_PARSE");
    }

    request_id = json_string(root, "request_id");
    const char *type = json_string(root, "type");
    action = json_string(root, "action");
    if (!action || action[0] == '\0') {
        action = "status";
    }

    if (!type || strcmp(type, MAINTENANCE_LAN_TYPE) != 0) {
        record_packet(handle, sender_ip, action, "BAD_TYPE", false, false);
        char *response = response_error(request_id, "BAD_TYPE");
        cJSON_Delete(root);
        return response;
    }

    char expected_token[CONFIG_MANAGER_MAINTENANCE_LAN_TOKEN_MAX] = { 0 };
    bool token_set = false;
    esp_err_t err = config_manager_get_maintenance_lan_token(handle->config.config_manager,
                                                             expected_token,
                                                             sizeof(expected_token),
                                                             &token_set);
    if (err != ESP_OK || !token_set) {
        record_packet(handle, sender_ip, action, "AUTH_REQUIRED", false, false);
        char *response = response_error(request_id, "AUTH_REQUIRED");
        cJSON_Delete(root);
        return response;
    }

    const char *token = json_string(root, "token");
    if (!token || strcmp(token, expected_token) != 0) {
        memset(expected_token, 0, sizeof(expected_token));
        record_packet(handle, sender_ip, action, "BAD_TOKEN", false, false);
        char *response = response_error(request_id, "BAD_TOKEN");
        cJSON_Delete(root);
        return response;
    }
    memset(expected_token, 0, sizeof(expected_token));

    char *response = NULL;
    if (strcasecmp(action, "hello") == 0) {
        record_packet(handle, sender_ip, action, "HELLO", true, false);
        response = response_status(handle, request_id, "HELLO");
    } else if (strcasecmp(action, "status") == 0) {
        record_packet(handle, sender_ip, action, "STATUS", true, false);
        response = response_status(handle, request_id, "STATUS");
    } else if (strcasecmp(action, "command") == 0) {
        const char *command = json_string(root, "command");
        if (!command || command[0] == '\0' || strnlen(command, SERIAL_GATEWAY_COMMAND_MAX) >= SERIAL_GATEWAY_COMMAND_MAX) {
            record_packet(handle, sender_ip, action, "BAD_COMMAND", false, false);
            response = response_error(request_id, "BAD_COMMAND");
        } else {
            response = response_command(handle, request_id, command, sender_ip, action);
        }
    } else {
        record_packet(handle, sender_ip, action, "BAD_ACTION", false, false);
        response = response_error(request_id, "BAD_ACTION");
    }

    cJSON_Delete(root);
    return response;
}

static void maintenance_lan_task(void *arg)
{
    maintenance_lan_handle_t handle = (maintenance_lan_handle_t)arg;
    int sock = -1;

    if (take_lock(handle) == ESP_OK) {
        handle->status.task_running = true;
        xSemaphoreGive(handle->lock);
    }

    while (!handle->stop_task) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct timeval timeout = {
            .tv_sec = MAINTENANCE_LAN_SOCKET_TIMEOUT_SEC,
            .tv_usec = 0,
        };
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in listen_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(handle->config.listen_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            ESP_LOGW(TAG, "UDP bind failed errno=%d", errno);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        while (!handle->stop_task) {
            char packet[MAINTENANCE_LAN_PACKET_MAX] = { 0 };
            struct sockaddr_in source_addr;
            socklen_t source_len = sizeof(source_addr);
            int len = recvfrom(sock,
                               packet,
                               sizeof(packet) - 1,
                               0,
                               (struct sockaddr *)&source_addr,
                               &source_len);
            if (len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    continue;
                }
                break;
            }
            packet[len] = '\0';

            char sender_ip[MAINTENANCE_LAN_SENDER_MAX] = { 0 };
            inet_ntoa_r(source_addr.sin_addr, sender_ip, sizeof(sender_ip));

            char *response = handle_packet(handle, packet, sender_ip);
            if (response) {
                (void)sendto(sock,
                              response,
                              strlen(response),
                              0,
                              (struct sockaddr *)&source_addr,
                              source_len);
                cJSON_free(response);
            }
        }

        close(sock);
        sock = -1;
    }

    if (sock >= 0) {
        close(sock);
    }
    if (take_lock(handle) == ESP_OK) {
        handle->status.task_running = false;
        xSemaphoreGive(handle->lock);
    }
    vTaskDelete(NULL);
}

esp_err_t maintenance_lan_init(const maintenance_lan_config_t *config, maintenance_lan_handle_t *out_handle)
{
    if (!config || !out_handle || !config->config_manager || !config->gateway) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    maintenance_lan_handle_t handle = calloc(1, sizeof(struct maintenance_lan_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    if (handle->config.listen_port == 0) {
        handle->config.listen_port = MAINTENANCE_LAN_DEFAULT_PORT;
    }
    handle->status.listen_port = handle->config.listen_port;
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), "NEVER_RUN");

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = handle;
    return ESP_OK;
}

void maintenance_lan_deinit(maintenance_lan_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->stop_task = true;
    if (handle->task) {
        for (int i = 0; i < 120 && handle->status.task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t maintenance_lan_start(maintenance_lan_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->task) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->stop_task = false;
    BaseType_t ok = xTaskCreate(maintenance_lan_task,
                                "maintenance_lan",
                                MAINTENANCE_LAN_TASK_STACK,
                                handle,
                                MAINTENANCE_LAN_TASK_PRIORITY,
                                &handle->task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "maintenance LAN listener started UDP/%u", handle->config.listen_port);
    return ESP_OK;
}

esp_err_t maintenance_lan_get_status(maintenance_lan_handle_t handle, maintenance_lan_status_t *status)
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
