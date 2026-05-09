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

struct wifi_manager_t {
    config_manager_handle_t config_manager;
    esp_netif_t *sta_netif;
    SemaphoreHandle_t lock;
    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t ip_event_handler;
    wifi_manager_state_t state;
    char ssid[CONFIG_MANAGER_WIFI_SSID_MAX];
    char ip_addr[WIFI_MANAGER_IP_ADDR_MAX];
    uint8_t retry_count;
    uint8_t max_retries;
    uint16_t disconnect_reason;
    esp_err_t last_error;
    bool started;
    bool manual_disconnect;
    uint32_t connect_generation;
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
    case WIFI_MANAGER_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
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

void wifi_manager_deinit(wifi_manager_handle_t handle)
{
    if (!handle) {
        return;
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
    if (handle->started) {
        (void)esp_wifi_stop();
    }
    (void)esp_wifi_deinit();
    if (handle->sta_netif) {
        esp_netif_destroy_default_wifi(handle->sta_netif);
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

    config_manager_snapshot_t snapshot;
    bool have_snapshot = config_manager_get_snapshot(handle->config_manager, &snapshot) == ESP_OK;
    bool started = false;
    if (take_lock(handle) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    handle->connect_generation++;
    handle->manual_disconnect = true;
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

esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle, wifi_manager_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    config_manager_snapshot_t snapshot;
    bool have_snapshot = config_manager_get_snapshot(handle->config_manager, &snapshot) == ESP_OK;

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (handle->state != WIFI_MANAGER_STATE_CONNECTED &&
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
    status->state = handle->state;
    status->retry_count = handle->retry_count;
    status->max_retries = handle->max_retries;
    status->disconnect_reason = handle->disconnect_reason;
    status->last_error = handle->last_error;
    copy_text(status->ssid, sizeof(status->ssid), handle->ssid);
    copy_text(status->ip_addr, sizeof(status->ip_addr), handle->ip_addr);
    xSemaphoreGive(handle->lock);

    if (status->state == WIFI_MANAGER_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
        }
    }

    return ESP_OK;
}
