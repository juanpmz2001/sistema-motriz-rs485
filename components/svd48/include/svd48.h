#ifndef SVD48_H
#define SVD48_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "svd48_device.h"
#include "svd48_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_DRIVE_COUNT 2
#define SVD48_MOTORS_PER_DRIVE 2
#define SVD48_MOTOR_COUNT (SVD48_DRIVE_COUNT * SVD48_MOTORS_PER_DRIVE)

typedef struct svd48_t *svd48_handle_t;

/* Legacy-index compatibility view. Indices never enter svd48_device. */
typedef struct {
    svd48_device_t *device;
    svd48_channel_id_t channel;
} svd48_legacy_channel_binding_t;

typedef enum {
    SVD48_MOTOR_CMD_STOP = 0,
    SVD48_MOTOR_CMD_START = 1,
    SVD48_MOTOR_CMD_CLEAR_ALARM = 2,
} svd48_motor_command_t;

typedef enum {
    SVD48_OK = 0,
    SVD48_ERR_INVALID_ARG = 1,
    SVD48_ERR_TIMEOUT = 2,
    SVD48_ERR_CRC = 3,
    SVD48_ERR_EXCEPTION = 4,
    SVD48_ERR_BAD_RESPONSE = 5,
    SVD48_ERR_UART = 6,
} svd48_status_t;

typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    int rts_pin;                 // Use UART_PIN_NO_CHANGE for auto-direction RS485 adapters.
    bool use_rs485_half_duplex;  // True when rts_pin controls DE/RE.
    uint32_t baud_rate;
    uint8_t drive_ids[SVD48_DRIVE_COUNT];
    uint32_t response_timeout_ms;
    uint8_t retries;
    uint32_t telemetry_period_ms;
    uint32_t stale_timeout_ms;
} svd48_config_t;

typedef struct {
    uint8_t logical_motor;
    uint8_t drive_id;
    uint8_t drive_index;
    uint8_t channel; // 0=M1, 1=M2
    bool online;
    bool stale;
    svd48_status_t last_error;
    uint32_t last_update_ms;
    uint8_t last_exception_function;
    uint8_t last_exception_code;
    uint32_t last_exception_ms;

    int16_t status;              // 0=stop, 1=running
    /* Compatibility field: raw vendor register value, interpreted as RPM. */
    int16_t actual_rpm;
    int16_t current_deciamp;     // 0.1 A
    int16_t motor_temp_decic;    // 0.1 C
    int16_t bus_voltage_deciv;   // 0.1 V
    int16_t mos_temp_decic;      // 0.1 C
    int32_t position_counts;
    uint32_t error_code;
} svd48_motor_telemetry_t;

svd48_handle_t svd48_init(const svd48_config_t *config);
svd48_handle_t svd48_attach_devices(
    svd48_device_t *const *devices,
    size_t device_count,
    const svd48_legacy_channel_binding_t *bindings,
    size_t binding_count);
void svd48_deinit(svd48_handle_t handle);
size_t svd48_get_motor_count(svd48_handle_t handle);

esp_err_t svd48_start_polling(svd48_handle_t handle);
esp_err_t svd48_poll_once(svd48_handle_t handle);

void svd48_set_trace_enabled(svd48_handle_t handle, bool enabled);
bool svd48_get_trace_enabled(svd48_handle_t handle);

esp_err_t svd48_set_motor_command(svd48_handle_t handle, uint8_t logical_motor, svd48_motor_command_t command);
esp_err_t svd48_set_motor_speed(svd48_handle_t handle, uint8_t logical_motor, int16_t rpm);
esp_err_t svd48_set_motor_current(svd48_handle_t handle, uint8_t logical_motor, int16_t deciamp);
esp_err_t svd48_enable_motor(svd48_handle_t handle, uint8_t logical_motor);
esp_err_t svd48_stop_motor(svd48_handle_t handle, uint8_t logical_motor);
esp_err_t svd48_clear_motor_alarm(svd48_handle_t handle, uint8_t logical_motor);
esp_err_t svd48_stop_all(svd48_handle_t handle);

bool svd48_get_motor_telemetry(svd48_handle_t handle, uint8_t logical_motor, svd48_motor_telemetry_t *telemetry);
bool svd48_resolve_motor(svd48_handle_t handle, uint8_t logical_motor, uint8_t *drive_id, uint8_t *channel);
esp_err_t svd48_read_registers_by_id(svd48_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t quantity, uint16_t *out_regs);
esp_err_t svd48_probe_address(svd48_handle_t handle,
                              uint8_t address,
                              uint16_t reg,
                              uint16_t quantity,
                              uint16_t *out_regs);
esp_err_t svd48_write_register_by_id(svd48_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t value);
esp_err_t svd48_write_registers_by_id(svd48_handle_t handle,
                                      uint8_t drive_id,
                                      uint16_t start_reg,
                                      const uint16_t *values,
                                      uint16_t quantity);

#ifdef __cplusplus
}
#endif

#endif // SVD48_H
