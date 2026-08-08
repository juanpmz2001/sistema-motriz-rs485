#ifndef AS5600_DEVICE_H
#define AS5600_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AS5600's fixed 7-bit I2C address and one-turn raw-angle representation. */
#define AS5600_DEFAULT_I2C_ADDRESS 0x36U
#define AS5600_RAW_ANGLE_MASK 0x0FFFU
#define AS5600_RAW_COUNTS_PER_TURN 4096U
#define AS5600_DEGREES_PER_TURN 360.0f

/* The empirical correction grid is one node every 32 raw AS5600 counts. */
#define AS5600_CALIBRATION_LUT_NODE_COUNT 128U
#define AS5600_CALIBRATION_COUNTS_PER_NODE 32U
#define AS5600_CALIBRATION_FORMAT_VERSION 1U

/* Bits in the AS5600 STATUS register (0x0B). */
#define AS5600_STATUS_MAGNET_DETECTED 0x20U
#define AS5600_STATUS_MAGNET_TOO_WEAK 0x10U
#define AS5600_STATUS_MAGNET_TOO_STRONG 0x08U

typedef enum {
    AS5600_DEVICE_OK = 0,
    AS5600_DEVICE_INVALID_ARGUMENT,
    AS5600_DEVICE_NOT_READY,
    AS5600_DEVICE_BUS_BUSY,
    AS5600_DEVICE_TIMEOUT,
    AS5600_DEVICE_IO_ERROR,
    AS5600_DEVICE_BAD_RESPONSE,
    /* STATUS+RAW_ANGLE were read but the one-shot optional diagnostic read
     * failed. The primary phase remains available; the diagnostic result is
     * retained separately in the snapshot. */
    AS5600_DEVICE_PARTIAL,
} as5600_device_result_t;

typedef enum {
    AS5600_DEVICE_HEALTH_UNKNOWN = 0,
    AS5600_DEVICE_HEALTH_HEALTHY,
    AS5600_DEVICE_HEALTH_DEGRADED,
    AS5600_DEVICE_HEALTH_OFFLINE,
    AS5600_DEVICE_HEALTH_STALE,
} as5600_device_health_t;

/*
 * Platform adapter for one register read. The adapter owns its concrete I2C
 * bus, locking and ESP-IDF error mapping. This component never includes a
 * platform I2C header.
 */
typedef as5600_device_result_t (*as5600_register_read_fn)(
    void *context,
    uint8_t device_address,
    uint8_t register_address,
    uint8_t *out_bytes,
    size_t byte_count,
    uint32_t timeout_ms);

typedef struct {
    as5600_register_read_fn read;
    void *context;
} as5600_register_read_port_t;

typedef uint32_t (*as5600_clock_ms_fn)(void *context);

/* Optional state protection for callers that poll and read snapshots concurrently. */
typedef struct {
    bool (*acquire)(void *context);
    void (*release)(void *context);
    void *context;
} as5600_device_lock_t;

/*
 * Calibration corrections are signed centidegrees at each 32-count node.
 * They are interpolated cyclically, then added to the raw one-turn angle.
 *
 * A calibration belongs to one magnet/sensor/shaft assembly. Profiles must
 * give it a stable identifier, identify that assembly, and point to the
 * reproducible external calibration evidence or procedure. Do not put raw
 * capture streams in this object or source tree.
 */
typedef struct {
    uint16_t format_version;
    const char *calibration_id;
    const char *hardware_identity;
    const char *provenance;
} as5600_calibration_metadata_t;

typedef struct {
    as5600_calibration_metadata_t metadata;
    int16_t correction_centidegrees[AS5600_CALIBRATION_LUT_NODE_COUNT];
} as5600_calibration_lut_t;

typedef struct {
    uint16_t device_id;
    uint8_t i2c_address;
    as5600_register_read_port_t register_read;
    as5600_clock_ms_fn clock_ms;
    void *clock_context;
    uint32_t response_timeout_ms;
    uint32_t stale_timeout_ms;
    /* Read AGC/MAGNITUDE once after the first successful STATUS+RAW_ANGLE
     * sample. They are device diagnostics, not part of the control-rate read
     * path; a later retry requires reinitializing the device. */
    bool read_diagnostics;
    const as5600_calibration_lut_t *calibration;
    as5600_device_lock_t state_lock;
} as5600_device_config_t;

/*
 * This is intentionally device-specific. It retains raw status and magnetic
 * diagnostics so a later application adapter can make policy decisions
 * without pretending that a position reading alone proves sensor health.
 */
