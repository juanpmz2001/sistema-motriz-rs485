#include "control_lan.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "control_lan";

#define CONTROL_LAN_RESPONSE_MAX 1024U
#define CONTROL_LAN_JSON_DEPTH_MAX 8U
#define CONTROL_LAN_SOCKET_TIMEOUT_US 20000
#define CONTROL_LAN_RETRY_DELAY_MS 500U

struct control_lan_t {
    control_lan_config_t config;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t task_done;
    TaskHandle_t task;
    int socket_fd;
    bool stop_requested;
    bool deinitializing;
    bool stream_active;
    uint64_t active_stream_hash;
    uint64_t last_sequence;
    uint32_t active_authority_epoch;
    control_lan_status_t status;
};

static void state_lock(control_lan_handle_t handle)
{
    (void)xSemaphoreTake(handle->lock, portMAX_DELAY);
}

static void state_unlock(control_lan_handle_t handle)
{
    xSemaphoreGive(handle->lock);
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    snprintf(dest, dest_size, "%s", src ? src : "");
}

static void secure_zero(void *data, size_t size)
{
    volatile unsigned char *cursor = (volatile unsigned char *)data;
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

static bool constant_time_equal(const unsigned char *left,
                                const unsigned char *right,
                                size_t size)
{
    unsigned char difference = 0;
    for (size_t i = 0; i < size; i++) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static size_t object_key_count(const cJSON *object, const char *key, const cJSON **first)
{
    size_t count = 0;
    if (first) {
        *first = NULL;
    }
    if (!cJSON_IsObject(object) || !key) {
        return 0;
    }

    for (const cJSON *item = object->child; item; item = item->next) {
        if (item->string && strcmp(item->string, key) == 0) {
            if (count == 0 && first) {
                *first = item;
            }
            count++;
        }
    }
    return count;
}

static bool unique_item(const cJSON *object, const char *key, const cJSON **item)
{
    return object_key_count(object, key, item) == 1;
}

static bool bounded_json_string(const cJSON *object,
                                const char *key,
                                size_t capacity,
                                bool allow_empty,
                                const char **value,
                                size_t *value_len)
{
    const cJSON *item = NULL;
    if (!unique_item(object, key, &item) || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }

    size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity || (!allow_empty && length == 0)) {
        return false;
    }
    if (value) {
        *value = item->valuestring;
    }
    if (value_len) {
        *value_len = length;
    }
    return true;
}

static bool payload_precheck(const char *payload, size_t payload_len, const char **detail)
{
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < payload_len; i++) {
        unsigned char ch = (unsigned char)payload[i];
        if (ch == '\0') {
            *detail = "JSON_NUL";
            return false;
        }

        if (in_string) {
            if (escaped) {
                if (ch == 'u' && i + 4 < payload_len &&
                    payload[i + 1] == '0' && payload[i + 2] == '0' &&
                    payload[i + 3] == '0' && payload[i + 4] == '0') {
                    *detail = "JSON_NUL";
                    return false;
                }
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            depth++;
            if (depth > CONTROL_LAN_JSON_DEPTH_MAX) {
                *detail = "JSON_DEPTH";
                return false;
            }
        } else if ((ch == '}' || ch == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

static bool json_whitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static cJSON *parse_payload(const char *payload, size_t payload_len)
{
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, payload_len, &parse_end, false);
    if (!root || !parse_end) {
        cJSON_Delete(root);
        return NULL;
    }

    const char *payload_end = payload + payload_len;
    while (parse_end < payload_end && json_whitespace(*parse_end)) {
        parse_end++;
    }
    if (parse_end != payload_end || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static const char *action_name(control_lan_action_t action)
{
    switch (action) {
        case CONTROL_LAN_ACTION_ARM:
            return "arm";
        case CONTROL_LAN_ACTION_COMMAND:
            return "command";
        case CONTROL_LAN_ACTION_DISARM:
            return "disarm";
        case CONTROL_LAN_ACTION_STOP:
            return "stop";
        default:
            return "unknown";
    }
}

static bool parse_action(const char *value, control_lan_action_t *action)
{
    if (strcmp(value, "arm") == 0) {
        *action = CONTROL_LAN_ACTION_ARM;
    } else if (strcmp(value, "command") == 0) {
        *action = CONTROL_LAN_ACTION_COMMAND;
    } else if (strcmp(value, "disarm") == 0) {
        *action = CONTROL_LAN_ACTION_DISARM;
    } else if (strcmp(value, "stop") == 0) {
        *action = CONTROL_LAN_ACTION_STOP;
    } else {
        return false;
    }
    return true;
}

static bool parse_sequence(const cJSON *root,
                           const char *payload,
                           size_t payload_len,
                           uint64_t *sequence)
{
    const cJSON *item = NULL;
    if (!unique_item(root, "sequence", &item) || !cJSON_IsNumber(item)) {
        return false;
    }

    static const char sequence_key[] = "sequence";
    size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;
    bool found = false;
    uint64_t parsed = 0U;

    for (size_t index = 0U; index < payload_len; index++) {
        char ch = payload[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '{' || ch == '[') {
            depth++;
            continue;
        }
        if (ch == '}' || ch == ']') {
            if (depth > 0U) {
                depth--;
            }
            continue;
        }
        if (ch != '"') {
            continue;
        }

        if (depth != 1U ||
            index + sizeof(sequence_key) + 1U > payload_len ||
            memcmp(payload + index + 1U,
                   sequence_key,
                   sizeof(sequence_key) - 1U) != 0 ||
            payload[index + sizeof(sequence_key)] != '"') {
            in_string = true;
            continue;
        }

        size_t cursor = index + sizeof(sequence_key) + 1U;
        while (cursor < payload_len && json_whitespace(payload[cursor])) {
            cursor++;
        }
        if (cursor >= payload_len || payload[cursor] != ':') {
            index += sizeof(sequence_key);
            continue;
        }
        cursor++;
        while (cursor < payload_len && json_whitespace(payload[cursor])) {
            cursor++;
        }
        if (cursor >= payload_len || payload[cursor] < '0' ||
            payload[cursor] > '9' || found) {
            return false;
        }

        uint64_t value = 0U;
        while (cursor < payload_len && payload[cursor] >= '0' &&
               payload[cursor] <= '9') {
            uint64_t digit = (uint64_t)(payload[cursor] - '0');
            if (value > (CONTROL_LAN_MAX_EXACT_SEQUENCE - digit) / 10U) {
                return false;
            }
            value = value * 10U + digit;
            cursor++;
        }
        while (cursor < payload_len && json_whitespace(payload[cursor])) {
            cursor++;
        }
        if (cursor >= payload_len ||
            (payload[cursor] != ',' && payload[cursor] != '}')) {
            return false;
        }
        parsed = value;
        found = true;
        index = cursor - 1U;
    }

    if (!found || item->valuedouble != (double)parsed) {
        return false;
    }
    *sequence = parsed;
    return true;
}

static bool parse_finite_number(const cJSON *object, const char *key, double *value)
{
    const cJSON *item = NULL;
    if (!unique_item(object, key, &item) || !cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool parse_command(control_lan_handle_t handle,
                          const cJSON *root,
                          control_lan_event_t *event,
                          const char **detail)
{
    const cJSON *command = NULL;
    if (!unique_item(root, "command", &command) || !cJSON_IsObject(command)) {
        *detail = "BAD_COMMAND";
        return false;
    }
    double vx_mps;
    double vy_mps;
    double wz_radps;
    if (!parse_finite_number(command, "vx_mps", &vx_mps) ||
        !parse_finite_number(command, "vy_mps", &vy_mps) ||
        !parse_finite_number(command, "wz_radps", &wz_radps) ||
        fabs(vx_mps) > FLT_MAX || fabs(vy_mps) > FLT_MAX ||
        fabs(wz_radps) > FLT_MAX) {
        *detail = "BAD_VELOCITY";
        return false;
    }
    /*
     * The profile limits and event payload are floats, while cJSON exposes
     * decimal JSON as double. Compare in the same representation consumed by
     * motion_application so a decimal equal to a published limit (for example
     * 0.02) is not rejected merely because its configured float widens to a
     * slightly smaller double.
     */
    const float requested_vx_mps = (float)vx_mps;
    const float requested_vy_mps = (float)vy_mps;
    const float requested_wz_radps = (float)wz_radps;
    if (fabsf(requested_vx_mps) > handle->config.max_abs_vx_mps ||
        fabsf(requested_vy_mps) > handle->config.max_abs_vy_mps ||
        fabsf(requested_wz_radps) > handle->config.max_abs_wz_radps) {
        *detail = "BAD_VELOCITY";
        return false;
    }
    event->vx_mps = requested_vx_mps;
    event->vy_mps = requested_vy_mps;
    event->wz_radps = requested_wz_radps;

    const cJSON *deadman = NULL;
    if (!unique_item(command, "deadman", &deadman) || !cJSON_IsBool(deadman)) {
        *detail = "BAD_DEADMAN";
        return false;
    }
    event->deadman = cJSON_IsTrue(deadman);
    return true;
}

static bool authenticate(control_lan_handle_t handle, const char *token, size_t token_len)
{
    char supplied_token[CONFIG_MANAGER_MAINTENANCE_LAN_TOKEN_MAX] = { 0 };
    char expected_token[CONFIG_MANAGER_MAINTENANCE_LAN_TOKEN_MAX] = { 0 };
    bool token_set = false;

    memcpy(supplied_token, token, token_len);
    esp_err_t err = config_manager_get_maintenance_lan_token(handle->config.config_manager,
                                                             expected_token,
                                                             sizeof(expected_token),
                                                             &token_set);
    bool authenticated = err == ESP_OK && token_set &&
                         constant_time_equal((const unsigned char *)supplied_token,
                                             (const unsigned char *)expected_token,
                                             sizeof(expected_token));
    secure_zero(supplied_token, sizeof(supplied_token));
    secure_zero(expected_token, sizeof(expected_token));
    return authenticated;
}

static bool write_response(char *response,
                           size_t response_size,
                           const char *request_id,
                           const char *status,
                           const char *detail)
{
    response[0] = '\0';
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }

    bool complete = cJSON_AddStringToObject(root, "type", CONTROL_LAN_RESPONSE_TYPE) != NULL &&
                    cJSON_AddStringToObject(root, "protocol_version", CONTROL_LAN_PROTOCOL_VERSION) != NULL &&
                    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "") != NULL &&
                    cJSON_AddStringToObject(root, "status", status) != NULL &&
                    cJSON_AddStringToObject(root, "detail", detail) != NULL;
    bool printed = complete &&
                   cJSON_PrintPreallocated(root, response, (int)response_size, false);
    cJSON_Delete(root);
    if (!printed) {
        response[0] = '\0';
    }
    return printed;
}

static void record_packet(control_lan_handle_t handle,
                          const char *sender,
                          const char *action,
                          const char *detail,
                          bool accepted)
{
    state_lock(handle);
    handle->status.packets_seen++;
    if (accepted) {
        handle->status.packets_accepted++;
    } else {
        handle->status.packets_rejected++;
    }
    copy_text(handle->status.last_sender, sizeof(handle->status.last_sender), sender);
    copy_text(handle->status.last_action, sizeof(handle->status.last_action), action);
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), detail);
    state_unlock(handle);
}

static void reject_packet(control_lan_handle_t handle,
                          const char *sender,
                          const char *action,
                          const char *request_id,
                          const char *detail,
                          char *response,
                          size_t response_size)
{
    record_packet(handle, sender, action, detail, false);
    (void)write_response(response, response_size, request_id, "err", detail);
}

static control_lan_authority_status_t authority_status(
    control_lan_handle_t handle)
{
    control_lan_authority_status_t status = {
        .lan_allowed = true,
        .revocation_epoch = 0U,
    };
    copy_text(status.detail, sizeof(status.detail), "LAN_ALLOWED");
    if (handle->config.authority_status_callback) {
        status = handle->config.authority_status_callback(
            handle->config.authority_context);
        if (!memchr(status.detail, '\0', sizeof(status.detail)) ||
            status.detail[0] == '\0') {
            copy_text(status.detail,
                      sizeof(status.detail),
                      status.lan_allowed ? "LAN_ALLOWED" : "LAN_BLOCKED");
        }
    }
    state_lock(handle);
    handle->status.lan_allowed = status.lan_allowed;
    handle->status.revocation_epoch = status.revocation_epoch;
    copy_text(handle->status.authority_detail,
              sizeof(handle->status.authority_detail),
              status.detail);
    state_unlock(handle);
    return status;
}

static void normalize_callback_detail(const control_lan_callback_result_t *result,
                                      char *detail,
                                      size_t detail_size)
{
    const char *fallback = result->accepted ? "ACCEPTED" : "REJECTED";
    if (!memchr(result->detail, '\0', sizeof(result->detail)) || result->detail[0] == '\0') {
        copy_text(detail, detail_size, fallback);
        return;
    }
    copy_text(detail, detail_size, result->detail);
}

static const char *validate_event_order(control_lan_handle_t handle,
                                        const control_lan_event_t *event)
{
    if (event->action == CONTROL_LAN_ACTION_STOP ||
        event->action == CONTROL_LAN_ACTION_DISARM) {
        return NULL;
    }
    if (event->action == CONTROL_LAN_ACTION_COMMAND) {
        if (!handle->stream_active) {
            return "STREAM_NOT_ARMED";
        }
        if (event->stream_id_hash != handle->active_stream_hash) {
            return "STREAM_MISMATCH";
        }
        return event->sequence > handle->last_sequence ? NULL
                                                       : "SEQUENCE_NOT_INCREASING";
    }

    if (handle->stream_active &&
        event->stream_id_hash == handle->active_stream_hash) {
        return event->sequence > handle->last_sequence ? NULL
                                                       : "SEQUENCE_NOT_INCREASING";
    }
    return NULL;
}

static void retire_active_stream(control_lan_handle_t handle)
{
    handle->stream_active = false;
    handle->active_stream_hash = 0U;
    handle->last_sequence = 0U;
    handle->active_authority_epoch = 0U;
}

static void enforce_authority(control_lan_handle_t handle,
                              const control_lan_authority_status_t *authority,
                              uint64_t timestamp_us)
{
    if (!handle->stream_active ||
        (authority->lan_allowed &&
         handle->active_authority_epoch == authority->revocation_epoch)) {
        return;
    }

    control_lan_event_t stop_event = {
        .action = CONTROL_LAN_ACTION_STOP,
        .stream_id_hash = handle->active_stream_hash,
        .sequence = handle->last_sequence,
        .timestamp_us = timestamp_us,
    };
    control_lan_callback_result_t stop_result =
        handle->config.event_callback(stop_event, handle->config.callback_context);
    const bool accepted = stop_result.accepted;
    retire_active_stream(handle);

    state_lock(handle);
    copy_text(handle->status.last_action,
              sizeof(handle->status.last_action),
              "authority");
    copy_text(handle->status.last_detail,
              sizeof(handle->status.last_detail),
              accepted ? authority->detail : "AUTHORITY_STOP_REJECTED");
    state_unlock(handle);
    ESP_LOGW(TAG,
             "LAN stream revoked authority=%s epoch=%lu stop=%s",
             authority->detail,
             (unsigned long)authority->revocation_epoch,
             accepted ? "QUEUED" : "REJECTED");
}

static void commit_event_order(control_lan_handle_t handle,
                               const control_lan_event_t *event,
                               bool callback_accepted,
                               uint32_t authority_epoch)
{
    if (event->action == CONTROL_LAN_ACTION_STOP ||
        event->action == CONTROL_LAN_ACTION_DISARM) {
        retire_active_stream(handle);
        return;
    }
    if (!callback_accepted) {
        return;
    }
    if (event->action == CONTROL_LAN_ACTION_ARM &&
        (!handle->stream_active ||
         event->stream_id_hash != handle->active_stream_hash)) {
        retire_active_stream(handle);
        handle->stream_active = true;
        handle->active_stream_hash = event->stream_id_hash;
        handle->active_authority_epoch = authority_epoch;
    }
    handle->last_sequence = event->sequence;
}

static void handle_packet(control_lan_handle_t handle,
                          const char *payload,
                          size_t payload_len,
                          const char *sender,
                          uint64_t received_at_us,
                          char *response,
                          size_t response_size)
{
    char request_id[CONTROL_LAN_REQUEST_ID_MAX] = { 0 };
    const char *action_text = "unknown";
    const char *precheck_detail = "JSON_PARSE";
    response[0] = '\0';

    if (!payload_precheck(payload, payload_len, &precheck_detail)) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      precheck_detail,
                      response,
                      response_size);
        return;
    }

    cJSON *root = parse_payload(payload, payload_len);
    if (!root) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "JSON_PARSE",
                      response,
                      response_size);
        return;
    }

    const char *json_request_id = NULL;
    size_t request_id_len = 0;
    if (!bounded_json_string(root,
                             "request_id",
                             sizeof(request_id),
                             true,
                             &json_request_id,
                             &request_id_len)) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_REQUEST_ID",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }
    memcpy(request_id, json_request_id, request_id_len);
    request_id[request_id_len] = '\0';

    const char *type = NULL;
    if (!bounded_json_string(root, "type", sizeof(CONTROL_LAN_REQUEST_TYPE), false, &type, NULL) ||
        strcmp(type, CONTROL_LAN_REQUEST_TYPE) != 0) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_TYPE",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    const char *protocol_version = NULL;
    if (!bounded_json_string(root,
                             "protocol_version",
                             sizeof(CONTROL_LAN_PROTOCOL_VERSION),
                             false,
                             &protocol_version,
                             NULL) ||
        strcmp(protocol_version, CONTROL_LAN_PROTOCOL_VERSION) != 0) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_PROTOCOL_VERSION",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    const char *json_action = NULL;
    control_lan_action_t action = CONTROL_LAN_ACTION_STOP;
    if (!bounded_json_string(root,
                             "action",
                             CONTROL_LAN_ACTION_MAX,
                             false,
                             &json_action,
                             NULL) ||
        !parse_action(json_action, &action)) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_ACTION",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }
    action_text = action_name(action);

    const char *token = NULL;
    size_t token_len = 0;
    bool token_valid = bounded_json_string(root,
                                           "token",
                                           CONFIG_MANAGER_MAINTENANCE_LAN_TOKEN_MAX,
                                           false,
                                           &token,
                                           &token_len);
    bool authenticated = token_valid && authenticate(handle, token, token_len);
    if (token_valid) {
        secure_zero((void *)token, token_len);
    }
    if (!authenticated) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "AUTH_FAILED",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    const char *stream_id = NULL;
    if (!bounded_json_string(root,
                             "stream_id",
                             CONTROL_LAN_STREAM_ID_MAX,
                             false,
                             &stream_id,
                             NULL)) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_STREAM_ID",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    control_lan_event_t event = {
        .action = action,
        .stream_id_hash = control_lan_hash_stream_id(stream_id),
    };
    if (!parse_sequence(root, payload, payload_len, &event.sequence)) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      "BAD_SEQUENCE",
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    if (action == CONTROL_LAN_ACTION_COMMAND) {
        const char *command_detail = "BAD_COMMAND";
        if (!parse_command(handle, root, &event, &command_detail)) {
            reject_packet(handle,
                          sender,
                          action_text,
                          request_id,
                          command_detail,
                          response,
                          response_size);
            cJSON_Delete(root);
            return;
        }
    }

    event.timestamp_us = received_at_us;
    control_lan_authority_status_t authority = authority_status(handle);
    enforce_authority(handle, &authority, received_at_us);
    if ((action == CONTROL_LAN_ACTION_ARM || action == CONTROL_LAN_ACTION_COMMAND) &&
        !authority.lan_allowed) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      authority.detail,
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }
    const char *order_error = validate_event_order(handle, &event);
    if (order_error) {
        reject_packet(handle,
                      sender,
                      action_text,
                      request_id,
                      order_error,
                      response,
                      response_size);
        cJSON_Delete(root);
        return;
    }

    if (action == CONTROL_LAN_ACTION_ARM && handle->stream_active &&
        event.stream_id_hash != handle->active_stream_hash) {
        control_lan_event_t stop_event = {
            .action = CONTROL_LAN_ACTION_STOP,
            .stream_id_hash = handle->active_stream_hash,
            .sequence = handle->last_sequence,
            .timestamp_us = event.timestamp_us,
        };
        control_lan_callback_result_t stop_result =
            handle->config.event_callback(stop_event,
                                          handle->config.callback_context);
        retire_active_stream(handle);
        if (!stop_result.accepted) {
            reject_packet(handle,
                          sender,
                          action_text,
                          request_id,
                          "STREAM_SWITCH_STOP_REJECTED",
                          response,
                          response_size);
            cJSON_Delete(root);
            return;
        }
    }

    control_lan_callback_result_t callback_result =
        handle->config.event_callback(event, handle->config.callback_context);
    commit_event_order(handle,
                       &event,
                       callback_result.accepted,
                       authority.revocation_epoch);

    char callback_detail[CONTROL_LAN_DETAIL_MAX] = { 0 };
    normalize_callback_detail(&callback_result, callback_detail, sizeof(callback_detail));
    record_packet(handle, sender, action_text, callback_detail, callback_result.accepted);
    (void)write_response(response,
                         response_size,
                         request_id,
                         callback_result.accepted ? "ok" : "err",
                         callback_detail);
    cJSON_Delete(root);
}

