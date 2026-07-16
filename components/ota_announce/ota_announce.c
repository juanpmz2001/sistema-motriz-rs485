#include "ota_announce.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "ota_announce";

#define OTA_ANNOUNCE_TASK_STACK 8192
#define OTA_ANNOUNCE_TASK_PRIORITY 2
#define OTA_ANNOUNCE_PACKET_MAX 512
#define OTA_ANNOUNCE_RESPONSE_MAX 256
#define OTA_ANNOUNCE_LOCK_TIMEOUT_MS 100
#define OTA_ANNOUNCE_SOCKET_TIMEOUT_SEC 1
#define OTA_ANNOUNCE_TYPE "botfarms_ota_offer"

struct ota_announce_t {
    ota_announce_config_t config;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool stop_task;
    ota_announce_status_t status;
};

static esp_err_t take_lock(ota_announce_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(OTA_ANNOUNCE_LOCK_TIMEOUT_MS)) == pdTRUE
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

static bool json_u16(const cJSON *root, const char *key, uint16_t *out_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) ||
        item->valuedouble <= 0.0 ||
        item->valuedouble > 65535.0 ||
        item->valuedouble != (double)(uint16_t)item->valuedouble) {
        return false;
    }
    *out_value = (uint16_t)item->valuedouble;
    return true;
}

static void record_packet(ota_announce_handle_t handle,
                          const char *sender,
                          const char *action,
                          const char *detail,
                          bool accepted)
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
    copy_text(handle->status.last_sender, sizeof(handle->status.last_sender), sender);
    copy_text(handle->status.last_action, sizeof(handle->status.last_action), action);
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), detail);
    xSemaphoreGive(handle->lock);
}

static void record_action_success(ota_announce_handle_t handle, const char *action, const char *detail)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    if (strcasecmp(action, "check") == 0) {
        handle->status.checks++;
    } else if (strcasecmp(action, "download_test") == 0) {
        handle->status.download_tests++;
    } else if (strcasecmp(action, "update") == 0) {
        handle->status.updates++;
    }
    copy_text(handle->status.last_action, sizeof(handle->status.last_action), action);
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), detail);
    xSemaphoreGive(handle->lock);
}

static void response_error(char *response, size_t response_size, const char *action, const char *detail)
{
    snprintf(response,
             response_size,
             "{\"status\":\"err\",\"action\":\"%s\",\"detail\":\"%s\"}",
             action ? action : "unknown",
             detail ? detail : "UNKNOWN");
}

static void response_ok(char *response, size_t response_size, const char *action, const char *detail)
{
    snprintf(response,
             response_size,
             "{\"status\":\"ok\",\"action\":\"%s\",\"detail\":\"%s\"}",
             action ? action : "unknown",
             detail ? detail : "OK");
}

static bool wifi_connected(ota_announce_handle_t handle, char *detail, size_t detail_size)
{
    wifi_manager_status_t wifi_status;
    esp_err_t err = wifi_manager_get_status(handle->config.wifi_manager, &wifi_status);
    if (err != ESP_OK) {
        snprintf(detail, detail_size, "WIFI_STATUS_0x%x", err);
        return false;
    }
    if (wifi_status.state != WIFI_MANAGER_STATE_CONNECTED) {
        snprintf(detail,
                 detail_size,
                 "WIFI_%s",
                 wifi_manager_state_to_string(wifi_status.state));
        return false;
    }
    copy_text(detail, detail_size, "OK");
    return true;
}

static bool robot_safe_for_ota(ota_announce_handle_t handle, char *detail, size_t detail_size)
{
    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        snprintf(detail, detail_size, "ROBOT_NOT_SAFE_%s", reason[0] ? reason : "UNKNOWN");
        return false;
    }
    copy_text(detail, detail_size, "SAFE");
    return true;
}

static void perform_check(ota_announce_handle_t handle,
                          const char *action,
                          char *response,
                          size_t response_size)
{
    ota_manager_check_result_t result;
    esp_err_t err = ota_manager_force_check(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        const char *detail = result.detail[0] ? result.detail : "OTA_CHECK";
        response_error(response, response_size, action, detail);
        return;
    }

    const char *detail = ota_manager_check_status_to_string(result.status);
    snprintf(response,
             response_size,
             "{\"status\":\"ok\",\"action\":\"%s\",\"detail\":\"%s\",\"current_build\":%lu,\"remote_build\":%lu}",
             action,
             detail,
             (unsigned long)result.current_build_number,
             (unsigned long)result.build_number);
    record_action_success(handle, action, detail);
}

