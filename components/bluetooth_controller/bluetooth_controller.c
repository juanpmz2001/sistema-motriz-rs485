#include "bluetooth_controller.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "bluetooth_controller";

// UUIDs para el servicio BLE (mismos que en el código Arduino)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Estructura interna del controlador
struct bluetooth_controller_t {
    bluetooth_joystick_callback_t joystick_callback;
    bluetooth_connection_callback_t connection_callback;
    void *user_data;
    joystick_data_t last_joystick_data;
    bluetooth_state_t state;
    bool initialized;
    uint16_t gatts_if;
    uint16_t conn_id;
    uint16_t char_handle;
    uint16_t service_handle;
    uint64_t start_time;
    uint32_t connection_timeout_ms;
};

// Variables globales para BLE
static bluetooth_controller_handle_t g_bt_handle = NULL;
static uint8_t adv_config_done = 0;

// Configuración de advertising
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// UUID del servicio en formato binario
static uint8_t service_uuid[16] = {
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
    0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f
};

// UUID de la característica en formato binario
static uint8_t char_uuid[16] = {
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7,
    0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe
};

// Prototipos de funciones internas
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void parse_joystick_data(const char *data, size_t len, joystick_data_t *joystick);
static int map_joystick_value(int value, int in_min, int in_max, int out_min, int out_max);

bluetooth_controller_handle_t bluetooth_controller_init(const bluetooth_controller_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return NULL;
    }

    if (g_bt_handle != NULL) {
        ESP_LOGE(TAG, "Bluetooth controller already initialized");
        return NULL;
    }

    bluetooth_controller_handle_t handle = malloc(sizeof(struct bluetooth_controller_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for Bluetooth controller");
        return NULL;
    }

    handle->joystick_callback = config->joystick_callback;
    handle->connection_callback = config->connection_callback;
    handle->user_data = config->user_data;
    handle->state = BT_STATE_DISCONNECTED;
    handle->initialized = false;
    handle->gatts_if = ESP_GATT_IF_NONE;
    handle->conn_id = 0;
    handle->char_handle = 0;
    handle->service_handle = 0;
    handle->connection_timeout_ms = config->connection_timeout_ms;
    handle->start_time = esp_timer_get_time();
    handle->last_joystick_data.x = 0;
    handle->last_joystick_data.y = 0;
    handle->last_joystick_data.valid = false;
    handle->last_joystick_data.timestamp = 0;

    g_bt_handle = handle;

    // Inicializar NVS (requerido para Bluetooth)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializar Bluetooth
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller release classic bt memory failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize controller failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable controller failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init bluetooth failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable bluetooth failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    // Registrar callbacks
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    // Registrar aplicación GATT
    ret = esp_ble_gatts_app_register(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        free(handle);
        g_bt_handle = NULL;
        return NULL;
    }

    // Configurar MTU
    ret = esp_ble_gatt_set_local_mtu(500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set local MTU failed: %s", esp_err_to_name(ret));
    }

    handle->initialized = true;
    handle->state = BT_STATE_ADVERTISING;
    
    ESP_LOGI(TAG, "Bluetooth controller initialized with device name: %s", config->device_name);
    ESP_LOGI(TAG, "Connection timeout: %" PRIu32 " ms", config->connection_timeout_ms);

    return handle;
}

void bluetooth_controller_deinit(bluetooth_controller_handle_t handle)
{
    if (!handle) return;

    if (handle->initialized) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        g_bt_handle = NULL;
    }

    free(handle);
    ESP_LOGI(TAG, "Bluetooth controller deinitialized");
}

bool bluetooth_controller_is_connected(bluetooth_controller_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return false;
    }

    return handle->state == BT_STATE_CONNECTED;
}

bluetooth_state_t bluetooth_controller_get_state(bluetooth_controller_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return BT_STATE_DISCONNECTED;
    }

    return handle->state;
}

bool bluetooth_controller_get_joystick_data(bluetooth_controller_handle_t handle, joystick_data_t *data)
{
    if (!handle || !handle->initialized || !data) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    *data = handle->last_joystick_data;
    return true;
}

bool bluetooth_controller_is_connection_timeout(bluetooth_controller_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return true;
    }

    if (handle->state == BT_STATE_CONNECTED) {
        return false;  // Ya conectado, no hay timeout
    }

    uint64_t current_time = esp_timer_get_time();
    uint64_t elapsed_ms = (current_time - handle->start_time) / 1000;
    
    return elapsed_ms >= handle->connection_timeout_ms;
}

void bluetooth_controller_stop_advertising(bluetooth_controller_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return;
    }

    if (handle->state == BT_STATE_ADVERTISING) {
        esp_ble_gap_stop_advertising();
        handle->state = BT_STATE_DISCONNECTED;
        ESP_LOGI(TAG, "Bluetooth advertising stopped");
    }
}

// Funciones internas

