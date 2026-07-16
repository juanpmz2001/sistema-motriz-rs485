#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MANAGER_WIFI_SSID_MAX 33
#define CONFIG_MANAGER_WIFI_PASSWORD_MAX 65
#define CONFIG_MANAGER_OTA_HOST_MAX 64
#define CONFIG_MANAGER_OTA_MANIFEST_PATH_MAX 128
#define CONFIG_MANAGER_OTA_ANNOUNCE_TOKEN_MAX 65
#define CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_DEFAULT_MS 600000U
#define CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS 60000U
#define CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS 86400000U

typedef struct config_manager_t *config_manager_handle_t;

typedef struct {
    char wifi_ssid[CONFIG_MANAGER_WIFI_SSID_MAX];
    bool wifi_password_set;
    char ota_server_host[CONFIG_MANAGER_OTA_HOST_MAX];
    uint16_t ota_server_port;
    char ota_manifest_path[CONFIG_MANAGER_OTA_MANIFEST_PATH_MAX];
    bool ota_auto_check_enabled;
    bool ota_auto_update_enabled;
    uint32_t ota_auto_check_interval_ms;
    bool ota_announce_token_set;
} config_manager_snapshot_t;

esp_err_t config_manager_init(config_manager_handle_t *out_handle);
void config_manager_deinit(config_manager_handle_t handle);

esp_err_t config_manager_get_snapshot(config_manager_handle_t handle, config_manager_snapshot_t *snapshot);
esp_err_t config_manager_get_wifi_password(config_manager_handle_t handle,
                                           char *password,
                                           size_t password_size,
                                           bool *password_set);
esp_err_t config_manager_get_ota_announce_token(config_manager_handle_t handle,
                                                char *token,
                                                size_t token_size,
                                                bool *token_set);

esp_err_t config_manager_set_wifi(config_manager_handle_t handle, const char *ssid, const char *password);
esp_err_t config_manager_clear_wifi(config_manager_handle_t handle);
esp_err_t config_manager_set_ota_server(config_manager_handle_t handle, const char *host, uint16_t port);
esp_err_t config_manager_set_ota_manifest_path(config_manager_handle_t handle, const char *path);
esp_err_t config_manager_set_ota_announce_token(config_manager_handle_t handle, const char *token);
esp_err_t config_manager_clear_ota_announce_token(config_manager_handle_t handle);
esp_err_t config_manager_set_ota_auto_check(config_manager_handle_t handle, bool enabled);
esp_err_t config_manager_set_ota_auto_update(config_manager_handle_t handle, bool enabled);
esp_err_t config_manager_set_ota_auto_check_interval_ms(config_manager_handle_t handle, uint32_t interval_ms);
esp_err_t config_manager_clear(config_manager_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
