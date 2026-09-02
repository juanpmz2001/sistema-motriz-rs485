#ifndef SVD48_WORKSPACE_PORT_H
#define SVD48_WORKSPACE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "communication_quality_model.h"
#include "robot_capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_WORKSPACE_CHANNEL_COUNT 2U
/* Bounded, read-only projection of raw frames emitted by one typed Hall
 * operation. It is deliberately not a general-purpose RS485 command port. */
#define SVD48_WORKSPACE_HALL_TRACE_MAX_TRANSACTIONS 48U
#define SVD48_WORKSPACE_HALL_TRACE_REQUEST_MAX_BYTES 8U
#define SVD48_WORKSPACE_HALL_TRACE_RESPONSE_MAX_BYTES 64U
#define SVD48_WORKSPACE_HALL_PREFLIGHT_MAX_READS 10U
#define SVD48_WORKSPACE_HALL_STATUS_SAMPLES_MAX 26U
/* Bounded evidence for the next existing STOP ALL operation.  This is a
 * diagnostic projection, never a general RS485 command interface. */
#define SVD48_WORKSPACE_STOP_TRACE_MAX_TRANSACTIONS 64U
#define SVD48_WORKSPACE_STOP_TRACE_REQUEST_MAX_BYTES 8U
#define SVD48_WORKSPACE_STOP_TRACE_RESPONSE_MAX_BYTES 64U
#define SVD48_WORKSPACE_STOP_OBSERVATION_COUNT 4U
#define SVD48_WORKSPACE_STOP_POST_WINDOW_MS 250U

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

typedef enum {
    SVD48_WORKSPACE_HALL_OUTCOME_TRIGGER_NOT_CONFIRMED = 0,
    SVD48_WORKSPACE_HALL_OUTCOME_SUCCESS,
    SVD48_WORKSPACE_HALL_OUTCOME_FAILED,
    SVD48_WORKSPACE_HALL_OUTCOME_TIMEOUT,
    SVD48_WORKSPACE_HALL_OUTCOME_COMMUNICATION_ERROR,
} svd48_workspace_hall_outcome_t;

typedef enum {
    SVD48_WORKSPACE_STOP_DIAG_UNAVAILABLE = 0,
    SVD48_WORKSPACE_STOP_DIAG_ARMED,
    SVD48_WORKSPACE_STOP_DIAG_CAPTURING,
    SVD48_WORKSPACE_STOP_DIAG_COMPLETE,
} svd48_workspace_stop_diagnostic_state_t;

typedef enum {
    SVD48_WORKSPACE_STOP_TRACE_OTHER = 0,
    SVD48_WORKSPACE_STOP_TRACE_M1_TARGET_ZERO,
    SVD48_WORKSPACE_STOP_TRACE_M1_STOP,
    SVD48_WORKSPACE_STOP_TRACE_M2_TARGET_ZERO,
    SVD48_WORKSPACE_STOP_TRACE_M2_STOP,
    SVD48_WORKSPACE_STOP_TRACE_CONTROL_WRITE,
    SVD48_WORKSPACE_STOP_TRACE_SPEED_TARGET_WRITE,
    SVD48_WORKSPACE_STOP_TRACE_TELEMETRY_READ,
} svd48_workspace_stop_trace_type_t;

typedef enum {
    SVD48_WORKSPACE_STOP_OBSERVATION_BEFORE_STOP = 0,
    SVD48_WORKSPACE_STOP_OBSERVATION_IMMEDIATE_AFTER_STOP,
    SVD48_WORKSPACE_STOP_OBSERVATION_AFTER_FIRST_FRESH_POLL,
    SVD48_WORKSPACE_STOP_OBSERVATION_FINAL,
} svd48_workspace_stop_observation_point_t;

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
    communication_quality_snapshot_t communication_quality;
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
    uint32_t timestamp_ms;
    uint8_t attempt;
    uint8_t address;
    uint16_t result;
    uint8_t request_length;
    uint8_t request[SVD48_WORKSPACE_HALL_TRACE_REQUEST_MAX_BYTES];
    uint8_t response_length;
    uint8_t response[SVD48_WORKSPACE_HALL_TRACE_RESPONSE_MAX_BYTES];
} svd48_workspace_hall_trace_entry_t;

typedef struct {
    uint16_t start_register;
    uint8_t quantity;
    uint16_t values[8];
    uint16_t result;
} svd48_workspace_hall_preflight_read_t;

typedef struct {
    uint32_t elapsed_ms;
    uint16_t value;
    uint16_t result;
} svd48_workspace_hall_status_sample_t;

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
    svd48_workspace_hall_outcome_t outcome;
    uint16_t write_result;
    uint16_t status_read_result;
    uint32_t started_ms;
    uint32_t finished_ms;
    uint8_t preflight_count;
    svd48_workspace_hall_preflight_read_t
        preflight[SVD48_WORKSPACE_HALL_PREFLIGHT_MAX_READS];
    uint8_t status_sample_count;
    svd48_workspace_hall_status_sample_t
        status_samples[SVD48_WORKSPACE_HALL_STATUS_SAMPLES_MAX];
    uint8_t trace_count;
    svd48_workspace_hall_trace_entry_t
        trace[SVD48_WORKSPACE_HALL_TRACE_MAX_TRANSACTIONS];
} svd48_workspace_hall_calibration_result_t;

