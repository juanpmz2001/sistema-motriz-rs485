#include "motor_controller.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motor_controller";

// Estructura interna del controlador
struct motor_controller_t {
    uart_port_t uart_port;
    uint8_t device_address;
    bool initialized;
};

// Registros Modbus del controlador HUB
#define REG_M1_CONTROL_CMD  0x5300
#define REG_M1_GIVEN_SPEED  0x5304
#define REG_M2_CONTROL_CMD  0x5301
#define REG_M2_GIVEN_SPEED  0x5305
#define REG_M1_ACTUAL_SPEED 0x5410
#define REG_M2_ACTUAL_SPEED 0x5411

// Funciones Modbus
#define MODBUS_FUNC_READ_HOLDING_REGISTERS  0x03
#define MODBUS_FUNC_WRITE_SINGLE_REGISTER   0x06

// Prototipos de funciones internas
static uint16_t compute_crc(const uint8_t *data, size_t length);
static bool send_modbus_command(motor_controller_handle_t handle, const uint8_t *command, size_t cmd_length);
static bool write_register16(motor_controller_handle_t handle, uint16_t reg_address, int16_t value);
static bool read_register16(motor_controller_handle_t handle, uint16_t reg_address, int16_t *value);
static size_t read_response(motor_controller_handle_t handle, uint8_t *buffer, size_t max_length, uint32_t timeout_ms);

