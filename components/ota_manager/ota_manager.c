#include "ota_manager.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "ota_manager";

#define OTA_MANAGER_HTTP_TIMEOUT_MS 5000
#define OTA_MANAGER_MANIFEST_MAX 4096

struct ota_manager_t {
    config_manager_handle_t config_manager;
    char current_project[CONFIG_MANAGER_OTA_HOST_MAX];
    char current_target[CONFIG_MANAGER_OTA_HOST_MAX];
    uint32_t current_build_number;
};

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} manifest_capture_t;

static esp_err_t copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0 || !src) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strnlen(src, dest_size);
    if (len >= dest_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
    return ESP_OK;
}

static void set_detail(ota_manager_check_result_t *result, const char *detail)
{
    if (result && detail) {
        (void)copy_text(result->detail, sizeof(result->detail), detail);
    }
}

static bool string_is_localhost(const char *host)
{
    if (!host || host[0] == '\0') {
        return true;
    }
    return strcasecmp(host, "localhost") == 0 ||
           strcmp(host, "127.0.0.1") == 0 ||
           strcmp(host, "0.0.0.0") == 0 ||
           strcmp(host, "::1") == 0 ||
           strcmp(host, "[::1]") == 0;
}

static bool extract_http_host(const char *url, char *host, size_t host_size)
{
    const char *prefix = "http://";
    if (!url || strncasecmp(url, prefix, strlen(prefix)) != 0 || !host || host_size == 0) {
        return false;
    }

    const char *cursor = url + strlen(prefix);
    const char *host_start = cursor;
    if (*cursor == '[') {
        const char *end = strchr(cursor, ']');
        if (!end) {
            return false;
        }
        cursor = end + 1;
    } else {
        while (*cursor && *cursor != ':' && *cursor != '/') {
            cursor++;
        }
    }

    size_t len = (size_t)(cursor - host_start);
    if (len == 0 || len >= host_size) {
        return false;
    }
    memcpy(host, host_start, len);
    host[len] = '\0';
    return true;
}

static bool url_is_allowed(const char *url)
{
    char host[CONFIG_MANAGER_OTA_HOST_MAX] = { 0 };
    return extract_http_host(url, host, sizeof(host)) && !string_is_localhost(host);
}

static bool filename_is_valid(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        return false;
    }
    size_t len = strlen(filename);
    if (len < 5 || len >= OTA_MANAGER_FILENAME_MAX || strcmp(filename + len - 4, ".bin") != 0) {
        return false;
    }
    return strchr(filename, '/') == NULL && strchr(filename, '\\') == NULL;
}

static bool sha256_is_valid(const char *sha256)
{
    if (!sha256 || strlen(sha256) != OTA_MANAGER_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < OTA_MANAGER_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)sha256[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t get_required_string(const cJSON *root,
                                     const char *key,
                                     char *dest,
                                     size_t dest_size,
                                     ota_manager_check_result_t *result)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        set_detail(result, key);
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t err = copy_text(dest, dest_size, item->valuestring);
    if (err != ESP_OK) {
        set_detail(result, key);
    }
    return err;
}

static esp_err_t get_required_u32(const cJSON *root,
                                  const char *key,
                                  uint32_t *value,
                                  ota_manager_check_result_t *result)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        set_detail(result, key);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *value = (uint32_t)item->valuedouble;
    return ESP_OK;
}

