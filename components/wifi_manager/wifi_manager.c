#include "wifi_manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "wifi_manager";

#define WIFI_MANAGER_MAX_RETRIES 3
#define WIFI_MANAGER_CONNECT_TIMEOUT_MS 15000
#define WIFI_MANAGER_LOCK_TIMEOUT_MS 100
#define WIFI_MANAGER_TIMEOUT_TASK_STACK 3072
#define WIFI_MANAGER_TIMEOUT_TASK_PRIORITY 3
#define WIFI_MANAGER_SUPERVISOR_TASK_STACK 4096
#define WIFI_MANAGER_SUPERVISOR_TASK_PRIORITY 2
#define WIFI_MANAGER_SUPERVISOR_IDLE_MS 5000
#define WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS 5000
#define WIFI_MANAGER_SUPERVISOR_BACKOFF_MAX_MS (5U * 60U * 1000U)

struct wifi_manager_t {
    config_manager_handle_t config_manager;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    SemaphoreHandle_t lock;
    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t ip_event_handler;
    esp_event_handler_instance_t softap_connect_event_handler;
    esp_event_handler_instance_t softap_disconnect_event_handler;
    wifi_manager_mode_t mode;
    wifi_manager_state_t state;
    char ssid[CONFIG_MANAGER_WIFI_SSID_MAX];
    char ip_addr[WIFI_MANAGER_IP_ADDR_MAX];
    uint8_t retry_count;
    uint8_t max_retries;
    uint16_t disconnect_reason;
    esp_err_t last_error;
    bool started;
    bool manual_disconnect;
    bool auto_connect_paused;
    bool supervisor_stop;
    bool supervisor_running;
    uint32_t supervisor_retry_delay_ms;
    uint32_t connect_generation;
    TaskHandle_t supervisor_task;
    uint8_t connected_clients;
    bool dhcp_server_running;
};

typedef struct {
    wifi_manager_handle_t handle;
    uint32_t generation;
} timeout_task_arg_t;

static esp_err_t take_lock(wifi_manager_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

const char *wifi_manager_state_to_string(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_UNCONFIGURED:
        return "UNCONFIGURED";
    case WIFI_MANAGER_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case WIFI_MANAGER_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_MANAGER_STATE_CONNECTED:
        return "CONNECTED";
    case WIFI_MANAGER_STATE_AP_RUNNING:
        return "AP_RUNNING";
    case WIFI_MANAGER_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *wifi_manager_mode_to_string(wifi_manager_mode_t mode)
{
    switch (mode) {
    case WIFI_MANAGER_MODE_STATION:
        return "STA";
    case WIFI_MANAGER_MODE_SOFTAP:
        return "AP";
    default:
        return "UNKNOWN";
    }
}

bool wifi_manager_status_network_ready(const wifi_manager_status_t *status)
{
    if (!status) {
        return false;
    }
    if (status->mode == WIFI_MANAGER_MODE_STATION) {
        return status->state == WIFI_MANAGER_STATE_CONNECTED;
    }
    return status->mode == WIFI_MANAGER_MODE_SOFTAP &&
           status->state == WIFI_MANAGER_STATE_AP_RUNNING &&
           status->dhcp_server_running && status->ip_addr[0] != '\0';
}

static void set_failed(wifi_manager_handle_t handle, esp_err_t err)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->state = WIFI_MANAGER_STATE_FAILED;
    handle->last_error = err;
    handle->ip_addr[0] = '\0';
    xSemaphoreGive(handle->lock);
}

static void connect_timeout_task(void *arg)
{
    timeout_task_arg_t *task_arg = (timeout_task_arg_t *)arg;
    wifi_manager_handle_t handle = task_arg->handle;
    uint32_t generation = task_arg->generation;
    free(task_arg);

    vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_CONNECT_TIMEOUT_MS));

    bool should_disconnect = false;
    if (take_lock(handle) == ESP_OK) {
        if (handle->state == WIFI_MANAGER_STATE_CONNECTING &&
            handle->connect_generation == generation) {
            handle->state = WIFI_MANAGER_STATE_FAILED;
            handle->last_error = ESP_ERR_TIMEOUT;
            handle->ip_addr[0] = '\0';
            should_disconnect = true;
        }
        xSemaphoreGive(handle->lock);
    }

    if (should_disconnect) {
        (void)esp_wifi_disconnect();
    }

    vTaskDelete(NULL);
}

