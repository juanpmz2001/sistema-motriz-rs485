#include "config_manager.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define CONFIG_NAMESPACE "bot_config"

#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASSWORD "wifi_pass"
#define KEY_OTA_HOST "ota_host"
#define KEY_OTA_PORT "ota_port"
#define KEY_OTA_MANIFEST "ota_path"
#define KEY_OTA_ANNOUNCE_TOKEN "ota_ann_tok"
#define KEY_AUTO_CHECK "auto_check"
#define KEY_AUTO_UPDATE "auto_update"
#define KEY_AUTO_INTERVAL "auto_int_ms"

#define DEFAULT_OTA_HOST "192.168.10.10"
#define DEFAULT_OTA_PORT 8080
#define DEFAULT_OTA_MANIFEST "/api/firmware/latest"
#define CONFIG_MANAGER_LOCK_TIMEOUT_MS 100

struct config_manager_t {
    nvs_handle_t nvs;
    SemaphoreHandle_t lock;
};

static bool valid_string_size(const char *value, size_t max_size)
{
    return value && strnlen(value, max_size) < max_size;
}

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

static void snapshot_defaults(config_manager_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    (void)copy_text(snapshot->ota_server_host, sizeof(snapshot->ota_server_host), DEFAULT_OTA_HOST);
    snapshot->ota_server_port = DEFAULT_OTA_PORT;
    (void)copy_text(snapshot->ota_manifest_path, sizeof(snapshot->ota_manifest_path), DEFAULT_OTA_MANIFEST);
    snapshot->ota_auto_check_enabled = false;
    snapshot->ota_auto_update_enabled = false;
    snapshot->ota_auto_check_interval_ms = CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_DEFAULT_MS;
}