static esp_err_t build_manifest_url(ota_manager_handle_t handle,
                                    char *url,
                                    size_t url_size,
                                    ota_manager_check_result_t *result)
{
    config_manager_snapshot_t snapshot;
    esp_err_t err = config_manager_get_snapshot(handle->config_manager, &snapshot);
    if (err != ESP_OK) {
        set_detail(result, "CONFIG_READ");
        return err;
    }

    if (snapshot.ota_server_host[0] == '\0' || snapshot.ota_manifest_path[0] != '/' ||
        strstr(snapshot.ota_server_host, "://") || string_is_localhost(snapshot.ota_server_host)) {
        set_detail(result, "BAD_SERVER_CONFIG");
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(url,
                           url_size,
                           "http://%s:%u%s",
                           snapshot.ota_server_host,
                           snapshot.ota_server_port,
                           snapshot.ota_manifest_path);
    if (written < 0 || (size_t)written >= url_size) {
        set_detail(result, "URL_TOO_LONG");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || evt->data_len <= 0) {
        return ESP_OK;
    }

    manifest_capture_t *capture = (manifest_capture_t *)evt->user_data;
    size_t data_len = (size_t)evt->data_len;
    size_t available = capture->capacity - capture->length - 1;
    if (data_len > available) {
        capture->overflow = true;
        data_len = available;
    }
    if (data_len > 0) {
        memcpy(capture->buffer + capture->length, evt->data, data_len);
        capture->length += data_len;
        capture->buffer[capture->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch_manifest(const char *url, char *buffer, size_t buffer_size, ota_manager_check_result_t *result)
{
    manifest_capture_t capture = {
        .buffer = buffer,
        .capacity = buffer_size,
        .length = 0,
        .overflow = false,
    };
    buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_MANAGER_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 512,
        .event_handler = http_event_handler,
        .user_data = &capture,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_detail(result, "HTTP_INIT");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manifest fetch failed url=%s err=0x%x", url, err);
        set_detail(result, "HTTP_PERFORM");
        return err;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "manifest fetch status=%d url=%s", status_code, url);
        set_detail(result, "HTTP_STATUS");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (capture.overflow) {
        set_detail(result, "MANIFEST_TOO_LARGE");
        return ESP_ERR_INVALID_SIZE;
    }
    if (capture.length == 0) {
        set_detail(result, "EMPTY_MANIFEST");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t parse_manifest(ota_manager_handle_t handle,
                                const char *json,
                                ota_manager_check_result_t *result)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_detail(result, "JSON_PARSE");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = get_required_string(root, "project", result->project, sizeof(result->project), result);
    if (err == ESP_OK) {
        err = get_required_string(root, "target", result->target, sizeof(result->target), result);
    }
    if (err == ESP_OK) {
        err = get_required_string(root, "version", result->version, sizeof(result->version), result);
    }
    if (err == ESP_OK) {
        err = get_required_u32(root, "build_number", &result->build_number, result);
    }
    if (err == ESP_OK) {
        err = get_required_u32(root, "min_supported_build", &result->min_supported_build, result);
    }
    if (err == ESP_OK) {
        err = get_required_string(root, "url", result->url, sizeof(result->url), result);
    }
    if (err == ESP_OK) {
        err = get_required_string(root, "filename", result->filename, sizeof(result->filename), result);
    }
    if (err == ESP_OK) {
        err = get_required_u32(root, "size", &result->size, result);
    }
    if (err == ESP_OK) {
        err = get_required_string(root, "sha256", result->sha256, sizeof(result->sha256), result);
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    if (strcmp(result->project, handle->current_project) != 0) {
        set_detail(result, "PROJECT_MISMATCH");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(result->target, handle->current_target) != 0) {
        set_detail(result, "TARGET_MISMATCH");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!url_is_allowed(result->url)) {
        set_detail(result, "BAD_URL");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!filename_is_valid(result->filename)) {
        set_detail(result, "BAD_FILENAME");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result->size == 0) {
        set_detail(result, "BAD_SIZE");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!sha256_is_valid(result->sha256)) {
        set_detail(result, "BAD_SHA256");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (handle->current_build_number < result->min_supported_build) {
        set_detail(result, "CURRENT_BUILD_UNSUPPORTED");
        return ESP_ERR_NOT_SUPPORTED;
    }

    result->status = result->build_number > handle->current_build_number
                         ? OTA_MANAGER_CHECK_STATUS_UPDATE_AVAILABLE
                         : OTA_MANAGER_CHECK_STATUS_UP_TO_DATE;
    result->current_build_number = handle->current_build_number;
    return ESP_OK;
}

esp_err_t ota_manager_init(const ota_manager_config_t *config, ota_manager_handle_t *out_handle)
{
    if (!config || !out_handle || !config->config_manager ||
        !config->current_project || !config->current_target) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    ota_manager_handle_t handle = calloc(1, sizeof(struct ota_manager_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config_manager = config->config_manager;
    handle->current_build_number = config->current_build_number;
    esp_err_t err = copy_text(handle->current_project, sizeof(handle->current_project), config->current_project);
    if (err == ESP_OK) {
        err = copy_text(handle->current_target, sizeof(handle->current_target), config->current_target);
    }
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "OTA check manager ready");
    return ESP_OK;
}

void ota_manager_deinit(ota_manager_handle_t handle)
{
    free(handle);
}

esp_err_t ota_manager_check(ota_manager_handle_t handle, ota_manager_check_result_t *result)
{
    if (!handle || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->status = OTA_MANAGER_CHECK_STATUS_UNKNOWN;
    result->current_build_number = handle->current_build_number;

    char manifest_url[OTA_MANAGER_URL_MAX] = { 0 };
    esp_err_t err = build_manifest_url(handle, manifest_url, sizeof(manifest_url), result);
    if (err != ESP_OK) {
        return err;
    }

    char *manifest = calloc(1, OTA_MANAGER_MANIFEST_MAX);
    if (!manifest) {
        set_detail(result, "NO_MEM");
        return ESP_ERR_NO_MEM;
    }

    err = fetch_manifest(manifest_url, manifest, OTA_MANAGER_MANIFEST_MAX, result);
    if (err == ESP_OK) {
        err = parse_manifest(handle, manifest, result);
    }
    free(manifest);
    return err;
}

const char *ota_manager_check_status_to_string(ota_manager_check_status_t status)
{
    switch (status) {
    case OTA_MANAGER_CHECK_STATUS_UP_TO_DATE:
        return "UP_TO_DATE";
    case OTA_MANAGER_CHECK_STATUS_UPDATE_AVAILABLE:
        return "UPDATE_AVAILABLE";
    case OTA_MANAGER_CHECK_STATUS_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}