typedef struct {
    bool raw_angle_valid;
    uint16_t raw_angle;
    uint8_t status;
    bool magnet_detected;
    bool magnet_too_weak;
    bool magnet_too_strong;
    bool diagnostics_requested;
    bool diagnostics_attempted;
    bool diagnostics_valid;
    uint8_t automatic_gain_control;
    uint16_t magnitude;
    uint32_t diagnostics_timestamp_ms;
    as5600_device_result_t diagnostics_last_result;
    uint32_t sample_timestamp_ms;
    uint32_t last_poll_timestamp_ms;
    as5600_device_result_t last_poll_result;
    as5600_device_result_t last_error;
    bool online;
    bool stale;
    as5600_device_health_t health;
} as5600_device_snapshot_t;

/*
 * This is the later-controller-friendly cyclic sensor view. `valid` means a
 * fresh successful raw-angle sample with STATUS.MD; it does not by itself
 * prove a mechanical steering coordinate. The endpoint adapter must require a
 * profile-approved calibration before treating it as qualified physical
 * feedback. ML/MH leave the phase readable but report DEGRADED health; a
 * safety policy may impose stricter requirements for a particular endpoint.
 */
typedef struct {
    bool valid;
    float degrees;
    uint16_t raw_angle;
    uint32_t timestamp_ms;
    bool online;
    bool stale;
    as5600_device_health_t health;
    bool calibration_applied;
} as5600_position_snapshot_t;

typedef struct {
    uint32_t polls;
    uint32_t successful_samples;
    uint32_t failed_polls;
    uint32_t consecutive_failures;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;
    as5600_device_result_t last_error;
} as5600_device_communication_t;

/*
 * Read-only L2/L3 diagnostic view of a single AS5600 device.  This is
 * deliberately device-specific: raw phase, STATUS bits and one-shot magnetic
 * diagnostics and communication counters do not belong in the generic
 * PositionObservation contract used by L4/L5 clients.
 * `calibration_metadata` contains borrowed immutable profile strings and is
 * valid only while the device remains initialized.
 */
typedef struct {
    uint16_t device_id;
    uint8_t i2c_address;
    as5600_device_snapshot_t snapshot;
    as5600_device_communication_t communication;
    bool calibration_configured;
    as5600_calibration_metadata_t calibration_metadata;
} as5600_device_diagnostics_t;

typedef struct as5600_device as5600_device_t;

struct as5600_device {
    as5600_device_config_t config;
    as5600_device_snapshot_t snapshot;
    as5600_device_communication_t communication;
    bool poll_in_progress;
    bool diagnostics_attempted;
    bool initialized;
};

/*
 * Validation includes metadata and checks every integer raw code, including
 * the 4095-to-0 cyclic transition, for a strictly increasing corrected map.
 */
bool as5600_calibration_lut_validate(const as5600_calibration_lut_t *calibration);

float as5600_raw_angle_degrees(uint16_t raw_angle);
/* Validate a non-NULL profile LUT before use. A NULL calibration means zero correction. */
float as5600_calibration_correction_degrees(
    const as5600_calibration_lut_t *calibration,
    uint16_t raw_angle);
float as5600_calibration_corrected_degrees(
    const as5600_calibration_lut_t *calibration,
    uint16_t raw_angle);

bool as5600_device_init(as5600_device_t *device,
                        const as5600_device_config_t *config);
void as5600_device_deinit(as5600_device_t *device);

uint16_t as5600_device_id(const as5600_device_t *device);
uint8_t as5600_device_i2c_address(const as5600_device_t *device);

/* Reads contiguous STATUS+RAW_ANGLE on every poll. When configured,
 * AGC/MAGNITUDE are read once after the first successful primary sample, so
 * diagnostics cannot consume every control-rate I2C slot. */
as5600_device_result_t as5600_device_poll(as5600_device_t *device);

bool as5600_device_get_snapshot(as5600_device_t *device,
                                as5600_device_snapshot_t *snapshot);
/* Copies only already-cached state and immutable calibration provenance.  It
 * never starts an I2C transaction, changes poll cadence, or commands an
 * actuator. */
bool as5600_device_get_diagnostics(
    as5600_device_t *device,
    as5600_device_diagnostics_t *diagnostics);
bool as5600_device_get_position_snapshot(as5600_device_t *device,
                                         as5600_position_snapshot_t *snapshot);
bool as5600_device_get_communication(
    as5600_device_t *device,
    as5600_device_communication_t *communication);

as5600_device_health_t as5600_device_health_from_snapshot(
    const as5600_device_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
