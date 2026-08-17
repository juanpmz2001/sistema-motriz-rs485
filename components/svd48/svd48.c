#include "svd48.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "svd48";

#define SVD48_UART_RX_BUFFER_SIZE 512
#define SVD48_UART_TX_BUFFER_SIZE 512
#define SVD48_DEFAULT_TIMEOUT_MS 100
#define SVD48_DEFAULT_RETRIES 2
#define SVD48_MAX_RETRIES 5
#define SVD48_DEFAULT_TELEMETRY_PERIOD_MS 30
#define SVD48_DEFAULT_STALE_TIMEOUT_MS 1000
#define SVD48_BUS_LOCK_TIMEOUT_MS 1000
#define SVD48_POLL_SLOW_DIVIDER 20
#define SVD48_POLL_BACKOFF_BASE_MS 250
#define SVD48_POLL_BACKOFF_MAX_MS 1500
#define SVD48_LEGACY_DEVICE_MAX 4U

#define REG_M1_STATUS 0x5400
#define REG_M1_MOTOR_TEMP 0x5404
#define REG_M1_MOS_TEMP 0x5408
#define REG_M1_BUS_VOLTAGE 0x540C
#define REG_M1_ACTUAL_SPEED 0x5410
#define REG_M1_ACTUAL_CURRENT 0x5414
#define REG_M1_POSITION 0x5418
#define REG_M1_ERROR_CODE 0x5420

#define REG_M1_CONTROL_CMD 0x5300
#define REG_M1_GIVEN_SPEED 0x5304
#define REG_M1_GIVEN_CURRENT 0x5308

#define REG_M2_CONTROL_CMD 0x5301
#define REG_M2_GIVEN_SPEED 0x5305
#define REG_M2_GIVEN_CURRENT 0x5309

#define REG_WHEEL_DIAMETER_MM 0x2201
#define REG_MOTOR_TEETH 0x2202
#define REG_WHEEL_TEETH 0x2203
#define REG_M1_POLE_PAIRS 0x5018
#define REG_M2_POLE_PAIRS 0x5019
#define REG_M1_SENSOR_TYPE 0x502C
#define REG_M2_SENSOR_TYPE 0x502D
#define REG_M1_HALL_INSTALLATION 0x5620
#define REG_M2_HALL_INSTALLATION 0x5621
#define REG_M1_HALL_STATUS 0x5688
#define REG_M2_HALL_STATUS 0x5689
#define REG_M1_HALL_ANGLE 0x568C
#define REG_M2_HALL_ANGLE 0x568D

struct svd48_t {
    svd48_config_t config;
    bool attached_devices;
    size_t attached_device_count;
    svd48_device_t *attached_device_items[SVD48_LEGACY_DEVICE_MAX];
    size_t attached_binding_count;
    svd48_legacy_channel_binding_t attached_bindings[SVD48_MOTOR_COUNT];
    SemaphoreHandle_t bus_lock;
    SemaphoreHandle_t state_lock;
    SemaphoreHandle_t trace_lock;
    TaskHandle_t poll_task;
    bool polling;
    bool trace_enabled;
    bool trace_polling_enabled;
    uint32_t poll_count;
    uint32_t trace_seq;
    uint32_t drive_next_poll_ms[SVD48_DRIVE_COUNT];
    uint8_t drive_fail_count[SVD48_DRIVE_COUNT];
    svd48_motor_telemetry_t motors[SVD48_MOTOR_COUNT];
};

typedef enum {
    PAIR_FIELD_STATUS,
    PAIR_FIELD_MOTOR_TEMP,
    PAIR_FIELD_BUS_VOLTAGE,
    PAIR_FIELD_MOS_TEMP,
    PAIR_FIELD_SPEED,
    PAIR_FIELD_CURRENT,
} pair_field_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t validate_motor(svd48_handle_t handle, uint8_t logical_motor)
{
    size_t motor_count = handle && handle->attached_devices
                             ? handle->attached_binding_count
                             : SVD48_MOTOR_COUNT;
    if (!handle || logical_motor >= motor_count) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t device_result_to_esp(svd48_device_result_t result)
{
    switch (result) {
    case SVD48_DEVICE_OK:
        return ESP_OK;
    case SVD48_DEVICE_INVALID_ARGUMENT:
        return ESP_ERR_INVALID_ARG;
    case SVD48_DEVICE_TIMEOUT:
    case SVD48_DEVICE_BUS_BUSY:
        return ESP_ERR_TIMEOUT;
    case SVD48_DEVICE_CRC_ERROR:
        return ESP_ERR_INVALID_CRC;
    case SVD48_DEVICE_EXCEPTION:
    case SVD48_DEVICE_BAD_RESPONSE:
    case SVD48_DEVICE_INCOMPLETE_FRAME:
    case SVD48_DEVICE_PARTIAL:
        return ESP_ERR_INVALID_RESPONSE;
    case SVD48_DEVICE_UNSUPPORTED:
        return ESP_ERR_NOT_ALLOWED;
    case SVD48_DEVICE_CANCELLED:
        return ESP_ERR_INVALID_STATE;
    case SVD48_DEVICE_IO_ERROR:
    default:
        return ESP_FAIL;
    }
}

static svd48_status_t device_result_to_status(svd48_device_result_t result)
{
    switch (result) {
    case SVD48_DEVICE_OK:
        return SVD48_OK;
    case SVD48_DEVICE_INVALID_ARGUMENT:
        return SVD48_ERR_INVALID_ARG;
    case SVD48_DEVICE_TIMEOUT:
    case SVD48_DEVICE_BUS_BUSY:
        return SVD48_ERR_TIMEOUT;
    case SVD48_DEVICE_CRC_ERROR:
        return SVD48_ERR_CRC;
    case SVD48_DEVICE_EXCEPTION:
        return SVD48_ERR_EXCEPTION;
    case SVD48_DEVICE_BAD_RESPONSE:
    case SVD48_DEVICE_INCOMPLETE_FRAME:
    case SVD48_DEVICE_PARTIAL:
        return SVD48_ERR_BAD_RESPONSE;
    default:
        return SVD48_ERR_UART;
    }
}

static svd48_device_t *attached_device_by_address(svd48_handle_t handle,
                                                  uint8_t address)
{
    if (!handle || !handle->attached_devices) {
        return NULL;
    }
    for (size_t index = 0; index < handle->attached_device_count; ++index) {
        if (svd48_device_address(handle->attached_device_items[index]) == address) {
            return handle->attached_device_items[index];
        }
    }
    return NULL;
}

static svd48_channel_t *attached_channel(svd48_handle_t handle,
                                         uint8_t logical_motor)
{
    if (validate_motor(handle, logical_motor) != ESP_OK ||
        !handle->attached_devices) {
        return NULL;
    }
    const svd48_legacy_channel_binding_t *binding =
        &handle->attached_bindings[logical_motor];
    return svd48_device_channel(binding->device, binding->channel);
}

static uint16_t channel_register(uint8_t channel, uint16_t m1_reg, uint16_t m2_reg)
{
    return channel == 0 ? m1_reg : m2_reg;
}

static bool drive_index_from_id(svd48_handle_t handle, uint8_t drive_id, uint8_t *drive_index)
{
    if (!handle || !drive_index) {
        return false;
    }
    for (uint8_t i = 0; i < SVD48_DRIVE_COUNT; i++) {
        if (handle->config.drive_ids[i] == drive_id) {
            *drive_index = i;
            return true;
        }
    }
    return false;
}

static svd48_status_t esp_to_svd48_status(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return SVD48_OK;
        case ESP_ERR_TIMEOUT:
            return SVD48_ERR_TIMEOUT;
        case ESP_ERR_INVALID_CRC:
            return SVD48_ERR_CRC;
        case ESP_ERR_INVALID_RESPONSE:
            return SVD48_ERR_BAD_RESPONSE;
        case ESP_ERR_INVALID_ARG:
            return SVD48_ERR_INVALID_ARG;
        default:
            return SVD48_ERR_UART;
    }
}