static uint32_t next_supervisor_backoff_ms(uint32_t current)
{
    if (current == 0) {
        return WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS;
    }
    if (current >= WIFI_MANAGER_SUPERVISOR_BACKOFF_MAX_MS / 2U) {
        return WIFI_MANAGER_SUPERVISOR_BACKOFF_MAX_MS;
    }
    return current * 2U;
}

static void set_supervisor_state(wifi_manager_handle_t handle,
                                 bool running,
                                 uint32_t retry_delay_ms)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->supervisor_running = running;
    handle->supervisor_retry_delay_ms = retry_delay_ms;
    xSemaphoreGive(handle->lock);
}

static esp_err_t load_credentials(wifi_manager_handle_t handle,
                                  char *ssid,
                                  size_t ssid_size,
                                  char *password,
                                  size_t password_size)
{
    config_manager_snapshot_t snapshot;
    esp_err_t err = config_manager_get_snapshot(handle->config_manager, &snapshot);
    if (err != ESP_OK) {
        return err;
    }
    if (snapshot.wifi_ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    copy_text(ssid, ssid_size, snapshot.wifi_ssid);
    bool password_set = false;
    err = config_manager_get_wifi_password(handle->config_manager, password, password_size, &password_set);
    if (err != ESP_OK) {
        return err;
    }
    if (!password_set) {
        password[0] = '\0';
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)arg;
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
    bool reconnect = false;

    if (take_lock(handle) != ESP_OK) {
        return;
    }

    handle->ip_addr[0] = '\0';
    handle->disconnect_reason = event ? event->reason : 0;

    if (handle->manual_disconnect) {
        handle->state = handle->ssid[0] == '\0' ? WIFI_MANAGER_STATE_UNCONFIGURED : WIFI_MANAGER_STATE_DISCONNECTED;
        handle->last_error = ESP_OK;
        handle->manual_disconnect = false;
    } else if (handle->state == WIFI_MANAGER_STATE_CONNECTING &&
               handle->retry_count < handle->max_retries) {
        handle->retry_count++;
        reconnect = true;
    } else if (handle->state != WIFI_MANAGER_STATE_FAILED) {
        handle->state = WIFI_MANAGER_STATE_FAILED;
        handle->last_error = ESP_ERR_WIFI_CONN;
    }

    xSemaphoreGive(handle->lock);

    if (reconnect) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            set_failed(handle, err);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)arg;
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (take_lock(handle) != ESP_OK) {
        return;
    }

    handle->state = WIFI_MANAGER_STATE_CONNECTED;
    handle->last_error = ESP_OK;
    handle->disconnect_reason = 0;
    snprintf(handle->ip_addr, sizeof(handle->ip_addr), IPSTR, IP2STR(&event->ip_info.ip));

    xSemaphoreGive(handle->lock);
}

static void softap_wifi_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)event_data;
    wifi_manager_handle_t handle = (wifi_manager_handle_t)arg;
    if (!handle || event_base != WIFI_EVENT) {
        return;
    }

    uint8_t clients = 0U;
    bool changed = false;
    if (take_lock(handle) == ESP_OK) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            if (handle->connected_clients < UINT8_MAX) {
                handle->connected_clients++;
            }
            changed = true;
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (handle->connected_clients > 0U) {
                handle->connected_clients--;
            }
            changed = true;
        }
        clients = handle->connected_clients;
        xSemaphoreGive(handle->lock);
    }

    if (!changed) {
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "SOFTAP_CLIENT_CONNECTED clients=%u", (unsigned)clients);
    } else {
        ESP_LOGI(TAG, "SOFTAP_CLIENT_DISCONNECTED clients=%u", (unsigned)clients);
    }
}