static void perform_download_test(ota_announce_handle_t handle,
                                  const char *action,
                                  char *response,
                                  size_t response_size)
{
    char detail[64] = { 0 };
    if (!wifi_connected(handle, detail, sizeof(detail)) ||
        !robot_safe_for_ota(handle, detail, sizeof(detail))) {
        response_error(response, response_size, action, detail);
        return;
    }

    ota_manager_download_result_t result;
    esp_err_t err = ota_manager_download_test(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        response_error(response,
                       response_size,
                       action,
                       result.detail[0] ? result.detail : "OTA_DOWNLOAD_TEST");
        return;
    }

    snprintf(response,
             response_size,
             "{\"status\":\"ok\",\"action\":\"%s\",\"detail\":\"VERIFIED\",\"partition\":\"%s\",\"bytes\":%lu}",
             action,
             result.partition_label,
             (unsigned long)result.bytes_written);
    record_action_success(handle, action, "VERIFIED");
}

static void perform_update(ota_announce_handle_t handle,
                           const char *action,
                           char *response,
                           size_t response_size)
{
    char detail[64] = { 0 };
    if (!wifi_connected(handle, detail, sizeof(detail)) ||
        !robot_safe_for_ota(handle, detail, sizeof(detail))) {
        response_error(response, response_size, action, detail);
        return;
    }

    ota_manager_download_result_t result;
    esp_err_t err = ota_manager_download_to_inactive(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        response_error(response,
                       response_size,
                       action,
                       result.detail[0] ? result.detail : "OTA_DOWNLOAD");
        return;
    }

    err = robot_control_prepare_for_ota(handle->config.robot);
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "PREPARE_0x%x", err);
        response_error(response, response_size, action, detail);
        return;
    }

    err = ota_manager_set_boot_partition(result.partition_label);
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "SET_BOOT_0x%x", err);
        response_error(response, response_size, action, detail);
        return;
    }

    snprintf(response,
             response_size,
             "{\"status\":\"ok\",\"action\":\"%s\",\"detail\":\"REBOOTING\",\"partition\":\"%s\",\"bytes\":%lu}",
             action,
             result.partition_label,
             (unsigned long)result.bytes_written);
    record_action_success(handle, action, "REBOOTING");
}