typedef struct {
    uint32_t timestamp_ms;
    uint16_t device_id;
    uint8_t address;
    uint8_t attempt;
    uint16_t result;
    svd48_workspace_stop_trace_type_t type;
    uint8_t request_length;
    uint8_t request[SVD48_WORKSPACE_STOP_TRACE_REQUEST_MAX_BYTES];
    uint8_t response_length;
    uint8_t response[SVD48_WORKSPACE_STOP_TRACE_RESPONSE_MAX_BYTES];
} svd48_workspace_stop_trace_entry_t;

typedef struct {
    bool online;
    bool stale;
    robot_endpoint_health_t health;
    int16_t status;
    int16_t observed_speed_rpm;
    uint32_t error_code;
    uint32_t last_poll_ms;
} svd48_workspace_stop_channel_observation_t;

typedef struct {
    uint32_t timestamp_ms;
    svd48_workspace_stop_observation_point_t point;
    /* BEFORE_STOP and IMMEDIATE_AFTER_STOP are cache observations.  The
     * AFTER_FIRST_FRESH_POLL point is emitted only after a later poll stamp. */
    bool fresh_poll_observation;
    svd48_workspace_stop_channel_observation_t m1;
    svd48_workspace_stop_channel_observation_t m2;
    bool motion_active;
    uint8_t platform_state; /* 0 SAFE_IDLE, 1 MOTION_ACTIVE, 2 FAULT */
} svd48_workspace_stop_observation_t;

typedef struct {
    uint32_t id;
    svd48_workspace_stop_diagnostic_state_t state;
    uint16_t device_id;
    uint8_t address;
    uint32_t armed_ms;
    uint32_t stop_started_ms;
    uint32_t stop_finished_ms;
    uint32_t completed_ms;
    uint32_t post_window_ms;
    uint8_t trace_count;
    uint8_t observation_count;
    svd48_workspace_stop_trace_entry_t
        trace[SVD48_WORKSPACE_STOP_TRACE_MAX_TRANSACTIONS];
    svd48_workspace_stop_observation_t
        observations[SVD48_WORKSPACE_STOP_OBSERVATION_COUNT];
} svd48_workspace_stop_diagnostic_result_t;

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
    /* Typed controller-fault acknowledgement for one configured channel.  It
     * does not enable the channel or alter any speed target. */
    bool (*clear_fault)(svd48_workspace_port_t *port,
                        uint16_t device_id,
                        svd48_workspace_channel_id_t channel);
    bool (*stop_diagnostic_arm)(svd48_workspace_port_t *port,
                                uint32_t *diagnostic_id);
    /* These hooks only observe the gateway's existing STOP ALL path. */
    bool (*stop_diagnostic_before_stop)(svd48_workspace_port_t *port,
                                        uint32_t diagnostic_id);
    void (*stop_diagnostic_after_stop)(svd48_workspace_port_t *port,
                                       uint32_t diagnostic_id);
    bool (*stop_diagnostic_get)(svd48_workspace_port_t *port,
                                svd48_workspace_stop_diagnostic_result_t *result);
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

static inline bool svd48_workspace_clear_fault(
    svd48_workspace_port_t *port,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel)
{
    return port && port->ops && port->ops->clear_fault
               ? port->ops->clear_fault(port, device_id, channel)
               : false;
}

static inline bool svd48_workspace_stop_diagnostic_arm(
    svd48_workspace_port_t *port, uint32_t *diagnostic_id)
{
    return port && port->ops && port->ops->stop_diagnostic_arm
               ? port->ops->stop_diagnostic_arm(port, diagnostic_id)
               : false;
}

static inline bool svd48_workspace_stop_diagnostic_before_stop(
    svd48_workspace_port_t *port, uint32_t diagnostic_id)
{
    return port && port->ops && port->ops->stop_diagnostic_before_stop
               ? port->ops->stop_diagnostic_before_stop(port, diagnostic_id)
               : false;
}

static inline void svd48_workspace_stop_diagnostic_after_stop(
    svd48_workspace_port_t *port, uint32_t diagnostic_id)
{
    if (port && port->ops && port->ops->stop_diagnostic_after_stop) {
        port->ops->stop_diagnostic_after_stop(port, diagnostic_id);
    }
}

static inline bool svd48_workspace_stop_diagnostic_get(
    svd48_workspace_port_t *port,
    svd48_workspace_stop_diagnostic_result_t *result)
{
    return port && port->ops && port->ops->stop_diagnostic_get
               ? port->ops->stop_diagnostic_get(port, result)
               : false;
}

#ifdef __cplusplus
}
#endif

#endif