static void parse_joystick_data(const char *data, size_t len, joystick_data_t *joystick)
{
    if (!data || !joystick || len == 0) return;

    // Buscar los marcadores de inicio y fin (como en el código Arduino)
    const char *start = NULL;
    const char *end = NULL;
    const char *sep = NULL;

    // Buscar STX (0x02)
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\x02') {
            start = &data[i];
            break;
        }
    }

    if (!start) {
        joystick->valid = false;
        return;
    }

    // Buscar ETX (0x03)
    for (const char *p = start + 1; p < data + len; p++) {
        if (*p == '\x03') {
            end = p;
            break;
        }
    }

    if (!end) {
        joystick->valid = false;
        return;
    }

    // Buscar separador ','
    for (const char *p = start + 1; p < end; p++) {
        if (*p == ',') {
            sep = p;
            break;
        }
    }

    if (!sep || sep <= start || sep >= end) {
        joystick->valid = false;
        return;
    }

    // Extraer coordenadas X e Y
    char x_str[16], y_str[16];
    int x_len = sep - start - 1;
    int y_len = end - sep - 1;

    if (x_len > 0 && x_len < 15 && y_len > 0 && y_len < 15) {
        strncpy(x_str, start + 1, x_len);
        x_str[x_len] = '\0';
        strncpy(y_str, sep + 1, y_len);
        y_str[y_len] = '\0';

        joystick->x = atoi(x_str);
        joystick->y = atoi(y_str);
        joystick->valid = true;
        joystick->timestamp = esp_timer_get_time() / 1000;  // timestamp en ms

        ESP_LOGD(TAG, "Parsed joystick data: X=%d, Y=%d", joystick->x, joystick->y);
    } else {
        joystick->valid = false;
    }
}

static int map_joystick_value(int value, int in_min, int in_max, int out_min, int out_max)
{
    return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (!g_bt_handle) return;

    switch (event) {
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(TAG, "REGISTER_APP_EVT, status %d, app_id %d", param->reg.status, param->reg.app_id);
            g_bt_handle->gatts_if = gatts_if;

            // Configurar nombre del dispositivo
            esp_ble_gap_set_device_name("ESP32-Joystick");

            // Configurar advertising data
            esp_ble_adv_data_t adv_data = {
                .set_scan_rsp = false,
                .include_name = true,
                .include_txpower = false,
                .min_interval = 0x0006,
                .max_interval = 0x0010,
                .appearance = 0x00,
                .manufacturer_len = 0,
                .p_manufacturer_data = NULL,
                .service_data_len = 0,
                .p_service_data = NULL,
                .service_uuid_len = 16,
                .p_service_uuid = service_uuid,
                .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
            };
            esp_ble_gap_config_adv_data(&adv_data);
            adv_config_done |= 1;

            // Crear servicio GATT
            esp_gatt_srvc_id_t service_id = {
                .is_primary = true,
                .id.inst_id = 0x00,
                .id.uuid.len = ESP_UUID_LEN_128,
            };
            memcpy(service_id.id.uuid.uuid.uuid128, service_uuid, 16);
            esp_ble_gatts_create_service(gatts_if, &service_id, 4);
            break;

        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(TAG, "CREATE_SERVICE_EVT, status %d, service_handle %d", param->create.status, param->create.service_handle);
            g_bt_handle->service_handle = param->create.service_handle;
            esp_ble_gatts_start_service(g_bt_handle->service_handle);

            // Agregar característica
            esp_bt_uuid_t char_uuid_bt = {
                .len = ESP_UUID_LEN_128,
            };
            memcpy(char_uuid_bt.uuid.uuid128, char_uuid, 16);

            esp_ble_gatts_add_char(g_bt_handle->service_handle, &char_uuid_bt,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   NULL, NULL);
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            ESP_LOGI(TAG, "ADD_CHAR_EVT, status %d, attr_handle %d", param->add_char.status, param->add_char.attr_handle);
            g_bt_handle->char_handle = param->add_char.attr_handle;
            break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.len > 0) {
                ESP_LOGD(TAG, "Received data: len=%d", param->write.len);
                
                joystick_data_t joystick;
                parse_joystick_data((char*)param->write.value, param->write.len, &joystick);

                if (joystick.valid) {
                    g_bt_handle->last_joystick_data = joystick;
                    if (g_bt_handle->joystick_callback) {
                        g_bt_handle->joystick_callback(&joystick, g_bt_handle->user_data);
                    }
                }
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "Client connected");
            g_bt_handle->state = BT_STATE_CONNECTED;
            g_bt_handle->conn_id = param->connect.conn_id;
            
            if (g_bt_handle->connection_callback) {
                g_bt_handle->connection_callback(BT_STATE_CONNECTED, g_bt_handle->user_data);
            }
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Client disconnected");
            g_bt_handle->state = BT_STATE_ADVERTISING;
            
            if (g_bt_handle->connection_callback) {
                g_bt_handle->connection_callback(BT_STATE_ADVERTISING, g_bt_handle->user_data);
            }
            
            // Reiniciar advertising
            esp_ble_gap_start_advertising(&adv_params);
            break;

        default:
            break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~1);
            if (adv_config_done == 0) {
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed");
            } else {
                ESP_LOGI(TAG, "Advertising started successfully");
                if (g_bt_handle) {
                    g_bt_handle->state = BT_STATE_ADVERTISING;
                }
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertising stopped");
            if (g_bt_handle) {
                g_bt_handle->state = BT_STATE_DISCONNECTED;
            }
            break;

        default:
            break;
    }
} 