uint64_t control_lan_hash_stream_id(const char *stream_id)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    if (stream_id) {
        for (const unsigned char *cursor = (const unsigned char *)stream_id; *cursor; cursor++) {
            hash ^= (uint64_t)*cursor;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash == 0 ? UINT64_C(1) : hash;
}

static bool stop_requested(control_lan_handle_t handle)
{
    state_lock(handle);
    bool stop = handle->stop_requested;
    state_unlock(handle);
    return stop;
}

static void set_socket(control_lan_handle_t handle, int socket_fd)
{
    state_lock(handle);
    handle->socket_fd = socket_fd;
    state_unlock(handle);
}

static void close_task_socket(control_lan_handle_t handle, int socket_fd)
{
    set_socket(handle, -1);
    close(socket_fd);
}

static void control_lan_task(void *arg)
{
    control_lan_handle_t handle = (control_lan_handle_t)arg;

    state_lock(handle);
    handle->status.task_running = true;
    state_unlock(handle);

    while (!stop_requested(handle)) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "UDP socket creation failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_LAN_RETRY_DELAY_MS));
            continue;
        }
        set_socket(handle, sock);

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = CONTROL_LAN_SOCKET_TIMEOUT_US,
        };
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            ESP_LOGW(TAG, "UDP receive timeout setup failed errno=%d", errno);
            close_task_socket(handle, sock);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_LAN_RETRY_DELAY_MS));
            continue;
        }

        struct sockaddr_in listen_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(handle->config.listen_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            ESP_LOGW(TAG, "UDP/%u bind failed errno=%d", handle->config.listen_port, errno);
            close_task_socket(handle, sock);
            vTaskDelay(pdMS_TO_TICKS(CONTROL_LAN_RETRY_DELAY_MS));
            continue;
        }

        bool receive_burst_active = false;
        uint64_t receive_burst_timestamp_us = 0U;
        while (!stop_requested(handle)) {
            int64_t authority_timer_us = esp_timer_get_time();
            const control_lan_authority_status_t authority = authority_status(handle);
            enforce_authority(handle,
                              &authority,
                              authority_timer_us > 0 ?
                                  (uint64_t)authority_timer_us : 0U);
            char packet[CONTROL_LAN_PACKET_MAX + 1U];
            struct sockaddr_in source_addr = { 0 };
            socklen_t source_len = sizeof(source_addr);
            int receive_flags = receive_burst_active ? MSG_DONTWAIT : 0;
            int received = recvfrom(sock,
                                    packet,
                                    sizeof(packet),
                                    receive_flags,
                                    (struct sockaddr *)&source_addr,
                                    &source_len);
            if (received < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    receive_burst_active = false;
                    receive_burst_timestamp_us = 0U;
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (!receive_burst_active) {
                int64_t receive_timer_us = esp_timer_get_time();
                receive_burst_timestamp_us =
                    receive_timer_us > 0 ? (uint64_t)receive_timer_us : 0U;
                receive_burst_active = true;
            }
            uint64_t received_at_us = receive_burst_timestamp_us;
            if (stop_requested(handle)) {
                break;
            }

            char sender[CONTROL_LAN_SENDER_MAX] = { 0 };
            char sender_ip[INET_ADDRSTRLEN] = { 0 };
            if (!inet_ntoa_r(source_addr.sin_addr, sender_ip, sizeof(sender_ip))) {
                copy_text(sender_ip, sizeof(sender_ip), "unknown");
            }
            snprintf(sender,
                     sizeof(sender),
                     "%s:%u",
                     sender_ip,
                     (unsigned int)ntohs(source_addr.sin_port));

            char response[CONTROL_LAN_RESPONSE_MAX] = { 0 };
            if ((size_t)received > CONTROL_LAN_PACKET_MAX) {
                reject_packet(handle,
                              sender,
                              "unknown",
                              "",
                              "PACKET_TOO_LARGE",
                              response,
                              sizeof(response));
            } else {
                packet[received] = '\0';
                handle_packet(handle,
                              packet,
                              (size_t)received,
                              sender,
                              received_at_us,
                              response,
                              sizeof(response));
            }
            secure_zero(packet, sizeof(packet));

            size_t response_len = strnlen(response, sizeof(response));
            if (response_len > 0 && response_len < sizeof(response)) {
                (void)sendto(sock,
                             response,
                             response_len,
                             0,
                             (struct sockaddr *)&source_addr,
                             source_len);
            }
        }

        close_task_socket(handle, sock);
    }

    state_lock(handle);
    handle->status.task_running = false;
    handle->socket_fd = -1;
    handle->task = NULL;
    state_unlock(handle);
    xSemaphoreGive(handle->task_done);
    vTaskDelete(NULL);
}

