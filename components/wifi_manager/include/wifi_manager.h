#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "wifi_manager_softap_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_IP_ADDR_MAX 16

typedef struct wifi_manager_t *wifi_manager_handle_t;

typedef enum {
    WIFI_MANAGER_MODE_STATION = 0,
    WIFI_MANAGER_MODE_SOFTAP,
} wifi_manager_mode_t;

typedef enum {
    WIFI_MANAGER_STATE_UNCONFIGURED = 0,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_AP_RUNNING,
    WIFI_MANAGER_STATE_FAILED,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_mode_t mode;
    wifi_manager_state_t state;
    char ssid[CONFIG_MANAGER_WIFI_SSID_MAX];
    char ip_addr[WIFI_MANAGER_IP_ADDR_MAX];
    uint8_t retry_count;
    uint8_t max_retries;
    int8_t rssi;
    uint16_t disconnect_reason;
    esp_err_t last_error;
    bool auto_connect_running;
    bool auto_connect_paused;
    uint32_t auto_retry_delay_ms;
    uint8_t connected_clients;
    bool dhcp_server_running;
} wifi_manager_status_t;

esp_err_t wifi_manager_init(config_manager_handle_t config_manager, wifi_manager_handle_t *out_handle);
esp_err_t wifi_manager_init_softap(const wifi_manager_softap_config_t *config,
                                   wifi_manager_handle_t *out_handle);
void wifi_manager_deinit(wifi_manager_handle_t handle);

esp_err_t wifi_manager_connect(wifi_manager_handle_t handle);
esp_err_t wifi_manager_disconnect(wifi_manager_handle_t handle);
esp_err_t wifi_manager_start_auto_connect_task(wifi_manager_handle_t handle);
esp_err_t wifi_manager_set_auto_connect_paused(wifi_manager_handle_t handle, bool paused);
esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle, wifi_manager_status_t *status);
bool wifi_manager_is_softap(wifi_manager_handle_t handle);
bool wifi_manager_status_network_ready(const wifi_manager_status_t *status);
const char *wifi_manager_mode_to_string(wifi_manager_mode_t mode);
const char *wifi_manager_state_to_string(wifi_manager_state_t state);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