static esp_err_t create_default_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t wifi_manager_init(config_manager_handle_t config_manager, wifi_manager_handle_t *out_handle)
{
    if (!config_manager || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    wifi_manager_handle_t handle = calloc(1, sizeof(struct wifi_manager_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config_manager = config_manager;
    handle->mode = WIFI_MANAGER_MODE_STATION;
    handle->state = WIFI_MANAGER_STATE_UNCONFIGURED;
    handle->max_retries = WIFI_MANAGER_MAX_RETRIES;
    handle->last_error = ESP_OK;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = create_default_event_loop();
    }
    if (err == ESP_OK) {
        handle->sta_netif = esp_netif_create_default_wifi_sta();
        if (!handle->sta_netif) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  WIFI_EVENT_STA_DISCONNECTED,
                                                  &wifi_event_handler,
                                                  handle,
                                                  &handle->wifi_event_handler);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &ip_event_handler,
                                                  handle,
                                                  &handle->ip_event_handler);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
    }

    config_manager_snapshot_t snapshot;
    if (err == ESP_OK && config_manager_get_snapshot(config_manager, &snapshot) == ESP_OK) {
        if (snapshot.wifi_ssid[0] != '\0') {
            handle->state = WIFI_MANAGER_STATE_DISCONNECTED;
            copy_text(handle->ssid, sizeof(handle->ssid), snapshot.wifi_ssid);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi manager init failed, err=0x%x", err);
        wifi_manager_deinit(handle);
        return err;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "Wi-Fi station manager ready");
    return ESP_OK;
}

static esp_err_t configure_softap_ip(esp_netif_t *ap_netif)
{
    if (!ap_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    esp_netif_ip_info_t ip_info = {0};
    err = esp_netif_str_to_ip4(WIFI_MANAGER_SOFTAP_IP_ADDR, &ip_info.ip);
    if (err == ESP_OK) {
        err = esp_netif_str_to_ip4(WIFI_MANAGER_SOFTAP_NETMASK, &ip_info.netmask);
    }
    if (err == ESP_OK) {
        err = esp_netif_str_to_ip4(WIFI_MANAGER_SOFTAP_GATEWAY, &ip_info.gw);
    }
    if (err == ESP_OK) {
        err = esp_netif_set_ip_info(ap_netif, &ip_info);
    }
    return err;
}

static esp_err_t start_softap_dhcp_server(esp_netif_t *ap_netif)
{
    const esp_err_t err = esp_netif_dhcps_start(ap_netif);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
}

esp_err_t wifi_manager_init_softap(const wifi_manager_softap_config_t *config,
                                   wifi_manager_handle_t *out_handle)
{
    if (!out_handle || !wifi_manager_softap_config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    wifi_manager_handle_t handle = calloc(1, sizeof(struct wifi_manager_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->mode = WIFI_MANAGER_MODE_SOFTAP;
    handle->state = WIFI_MANAGER_STATE_FAILED;
    handle->last_error = ESP_OK;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = create_default_event_loop();
    }
    if (err == ESP_OK) {
        handle->ap_netif = esp_netif_create_default_wifi_ap();
        if (!handle->ap_netif) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wifi_init);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  WIFI_EVENT_AP_STACONNECTED,
                                                  &softap_wifi_event_handler,
                                                  handle,
                                                  &handle->softap_connect_event_handler);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  WIFI_EVENT_AP_STADISCONNECTED,
                                                  &softap_wifi_event_handler,
                                                  handle,
                                                  &handle->softap_disconnect_event_handler);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }

    wifi_config_t wifi_config = {0};
    if (err == ESP_OK) {
        copy_text((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), config->ssid);
        copy_text((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), config->passphrase);
        wifi_config.ap.ssid_len = strlen(config->ssid);
        wifi_config.ap.channel = config->channel;
        wifi_config.ap.max_connection = config->max_clients;
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.ap.pmf_cfg.required = true;
        err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            handle->started = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    }
    if (err == ESP_OK) {
        err = configure_softap_ip(handle->ap_netif);
    }
    if (err == ESP_OK) {
        err = start_softap_dhcp_server(handle->ap_netif);
    }
    memset(&wifi_config, 0, sizeof(wifi_config));

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP initialization failed, err=0x%x", err);
        wifi_manager_deinit(handle);
        return err;
    }

    if (take_lock(handle) == ESP_OK) {
        handle->state = WIFI_MANAGER_STATE_AP_RUNNING;
        handle->last_error = ESP_OK;
        handle->dhcp_server_running = true;
        copy_text(handle->ssid, sizeof(handle->ssid), config->ssid);
        copy_text(handle->ip_addr, sizeof(handle->ip_addr), WIFI_MANAGER_SOFTAP_IP_ADDR);
        xSemaphoreGive(handle->lock);
    }
    *out_handle = handle;
    ESP_LOGW(TAG,
             "SOFTAP_STARTED ssid=%s ip=%s channel=%u bandwidth=HT20 dhcp=ON",
             config->ssid,
             WIFI_MANAGER_SOFTAP_IP_ADDR,
             (unsigned)config->channel);
    return ESP_OK;
}

