#ifndef SVD48_WORKSPACE_PORT_H
#define SVD48_WORKSPACE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_WORKSPACE_CHANNEL_COUNT 2U
/* Bounded, read-only projection of raw frames emitted by one typed Hall
 * operation. It is deliberately not a general-purpose RS485 command port. */
#define SVD48_WORKSPACE_HALL_TRACE_MAX_TRANSACTIONS 7U
#define SVD48_WORKSPACE_HALL_TRACE_REQUEST_MAX_BYTES 8U
#define SVD48_WORKSPACE_HALL_TRACE_RESPONSE_MAX_BYTES 64U

typedef enum {
    SVD48_WORKSPACE_CHANNEL_M1 = 0,
    SVD48_WORKSPACE_CHANNEL_M2 = 1,
} svd48_workspace_channel_id_t;

typedef enum {
    SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_UNKNOWN = 0,
    SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_SUCCESS,
    SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_CALIBRATING,
    SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_FAILED,
} svd48_workspace_hall_calibration_status_t;

/* Device-specific read DTOs for the maintenance workspace. They preserve the
 * build-selected profile identity without exposing robot_profile to transport
 * handlers. This is an internal source-level port, not a stable binary ABI. */
typedef struct {
    svd48_workspace_channel_id_t channel;
    bool endpoint_bound;
    robot_endpoint_id_t endpoint_id;
    const char *endpoint_name;
    uint32_t capabilities;
    robot_endpoint_criticality_t criticality;
    bool available;
    int16_t min_rpm;
    int16_t max_rpm;
    robot_endpoint_health_t health;
} svd48_workspace_channel_info_t;

typedef struct {
    uint16_t device_id;
    uint16_t bus_id;
    uint8_t address;
    const char *driver;
    bool available;
    robot_endpoint_health_t health;
    size_t channel_count;
    svd48_workspace_channel_info_t channels[SVD48_WORKSPACE_CHANNEL_COUNT];
} svd48_workspace_controller_info_t;

typedef struct {
    uint16_t device_id;
    svd48_workspace_channel_id_t channel;
    bool endpoint_bound;
    robot_endpoint_id_t endpoint_id;
    bool online;
    bool stale;
    robot_endpoint_health_t health;
    uint32_t valid_observations;
    uint32_t failed_observations;
    uint32_t stale_observations;
    int16_t status;
    int16_t observed_speed_rpm;
    int16_t current_deciamp;
    int16_t motor_temp_decic;
    int16_t bus_voltage_deciv;
    int16_t mos_temp_decic;
    int32_t position_counts;
    uint32_t error_code;
    uint16_t communication_error;
    uint8_t last_exception_function;
    uint8_t last_exception_code;
    uint32_t last_exception_ms;
} svd48_workspace_channel_telemetry_t;

typedef struct {
    uint8_t attempt;
    uint16_t result;
    uint8_t request_length;
    uint8_t request[SVD48_WORKSPACE_HALL_TRACE_REQUEST_MAX_BYTES];
    uint8_t response_length;
    uint8_t response[SVD48_WORKSPACE_HALL_TRACE_RESPONSE_MAX_BYTES];
} svd48_workspace_hall_trace_entry_t;

/* Result of the typed one-shot Hall calibration request. Acknowledged and
 * status-readable are intentionally separate: an ACK proves only that the
 * controller accepted the trigger, not a physical calibration result. */
typedef struct {
    uint16_t device_id;
    svd48_workspace_channel_id_t channel;
    uint8_t address;
    uint16_t trigger_register;
    uint16_t status_register;
    bool write_acknowledged;
    bool status_available;
    uint16_t status_value;
    svd48_workspace_hall_calibration_status_t status;
    uint16_t write_result;
    uint16_t status_read_result;
    uint8_t trace_count;
    svd48_workspace_hall_trace_entry_t
        trace[SVD48_WORKSPACE_HALL_TRACE_MAX_TRANSACTIONS];
} svd48_workspace_hall_calibration_result_t;

typedef struct svd48_workspace_port svd48_workspace_port_t;

typedef struct {
    size_t (*controller_count)(const svd48_workspace_port_t *port);
    bool (*controller_at)(svd48_workspace_port_t *port,
                          size_t index,
                          svd48_workspace_controller_info_t *controller);
    bool (*channel_telemetry)(
        svd48_workspace_port_t *port,
        uint16_t device_id,
        svd48_workspace_channel_id_t channel,
        svd48_workspace_channel_telemetry_t *telemetry);
    bool (*hall_calibrate)(
        svd48_workspace_port_t *port,
        uint16_t device_id,
        svd48_workspace_channel_id_t channel,
        svd48_workspace_hall_calibration_result_t *result);
} svd48_workspace_ops_t;

struct svd48_workspace_port {
    const svd48_workspace_ops_t *ops;
    void *context;
};

static inline size_t svd48_workspace_controller_count(
    const svd48_workspace_port_t *port)
{
    return port && port->ops && port->ops->controller_count
               ? port->ops->controller_count(port)
               : 0U;
}

static inline bool svd48_workspace_controller_at(
    svd48_workspace_port_t *port,
    size_t index,
    svd48_workspace_controller_info_t *controller)
{
    return port && port->ops && port->ops->controller_at
               ? port->ops->controller_at(port, index, controller)
               : false;
}

static inline bool svd48_workspace_get_channel_telemetry(
    svd48_workspace_port_t *port,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel,
    svd48_workspace_channel_telemetry_t *telemetry)
{
    return port && port->ops && port->ops->channel_telemetry
               ? port->ops->channel_telemetry(
                     port, device_id, channel, telemetry)
               : false;
}

static inline bool svd48_workspace_hall_calibrate(
    svd48_workspace_port_t *port,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel,
    svd48_workspace_hall_calibration_result_t *result)
{
    return port && port->ops && port->ops->hall_calibrate
               ? port->ops->hall_calibrate(port, device_id, channel, result)
               : false;
}

#ifdef __cplusplus
}
#endif

#endif