static const char *esp_err_short_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return "OK";
        case ESP_ERR_TIMEOUT:
            return "TIMEOUT";
        case ESP_ERR_INVALID_CRC:
            return "INVALID_CRC";
        case ESP_ERR_INVALID_RESPONSE:
            return "INVALID_RESPONSE";
        case ESP_ERR_INVALID_SIZE:
            return "INVALID_SIZE";
        case ESP_ERR_INVALID_ARG:
            return "INVALID_ARG";
        default:
            return "UART_OR_UNKNOWN";
    }
}

static const char *register_name(uint16_t reg)
{
    switch (reg) {
        case REG_M1_CONTROL_CMD:
            return "M1_CONTROL_CMD";
        case REG_M2_CONTROL_CMD:
            return "M2_CONTROL_CMD";
        case REG_M1_GIVEN_SPEED:
            return "M1_GIVEN_SPEED_RPM";
        case REG_M2_GIVEN_SPEED:
            return "M2_GIVEN_SPEED_RPM";
        case REG_M1_GIVEN_CURRENT:
            return "M1_GIVEN_CURRENT_DA";
        case REG_M2_GIVEN_CURRENT:
            return "M2_GIVEN_CURRENT_DA";
        case REG_WHEEL_DIAMETER_MM:
            return "WHEEL_DIAMETER_MM";
        case REG_MOTOR_TEETH:
            return "MOTOR_TEETH";
        case REG_WHEEL_TEETH:
            return "WHEEL_TEETH";
        case REG_M1_POLE_PAIRS:
            return "M1_POLE_PAIRS";
        case REG_M2_POLE_PAIRS:
            return "M2_POLE_PAIRS";
        case REG_M1_SENSOR_TYPE:
            return "M1_SENSOR_TYPE";
        case REG_M2_SENSOR_TYPE:
            return "M2_SENSOR_TYPE";
        case REG_M1_HALL_INSTALLATION:
            return "M1_HALL_INSTALLATION";
        case REG_M2_HALL_INSTALLATION:
            return "M2_HALL_INSTALLATION";
        case REG_M1_HALL_STATUS:
            return "M1_HALL_STATUS";
        case REG_M2_HALL_STATUS:
            return "M2_HALL_STATUS";
        case REG_M1_HALL_ANGLE:
            return "M1_HALL_ANGLE";
        case REG_M2_HALL_ANGLE:
            return "M2_HALL_ANGLE";
        case REG_M1_STATUS:
            return "M1_STATUS";
        case REG_M1_MOTOR_TEMP:
            return "M1_MOTOR_TEMP_DC";
        case REG_M1_BUS_VOLTAGE:
            return "M1_BUS_VOLTAGE_DV";
        case REG_M1_MOS_TEMP:
            return "M1_MOS_TEMP_DC";
        case REG_M1_ACTUAL_SPEED:
            return "M1_ACTUAL_SPEED_RPM";
        case REG_M1_ACTUAL_CURRENT:
            return "M1_ACTUAL_CURRENT_DA";
        case REG_M1_POSITION:
            return "M1_POSITION_HI";
        case REG_M1_ERROR_CODE:
            return "M1_ERROR_CODE_HI";
        default:
            return "UNKNOWN";
    }
}

static bool register_value_is_signed(uint16_t reg)
{
    return reg == REG_M1_GIVEN_SPEED ||
           reg == REG_M2_GIVEN_SPEED ||
           reg == REG_M1_GIVEN_CURRENT ||
           reg == REG_M2_GIVEN_CURRENT ||
           reg == REG_M1_MOTOR_TEMP ||
           reg == REG_M1_MOTOR_TEMP + 1 ||
           reg == REG_M1_MOS_TEMP ||
           reg == REG_M1_MOS_TEMP + 1 ||
           reg == REG_M1_ACTUAL_SPEED ||
           reg == REG_M1_ACTUAL_SPEED + 1 ||
           reg == REG_M1_ACTUAL_CURRENT ||
           reg == REG_M1_ACTUAL_CURRENT + 1;
}

static bool trace_should_print(svd48_handle_t handle)
{
    if (!handle || !handle->trace_enabled) {
        return false;
    }
    if (!handle->trace_polling_enabled && handle->poll_task &&
        xTaskGetCurrentTaskHandle() == handle->poll_task) {
        return false;
    }
    return true;
}

static uint32_t trace_next_seq(svd48_handle_t handle)
{
    if (!trace_should_print(handle)) {
        return 0;
    }
    return ++handle->trace_seq;
}

static void trace_print_hex(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        printf("%s%02X", i == 0 ? "" : " ", bytes[i]);
    }
}

static void trace_printf_line(svd48_handle_t handle, const char *fmt, ...)
{
    if (!trace_should_print(handle)) {
        return;
    }

    xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
    xSemaphoreGive(handle->trace_lock);
}

static void trace_request(svd48_handle_t handle, uint32_t seq, uint8_t attempt, const uint8_t *request, size_t request_len)
{
    if (seq == 0 || !trace_should_print(handle) || !request || request_len < 4) {
        return;
    }

    uint8_t slave_id = request[0];
    uint8_t function = request[1];
    uint16_t reg = request_len >= 4 ? ((uint16_t)request[2] << 8) | request[3] : 0;
    uint16_t field = request_len >= 6 ? ((uint16_t)request[4] << 8) | request[5] : 0;
    uint16_t crc = request_len >= 2 ? ((uint16_t)request[request_len - 2] << 8) | request[request_len - 1] : 0;

    xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
    printf("TRACE RS485_TX SEQ:%lu ATTEMPT:%u DRIVE:%u FUNC:0x%02X REG:0x%04X REG_NAME:%s ",
           (unsigned long)seq,
           attempt,
           slave_id,
           function,
           reg,
           register_name(reg));

    if (function == SVD48_FUNC_READ_HOLDING) {
        printf("QTY:%u ", field);
    } else if (function == SVD48_FUNC_WRITE_SINGLE) {
        if (register_value_is_signed(reg)) {
            printf("VALUE:%d RAW:0x%04X ", (int16_t)field, field);
        } else {
            printf("VALUE:%u RAW:0x%04X ", field, field);
        }
    } else if (function == SVD48_FUNC_WRITE_MULTI) {
        uint8_t byte_count = request_len > 6 ? request[6] : 0;
        printf("QTY:%u BYTE_COUNT:%u ", field, byte_count);
    }

    printf("CRC:0x%04X CRC_ORDER:HIGH_LOW HEX:", crc);
    trace_print_hex(request, request_len);
    printf("\n");
    fflush(stdout);
    xSemaphoreGive(handle->trace_lock);
}