static void wifi_supervisor_task(void *arg)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)arg;
    uint32_t retry_delay_ms = WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS;
    bool retry_ready = true;

    set_supervisor_state(handle, true, retry_delay_ms);

    while (!handle->supervisor_stop) {
        wifi_manager_status_t status;
        esp_err_t status_err = wifi_manager_get_status(handle, &status);
        if (status_err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
            continue;
        }

        if (status.state == WIFI_MANAGER_STATE_CONNECTED) {
            retry_delay_ms = WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS;
            retry_ready = true;
            set_supervisor_state(handle, true, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
            continue;
        }

        if (status.state == WIFI_MANAGER_STATE_UNCONFIGURED ||
            status.state == WIFI_MANAGER_STATE_CONNECTING ||
            status.auto_connect_paused ||
            status.ssid[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
            continue;
        }

        if (!retry_ready) {
            set_supervisor_state(handle, true, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            retry_delay_ms = next_supervisor_backoff_ms(retry_delay_ms);
            retry_ready = true;
            continue;
        }

        esp_err_t err = wifi_manager_connect(handle);
        retry_ready = false;
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            set_supervisor_state(handle, true, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
            continue;
        }

        if (err == ESP_ERR_NOT_FOUND) {
            retry_delay_ms = WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS;
            retry_ready = true;
            set_supervisor_state(handle, true, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
            continue;
        }

        set_supervisor_state(handle, true, retry_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGER_SUPERVISOR_IDLE_MS));
    }

    set_supervisor_state(handle, false, retry_delay_ms);
    if (take_lock(handle) == ESP_OK) {
        handle->supervisor_task = NULL;
        xSemaphoreGive(handle->lock);
    }
    vTaskDelete(NULL);
}

void wifi_manager_deinit(wifi_manager_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->supervisor_stop = true;
    if (handle->supervisor_task) {
        for (int i = 0; i < 120 && handle->supervisor_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (handle->wifi_event_handler) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                    WIFI_EVENT_STA_DISCONNECTED,
                                                    handle->wifi_event_handler);
    }
    if (handle->ip_event_handler) {
        (void)esp_event_handler_instance_unregister(IP_EVENT,
                                                    IP_EVENT_STA_GOT_IP,
                                                    handle->ip_event_handler);
    }
    if (handle->softap_connect_event_handler) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                    WIFI_EVENT_AP_STACONNECTED,
                                                    handle->softap_connect_event_handler);
    }
    if (handle->softap_disconnect_event_handler) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                    WIFI_EVENT_AP_STADISCONNECTED,
                                                    handle->softap_disconnect_event_handler);
    }
    if (handle->started) {
        (void)esp_wifi_stop();
    }
    (void)esp_wifi_deinit();
    if (handle->sta_netif) {
        esp_netif_destroy_default_wifi(handle->sta_netif);
    }
    if (handle->ap_netif) {
        esp_netif_destroy_default_wifi(handle->ap_netif);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t wifi_manager_connect(wifi_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->mode == WIFI_MANAGER_MODE_SOFTAP) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char ssid[CONFIG_MANAGER_WIFI_SSID_MAX] = { 0 };
    char password[CONFIG_MANAGER_WIFI_PASSWORD_MAX] = { 0 };
    esp_err_t err = load_credentials(handle, ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND && take_lock(handle) == ESP_OK) {
            handle->state = WIFI_MANAGER_STATE_UNCONFIGURED;
            handle->ssid[0] = '\0';
            handle->ip_addr[0] = '\0';
            handle->last_error = err;
            xSemaphoreGive(handle->lock);
        }
        return err;
    }

    if (take_lock(handle) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (handle->state == WIFI_MANAGER_STATE_CONNECTING ||
        handle->state == WIFI_MANAGER_STATE_CONNECTED) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->connect_generation++;
    uint32_t generation = handle->connect_generation;
    bool need_start = !handle->started;
    handle->state = WIFI_MANAGER_STATE_CONNECTING;
    handle->retry_count = 0;
    handle->max_retries = WIFI_MANAGER_MAX_RETRIES;
    handle->disconnect_reason = 0;
    handle->last_error = ESP_OK;
    handle->manual_disconnect = false;
    handle->auto_connect_paused = false;
    handle->ip_addr[0] = '\0';
    copy_text(handle->ssid, sizeof(handle->ssid), ssid);
    xSemaphoreGive(handle->lock);

    wifi_config_t wifi_config = { 0 };
    copy_text((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    copy_text((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), password);
    wifi_config.sta.threshold.authmode = password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(password, 0, sizeof(password));
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (err != ESP_OK) {
        set_failed(handle, err);
        return err;
    }

    if (need_start) {
        err = esp_wifi_start();
        if (err == ESP_OK && take_lock(handle) == ESP_OK) {
            handle->started = true;
            xSemaphoreGive(handle->lock);
        }
        if (err != ESP_OK) {
            set_failed(handle, err);
            return err;
        }
    }

    timeout_task_arg_t *task_arg = calloc(1, sizeof(timeout_task_arg_t));
    if (!task_arg) {
        set_failed(handle, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    task_arg->handle = handle;
    task_arg->generation = generation;
    BaseType_t task_ok = xTaskCreate(connect_timeout_task,
                                     "wifi_timeout",
                                     WIFI_MANAGER_TIMEOUT_TASK_STACK,
                                     task_arg,
                                     WIFI_MANAGER_TIMEOUT_TASK_PRIORITY,
                                     NULL);
    if (task_ok != pdPASS) {
        free(task_arg);
        set_failed(handle, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        set_failed(handle, err);
    }
    return err;
}

esp_err_t wifi_manager_disconnect(wifi_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->mode == WIFI_MANAGER_MODE_SOFTAP) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    config_manager_snapshot_t snapshot;
    bool have_snapshot = config_manager_get_snapshot(handle->config_manager, &snapshot) == ESP_OK;
    bool started = false;
    if (take_lock(handle) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    handle->connect_generation++;
    handle->manual_disconnect = true;
    handle->auto_connect_paused = true;
    handle->ip_addr[0] = '\0';
    handle->last_error = ESP_OK;
    started = handle->started;

    if (have_snapshot && snapshot.wifi_ssid[0] == '\0') {
        handle->state = WIFI_MANAGER_STATE_UNCONFIGURED;
        handle->ssid[0] = '\0';
    } else if (handle->state != WIFI_MANAGER_STATE_UNCONFIGURED) {
        handle->state = WIFI_MANAGER_STATE_DISCONNECTED;
    }
    xSemaphoreGive(handle->lock);

    if (!started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_disconnect();
    return err == ESP_ERR_WIFI_NOT_CONNECT ? ESP_OK : err;
}

esp_err_t wifi_manager_start_auto_connect_task(wifi_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (take_lock(handle) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (handle->mode == WIFI_MANAGER_MODE_SOFTAP) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (handle->supervisor_task) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->supervisor_stop = false;
    handle->supervisor_retry_delay_ms = WIFI_MANAGER_SUPERVISOR_BACKOFF_INITIAL_MS;
    xSemaphoreGive(handle->lock);

    BaseType_t ok = xTaskCreate(wifi_supervisor_task,
                                "wifi_reconnect",
                                WIFI_MANAGER_SUPERVISOR_TASK_STACK,
                                handle,
                                WIFI_MANAGER_SUPERVISOR_TASK_PRIORITY,
                                &handle->supervisor_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_manager_set_auto_connect_paused(wifi_manager_handle_t handle, bool paused)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (handle->mode == WIFI_MANAGER_MODE_SOFTAP) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    handle->auto_connect_paused = paused;
    if (!paused) {
        handle->manual_disconnect = false;
    }
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle, wifi_manager_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    config_manager_snapshot_t snapshot;
    bool have_snapshot = handle->mode == WIFI_MANAGER_MODE_STATION &&
                         config_manager_get_snapshot(handle->config_manager, &snapshot) == ESP_OK;

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (handle->mode == WIFI_MANAGER_MODE_STATION &&
        handle->state != WIFI_MANAGER_STATE_CONNECTED &&
        handle->state != WIFI_MANAGER_STATE_CONNECTING &&
        have_snapshot) {
        if (snapshot.wifi_ssid[0] == '\0') {
            handle->state = WIFI_MANAGER_STATE_UNCONFIGURED;
            handle->ssid[0] = '\0';
        } else if (handle->state == WIFI_MANAGER_STATE_UNCONFIGURED) {
            handle->state = WIFI_MANAGER_STATE_DISCONNECTED;
            copy_text(handle->ssid, sizeof(handle->ssid), snapshot.wifi_ssid);
        }
    }

    memset(status, 0, sizeof(*status));
    status->mode = handle->mode;
    status->state = handle->state;
    status->retry_count = handle->retry_count;
    status->max_retries = handle->max_retries;
    status->disconnect_reason = handle->disconnect_reason;
    status->last_error = handle->last_error;
    status->auto_connect_running = handle->supervisor_running;
    status->auto_connect_paused = handle->auto_connect_paused;
    status->auto_retry_delay_ms = handle->supervisor_retry_delay_ms;
    status->connected_clients = handle->connected_clients;
    status->dhcp_server_running = handle->dhcp_server_running;
    copy_text(status->ssid, sizeof(status->ssid), handle->ssid);
    copy_text(status->ip_addr, sizeof(status->ip_addr), handle->ip_addr);
    xSemaphoreGive(handle->lock);

    if (status->mode == WIFI_MANAGER_MODE_STATION &&
        status->state == WIFI_MANAGER_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
        }
    } else if (status->mode == WIFI_MANAGER_MODE_SOFTAP && handle->ap_netif) {
        esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
        if (esp_netif_dhcps_get_status(handle->ap_netif, &dhcp_status) == ESP_OK) {
            status->dhcp_server_running = dhcp_status == ESP_NETIF_DHCP_STARTED;
        }
    }

    return ESP_OK;
}

bool wifi_manager_is_softap(wifi_manager_handle_t handle)
{
    if (!handle || take_lock(handle) != ESP_OK) {
        return false;
    }
    const bool is_softap = handle->mode == WIFI_MANAGER_MODE_SOFTAP;
    xSemaphoreGive(handle->lock);
    return is_softap;
}
