#include "ota_manager.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "ota_manager";

#define OTA_MANAGER_HTTP_TIMEOUT_MS 5000
#define OTA_MANAGER_MANIFEST_MAX 4096
#define OTA_MANAGER_DOWNLOAD_CHUNK 4096
#define OTA_MANAGER_NVS_NAMESPACE "bot_ota"
#define OTA_MANAGER_NVS_KEY_ROLLBACK_TEST "rb_test"
#define OTA_MANAGER_OP_LOCK_TIMEOUT_MS 60000
#define OTA_MANAGER_AUTO_TASK_STACK 8192
#define OTA_MANAGER_AUTO_TASK_PRIORITY 2
#define OTA_MANAGER_AUTO_IDLE_MS 5000
#define OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS 30000
#define OTA_MANAGER_AUTO_BACKOFF_MAX_MS (5U * 60U * 1000U)

struct ota_manager_t {
    config_manager_handle_t config_manager;
    wifi_manager_handle_t wifi_manager;
    char current_project[CONFIG_MANAGER_OTA_HOST_MAX];
    char current_target[CONFIG_MANAGER_OTA_HOST_MAX];
    uint32_t current_build_number;
    SemaphoreHandle_t op_lock;
    SemaphoreHandle_t state_lock;
    TaskHandle_t auto_task;
    bool auto_stop;
    bool auto_task_running;
    bool auto_enabled;
    bool auto_checking;
    uint32_t auto_interval_ms;
    uint32_t auto_backoff_ms;
    uint32_t auto_checks;
    uint32_t auto_failures;
    int64_t auto_last_check_us;
    int64_t auto_next_check_us;
    esp_err_t auto_last_error;
    int auto_last_http_status;
    ota_manager_check_status_t auto_last_status;
    uint32_t auto_last_build_number;
    bool auto_update_available;
    bool auto_update_enabled;
    char auto_last_version[OTA_MANAGER_VERSION_MAX];
    char auto_last_detail[OTA_MANAGER_DETAIL_MAX];
    char auto_last_url[OTA_MANAGER_URL_MAX];
};

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} manifest_capture_t;

typedef struct {
    const char *tag;
    esp_log_level_t previous_level;
} log_level_guard_t;

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

static esp_err_t take_semaphore(SemaphoreHandle_t semaphore, uint32_t timeout_ms)
{
    if (!semaphore) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(semaphore, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint32_t ms_until(int64_t target_us, int64_t current_us)
{
    if (target_us <= current_us) {
        return 0;
    }
    int64_t delta_ms = (target_us - current_us + 999) / 1000;
    return delta_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_ms;
}

static uint32_t age_ms(int64_t past_us, int64_t current_us)
{
    if (past_us <= 0 || current_us <= past_us) {
        return 0;
    }
    int64_t delta_ms = (current_us - past_us) / 1000;
    return delta_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_ms;
}

static void log_level_guard_begin(log_level_guard_t *guards, size_t count)
{
    if (!guards) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        guards[i].previous_level = esp_log_level_get(guards[i].tag);
        esp_log_level_set(guards[i].tag, ESP_LOG_NONE);
    }
}

static void log_level_guard_end(const log_level_guard_t *guards, size_t count)
{
    if (!guards) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        esp_log_level_set(guards[i].tag, guards[i].previous_level);
    }
}

static bool interval_ms_is_valid(uint32_t interval_ms)
{
    return interval_ms >= CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS &&
           interval_ms <= CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS;
}

static void set_detail(ota_manager_check_result_t *result, const char *detail)
{
    if (result && detail) {
        (void)copy_text(result->detail, sizeof(result->detail), detail);
    }
}

static void set_download_detail(ota_manager_download_result_t *result, const char *detail)
{
    if (result && detail) {
        (void)copy_text(result->detail, sizeof(result->detail), detail);
    }
}

const char *ota_manager_image_state_to_string(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
        return "VALID";
    case ESP_OTA_IMG_INVALID:
        return "INVALID";
    case ESP_OTA_IMG_ABORTED:
        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
        return "UNDEFINED";
    default:
        return "UNKNOWN";
    }
}