motor_controller_handle_t motor_controller_init(const motor_controller_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return NULL;
    }

    motor_controller_handle_t handle = malloc(sizeof(struct motor_controller_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for motor controller");
        return NULL;
    }

    handle->uart_port = config->uart_port;
    handle->device_address = config->device_address;
    handle->initialized = false;

    // Configurar UART
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(handle->uart_port, 1024, 1024, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    ret = uart_param_config(handle->uart_port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(handle->uart_port);
        free(handle);
        return NULL;
    }

    ret = uart_set_pin(handle->uart_port, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(handle->uart_port);
        free(handle);
        return NULL;
    }

    handle->initialized = true;
    ESP_LOGI(TAG, "Motor controller initialized on UART%d", handle->uart_port);
    
    // Pequeña pausa para estabilizar la comunicación
    vTaskDelay(pdMS_TO_TICKS(100));
    
    return handle;
}

void motor_controller_deinit(motor_controller_handle_t handle)
{
    if (!handle) return;
    
    if (handle->initialized) {
        uart_driver_delete(handle->uart_port);
    }
    
    free(handle);
    ESP_LOGI(TAG, "Motor controller deinitialized");
}

bool motor_controller_set_m1_control_command(motor_controller_handle_t handle, motor_control_cmd_t cmd)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return false;
    }

    // Repetir comando 3 veces para mayor confiabilidad (como en el código original)
    for (int i = 0; i < 3; i++) {
        if (!write_register16(handle, REG_M1_CONTROL_CMD, (uint16_t)cmd)) {
            ESP_LOGW(TAG, "Failed to send M1 control command, attempt %d", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    return true;
}

bool motor_controller_set_m1_speed(motor_controller_handle_t handle, int16_t speed)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return false;
    }

    // Repetir comando 3 veces para mayor confiabilidad
    for (int i = 0; i < 3; i++) {
        if (!write_register16(handle, REG_M1_GIVEN_SPEED, speed)) {
            ESP_LOGW(TAG, "Failed to send M1 speed, attempt %d", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    return true;
}

bool motor_controller_set_m2_control_command(motor_controller_handle_t handle, motor_control_cmd_t cmd)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return false;
    }

    // Repetir comando 3 veces para mayor confiabilidad
    for (int i = 0; i < 3; i++) {
        if (!write_register16(handle, REG_M2_CONTROL_CMD, (uint16_t)cmd)) {
            ESP_LOGW(TAG, "Failed to send M2 control command, attempt %d", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    return true;
}

bool motor_controller_set_m2_speed(motor_controller_handle_t handle, int16_t speed)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Motor controller not initialized");
        return false;
    }

    // Repetir comando 3 veces para mayor confiabilidad
    for (int i = 0; i < 3; i++) {
        if (!write_register16(handle, REG_M2_GIVEN_SPEED, speed)) {
            ESP_LOGW(TAG, "Failed to send M2 speed, attempt %d", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    return true;
}

bool motor_controller_get_m1_speed(motor_controller_handle_t handle, int16_t *speed)
{
    if (!handle || !handle->initialized || !speed) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    return read_register16(handle, REG_M1_ACTUAL_SPEED, speed);
}

bool motor_controller_get_m2_speed(motor_controller_handle_t handle, int16_t *speed)
{
    if (!handle || !handle->initialized || !speed) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    return read_register16(handle, REG_M2_ACTUAL_SPEED, speed);
}

// Funciones internas

static bool write_register16(motor_controller_handle_t handle, uint16_t reg_address, int16_t value)
{
    uint8_t cmd[6];
    cmd[0] = handle->device_address;
    cmd[1] = MODBUS_FUNC_WRITE_SINGLE_REGISTER;
    cmd[2] = (reg_address >> 8) & 0xFF;
    cmd[3] = reg_address & 0xFF;
    cmd[4] = (value >> 8) & 0xFF;
    cmd[5] = value & 0xFF;

    return send_modbus_command(handle, cmd, 6);
}

static bool read_register16(motor_controller_handle_t handle, uint16_t reg_address, int16_t *value)
{
    uint8_t cmd[6];
    cmd[0] = handle->device_address;
    cmd[1] = MODBUS_FUNC_READ_HOLDING_REGISTERS;
    cmd[2] = (reg_address >> 8) & 0xFF;
    cmd[3] = reg_address & 0xFF;
    cmd[4] = 0x00;  // Número de registros (high byte)
    cmd[5] = 0x01;  // Número de registros (low byte)

    if (!send_modbus_command(handle, cmd, 6)) {
        return false;
    }

    uint8_t buffer[64];
    size_t len = read_response(handle, buffer, sizeof(buffer), 3000);
    
    if (len < 5) {
        ESP_LOGW(TAG, "Response too short: %d bytes", len);
        return false;
    }

    if (buffer[1] == MODBUS_FUNC_READ_HOLDING_REGISTERS) {
        uint8_t byte_count = buffer[2];
        if (byte_count == 2 && len >= 5) {
            *value = (buffer[3] << 8) | buffer[4];
            return true;
        }
    }

    ESP_LOGW(TAG, "Invalid response format");
    return false;
}

static bool send_modbus_command(motor_controller_handle_t handle, const uint8_t *command, size_t cmd_length)
{
    if (cmd_length > 62) {
        ESP_LOGE(TAG, "Command too long");
        return false;
    }

    uint8_t packet[64];
    memcpy(packet, command, cmd_length);

    uint16_t crc = compute_crc(command, cmd_length);
    // CRC en formato big-endian (byte alto primero)
    packet[cmd_length] = (crc >> 8) & 0xFF;
    packet[cmd_length + 1] = crc & 0xFF;
    size_t packet_length = cmd_length + 2;

    // Limpiar buffer de recepción antes de enviar
    uart_flush_input(handle->uart_port);

    int bytes_written = uart_write_bytes(handle->uart_port, packet, packet_length);
    if (bytes_written != packet_length) {
        ESP_LOGE(TAG, "Failed to write all bytes: %d/%d", bytes_written, packet_length);
        return false;
    }

    return true;
}

static size_t read_response(motor_controller_handle_t handle, uint8_t *buffer, size_t max_length, uint32_t timeout_ms)
{
    size_t index = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) < timeout_ms) {
        int available = uart_read_bytes(handle->uart_port, buffer + index, max_length - index, pdMS_TO_TICKS(100));
        if (available > 0) {
            index += available;
        }
        
        // Si hemos recibido algo y ha pasado un tiempo, consideramos que la respuesta está completa
        if (index > 0 && (xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) > 300) {
            break;
        }
    }
    
    return index;
}

static uint16_t compute_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
} 