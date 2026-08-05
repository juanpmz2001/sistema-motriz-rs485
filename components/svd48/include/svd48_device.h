#ifndef SVD48_DEVICE_H
#define SVD48_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus_transport.h"
#include "svd48_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_DEVICE_CHANNEL_COUNT 2U

typedef enum {
    SVD48_CHANNEL_M1 = 0,
    SVD48_CHANNEL_M2 = 1,
} svd48_channel_id_t;

typedef enum {
    SVD48_DEVICE_OK = 0,
    SVD48_DEVICE_INVALID_ARGUMENT,
    SVD48_DEVICE_TIMEOUT,
    SVD48_DEVICE_BUS_BUSY,
    SVD48_DEVICE_IO_ERROR,
    SVD48_DEVICE_INCOMPLETE_FRAME,
    SVD48_DEVICE_CANCELLED,
    SVD48_DEVICE_CRC_ERROR,
    SVD48_DEVICE_EXCEPTION,
    SVD48_DEVICE_BAD_RESPONSE,
    SVD48_DEVICE_UNSUPPORTED,
} svd48_device_result_t;

typedef enum {
    SVD48_CHANNEL_HEALTH_UNKNOWN = 0,
    SVD48_CHANNEL_HEALTH_HEALTHY,
    SVD48_CHANNEL_HEALTH_DEGRADED,
    SVD48_CHANNEL_HEALTH_OFFLINE,
    SVD48_CHANNEL_HEALTH_FAULT,
} svd48_channel_health_t;

typedef struct {
    bool online;
    bool stale;
    svd48_device_result_t last_error;
    uint32_t last_update_ms;
    uint8_t last_exception_function;
    uint8_t last_exception_code;
    uint32_t last_exception_ms;
    int16_t status;
    /* Observed register 0x5410/0x5411; evidence indicates 0.1 RPM units. */
    int16_t observed_speed_decirpm;
    int16_t current_deciamp;
    int16_t motor_temp_decic;
    int16_t bus_voltage_deciv;
    int16_t mos_temp_decic;
    int32_t position_counts;
    uint32_t error_code;
} svd48_channel_snapshot_t;

typedef struct {
    uint32_t transactions;
    uint32_t successful_transactions;
    uint32_t failed_transactions;
    uint32_t consecutive_failures;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;
    svd48_device_result_t last_error;
    uint8_t last_exception_function;
    uint8_t last_exception_code;
} svd48_device_communication_t;

typedef struct {
    bool (*acquire)(void *context);
    void (*release)(void *context);
    void *context;
} svd48_device_lock_t;

typedef uint32_t (*svd48_device_clock_ms_fn)(void *context);

typedef void (*svd48_device_trace_fn)(void *context,
                                     uint16_t device_id,
                                     uint8_t address,
                                     uint8_t attempt,
                                     const uint8_t *request,
                                     size_t request_length,
                                     const uint8_t *response,
                                     size_t response_length,
                                     svd48_device_result_t result);

typedef struct {
    uint16_t device_id;
    uint8_t address;
    bus_transport_t *transport;
    uint32_t response_timeout_ms;
    uint8_t retries;
    uint32_t stale_timeout_ms;
    svd48_device_lock_t state_lock;
    svd48_device_clock_ms_fn clock_ms;
    void *clock_context;
} svd48_device_config_t;

typedef struct svd48_device svd48_device_t;

typedef struct {
    svd48_device_t *device;
    svd48_channel_id_t id;
} svd48_channel_t;

struct svd48_device {
    svd48_device_config_t config;
    svd48_channel_t channels[SVD48_DEVICE_CHANNEL_COUNT];
    svd48_channel_snapshot_t snapshots[SVD48_DEVICE_CHANNEL_COUNT];
    svd48_device_communication_t communication;
    uint32_t poll_count;
    bool initialized;
    bool trace_enabled;
    svd48_device_trace_fn trace;
    void *trace_context;
};

bool svd48_device_init(svd48_device_t *device,
                       const svd48_device_config_t *config);
void svd48_device_deinit(svd48_device_t *device);
uint16_t svd48_device_id(const svd48_device_t *device);
uint8_t svd48_device_address(const svd48_device_t *device);
bus_transport_t *svd48_device_transport(const svd48_device_t *device);
svd48_channel_t *svd48_device_channel(svd48_device_t *device,
                                      svd48_channel_id_t channel);

uint16_t svd48_channel_control_register(svd48_channel_id_t channel);
uint16_t svd48_channel_velocity_register(svd48_channel_id_t channel);
uint16_t svd48_channel_current_register(svd48_channel_id_t channel);

svd48_device_result_t svd48_channel_set_target_rpm(svd48_channel_t *channel,
                                                   int16_t target_rpm);
svd48_device_result_t svd48_channel_enable(svd48_channel_t *channel);
svd48_device_result_t svd48_channel_stop(svd48_channel_t *channel);
svd48_device_result_t svd48_channel_clear_fault(svd48_channel_t *channel);
svd48_device_result_t svd48_channel_set_current_deciamp(svd48_channel_t *channel,
                                                       int16_t deciamp);

bool svd48_channel_get_snapshot(svd48_channel_t *channel,
                                svd48_channel_snapshot_t *snapshot);
svd48_channel_health_t svd48_channel_get_health(svd48_channel_t *channel);

svd48_device_result_t svd48_device_poll(svd48_device_t *device);
svd48_device_result_t svd48_device_read_registers(svd48_device_t *device,
                                                  uint16_t reg,
                                                  uint16_t quantity,
                                                  uint16_t *out_regs);
svd48_device_result_t svd48_device_write_register(svd48_device_t *device,
                                                  uint16_t reg,
                                                  uint16_t value);
svd48_device_result_t svd48_device_write_registers(svd48_device_t *device,
                                                   uint16_t start_reg,
                                                   const uint16_t *values,
                                                   uint16_t quantity);
bool svd48_device_get_communication(svd48_device_t *device,
                                    svd48_device_communication_t *communication);
void svd48_device_set_trace(svd48_device_t *device,
                            bool enabled,
                            svd48_device_trace_fn trace,
                            void *trace_context);

#ifdef __cplusplus
}
#endif

#endif