static void handle_packet(ota_announce_handle_t handle,
                          const char *payload,
                          const char *sender_ip,
                          char *response,
                          size_t response_size,
                          bool *restart_after_response)
{
    *restart_after_response = false;
    const char *action = "unknown";

    cJSON *root = cJSON_Parse(payload);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        record_packet(handle, sender_ip, action, "JSON_PARSE", false);
        response_error(response, response_size, action, "JSON_PARSE");
        return;
    }

    const char *type = json_string(root, "type");
    action = json_string(root, "action");
    if (!action || action[0] == '\0') {
        action = "check";
    }

    if (!type || strcmp(type, OTA_ANNOUNCE_TYPE) != 0) {
        record_packet(handle, sender_ip, action, "BAD_TYPE", false);
        response_error(response, response_size, action, "BAD_TYPE");
        cJSON_Delete(root);
        return;
    }

    char expected_token[CONFIG_MANAGER_OTA_ANNOUNCE_TOKEN_MAX] = { 0 };
    bool token_set = false;
    esp_err_t err = config_manager_get_ota_announce_token(handle->config.config_manager,
                                                          expected_token,
                                                          sizeof(expected_token),
                                                          &token_set);
    if (err != ESP_OK || !token_set) {
        record_packet(handle, sender_ip, action, "TOKEN_NOT_CONFIGURED", false);
        response_error(response, response_size, action, "TOKEN_NOT_CONFIGURED");
        cJSON_Delete(root);
        return;
    }

    const char *token = json_string(root, "token");
    if (!token || strcmp(token, expected_token) != 0) {
        record_packet(handle, sender_ip, action, "BAD_TOKEN", false);
        response_error(response, response_size, action, "BAD_TOKEN");
        memset(expected_token, 0, sizeof(expected_token));
        cJSON_Delete(root);
        return;
    }
    memset(expected_token, 0, sizeof(expected_token));

    uint16_t port = 0;
    if (!json_u16(root, "port", &port)) {
        record_packet(handle, sender_ip, action, "BAD_PORT", false);
        response_error(response, response_size, action, "BAD_PORT");
        cJSON_Delete(root);
        return;
    }

    const char *manifest = json_string(root, "manifest");
    if (!manifest || manifest[0] != '/') {
        record_packet(handle, sender_ip, action, "BAD_MANIFEST", false);
        response_error(response, response_size, action, "BAD_MANIFEST");
        cJSON_Delete(root);
        return;
    }

    err = config_manager_set_ota_server(handle->config.config_manager, sender_ip, port);
    if (err == ESP_OK) {
        err = config_manager_set_ota_manifest_path(handle->config.config_manager, manifest);
    }
    if (err != ESP_OK) {
        record_packet(handle, sender_ip, action, "CONFIG_WRITE", false);
        response_error(response, response_size, action, "CONFIG_WRITE");
        cJSON_Delete(root);
        return;
    }

    record_packet(handle, sender_ip, action, "ACCEPTED", true);

    if (strcasecmp(action, "config") == 0) {
        response_ok(response, response_size, action, "CONFIGURED");
    } else if (strcasecmp(action, "check") == 0) {
        perform_check(handle, action, response, response_size);
    } else if (strcasecmp(action, "download_test") == 0) {
        perform_download_test(handle, action, response, response_size);
    } else if (strcasecmp(action, "update") == 0) {
        perform_update(handle, action, response, response_size);
        *restart_after_response = strstr(response, "\"detail\":\"REBOOTING\"") != NULL;
    } else {
        response_error(response, response_size, action, "BAD_ACTION");
    }

    cJSON_Delete(root);
}

static void announce_task(void *arg)
{
    ota_announce_handle_t handle = (ota_announce_handle_t)arg;
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
            .tv_sec = OTA_ANNOUNCE_SOCKET_TIMEOUT_SEC,
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
            char packet[OTA_ANNOUNCE_PACKET_MAX] = { 0 };
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

            char sender_ip[OTA_ANNOUNCE_SENDER_MAX] = { 0 };
            inet_ntoa_r(source_addr.sin_addr, sender_ip, sizeof(sender_ip));

            char response[OTA_ANNOUNCE_RESPONSE_MAX] = { 0 };
            bool restart_after_response = false;
            handle_packet(handle,
                          packet,
                          sender_ip,
                          response,
                          sizeof(response),
                          &restart_after_response);

            if (response[0] != '\0') {
                (void)sendto(sock,
                              response,
                              strlen(response),
                              0,
                              (struct sockaddr *)&source_addr,
                              source_len);
            }

            if (restart_after_response) {
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_restart();
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
        handle->task = NULL;
        xSemaphoreGive(handle->lock);
    }
    vTaskDelete(NULL);
}

esp_err_t ota_announce_init(const ota_announce_config_t *config, ota_announce_handle_t *out_handle)
{
    if (!config || !out_handle || !config->config_manager || !config->wifi_manager ||
        !config->ota_manager || !config->robot) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    ota_announce_handle_t handle = calloc(1, sizeof(struct ota_announce_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    if (handle->config.listen_port == 0) {
        handle->config.listen_port = OTA_ANNOUNCE_DEFAULT_PORT;
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

void ota_announce_deinit(ota_announce_handle_t handle)
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

esp_err_t ota_announce_start(ota_announce_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->task) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->stop_task = false;
    BaseType_t ok = xTaskCreate(announce_task,
                                "ota_announce",
                                OTA_ANNOUNCE_TASK_STACK,
                                handle,
                                OTA_ANNOUNCE_TASK_PRIORITY,
                                &handle->task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA LAN announce listener started UDP/%u", handle->config.listen_port);
    return ESP_OK;
}

esp_err_t ota_announce_get_status(ota_announce_handle_t handle, ota_announce_status_t *status)
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