static void trace_decode_read_values(uint16_t start_reg, uint16_t quantity, const uint16_t *regs)
{
    if (start_reg == REG_M1_ACTUAL_SPEED && quantity == 2) {
        printf(" M1_ACTUAL_SPEED_RPM:%d M2_ACTUAL_SPEED_RPM:%d", (int16_t)regs[0], (int16_t)regs[1]);
    } else if (start_reg == REG_M1_ACTUAL_CURRENT && quantity == 2) {
        printf(" M1_ACTUAL_CURRENT_DA:%d M2_ACTUAL_CURRENT_DA:%d", (int16_t)regs[0], (int16_t)regs[1]);
    } else if (start_reg == REG_M1_STATUS && quantity == 2) {
        printf(" M1_STATUS:%d M2_STATUS:%d", (int16_t)regs[0], (int16_t)regs[1]);
    } else if (start_reg == REG_M1_MOTOR_TEMP && quantity == 2) {
        printf(" M1_MOTOR_TEMP_DC:%d M2_MOTOR_TEMP_DC:%d", (int16_t)regs[0], (int16_t)regs[1]);
    } else if (start_reg == REG_M1_BUS_VOLTAGE && quantity == 2) {
        printf(" M1_BUS_VOLTAGE_DV:%u M2_BUS_VOLTAGE_DV:%u", regs[0], regs[1]);
    } else if (start_reg == REG_M1_MOS_TEMP && quantity == 2) {
        printf(" M1_MOS_TEMP_DC:%d M2_MOS_TEMP_DC:%d", (int16_t)regs[0], (int16_t)regs[1]);
    } else if (start_reg == REG_M1_POSITION && quantity == 4) {
        int32_t m1 = (int32_t)(((uint32_t)regs[0] << 16) | regs[1]);
        int32_t m2 = (int32_t)(((uint32_t)regs[2] << 16) | regs[3]);
        printf(" M1_POSITION:%ld M2_POSITION:%ld", (long)m1, (long)m2);
    } else if (start_reg == REG_M1_ERROR_CODE && quantity == 4) {
        uint32_t m1 = ((uint32_t)regs[0] << 16) | regs[1];
        uint32_t m2 = ((uint32_t)regs[2] << 16) | regs[3];
        printf(" M1_ERROR:0x%08lX M2_ERROR:0x%08lX", (unsigned long)m1, (unsigned long)m2);
    }
}

static void trace_response(svd48_handle_t handle,
                           uint32_t seq,
                           uint8_t attempt,
                           const uint8_t *request,
                           size_t request_len,
                           const uint8_t *response,
                           size_t response_len,
                           esp_err_t err,
                           bool crc_ok)
{
    if (seq == 0 || !trace_should_print(handle)) {
        return;
    }

    uint8_t slave_id = request_len >= 1 ? request[0] : 0;
    uint8_t request_function = request_len >= 2 ? request[1] : 0;
    uint16_t request_reg = request_len >= 4 ? ((uint16_t)request[2] << 8) | request[3] : 0;
    uint16_t request_field = request_len >= 6 ? ((uint16_t)request[4] << 8) | request[5] : 0;

    xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
    printf("TRACE RS485_RX SEQ:%lu ATTEMPT:%u DRIVE:%u STATUS:%s LEN:%u CRC_OK:%u HEX:",
           (unsigned long)seq,
           attempt,
           slave_id,
           esp_err_short_name(err),
           (unsigned)response_len,
           crc_ok ? 1 : 0);
    if (response && response_len > 0) {
        trace_print_hex(response, response_len);
    }
    printf("\n");

    printf("TRACE RS485_DECODE SEQ:%lu ATTEMPT:%u DRIVE:%u ", (unsigned long)seq, attempt, slave_id);
    if (err != ESP_OK) {
        printf("ERROR:%s REQUEST_FUNC:0x%02X REQUEST_REG:0x%04X REG_NAME:%s",
               esp_err_short_name(err),
               request_function,
               request_reg,
               register_name(request_reg));
    } else if (!crc_ok) {
        printf("ERROR:CRC_FAIL REQUEST_FUNC:0x%02X REQUEST_REG:0x%04X REG_NAME:%s",
               request_function,
               request_reg,
               register_name(request_reg));
    } else if (response_len >= 5 && response && (response[1] & 0x80U)) {
        printf("EXCEPTION FUNC:0x%02X CODE:%u REQUEST_REG:0x%04X REG_NAME:%s",
               response[1],
               response[2],
               request_reg,
               register_name(request_reg));
    } else if (response && request_function == SVD48_FUNC_READ_HOLDING && response_len >= 5 && response[1] == SVD48_FUNC_READ_HOLDING) {
        uint8_t byte_count = response[2];
        uint16_t reg_count = byte_count / 2;
        uint16_t regs[16] = {0};
        if ((byte_count % 2) == 0 && reg_count <= 16 && response_len >= (size_t)(5 + byte_count)) {
            for (uint16_t i = 0; i < reg_count; i++) {
                regs[i] = ((uint16_t)response[3 + i * 2] << 8) | response[4 + i * 2];
            }
            printf("READ_OK REG:0x%04X REG_NAME:%s COUNT:%u", request_reg, register_name(request_reg), reg_count);
            for (uint16_t i = 0; i < reg_count; i++) {
                printf(" R%u:0x%04X", i, regs[i]);
            }
            trace_decode_read_values(request_reg, reg_count, regs);
        } else {
            printf("ERROR:BAD_READ_LENGTH BYTE_COUNT:%u", byte_count);
        }
    } else if (response && request_function == SVD48_FUNC_WRITE_SINGLE && response_len == 8 && response[1] == SVD48_FUNC_WRITE_SINGLE) {
        uint16_t value = ((uint16_t)response[4] << 8) | response[5];
        printf("WRITE_ACK REG:0x%04X REG_NAME:%s ", request_reg, register_name(request_reg));
        if (register_value_is_signed(request_reg)) {
            printf("VALUE:%d RAW:0x%04X", (int16_t)value, value);
        } else {
            printf("VALUE:%u RAW:0x%04X", value, value);
        }
    } else if (response && request_function == SVD48_FUNC_WRITE_MULTI) {
        svd48_write_multiple_response_t ack;
        if (svd48_parse_write_multiple_response(response,
                                                response_len,
                                                slave_id,
                                                request_reg,
                                                request_field,
                                                &ack)) {
            printf("WRITE_MULTI_ACK REG:0x%04X REG_NAME:%s COUNT:%u",
                   ack.start_register,
                   register_name(ack.start_register),
                   ack.quantity);
        } else {
            printf("ERROR:BAD_WRITE_MULTI_ACK REQUEST_REG:0x%04X REQUEST_COUNT:%u",
                   request_reg,
                   request_field);
        }
    } else {
        printf("UNDECODED REQUEST_FUNC:0x%02X REQUEST_REG:0x%04X REQUEST_FIELD:%u",
               request_function,
               request_reg,
               request_field);
    }
    printf("\n");
    fflush(stdout);
    xSemaphoreGive(handle->trace_lock);
}

static void trace_bus_lock_timeout(svd48_handle_t handle, uint32_t seq, const uint8_t *request, size_t request_len)
{
    if (seq == 0 || !trace_should_print(handle)) {
        return;
    }

    uint8_t slave_id = request_len >= 1 ? request[0] : 0;
    uint8_t function = request_len >= 2 ? request[1] : 0;
    uint16_t reg = request_len >= 4 ? ((uint16_t)request[2] << 8) | request[3] : 0;

    xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
    printf("TRACE RS485_DECODE SEQ:%lu ATTEMPT:0 DRIVE:%u ERROR:BUS_LOCK_TIMEOUT REQUEST_FUNC:0x%02X REQUEST_REG:0x%04X REG_NAME:%s\n",
           (unsigned long)seq,
           slave_id,
           function,
           reg,
           register_name(reg));
    fflush(stdout);
    xSemaphoreGive(handle->trace_lock);
}