esp_err_t control_lan_init(const control_lan_config_t *config, control_lan_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || !config->config_manager || !config->event_callback ||
        !isfinite(config->max_abs_vx_mps) || config->max_abs_vx_mps <= 0.0f ||
        !isfinite(config->max_abs_vy_mps) || config->max_abs_vy_mps <= 0.0f ||
        !isfinite(config->max_abs_wz_radps) ||
        config->max_abs_wz_radps <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t task_priority = config->task_priority == 0 ?
                                 CONTROL_LAN_DEFAULT_TASK_PRIORITY :
                                 config->task_priority;
    if (task_priority >= configMAX_PRIORITIES) {
        return ESP_ERR_INVALID_ARG;
    }

    control_lan_handle_t handle = calloc(1, sizeof(struct control_lan_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    handle->config.listen_port = config->listen_port == 0 ?
                                     CONTROL_LAN_DEFAULT_PORT :
                                     config->listen_port;
    handle->config.task_priority = task_priority;
    handle->socket_fd = -1;
    handle->status.listen_port = handle->config.listen_port;
    copy_text(handle->status.last_detail, sizeof(handle->status.last_detail), "NEVER_RUN");
    handle->status.lan_allowed = true;
    copy_text(handle->status.authority_detail,
              sizeof(handle->status.authority_detail),
              "LAN_ALLOWED");

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

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t control_lan_start(control_lan_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    state_lock(handle);
    if (handle->deinitializing || handle->task) {
        state_unlock(handle);
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(handle->task_done, 0) == pdTRUE) {
    }
    handle->stop_requested = false;

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(control_lan_task,
                                     "control_lan",
                                     CONTROL_LAN_TASK_STACK_SIZE,
                                     handle,
                                     (UBaseType_t)handle->config.task_priority,
                                     &task);
    if (created != pdPASS) {
        state_unlock(handle);
        return ESP_ERR_NO_MEM;
    }
    handle->task = task;
    state_unlock(handle);

    ESP_LOGI(TAG, "control ingress started on UDP/%u", handle->config.listen_port);
    return ESP_OK;
}

void control_lan_deinit(control_lan_handle_t handle)
{
    if (!handle) {
        return;
    }

    state_lock(handle);
    if (handle->deinitializing) {
        state_unlock(handle);
        return;
    }
    TaskHandle_t task = handle->task;
    if (task && task == xTaskGetCurrentTaskHandle()) {
        state_unlock(handle);
        ESP_LOGE(TAG, "deinit cannot run from the control callback");
        return;
    }
    handle->deinitializing = true;
    handle->stop_requested = true;
    if (handle->socket_fd >= 0) {
        (void)shutdown(handle->socket_fd, SHUT_RDWR);
    }
    state_unlock(handle);

    if (task) {
        (void)xSemaphoreTake(handle->task_done, portMAX_DELAY);
    }
    vSemaphoreDelete(handle->task_done);
    vSemaphoreDelete(handle->lock);
    free(handle);
}

esp_err_t control_lan_get_status(control_lan_handle_t handle, control_lan_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    state_lock(handle);
    *status = handle->status;
    state_unlock(handle);
    return ESP_OK;
}