const char *ota_manager_rollback_test_mode_to_string(ota_manager_rollback_test_mode_t mode)
{
    switch (mode) {
    case OTA_MANAGER_ROLLBACK_TEST_NONE:
        return "NONE";
    case OTA_MANAGER_ROLLBACK_TEST_NO_CONFIRM_ONCE:
        return "NO_CONFIRM_ONCE";
    case OTA_MANAGER_ROLLBACK_TEST_SELF_TEST_FAIL_ONCE:
        return "SELF_TEST_FAIL_ONCE";
    default:
        return "UNKNOWN";
    }
}

static bool rollback_test_mode_is_valid(ota_manager_rollback_test_mode_t mode)
{
    return mode == OTA_MANAGER_ROLLBACK_TEST_NONE ||
           mode == OTA_MANAGER_ROLLBACK_TEST_NO_CONFIRM_ONCE ||
           mode == OTA_MANAGER_ROLLBACK_TEST_SELF_TEST_FAIL_ONCE;
}

static esp_err_t open_ota_nvs(nvs_open_mode_t open_mode, nvs_handle_t *out_nvs)
{
    if (!out_nvs) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(OTA_MANAGER_NVS_NAMESPACE, open_mode, out_nvs);
}

static void bytes_to_hex(const unsigned char *input, size_t input_len, char *output, size_t output_size)
{
    static const char hex[] = "0123456789abcdef";
    if (!input || !output || output_size < (input_len * 2U + 1U)) {
        return;
    }
    for (size_t i = 0; i < input_len; i++) {
        output[i * 2U] = hex[(input[i] >> 4) & 0x0F];
        output[i * 2U + 1U] = hex[input[i] & 0x0F];
    }
    output[input_len * 2U] = '\0';
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
    if (result) {
        result->http_status = status_code;
    }

    if (err != ESP_OK) {
        set_detail(result, "HTTP_PERFORM");
        return err;
    }
    if (status_code != 200) {
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
    if (!config || !out_handle || !config->config_manager || !config->wifi_manager ||
        !config->current_project || !config->current_target) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    ota_manager_handle_t handle = calloc(1, sizeof(struct ota_manager_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config_manager = config->config_manager;
    handle->wifi_manager = config->wifi_manager;
    handle->current_build_number = config->current_build_number;
    handle->auto_interval_ms = CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_DEFAULT_MS;
    config_manager_snapshot_t snapshot;
    if (config_manager_get_snapshot(config->config_manager, &snapshot) == ESP_OK &&
        interval_ms_is_valid(snapshot.ota_auto_check_interval_ms)) {
        handle->auto_interval_ms = snapshot.ota_auto_check_interval_ms;
    }
    handle->auto_backoff_ms = OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    handle->auto_last_status = OTA_MANAGER_CHECK_STATUS_UNKNOWN;
    handle->auto_last_error = ESP_ERR_INVALID_STATE;
    handle->auto_last_http_status = 0;
    handle->auto_update_available = false;
    handle->auto_update_enabled = false;
    (void)copy_text(handle->auto_last_detail, sizeof(handle->auto_last_detail), "NEVER_RUN");
    handle->op_lock = xSemaphoreCreateMutex();
    handle->state_lock = xSemaphoreCreateMutex();
    if (!handle->op_lock || !handle->state_lock) {
        if (handle->op_lock) {
            vSemaphoreDelete(handle->op_lock);
        }
        if (handle->state_lock) {
            vSemaphoreDelete(handle->state_lock);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = copy_text(handle->current_project, sizeof(handle->current_project), config->current_project);
    if (err == ESP_OK) {
        err = copy_text(handle->current_target, sizeof(handle->current_target), config->current_target);
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(handle->op_lock);
        vSemaphoreDelete(handle->state_lock);
        free(handle);
        return err;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "OTA check manager ready");
    return ESP_OK;
}

void ota_manager_deinit(ota_manager_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->auto_stop = true;
    if (handle->auto_task) {
        for (int i = 0; i < 140 && handle->auto_task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (handle->op_lock) {
        vSemaphoreDelete(handle->op_lock);
    }
    if (handle->state_lock) {
        vSemaphoreDelete(handle->state_lock);
    }
    free(handle);
}

static esp_err_t ota_manager_check_unlocked(ota_manager_handle_t handle, ota_manager_check_result_t *result)
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

esp_err_t ota_manager_check(ota_manager_handle_t handle, ota_manager_check_result_t *result)
{
    if (!handle || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t lock_err = take_semaphore(handle->op_lock, OTA_MANAGER_OP_LOCK_TIMEOUT_MS);
    if (lock_err != ESP_OK) {
        memset(result, 0, sizeof(*result));
        result->status = OTA_MANAGER_CHECK_STATUS_UNKNOWN;
        result->current_build_number = handle->current_build_number;
        set_detail(result, "OTA_BUSY");
        return lock_err;
    }

    esp_err_t err = ota_manager_check_unlocked(handle, result);
    xSemaphoreGive(handle->op_lock);
    return err;
}

esp_err_t ota_manager_download_to_inactive(ota_manager_handle_t handle, ota_manager_download_result_t *result)
{
    if (!handle || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    ota_manager_check_result_t check;
    esp_err_t err = take_semaphore(handle->op_lock, OTA_MANAGER_OP_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        set_download_detail(result, "OTA_BUSY");
        return err;
    }

    err = ota_manager_check_unlocked(handle, &check);
    if (err != ESP_OK) {
        set_download_detail(result, check.detail[0] ? check.detail : "OTA_CHECK");
        xSemaphoreGive(handle->op_lock);
        return err;
    }
    if (check.build_number < check.current_build_number) {
        set_download_detail(result, "BUILD_DOWNGRADE");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (check.build_number == check.current_build_number) {
        set_download_detail(result, "BUILD_NOT_NEWER");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        set_download_detail(result, "NO_UPDATE_PARTITION");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_NOT_FOUND;
    }

    (void)copy_text(result->partition_label, sizeof(result->partition_label), partition->label);
    result->partition_size = partition->size;
    result->manifest_size = check.size;

    if (check.size == 0 || check.size >= partition->size) {
        set_download_detail(result, "IMAGE_TOO_LARGE");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc(OTA_MANAGER_DOWNLOAD_CHUNK);
    if (!buffer) {
        set_download_detail(result, "NO_MEM");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = check.url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_MANAGER_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_MANAGER_DOWNLOAD_CHUNK,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        set_download_detail(result, "HTTP_INIT");
        xSemaphoreGive(handle->op_lock);
        return ESP_ERR_NO_MEM;
    }

    bool client_open = false;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_download_detail(result, "HTTP_OPEN");
        goto cleanup;
    }
    client_open = true;

    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        err = content_length == -1 ? ESP_FAIL : (esp_err_t)(-content_length);
        set_download_detail(result, "HTTP_HEADERS");
        goto cleanup;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
        set_download_detail(result, "HTTP_STATUS");
        goto cleanup;
    }

    if (content_length > 0 && (uint64_t)content_length != (uint64_t)check.size) {
        err = ESP_ERR_INVALID_SIZE;
        set_download_detail(result, "CONTENT_LENGTH");
        goto cleanup;
    }

    err = esp_ota_begin(partition, check.size, &ota_handle);
    if (err != ESP_OK) {
        set_download_detail(result, "OTA_BEGIN");
        goto cleanup;
    }
    ota_started = true;

    int sha_result = mbedtls_sha256_starts(&sha_ctx, 0);
    if (sha_result != 0) {
        err = ESP_FAIL;
        set_download_detail(result, "SHA_INIT");
        goto cleanup;
    }

    while (result->bytes_written < check.size) {
        uint32_t remaining = check.size - result->bytes_written;
        int to_read = remaining > OTA_MANAGER_DOWNLOAD_CHUNK ? OTA_MANAGER_DOWNLOAD_CHUNK : (int)remaining;
        int read_len = esp_http_client_read(client, buffer, to_read);
        if (read_len < 0) {
            err = read_len == -1 ? ESP_FAIL : (esp_err_t)(-read_len);
            set_download_detail(result, "HTTP_READ");
            goto cleanup;
        }
        if (read_len == 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            set_download_detail(result, "HTTP_EOF");
            goto cleanup;
        }

        sha_result = mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buffer, (size_t)read_len);
        if (sha_result != 0) {
            err = ESP_FAIL;
            set_download_detail(result, "SHA_UPDATE");
            goto cleanup;
        }

        err = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            set_download_detail(result, "OTA_WRITE");
            goto cleanup;
        }

        result->bytes_written += (uint32_t)read_len;
    }

    if (result->bytes_written != check.size) {
        err = ESP_ERR_INVALID_SIZE;
        set_download_detail(result, "BYTES_MISMATCH");
        goto cleanup;
    }

    unsigned char digest[32];
    sha_result = mbedtls_sha256_finish(&sha_ctx, digest);
    if (sha_result != 0) {
        err = ESP_FAIL;
        set_download_detail(result, "SHA_FINISH");
        goto cleanup;
    }

    bytes_to_hex(digest, sizeof(digest), result->sha256, sizeof(result->sha256));
    if (strcasecmp(result->sha256, check.sha256) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        set_download_detail(result, "SHA256_MISMATCH");
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    ota_started = false;
    if (err != ESP_OK) {
        set_download_detail(result, "OTA_END");
        goto cleanup;
    }

    set_download_detail(result, "VERIFIED");

cleanup:
    if (ota_started) {
        (void)esp_ota_abort(ota_handle);
    }
    if (client_open) {
        (void)esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    mbedtls_sha256_free(&sha_ctx);
    free(buffer);
    xSemaphoreGive(handle->op_lock);
    return err;
}

esp_err_t ota_manager_download_test(ota_manager_handle_t handle, ota_manager_download_result_t *result)
{
    return ota_manager_download_to_inactive(handle, result);
}

static uint32_t next_backoff_ms(uint32_t current)
{
    if (current == 0) {
        return OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    }
    if (current >= OTA_MANAGER_AUTO_BACKOFF_MAX_MS / 2U) {
        return OTA_MANAGER_AUTO_BACKOFF_MAX_MS;
    }
    return current * 2U;
}

static void auto_status_set_checking(ota_manager_handle_t handle, bool checking)
{
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return;
    }
    handle->auto_checking = checking;
    xSemaphoreGive(handle->state_lock);
}

static void auto_status_set_enabled(ota_manager_handle_t handle, bool enabled, int64_t now)
{
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return;
    }
    handle->auto_enabled = enabled;
    if (!enabled) {
        handle->auto_checking = false;
        handle->auto_next_check_us = 0;
    } else if (handle->auto_next_check_us <= 0) {
        handle->auto_next_check_us = now;
    }
    xSemaphoreGive(handle->state_lock);
}

static void auto_status_record_failure(ota_manager_handle_t handle,
                                       esp_err_t err,
                                       const char *detail,
                                       uint32_t delay_ms,
                                       int http_status)
{
    int64_t now = now_us();
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return;
    }
    handle->auto_checking = false;
    handle->auto_checks++;
    handle->auto_failures++;
    handle->auto_last_check_us = now;
    handle->auto_next_check_us = handle->auto_enabled ? now + ((int64_t)delay_ms * 1000) : 0;
    handle->auto_last_error = err;
    handle->auto_last_http_status = http_status;
    handle->auto_last_status = OTA_MANAGER_CHECK_STATUS_UNKNOWN;
    handle->auto_last_build_number = 0;
    handle->auto_update_available = false;
    handle->auto_last_version[0] = '\0';
    handle->auto_last_url[0] = '\0';
    (void)copy_text(handle->auto_last_detail, sizeof(handle->auto_last_detail), detail ? detail : "FAILED");
    xSemaphoreGive(handle->state_lock);
}

static void auto_status_record_success(ota_manager_handle_t handle, const ota_manager_check_result_t *result)
{
    int64_t now = now_us();
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return;
    }
    handle->auto_checking = false;
    handle->auto_checks++;
    handle->auto_last_check_us = now;
    handle->auto_next_check_us = handle->auto_enabled ? now + ((int64_t)handle->auto_interval_ms * 1000) : 0;
    handle->auto_last_error = ESP_OK;
    handle->auto_last_http_status = result->http_status;
    handle->auto_last_status = result->status;
    handle->auto_last_build_number = result->build_number;
    handle->current_build_number = result->current_build_number;
    handle->auto_update_available = result->status == OTA_MANAGER_CHECK_STATUS_UPDATE_AVAILABLE;
    (void)copy_text(handle->auto_last_version, sizeof(handle->auto_last_version), result->version);
    (void)copy_text(handle->auto_last_url, sizeof(handle->auto_last_url), result->url);
    (void)copy_text(handle->auto_last_detail, sizeof(handle->auto_last_detail), "OK");
    handle->auto_backoff_ms = OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    xSemaphoreGive(handle->state_lock);
}

static bool auto_status_due(ota_manager_handle_t handle, int64_t now)
{
    bool due = false;
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return false;
    }
    due = handle->auto_enabled && !handle->auto_checking && handle->auto_next_check_us <= now;
    xSemaphoreGive(handle->state_lock);
    return due;
}

static uint32_t auto_status_failure_delay(ota_manager_handle_t handle)
{
    uint32_t delay_ms = OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    if (take_semaphore(handle->state_lock, 100) != ESP_OK) {
        return delay_ms;
    }
    delay_ms = handle->auto_backoff_ms ? handle->auto_backoff_ms : OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    handle->auto_backoff_ms = next_backoff_ms(delay_ms);
    xSemaphoreGive(handle->state_lock);
    return delay_ms;
}

static void ota_manager_auto_task(void *arg)
{
    ota_manager_handle_t handle = (ota_manager_handle_t)arg;
    bool was_enabled = false;

    if (take_semaphore(handle->state_lock, 100) == ESP_OK) {
        handle->auto_task_running = true;
        xSemaphoreGive(handle->state_lock);
    }

    while (!handle->auto_stop) {
        config_manager_snapshot_t snapshot;
        esp_err_t cfg_err = config_manager_get_snapshot(handle->config_manager, &snapshot);
        bool enabled = cfg_err == ESP_OK && snapshot.ota_auto_check_enabled;
        int64_t now = now_us();

        if (cfg_err != ESP_OK) {
            uint32_t delay_ms = auto_status_failure_delay(handle);
            auto_status_record_failure(handle, cfg_err, "CONFIG_READ", delay_ms, 0);
            vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
            continue;
        }

        if (take_semaphore(handle->state_lock, 100) == ESP_OK) {
            if (interval_ms_is_valid(snapshot.ota_auto_check_interval_ms)) {
                handle->auto_interval_ms = snapshot.ota_auto_check_interval_ms;
            }
            handle->auto_update_enabled = false;
            xSemaphoreGive(handle->state_lock);
        }

        if (enabled && !was_enabled) {
            if (take_semaphore(handle->state_lock, 100) == ESP_OK) {
                handle->auto_next_check_us = now;
                handle->auto_backoff_ms = OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
                xSemaphoreGive(handle->state_lock);
            }
        }
        was_enabled = enabled;
        auto_status_set_enabled(handle, enabled, now);

        if (!enabled || !auto_status_due(handle, now)) {
            vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
            continue;
        }

        wifi_manager_status_t wifi_status;
        esp_err_t wifi_err = wifi_manager_get_status(handle->wifi_manager, &wifi_status);
        if (wifi_err != ESP_OK || wifi_status.state != WIFI_MANAGER_STATE_CONNECTED) {
            uint32_t delay_ms = auto_status_failure_delay(handle);
            const char *detail = wifi_err == ESP_OK ? "WIFI_NOT_CONNECTED" : "WIFI_STATUS";
            esp_err_t stored_err = wifi_err == ESP_OK ? ESP_ERR_INVALID_STATE : wifi_err;
            auto_status_record_failure(handle, stored_err, detail, delay_ms, 0);
            vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
            continue;
        }

        esp_err_t lock_err = take_semaphore(handle->op_lock, 0);
        if (lock_err != ESP_OK) {
            uint32_t delay_ms = auto_status_failure_delay(handle);
            auto_status_record_failure(handle, lock_err, "OTA_BUSY", delay_ms, 0);
            vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
            continue;
        }

        auto_status_set_checking(handle, true);
        ota_manager_check_result_t result;
        log_level_guard_t quiet_logs[] = {
            { .tag = TAG },
            { .tag = "esp-tls" },
            { .tag = "transport_base" },
            { .tag = "HTTP_CLIENT" },
            { .tag = "esp_http_client" },
        };
        log_level_guard_begin(quiet_logs, sizeof(quiet_logs) / sizeof(quiet_logs[0]));
        esp_err_t err = ota_manager_check_unlocked(handle, &result);
        log_level_guard_end(quiet_logs, sizeof(quiet_logs) / sizeof(quiet_logs[0]));
        xSemaphoreGive(handle->op_lock);

        if (err != ESP_OK) {
            const char *detail = result.detail[0] ? result.detail : "OTA_CHECK";
            uint32_t delay_ms = auto_status_failure_delay(handle);
            auto_status_record_failure(handle, err, detail, delay_ms, result.http_status);
            vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
            continue;
        }

        auto_status_record_success(handle, &result);
        vTaskDelay(pdMS_TO_TICKS(OTA_MANAGER_AUTO_IDLE_MS));
    }

    if (take_semaphore(handle->state_lock, 100) == ESP_OK) {
        handle->auto_task_running = false;
        handle->auto_task = NULL;
        xSemaphoreGive(handle->state_lock);
    }
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_auto_check_task(ota_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->auto_task) {
        return ESP_ERR_INVALID_STATE;
    }

    handle->auto_stop = false;
    BaseType_t ok = xTaskCreate(ota_manager_auto_task,
                                "ota_auto_check",
                                OTA_MANAGER_AUTO_TASK_STACK,
                                handle,
                                OTA_MANAGER_AUTO_TASK_PRIORITY,
                                &handle->auto_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ota_manager_set_auto_check_runtime_enabled(ota_manager_handle_t handle, bool enabled)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = take_semaphore(handle->state_lock, 100);
    if (err != ESP_OK) {
        return err;
    }

    handle->auto_enabled = enabled;
    if (enabled) {
        handle->auto_next_check_us = now_us();
        handle->auto_backoff_ms = OTA_MANAGER_AUTO_BACKOFF_INITIAL_MS;
    } else {
        handle->auto_checking = false;
        handle->auto_next_check_us = 0;
    }

    xSemaphoreGive(handle->state_lock);
    return ESP_OK;
}

esp_err_t ota_manager_set_auto_check_interval_runtime_ms(ota_manager_handle_t handle, uint32_t interval_ms)
{
    if (!handle || !interval_ms_is_valid(interval_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_semaphore(handle->state_lock, 100);
    if (err != ESP_OK) {
        return err;
    }

    handle->auto_interval_ms = interval_ms;
    if (handle->auto_enabled) {
        int64_t now = now_us();
        int64_t max_next_us = now + ((int64_t)interval_ms * 1000);
        if (handle->auto_next_check_us <= 0 || handle->auto_next_check_us > max_next_us) {
            handle->auto_next_check_us = max_next_us;
        }
    }

    xSemaphoreGive(handle->state_lock);
    return ESP_OK;
}

esp_err_t ota_manager_force_check(ota_manager_handle_t handle, ota_manager_check_result_t *result)
{
    if (!handle || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->status = OTA_MANAGER_CHECK_STATUS_UNKNOWN;
    result->current_build_number = handle->current_build_number;

    wifi_manager_status_t wifi_status;
    esp_err_t err = wifi_manager_get_status(handle->wifi_manager, &wifi_status);
    if (err != ESP_OK) {
        set_detail(result, "WIFI_STATUS");
        auto_status_record_failure(handle, err, result->detail, auto_status_failure_delay(handle), 0);
        return err;
    }
    if (wifi_status.state != WIFI_MANAGER_STATE_CONNECTED) {
        set_detail(result, "WIFI_NOT_CONNECTED");
        auto_status_record_failure(handle, ESP_ERR_INVALID_STATE, result->detail, auto_status_failure_delay(handle), 0);
        return ESP_ERR_INVALID_STATE;
    }

    err = take_semaphore(handle->op_lock, 0);
    if (err != ESP_OK) {
        set_detail(result, "OTA_BUSY");
        auto_status_record_failure(handle, err, result->detail, auto_status_failure_delay(handle), 0);
        return err;
    }

    auto_status_set_checking(handle, true);
    err = ota_manager_check_unlocked(handle, result);
    xSemaphoreGive(handle->op_lock);

    if (err == ESP_OK) {
        auto_status_record_success(handle, result);
        return ESP_OK;
    }

    const char *detail = result->detail[0] ? result->detail : "OTA_CHECK";
    auto_status_record_failure(handle, err, detail, auto_status_failure_delay(handle), result->http_status);
    return err;
}

esp_err_t ota_manager_get_auto_status(ota_manager_handle_t handle, ota_manager_auto_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = take_semaphore(handle->state_lock, 100);
    if (err != ESP_OK) {
        return err;
    }

    int64_t now = now_us();
    memset(status, 0, sizeof(*status));
    status->task_running = handle->auto_task_running;
    status->enabled = handle->auto_enabled;
    status->checking = handle->auto_checking;
    status->interval_ms = handle->auto_interval_ms;
    status->backoff_ms = handle->auto_backoff_ms;
    status->checks = handle->auto_checks;
    status->failures = handle->auto_failures;
    status->last_check_age_ms = age_ms(handle->auto_last_check_us, now);
    status->last_check_time_ms = handle->auto_last_check_us > 0 ? (uint32_t)(handle->auto_last_check_us / 1000) : 0;
    status->next_check_in_ms = ms_until(handle->auto_next_check_us, now);
    status->last_error = handle->auto_last_error;
    status->last_http_status = handle->auto_last_http_status;
    status->last_status = handle->auto_last_status;
    status->last_build_number = handle->auto_last_build_number;
    status->current_build_number = handle->current_build_number;
    status->update_available = handle->auto_update_available;
    status->auto_update_enabled = handle->auto_update_enabled;
    (void)copy_text(status->last_version, sizeof(status->last_version), handle->auto_last_version);
    (void)copy_text(status->last_detail, sizeof(status->last_detail), handle->auto_last_detail);
    (void)copy_text(status->last_url, sizeof(status->last_url), handle->auto_last_url);

    xSemaphoreGive(handle->state_lock);
    return ESP_OK;
}

esp_err_t ota_manager_set_boot_partition(const char *partition_label)
{
    if (!partition_label || partition_label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                               ESP_PARTITION_SUBTYPE_ANY,
                                                               partition_label);
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && strcmp(running->label, partition->label) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_ota_set_boot_partition(partition);
}

esp_err_t ota_manager_get_boot_state(ota_manager_boot_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->state = ESP_OTA_IMG_UNDEFINED;

    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (!partition) {
        state->state_error = ESP_ERR_NOT_FOUND;
        return state->state_error;
    }

    (void)copy_text(state->partition_label, sizeof(state->partition_label), partition->label);

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(partition, &ota_state);
    state->state_error = err;
    if (err == ESP_OK) {
        state->state = ota_state;
        state->state_known = true;
        state->pending_verify = ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    state->rollback_possible = esp_ota_check_rollback_is_possible();
    return ESP_OK;
}

esp_err_t ota_manager_mark_app_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_manager_mark_app_invalid_and_rollback(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t ota_manager_get_rollback_test_mode(ota_manager_rollback_test_mode_t *mode)
{
    if (!mode) {
        return ESP_ERR_INVALID_ARG;
    }
    *mode = OTA_MANAGER_ROLLBACK_TEST_NONE;

    nvs_handle_t nvs = 0;
    esp_err_t err = open_ota_nvs(NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored = 0;
    err = nvs_get_u8(nvs, OTA_MANAGER_NVS_KEY_ROLLBACK_TEST, &stored);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    ota_manager_rollback_test_mode_t stored_mode = (ota_manager_rollback_test_mode_t)stored;
    if (!rollback_test_mode_is_valid(stored_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    *mode = stored_mode;
    return ESP_OK;
}

esp_err_t ota_manager_set_rollback_test_mode(ota_manager_rollback_test_mode_t mode)
{
    if (!rollback_test_mode_is_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = open_ota_nvs(NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (mode == OTA_MANAGER_ROLLBACK_TEST_NONE) {
        err = nvs_erase_key(nvs, OTA_MANAGER_NVS_KEY_ROLLBACK_TEST);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_u8(nvs, OTA_MANAGER_NVS_KEY_ROLLBACK_TEST, (uint8_t)mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t ota_manager_consume_rollback_test_mode(ota_manager_rollback_test_mode_t *mode)
{
    esp_err_t err = ota_manager_get_rollback_test_mode(mode);
    if (err != ESP_OK) {
        return err;
    }
    if (*mode != OTA_MANAGER_ROLLBACK_TEST_NONE) {
        err = ota_manager_set_rollback_test_mode(OTA_MANAGER_ROLLBACK_TEST_NONE);
    }
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