static esp_err_t read_frame(svd48_handle_t handle, uint8_t *buffer, size_t max_length, size_t *out_length)
{
    const int64_t start = esp_timer_get_time() / 1000;
    const uint32_t timeout_ms = handle->config.response_timeout_ms;
    size_t index = 0;
    size_t expected = 3;

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
        int remaining_ms = (int)(timeout_ms - (uint32_t)(esp_timer_get_time() / 1000 - start));
        if (remaining_ms < 1) {
            remaining_ms = 1;
        }

        int read_len = uart_read_bytes(handle->config.uart_port,
                                       buffer + index,
                                       expected - index,
                                       pdMS_TO_TICKS(remaining_ms > 10 ? 10 : remaining_ms));
        if (read_len > 0) {
            index += (size_t)read_len;
        }

        if (index >= 3 && expected == 3) {
            if (buffer[1] == SVD48_FUNC_READ_HOLDING) {
                expected = 5U + buffer[2];
            } else if (buffer[1] == SVD48_FUNC_WRITE_SINGLE || buffer[1] == SVD48_FUNC_WRITE_MULTI) {
                expected = 8;
            } else if (buffer[1] & 0x80U) {
                expected = 5;
            } else {
                expected = 5;
            }

            if (expected > max_length) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        if (index >= expected) {
            *out_length = index;
            return ESP_OK;
        }
    }

    *out_length = index;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_request_and_read_response(svd48_handle_t handle,
                                                const uint8_t *request,
                                                size_t request_len,
                                                uint8_t *response,
                                                size_t response_max,
                                                size_t *response_len)
{
    uart_flush_input(handle->config.uart_port);

    int written = uart_write_bytes(handle->config.uart_port, request, request_len);
    if (written != (int)request_len) {
        return ESP_FAIL;
    }

    esp_err_t err = uart_wait_tx_done(handle->config.uart_port, pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        return err;
    }

    return read_frame(handle, response, response_max, response_len);
}

static esp_err_t transact(svd48_handle_t handle,
                          const uint8_t *request,
                          size_t request_len,
                          uint8_t *response,
                          size_t response_max,
                          size_t *response_len,
                          uint8_t retries)
{
    uint32_t trace_seq = trace_next_seq(handle);
    if (xSemaphoreTake(handle->bus_lock, pdMS_TO_TICKS(SVD48_BUS_LOCK_TIMEOUT_MS)) != pdTRUE) {
        trace_bus_lock_timeout(handle, trace_seq, request, request_len);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t last_err = ESP_FAIL;
    for (uint16_t attempt = 0; attempt <= retries; attempt++) {
        if (response_len) {
            *response_len = 0;
        }
        trace_request(handle,
                      trace_seq,
                      (uint8_t)(attempt + 1U),
                      request,
                      request_len);
        last_err = send_request_and_read_response(handle, request, request_len, response, response_max, response_len);
        bool crc_ok = last_err == ESP_OK && svd48_frame_has_valid_crc(response, *response_len);
        trace_response(handle,
                       trace_seq,
                       (uint8_t)(attempt + 1U),
                       request,
                       request_len,
                       response,
                       response_len ? *response_len : 0,
                       last_err,
                       crc_ok);
        if (last_err == ESP_OK && crc_ok) {
            xSemaphoreGive(handle->bus_lock);
            return ESP_OK;
        }
        if (last_err == ESP_OK) {
            last_err = ESP_ERR_INVALID_CRC;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    xSemaphoreGive(handle->bus_lock);
    return last_err;
}

static void set_drive_error(svd48_handle_t handle, uint8_t drive_index, svd48_status_t error)
{
    if (drive_index >= SVD48_DRIVE_COUNT) {
        return;
    }

    xSemaphoreTake(handle->state_lock, portMAX_DELAY);
    for (uint8_t channel = 0; channel < SVD48_MOTORS_PER_DRIVE; channel++) {
        uint8_t motor = drive_index * SVD48_MOTORS_PER_DRIVE + channel;
        handle->motors[motor].last_error = error;
    }
    xSemaphoreGive(handle->state_lock);
}

static void set_drive_exception(svd48_handle_t handle,
                                uint8_t drive_index,
                                const svd48_exception_response_t *exception)
{
    if (drive_index >= SVD48_DRIVE_COUNT || !exception) {
        return;
    }

    uint32_t timestamp = now_ms();
    xSemaphoreTake(handle->state_lock, portMAX_DELAY);
    for (uint8_t channel = 0; channel < SVD48_MOTORS_PER_DRIVE; channel++) {
        uint8_t motor = drive_index * SVD48_MOTORS_PER_DRIVE + channel;
        handle->motors[motor].last_error = SVD48_ERR_EXCEPTION;
        handle->motors[motor].last_exception_function = exception->function;
        handle->motors[motor].last_exception_code = exception->code;
        handle->motors[motor].last_exception_ms = timestamp;
    }
    xSemaphoreGive(handle->state_lock);
}

static bool record_modbus_exception(svd48_handle_t handle,
                                    uint8_t drive_index,
                                    const uint8_t *response,
                                    size_t response_len,
                                    uint8_t slave_id,
                                    uint8_t request_function,
                                    const char *operation,
                                    uint16_t reg)
{
    svd48_exception_response_t exception;
    if (!svd48_parse_exception_response(response,
                                        response_len,
                                        slave_id,
                                        request_function,
                                        &exception)) {
        return false;
    }

    ESP_LOGW(TAG,
             "Drive %u exception func=0x%02X code=0x%02X %s 0x%04X",
             slave_id,
             exception.function,
             exception.code,
             operation,
             reg);
    set_drive_exception(handle, drive_index, &exception);
    return true;
}

static void update_pair_i16(svd48_handle_t handle, uint8_t drive_index, pair_field_t field, const uint16_t regs[2])
{
    const uint32_t timestamp = now_ms();

    xSemaphoreTake(handle->state_lock, portMAX_DELAY);
    for (uint8_t channel = 0; channel < SVD48_MOTORS_PER_DRIVE; channel++) {
        uint8_t motor = drive_index * SVD48_MOTORS_PER_DRIVE + channel;
        int16_t value = (int16_t)regs[channel];

        switch (field) {
            case PAIR_FIELD_STATUS:
                handle->motors[motor].status = value;
                break;
            case PAIR_FIELD_MOTOR_TEMP:
                handle->motors[motor].motor_temp_decic = value;
                break;
            case PAIR_FIELD_BUS_VOLTAGE:
                handle->motors[motor].bus_voltage_deciv = value;
                break;
            case PAIR_FIELD_MOS_TEMP:
                handle->motors[motor].mos_temp_decic = value;
                break;
            case PAIR_FIELD_SPEED:
                handle->motors[motor].actual_rpm = value;
                break;
            case PAIR_FIELD_CURRENT:
                handle->motors[motor].current_deciamp = value;
                break;
        }

        handle->motors[motor].online = true;
        handle->motors[motor].stale = false;
        handle->motors[motor].last_error = SVD48_OK;
        handle->motors[motor].last_update_ms = timestamp;
    }
    xSemaphoreGive(handle->state_lock);
}

static void update_pair_i32(svd48_handle_t handle, uint8_t drive_index, const uint16_t regs[4], bool is_error_code)
{
    const uint32_t timestamp = now_ms();

    xSemaphoreTake(handle->state_lock, portMAX_DELAY);
    for (uint8_t channel = 0; channel < SVD48_MOTORS_PER_DRIVE; channel++) {
        uint8_t motor = drive_index * SVD48_MOTORS_PER_DRIVE + channel;
        uint32_t raw = ((uint32_t)regs[channel * 2] << 16) | regs[channel * 2 + 1];

        if (is_error_code) {
            handle->motors[motor].error_code = raw;
        } else {
            handle->motors[motor].position_counts = (int32_t)raw;
        }

        handle->motors[motor].online = true;
        handle->motors[motor].stale = false;
        handle->motors[motor].last_error = SVD48_OK;
        handle->motors[motor].last_update_ms = timestamp;
    }
    xSemaphoreGive(handle->state_lock);
}

static bool drive_poll_due(svd48_handle_t handle, uint8_t drive_index, uint32_t timestamp)
{
    return handle->drive_next_poll_ms[drive_index] == 0 ||
           (int32_t)(timestamp - handle->drive_next_poll_ms[drive_index]) >= 0;
}

static void note_drive_poll_success(svd48_handle_t handle, uint8_t drive_index)
{
    handle->drive_fail_count[drive_index] = 0;
    handle->drive_next_poll_ms[drive_index] = 0;
}

static void note_drive_poll_failure(svd48_handle_t handle, uint8_t drive_index)
{
    uint8_t failures = handle->drive_fail_count[drive_index];
    if (failures < 6) {
        failures++;
    }
    handle->drive_fail_count[drive_index] = failures;

    uint8_t shift = failures > 4 ? 3 : failures - 1;
    uint32_t backoff_ms = SVD48_POLL_BACKOFF_BASE_MS << shift;
    if (backoff_ms > SVD48_POLL_BACKOFF_MAX_MS) {
        backoff_ms = SVD48_POLL_BACKOFF_MAX_MS;
    }
    handle->drive_next_poll_ms[drive_index] = now_ms() + backoff_ms;
}

static esp_err_t read_registers(svd48_handle_t handle,
                                uint8_t drive_index,
                                uint16_t reg,
                                uint16_t quantity,
                                uint16_t *out_regs,
                                uint8_t retries)
{
    if (!handle || drive_index >= SVD48_DRIVE_COUNT || quantity == 0 || quantity > 16 || !out_regs) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[8];
    uint8_t response[64];
    size_t response_len = 0;
    uint8_t slave_id = handle->config.drive_ids[drive_index];
    size_t request_len = svd48_build_read_request(slave_id,
                                                  reg,
                                                  quantity,
                                                  request);
    if (request_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = transact(handle,
                             request,
                             request_len,
                             response,
                             sizeof(response),
                             &response_len,
                             retries);
    if (err != ESP_OK) {
        set_drive_error(handle, drive_index, esp_to_svd48_status(err));
        return err;
    }

    if (record_modbus_exception(handle,
                                drive_index,
                                response,
                                response_len,
                                slave_id,
                                SVD48_FUNC_READ_HOLDING,
                                "reading",
                                reg)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response_len != 5U + (size_t)quantity * 2U || response[0] != slave_id ||
        response[1] != SVD48_FUNC_READ_HOLDING || response[2] != quantity * 2U) {
        set_drive_error(handle, drive_index, SVD48_ERR_BAD_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint16_t i = 0; i < quantity; i++) {
        out_regs[i] = ((uint16_t)response[3 + i * 2] << 8) | response[4 + i * 2];
    }

    return ESP_OK;
}

static esp_err_t write_register(svd48_handle_t handle, uint8_t drive_index, uint16_t reg, uint16_t value)
{
    if (!handle || drive_index >= SVD48_DRIVE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[8];
    uint8_t response[16];
    size_t response_len = 0;
    uint8_t slave_id = handle->config.drive_ids[drive_index];
    svd48_build_write_single_request(slave_id, reg, value, request);

    esp_err_t err = transact(handle, request, sizeof(request), response, sizeof(response), &response_len, handle->config.retries);
    if (err != ESP_OK) {
        set_drive_error(handle, drive_index, esp_to_svd48_status(err));
        return err;
    }

    if (record_modbus_exception(handle,
                                drive_index,
                                response,
                                response_len,
                                slave_id,
                                SVD48_FUNC_WRITE_SINGLE,
                                "writing",
                                reg)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response_len != 8 || memcmp(request, response, 8) != 0) {
        set_drive_error(handle, drive_index, SVD48_ERR_BAD_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t write_registers(svd48_handle_t handle,
                                 uint8_t drive_index,
                                 uint16_t start_reg,
                                 const uint16_t *values,
                                 uint16_t quantity)
{
    if (!handle || drive_index >= SVD48_DRIVE_COUNT || !values ||
        !svd48_write_multiple_range_is_valid(start_reg, quantity)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE];
    uint8_t response[SVD48_WRITE_MULTIPLE_RESPONSE_SIZE];
    uint8_t slave_id = handle->config.drive_ids[drive_index];
    size_t request_len = svd48_build_write_multiple_request(slave_id,
                                                           start_reg,
                                                           values,
                                                           quantity,
                                                           request,
                                                           sizeof(request));
    if (request_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t response_len = 0;
    // A missing ACK makes a configuration write ambiguous. Readback must classify it;
    // retransmitting here could apply a non-idempotent register twice.
    esp_err_t err = transact(handle,
                             request,
                             request_len,
                             response,
                             sizeof(response),
                             &response_len,
                             0);
    if (err != ESP_OK) {
        set_drive_error(handle, drive_index, esp_to_svd48_status(err));
        return err;
    }

    if (record_modbus_exception(handle,
                                drive_index,
                                response,
                                response_len,
                                slave_id,
                                SVD48_FUNC_WRITE_MULTI,
                                "writing multiple from",
                                start_reg)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    svd48_write_multiple_response_t ack;
    if (!svd48_parse_write_multiple_response(response,
                                             response_len,
                                             slave_id,
                                             start_reg,
                                             quantity,
                                             &ack)) {
        set_drive_error(handle, drive_index, SVD48_ERR_BAD_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static void poll_task(void *arg)
{
    svd48_handle_t handle = (svd48_handle_t)arg;
    while (handle->polling) {
        (void)svd48_poll_once(handle);
        vTaskDelay(pdMS_TO_TICKS(handle->config.telemetry_period_ms));
    }
    vTaskDelete(NULL);
}

static void attached_trace(void *context,
                           uint16_t device_id,
                           uint8_t address,
                           uint8_t attempt,
                           const uint8_t *request,
                           size_t request_length,
                           const uint8_t *response,
                           size_t response_length,
                           svd48_device_result_t result)
{
    svd48_handle_t handle = context;
    if (!handle || !handle->trace_enabled || !handle->trace_lock) {
        return;
    }
    xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
    printf("TRACE RS485_TX DEVICE_ID:%u DRIVE:%u ATTEMPT:%u HEX:",
           device_id,
           address,
           attempt);
    trace_print_hex(request, request_length);
    printf("\nTRACE RS485_RX DEVICE_ID:%u DRIVE:%u ATTEMPT:%u RESULT:%u LEN:%u HEX:",
           device_id,
           address,
           attempt,
           (unsigned)result,
           (unsigned)response_length);
    trace_print_hex(response, response_length);
    printf("\n");
    fflush(stdout);
    xSemaphoreGive(handle->trace_lock);
}

svd48_handle_t svd48_attach_devices(
    svd48_device_t *const *devices,
    size_t device_count,
    const svd48_legacy_channel_binding_t *bindings,
    size_t binding_count)
{
    if (!devices || device_count == 0U || device_count > SVD48_LEGACY_DEVICE_MAX ||
        !bindings || binding_count == 0U || binding_count > SVD48_MOTOR_COUNT) {
        return NULL;
    }
    svd48_handle_t handle = calloc(1, sizeof(struct svd48_t));
    if (!handle) {
        return NULL;
    }
    handle->attached_devices = true;
    handle->attached_device_count = device_count;
    handle->attached_binding_count = binding_count;
    for (size_t index = 0; index < device_count; ++index) {
        if (!devices[index] || !devices[index]->initialized) {
            free(handle);
            return NULL;
        }
        handle->attached_device_items[index] = devices[index];
    }
    for (size_t index = 0; index < binding_count; ++index) {
        bool device_is_attached = false;
        for (size_t device_index = 0; device_index < device_count; ++device_index) {
            if (devices[device_index] == bindings[index].device) {
                device_is_attached = true;
                break;
            }
        }
        if (!bindings[index].device ||
            bindings[index].channel >= SVD48_DEVICE_CHANNEL_COUNT ||
            !device_is_attached) {
            free(handle);
            return NULL;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].device == bindings[index].device &&
                bindings[previous].channel == bindings[index].channel) {
                free(handle);
                return NULL;
            }
        }
        handle->attached_bindings[index] = bindings[index];
    }
    handle->trace_lock = xSemaphoreCreateMutex();
    if (!handle->trace_lock) {
        free(handle);
        return NULL;
    }
    return handle;
}

svd48_handle_t svd48_init(const svd48_config_t *config)
{
    if (!config || config->retries > SVD48_MAX_RETRIES ||
        config->drive_ids[0] > SVD48_MODBUS_MAX_SLAVE_ID ||
        config->drive_ids[1] > SVD48_MODBUS_MAX_SLAVE_ID) {
        return NULL;
    }

    svd48_handle_t handle = calloc(1, sizeof(struct svd48_t));
    if (!handle) {
        return NULL;
    }

    handle->config = *config;
    if (handle->config.baud_rate == 0) {
        handle->config.baud_rate = 115200;
    }
    if (handle->config.response_timeout_ms == 0) {
        handle->config.response_timeout_ms = SVD48_DEFAULT_TIMEOUT_MS;
    }
    if (handle->config.retries == 0) {
        handle->config.retries = SVD48_DEFAULT_RETRIES;
    }
    if (handle->config.telemetry_period_ms == 0) {
        handle->config.telemetry_period_ms = SVD48_DEFAULT_TELEMETRY_PERIOD_MS;
    }
    if (handle->config.stale_timeout_ms == 0) {
        handle->config.stale_timeout_ms = SVD48_DEFAULT_STALE_TIMEOUT_MS;
    }
    if (handle->config.drive_ids[0] == 0) {
        handle->config.drive_ids[0] = 1;
    }
    if (handle->config.drive_ids[1] == 0) {
        handle->config.drive_ids[1] = 2;
    }

    handle->bus_lock = xSemaphoreCreateMutex();
    handle->state_lock = xSemaphoreCreateMutex();
    handle->trace_lock = xSemaphoreCreateMutex();
    if (!handle->bus_lock || !handle->state_lock || !handle->trace_lock) {
        svd48_deinit(handle);
        return NULL;
    }

    uart_config_t uart_config = {
        .baud_rate = (int)handle->config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(handle->config.uart_port,
                                        SVD48_UART_RX_BUFFER_SIZE,
                                        SVD48_UART_TX_BUFFER_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        svd48_deinit(handle);
        return NULL;
    }

    err = uart_param_config(handle->config.uart_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        svd48_deinit(handle);
        return NULL;
    }

    err = uart_set_pin(handle->config.uart_port,
                       handle->config.tx_pin,
                       handle->config.rx_pin,
                       handle->config.rts_pin,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        svd48_deinit(handle);
        return NULL;
    }

    if (handle->config.use_rs485_half_duplex) {
        err = uart_set_mode(handle->config.uart_port, UART_MODE_RS485_HALF_DUPLEX);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_mode RS485 failed: %s", esp_err_to_name(err));
            svd48_deinit(handle);
            return NULL;
        }
    }

    for (uint8_t i = 0; i < SVD48_MOTOR_COUNT; i++) {
        uint8_t drive_index = i / SVD48_MOTORS_PER_DRIVE;
        handle->motors[i].logical_motor = i;
        handle->motors[i].drive_index = drive_index;
        handle->motors[i].drive_id = handle->config.drive_ids[drive_index];
        handle->motors[i].channel = i % SVD48_MOTORS_PER_DRIVE;
        handle->motors[i].last_error = SVD48_ERR_TIMEOUT;
        handle->motors[i].stale = true;
    }

    ESP_LOGI(TAG, "SVD48 bus ready on UART%d, drives=%u/%u",
             handle->config.uart_port,
             handle->config.drive_ids[0],
             handle->config.drive_ids[1]);
    return handle;
}

void svd48_deinit(svd48_handle_t handle)
{
    if (!handle) {
        return;
    }

    if (handle->attached_devices) {
        for (size_t index = 0; index < handle->attached_device_count; ++index) {
            svd48_device_set_trace(handle->attached_device_items[index],
                                   false,
                                   NULL,
                                   NULL);
        }
        if (handle->trace_lock) {
            vSemaphoreDelete(handle->trace_lock);
        }
        free(handle);
        return;
    }

    handle->polling = false;
    if (handle->poll_task) {
        vTaskDelay(pdMS_TO_TICKS(handle->config.telemetry_period_ms + 10));
    }

    uart_driver_delete(handle->config.uart_port);

    if (handle->bus_lock) {
        vSemaphoreDelete(handle->bus_lock);
    }
    if (handle->state_lock) {
        vSemaphoreDelete(handle->state_lock);
    }
    if (handle->trace_lock) {
        vSemaphoreDelete(handle->trace_lock);
    }
    free(handle);
}

esp_err_t svd48_start_polling(svd48_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->attached_devices) {
        /* The profile composition owns the shared N-device polling service. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (handle->polling) {
        return ESP_OK;
    }

    handle->polling = true;
    BaseType_t ok = xTaskCreate(poll_task, "svd48_poll", 4096, handle, 8, &handle->poll_task);
    if (ok != pdPASS) {
        handle->polling = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void svd48_set_trace_enabled(svd48_handle_t handle, bool enabled)
{
    if (!handle) {
        return;
    }
    if (handle->attached_devices) {
        handle->trace_enabled = enabled;
        for (size_t index = 0; index < handle->attached_device_count; ++index) {
            svd48_device_set_trace(handle->attached_device_items[index],
                                   enabled,
                                   enabled ? attached_trace : NULL,
                                   enabled ? handle : NULL);
        }
        xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
        printf("TRACE RS485_CONFIG ENABLED:%u CRC_INIT:0xFFFF CRC_POLY:0xA001 CRC_ORDER:HIGH_LOW DEVICES:%u\n",
               enabled ? 1U : 0U,
               (unsigned)handle->attached_device_count);
        fflush(stdout);
        xSemaphoreGive(handle->trace_lock);
        return;
    }
    handle->trace_enabled = enabled;
    if (enabled) {
        trace_printf_line(handle,
                          "TRACE RS485_CONFIG ENABLED:1 CRC_INIT:0xFFFF CRC_POLY:0xA001 CRC_ORDER:HIGH_LOW UART:%d BAUD:%lu TIMEOUT_MS:%lu RETRIES:%u BACKGROUND_POLL_TRACE:%u",
                          handle->config.uart_port,
                          (unsigned long)handle->config.baud_rate,
                          (unsigned long)handle->config.response_timeout_ms,
                          handle->config.retries,
                          handle->trace_polling_enabled ? 1 : 0);
    } else {
        xSemaphoreTake(handle->trace_lock, portMAX_DELAY);
        printf("TRACE RS485_CONFIG ENABLED:0\n");
        fflush(stdout);
        xSemaphoreGive(handle->trace_lock);
    }
}

bool svd48_get_trace_enabled(svd48_handle_t handle)
{
    return handle ? handle->trace_enabled : false;
}

esp_err_t svd48_poll_once(svd48_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->attached_devices) {
        esp_err_t first_error = ESP_OK;
        for (size_t index = 0; index < handle->attached_device_count; ++index) {
            esp_err_t error = device_result_to_esp(
                svd48_device_poll(handle->attached_device_items[index]));
            if (error != ESP_OK && first_error == ESP_OK) {
                first_error = error;
            }
        }
        return first_error;
    }

    uint16_t regs2[2];
    uint16_t regs4[4];
    bool slow_poll = (handle->poll_count++ % SVD48_POLL_SLOW_DIVIDER) == 0;
    uint32_t timestamp = now_ms();

    for (uint8_t drive = 0; drive < SVD48_DRIVE_COUNT; drive++) {
        if (!drive_poll_due(handle, drive, timestamp)) {
            continue;
        }

        if (read_registers(handle, drive, REG_M1_POSITION, 4, regs4, 0) == ESP_OK) {
            update_pair_i32(handle, drive, regs4, false);
            note_drive_poll_success(handle, drive);
        } else {
            note_drive_poll_failure(handle, drive);
            continue;
        }

        if (read_registers(handle, drive, REG_M1_ACTUAL_SPEED, 2, regs2, 0) == ESP_OK) {
            update_pair_i16(handle, drive, PAIR_FIELD_SPEED, regs2);
        }
        if (read_registers(handle, drive, REG_M1_ACTUAL_CURRENT, 2, regs2, 0) == ESP_OK) {
            update_pair_i16(handle, drive, PAIR_FIELD_CURRENT, regs2);
        }

        if (slow_poll) {
            if (read_registers(handle, drive, REG_M1_STATUS, 2, regs2, 0) == ESP_OK) {
                update_pair_i16(handle, drive, PAIR_FIELD_STATUS, regs2);
            }
            if (read_registers(handle, drive, REG_M1_MOTOR_TEMP, 2, regs2, 0) == ESP_OK) {
                update_pair_i16(handle, drive, PAIR_FIELD_MOTOR_TEMP, regs2);
            }
            if (read_registers(handle, drive, REG_M1_BUS_VOLTAGE, 2, regs2, 0) == ESP_OK) {
                update_pair_i16(handle, drive, PAIR_FIELD_BUS_VOLTAGE, regs2);
            }
            if (read_registers(handle, drive, REG_M1_MOS_TEMP, 2, regs2, 0) == ESP_OK) {
                update_pair_i16(handle, drive, PAIR_FIELD_MOS_TEMP, regs2);
            }
            if (read_registers(handle, drive, REG_M1_ERROR_CODE, 4, regs4, 0) == ESP_OK) {
                update_pair_i32(handle, drive, regs4, true);
            }
        }
    }

    return ESP_OK;
}

bool svd48_resolve_motor(svd48_handle_t handle, uint8_t logical_motor, uint8_t *drive_id, uint8_t *channel)
{
    if (validate_motor(handle, logical_motor) != ESP_OK) {
        return false;
    }

    if (handle->attached_devices) {
        const svd48_legacy_channel_binding_t *binding =
            &handle->attached_bindings[logical_motor];
        if (drive_id) {
            *drive_id = svd48_device_address(binding->device);
        }
        if (channel) {
            *channel = (uint8_t)binding->channel;
        }
        return true;
    }

    uint8_t drive_index = logical_motor / SVD48_MOTORS_PER_DRIVE;
    if (drive_id) {
        *drive_id = handle->config.drive_ids[drive_index];
    }
    if (channel) {
        *channel = logical_motor % SVD48_MOTORS_PER_DRIVE;
    }
    return true;
}

esp_err_t svd48_read_registers_by_id(svd48_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t quantity, uint16_t *out_regs)
{
    if (handle && handle->attached_devices) {
        svd48_device_t *device = attached_device_by_address(handle, drive_id);
        return device ? device_result_to_esp(
                            svd48_device_read_registers(device, reg, quantity, out_regs))
                      : ESP_ERR_INVALID_ARG;
    }
    uint8_t drive_index = 0;
    if (!drive_index_from_id(handle, drive_id, &drive_index)) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_registers(handle, drive_index, reg, quantity, out_regs, handle->config.retries);
}

esp_err_t svd48_probe_address(svd48_handle_t handle,
                              uint8_t address,
                              uint16_t reg,
                              uint16_t quantity,
                              uint16_t *out_regs)
{
    if (!handle || !handle->attached_devices ||
        handle->attached_device_count == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return device_result_to_esp(svd48_device_probe_address(
        handle->attached_device_items[0], address, reg, quantity, out_regs));
}

esp_err_t svd48_write_register_by_id(svd48_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t value)
{
    if (handle && handle->attached_devices) {
        svd48_device_t *device = attached_device_by_address(handle, drive_id);
        return device ? device_result_to_esp(
                            svd48_device_write_register(device, reg, value))
                      : ESP_ERR_INVALID_ARG;
    }
    uint8_t drive_index = 0;
    if (!drive_index_from_id(handle, drive_id, &drive_index)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (svd48_register_is_runtime_actuation(reg)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return write_register(handle, drive_index, reg, value);
}

esp_err_t svd48_write_registers_by_id(svd48_handle_t handle,
                                      uint8_t drive_id,
                                      uint16_t start_reg,
                                      const uint16_t *values,
                                      uint16_t quantity)
{
    if (handle && handle->attached_devices) {
        svd48_device_t *device = attached_device_by_address(handle, drive_id);
        return device ? device_result_to_esp(svd48_device_write_registers(
                            device, start_reg, values, quantity))
                      : ESP_ERR_INVALID_ARG;
    }
    uint8_t drive_index = 0;
    if (!values || !svd48_write_multiple_range_is_valid(start_reg, quantity) ||
        !drive_index_from_id(handle, drive_id, &drive_index)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (svd48_register_range_has_runtime_actuation(start_reg, quantity)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return write_registers(handle, drive_index, start_reg, values, quantity);
}

esp_err_t svd48_set_motor_command(svd48_handle_t handle, uint8_t logical_motor, svd48_motor_command_t command)
{
    ESP_RETURN_ON_ERROR(validate_motor(handle, logical_motor), TAG, "invalid motor");
    if (handle->attached_devices) {
        svd48_channel_t *channel = attached_channel(handle, logical_motor);
        switch (command) {
        case SVD48_MOTOR_CMD_STOP:
            return device_result_to_esp(svd48_channel_stop(channel));
        case SVD48_MOTOR_CMD_START:
            return device_result_to_esp(svd48_channel_enable(channel));
        case SVD48_MOTOR_CMD_CLEAR_ALARM:
            return device_result_to_esp(svd48_channel_clear_fault(channel));
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }
    uint8_t drive_index = logical_motor / SVD48_MOTORS_PER_DRIVE;
    uint8_t channel = logical_motor % SVD48_MOTORS_PER_DRIVE;
    uint16_t reg = channel_register(channel, REG_M1_CONTROL_CMD, REG_M2_CONTROL_CMD);
    return write_register(handle, drive_index, reg, (uint16_t)command);
}

esp_err_t svd48_set_motor_speed(svd48_handle_t handle, uint8_t logical_motor, int16_t rpm)
{
    ESP_RETURN_ON_ERROR(validate_motor(handle, logical_motor), TAG, "invalid motor");
    if (handle->attached_devices) {
        return device_result_to_esp(
            svd48_channel_set_target_rpm(attached_channel(handle, logical_motor), rpm));
    }
    uint8_t drive_index = logical_motor / SVD48_MOTORS_PER_DRIVE;
    uint8_t channel = logical_motor % SVD48_MOTORS_PER_DRIVE;
    uint16_t reg = channel_register(channel, REG_M1_GIVEN_SPEED, REG_M2_GIVEN_SPEED);
    return write_register(handle, drive_index, reg, (uint16_t)rpm);
}

esp_err_t svd48_set_motor_current(svd48_handle_t handle, uint8_t logical_motor, int16_t deciamp)
{
    ESP_RETURN_ON_ERROR(validate_motor(handle, logical_motor), TAG, "invalid motor");
    if (handle->attached_devices) {
        return device_result_to_esp(svd48_channel_set_current_deciamp(
            attached_channel(handle, logical_motor), deciamp));
    }
    uint8_t drive_index = logical_motor / SVD48_MOTORS_PER_DRIVE;
    uint8_t channel = logical_motor % SVD48_MOTORS_PER_DRIVE;
    uint16_t reg = channel_register(channel, REG_M1_GIVEN_CURRENT, REG_M2_GIVEN_CURRENT);
    return write_register(handle, drive_index, reg, (uint16_t)deciamp);
}

esp_err_t svd48_enable_motor(svd48_handle_t handle, uint8_t logical_motor)
{
    if (handle && handle->attached_devices) {
        return validate_motor(handle, logical_motor) == ESP_OK
                   ? device_result_to_esp(svd48_channel_enable(
                         attached_channel(handle, logical_motor)))
                   : ESP_ERR_INVALID_ARG;
    }
    return svd48_set_motor_command(handle, logical_motor, SVD48_MOTOR_CMD_START);
}

esp_err_t svd48_stop_motor(svd48_handle_t handle, uint8_t logical_motor)
{
    if (handle && handle->attached_devices) {
        return validate_motor(handle, logical_motor) == ESP_OK
                   ? device_result_to_esp(svd48_channel_stop(
                         attached_channel(handle, logical_motor)))
                   : ESP_ERR_INVALID_ARG;
    }
    esp_err_t speed_err = svd48_set_motor_speed(handle, logical_motor, 0);
    esp_err_t stop_err = svd48_set_motor_command(handle,
                                                 logical_motor,
                                                 SVD48_MOTOR_CMD_STOP);
    return speed_err != ESP_OK ? speed_err : stop_err;
}

esp_err_t svd48_clear_motor_alarm(svd48_handle_t handle, uint8_t logical_motor)
{
    if (handle && handle->attached_devices) {
        return validate_motor(handle, logical_motor) == ESP_OK
                   ? device_result_to_esp(svd48_channel_clear_fault(
                         attached_channel(handle, logical_motor)))
                   : ESP_ERR_INVALID_ARG;
    }
    return svd48_set_motor_command(handle, logical_motor, SVD48_MOTOR_CMD_CLEAR_ALARM);
}

esp_err_t svd48_stop_all(svd48_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_error = ESP_OK;
    size_t motor_count = svd48_get_motor_count(handle);
    for (uint8_t motor = 0; motor < motor_count; motor++) {
        esp_err_t err = svd48_stop_motor(handle, motor);
        if (err != ESP_OK && first_error == ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}

bool svd48_get_motor_telemetry(svd48_handle_t handle, uint8_t logical_motor, svd48_motor_telemetry_t *telemetry)
{
    if (validate_motor(handle, logical_motor) != ESP_OK || !telemetry) {
        return false;
    }

    if (handle->attached_devices) {
        svd48_channel_snapshot_t snapshot;
        const svd48_legacy_channel_binding_t *binding =
            &handle->attached_bindings[logical_motor];
        if (!svd48_channel_get_snapshot(attached_channel(handle, logical_motor),
                                        &snapshot)) {
            return false;
        }
        memset(telemetry, 0, sizeof(*telemetry));
        telemetry->logical_motor = logical_motor;
        telemetry->drive_id = svd48_device_address(binding->device);
        telemetry->drive_index = 0U;
        for (size_t index = 0; index < handle->attached_device_count; ++index) {
            if (handle->attached_device_items[index] == binding->device) {
                telemetry->drive_index = (uint8_t)index;
                break;
            }
        }
        telemetry->channel = (uint8_t)binding->channel;
        telemetry->online = snapshot.online;
        telemetry->stale = snapshot.stale;
        telemetry->last_error = device_result_to_status(snapshot.last_error);
        telemetry->last_update_ms = snapshot.last_update_ms;
        telemetry->last_exception_function = snapshot.last_exception_function;
        telemetry->last_exception_code = snapshot.last_exception_code;
        telemetry->last_exception_ms = snapshot.last_exception_ms;
        telemetry->status = snapshot.status;
        telemetry->actual_rpm = snapshot.observed_speed_rpm;
        telemetry->current_deciamp = snapshot.current_deciamp;
        telemetry->motor_temp_decic = snapshot.motor_temp_decic;
        telemetry->bus_voltage_deciv = snapshot.bus_voltage_deciv;
        telemetry->mos_temp_decic = snapshot.mos_temp_decic;
        telemetry->position_counts = snapshot.position_counts;
        telemetry->error_code = snapshot.error_code;
        return true;
    }

    xSemaphoreTake(handle->state_lock, portMAX_DELAY);
    *telemetry = handle->motors[logical_motor];
    uint32_t age = now_ms() - telemetry->last_update_ms;
    telemetry->stale = telemetry->last_update_ms == 0 || age > handle->config.stale_timeout_ms;
    if (telemetry->stale) {
        telemetry->online = false;
    }
    handle->motors[logical_motor].stale = telemetry->stale;
    handle->motors[logical_motor].online = telemetry->online;
    xSemaphoreGive(handle->state_lock);
    return true;
}

size_t svd48_get_motor_count(svd48_handle_t handle)
{
    if (!handle) {
        return 0U;
    }
    return handle->attached_devices ? handle->attached_binding_count
                                    : SVD48_MOTOR_COUNT;
}