static esp_err_t read_string(nvs_handle_t nvs, const char *key, char *dest, size_t dest_size)
{
    size_t required = dest_size;
    esp_err_t err = nvs_get_str(nvs, key, dest, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t erase_key_if_present(nvs_handle_t nvs, const char *key)
{
    esp_err_t err = nvs_erase_key(nvs, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t take_lock(config_manager_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(CONFIG_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t config_manager_init(config_manager_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    config_manager_handle_t handle = calloc(1, sizeof(struct config_manager_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle->nvs);
    if (err != ESP_OK) {
        vSemaphoreDelete(handle->lock);
        free(handle);
        return err;
    }

    *out_handle = handle;
    return ESP_OK;
}

void config_manager_deinit(config_manager_handle_t handle)
{
    if (!handle) {
        return;
    }
    nvs_close(handle->nvs);
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t config_manager_get_snapshot(config_manager_handle_t handle, config_manager_snapshot_t *snapshot)
{
    if (!handle || !snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    snapshot_defaults(snapshot);
    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }

    err = read_string(handle->nvs, KEY_WIFI_SSID, snapshot->wifi_ssid, sizeof(snapshot->wifi_ssid));
    if (err == ESP_OK) {
        char password[CONFIG_MANAGER_WIFI_PASSWORD_MAX] = { 0 };
        err = read_string(handle->nvs, KEY_WIFI_PASSWORD, password, sizeof(password));
        if (err == ESP_OK) {
            snapshot->wifi_password_set = password[0] != '\0';
        }
    }
    if (err == ESP_OK) {
        err = read_string(handle->nvs, KEY_OTA_HOST, snapshot->ota_server_host, sizeof(snapshot->ota_server_host));
    }
    if (err == ESP_OK) {
        uint16_t port = DEFAULT_OTA_PORT;
        esp_err_t port_err = nvs_get_u16(handle->nvs, KEY_OTA_PORT, &port);
        if (port_err == ESP_OK) {
            snapshot->ota_server_port = port;
        } else if (port_err != ESP_ERR_NVS_NOT_FOUND) {
            err = port_err;
        }
    }
    if (err == ESP_OK) {
        err = read_string(handle->nvs, KEY_OTA_MANIFEST, snapshot->ota_manifest_path, sizeof(snapshot->ota_manifest_path));
    }
    if (err == ESP_OK) {
        uint8_t value = 0;
        esp_err_t flag_err = nvs_get_u8(handle->nvs, KEY_AUTO_CHECK, &value);
        if (flag_err == ESP_OK) {
            snapshot->ota_auto_check_enabled = value != 0;
        } else if (flag_err != ESP_ERR_NVS_NOT_FOUND) {
            err = flag_err;
        }
    }
    if (err == ESP_OK) {
        uint8_t value = 0;
        esp_err_t flag_err = nvs_get_u8(handle->nvs, KEY_AUTO_UPDATE, &value);
        if (flag_err == ESP_OK) {
            snapshot->ota_auto_update_enabled = value != 0;
        } else if (flag_err != ESP_ERR_NVS_NOT_FOUND) {
            err = flag_err;
        }
    }
    if (err == ESP_OK) {
        uint32_t interval_ms = CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_DEFAULT_MS;
        esp_err_t interval_err = nvs_get_u32(handle->nvs, KEY_AUTO_INTERVAL, &interval_ms);
        if (interval_err == ESP_OK) {
            if (interval_ms >= CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS &&
                interval_ms <= CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS) {
                snapshot->ota_auto_check_interval_ms = interval_ms;
            }
        } else if (interval_err != ESP_ERR_NVS_NOT_FOUND) {
            err = interval_err;
        }
    }
    if (err == ESP_OK) {
        char token[CONFIG_MANAGER_OTA_ANNOUNCE_TOKEN_MAX] = { 0 };
        esp_err_t token_err = read_string(handle->nvs, KEY_OTA_ANNOUNCE_TOKEN, token, sizeof(token));
        if (token_err == ESP_OK) {
            snapshot->ota_announce_token_set = token[0] != '\0';
        } else {
            err = token_err;
        }
    }

    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_get_wifi_password(config_manager_handle_t handle,
                                           char *password,
                                           size_t password_size,
                                           bool *password_set)
{
    if (!handle || !password || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    password[0] = '\0';
    if (password_set) {
        *password_set = false;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = read_string(handle->nvs, KEY_WIFI_PASSWORD, password, password_size);
    xSemaphoreGive(handle->lock);

    if (err == ESP_OK && password_set) {
        *password_set = password[0] != '\0';
    }
    return err;
}

esp_err_t config_manager_get_ota_announce_token(config_manager_handle_t handle,
                                                char *token,
                                                size_t token_size,
                                                bool *token_set)
{
    if (!handle || !token || token_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    token[0] = '\0';
    if (token_set) {
        *token_set = false;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = read_string(handle->nvs, KEY_OTA_ANNOUNCE_TOKEN, token, token_size);
    xSemaphoreGive(handle->lock);

    if (err == ESP_OK && token_set) {
        *token_set = token[0] != '\0';
    }
    return err;
}

esp_err_t config_manager_set_wifi(config_manager_handle_t handle, const char *ssid, const char *password)
{
    if (!handle || !valid_string_size(ssid, CONFIG_MANAGER_WIFI_SSID_MAX) ||
        !valid_string_size(password, CONFIG_MANAGER_WIFI_PASSWORD_MAX) ||
        ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle->nvs, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle->nvs, KEY_WIFI_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_clear_wifi(config_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = erase_key_if_present(handle->nvs, KEY_WIFI_SSID);
    if (err == ESP_OK) {
        err = erase_key_if_present(handle->nvs, KEY_WIFI_PASSWORD);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_server(config_manager_handle_t handle, const char *host, uint16_t port)
{
    if (!handle || !valid_string_size(host, CONFIG_MANAGER_OTA_HOST_MAX) ||
        host[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle->nvs, KEY_OTA_HOST, host);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle->nvs, KEY_OTA_PORT, port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_manifest_path(config_manager_handle_t handle, const char *path)
{
    if (!handle || !valid_string_size(path, CONFIG_MANAGER_OTA_MANIFEST_PATH_MAX) ||
        path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle->nvs, KEY_OTA_MANIFEST, path);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_announce_token(config_manager_handle_t handle, const char *token)
{
    if (!handle || !valid_string_size(token, CONFIG_MANAGER_OTA_ANNOUNCE_TOKEN_MAX) ||
        token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle->nvs, KEY_OTA_ANNOUNCE_TOKEN, token);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_clear_ota_announce_token(config_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = erase_key_if_present(handle->nvs, KEY_OTA_ANNOUNCE_TOKEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_auto_check(config_manager_handle_t handle, bool enabled)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle->nvs, KEY_AUTO_CHECK, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_auto_update(config_manager_handle_t handle, bool enabled)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle->nvs, KEY_AUTO_UPDATE, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_set_ota_auto_check_interval_ms(config_manager_handle_t handle, uint32_t interval_ms)
{
    if (!handle ||
        interval_ms < CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS ||
        interval_ms > CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(handle->nvs, KEY_AUTO_INTERVAL, interval_ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t config_manager_clear(config_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle->nvs);
    if (err == ESP_OK) {
        err = nvs_commit(handle->nvs);
    }
    xSemaphoreGive(handle->lock);
    return err;
}
