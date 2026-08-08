#include "as5600_device.h"

#include <string.h>

#define AS5600_REGISTER_STATUS 0x0BU
#define AS5600_REGISTER_AGC 0x1AU

#define AS5600_CENTIDEGREES_PER_TURN 36000
#define AS5600_CENTIDEGREE_SCALE ((int32_t)AS5600_RAW_COUNTS_PER_TURN)
#define AS5600_FULL_TURN_SCALED \
    ((int32_t)AS5600_CENTIDEGREES_PER_TURN * AS5600_CENTIDEGREE_SCALE)

_Static_assert(AS5600_CALIBRATION_LUT_NODE_COUNT *
                       AS5600_CALIBRATION_COUNTS_PER_NODE ==
                   AS5600_RAW_COUNTS_PER_TURN,
               "AS5600 calibration grid must cover one full raw turn");

static bool text_present(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static uint16_t normalize_raw_angle(uint16_t raw_angle)
{
    return raw_angle & AS5600_RAW_ANGLE_MASK;
}

static int32_t correction_scaled(const as5600_calibration_lut_t *calibration,
                                 uint16_t raw_angle)
{
    if (calibration == NULL) {
        return 0;
    }

    const uint16_t normalized = normalize_raw_angle(raw_angle);
    const uint16_t index = normalized / AS5600_CALIBRATION_COUNTS_PER_NODE;
    const uint16_t offset = normalized % AS5600_CALIBRATION_COUNTS_PER_NODE;
    const uint16_t next =
        (uint16_t)((index + 1U) % AS5600_CALIBRATION_LUT_NODE_COUNT);
    const int32_t first = calibration->correction_centidegrees[index];
    const int32_t second = calibration->correction_centidegrees[next];

    /*
     * Correction is linearly interpolated in centidegrees. Multiplying by
     * 128 retains its exact value in centidegrees * 4096 without float
     * rounding: 4096 / 32 == 128.
     */
    return (first * (int32_t)AS5600_CALIBRATION_COUNTS_PER_NODE +
            (second - first) * (int32_t)offset) *
           (AS5600_CENTIDEGREE_SCALE /
            (int32_t)AS5600_CALIBRATION_COUNTS_PER_NODE);
}

static int32_t corrected_scaled(const as5600_calibration_lut_t *calibration,
                                uint16_t raw_angle)
{
    return (int32_t)normalize_raw_angle(raw_angle) *
               (int32_t)AS5600_CENTIDEGREES_PER_TURN +
           correction_scaled(calibration, raw_angle);
}

static int32_t wrap_scaled_turn(int32_t value)
{
    int32_t wrapped = value % AS5600_FULL_TURN_SCALED;
    if (wrapped < 0) {
        wrapped += AS5600_FULL_TURN_SCALED;
    }
    return wrapped;
}

static uint32_t now_ms(const as5600_device_t *device)
{
    return device != NULL && device->config.clock_ms != NULL
               ? device->config.clock_ms(device->config.clock_context)
               : 0U;
}

static bool lock_state(as5600_device_t *device)
{
    return device != NULL &&
           (device->config.state_lock.acquire == NULL ||
            device->config.state_lock.acquire(
                device->config.state_lock.context));
}

static void unlock_state(as5600_device_t *device)
{
    if (device != NULL && device->config.state_lock.release != NULL) {
        device->config.state_lock.release(device->config.state_lock.context);
    }
}

static bool lock_callbacks_are_valid(const as5600_device_lock_t *lock)
{
    return lock != NULL &&
           ((lock->acquire == NULL && lock->release == NULL) ||
            (lock->acquire != NULL && lock->release != NULL));
}

static as5600_device_result_t normalize_read_result(
    as5600_device_result_t result)
{
    switch (result) {
    case AS5600_DEVICE_OK:
    case AS5600_DEVICE_INVALID_ARGUMENT:
    case AS5600_DEVICE_BUS_BUSY:
    case AS5600_DEVICE_TIMEOUT:
    case AS5600_DEVICE_IO_ERROR:
    case AS5600_DEVICE_BAD_RESPONSE:
        return result;
    default:
        return AS5600_DEVICE_IO_ERROR;
    }
}

static as5600_device_result_t read_registers(as5600_device_t *device,
                                              uint8_t register_address,
                                              uint8_t *out_bytes,
                                              size_t byte_count)
{
    if (device == NULL || !device->initialized || out_bytes == NULL ||
        byte_count == 0U || device->config.register_read.read == NULL) {
        return AS5600_DEVICE_INVALID_ARGUMENT;
    }

    return normalize_read_result(device->config.register_read.read(
        device->config.register_read.context,
        device->config.i2c_address,
        register_address,
        out_bytes,
        byte_count,
        device->config.response_timeout_ms));
}

static void update_timing(const as5600_device_t *device,
                          as5600_device_snapshot_t *snapshot)
{
    if (device == NULL || snapshot == NULL) {
        return;
    }

    const bool have_sample = snapshot->raw_angle_valid;
    const uint32_t age = have_sample ? now_ms(device) - snapshot->sample_timestamp_ms
                                     : UINT32_MAX;
    snapshot->stale = !have_sample || age > device->config.stale_timeout_ms;
    snapshot->online = have_sample && !snapshot->stale;
    snapshot->health = as5600_device_health_from_snapshot(snapshot);
}

static bool begin_poll(as5600_device_t *device)
{
    if (!lock_state(device)) {
        return false;
    }
    if (device->poll_in_progress) {
        unlock_state(device);
        return false;
    }
    device->poll_in_progress = true;
    unlock_state(device);
    return true;
}

static void finish_failed_poll(as5600_device_t *device,
                               as5600_device_result_t result)
{
    if (!lock_state(device)) {
        return;
    }

    const uint32_t timestamp = now_ms(device);
    device->communication.polls++;
    device->communication.failed_polls++;
    device->communication.consecutive_failures++;
    device->communication.last_failure_ms = timestamp;
    device->communication.last_error = result;
    device->snapshot.last_poll_timestamp_ms = timestamp;
    device->snapshot.last_poll_result = result;
    device->snapshot.last_error = result;
    device->poll_in_progress = false;
    unlock_state(device);
}

static void finish_sample(as5600_device_t *device,
                          uint8_t status,
                          uint16_t raw_angle,
                          uint32_t sample_timestamp_ms,
                          bool diagnostics_attempted,
                          bool diagnostics_valid,
                          uint8_t automatic_gain_control,
                          uint16_t magnitude,
                          as5600_device_result_t poll_result,
                          as5600_device_result_t error)
{
    if (!lock_state(device)) {
        return;
    }

    const uint32_t poll_timestamp_ms = now_ms(device);
    as5600_device_snapshot_t *snapshot = &device->snapshot;
    device->communication.polls++;
    device->communication.successful_samples++;
    device->communication.last_success_ms = sample_timestamp_ms;
    device->communication.last_error = error;
    if (poll_result == AS5600_DEVICE_OK) {
        device->communication.consecutive_failures = 0U;
    } else {
        device->communication.failed_polls++;
        device->communication.consecutive_failures++;
        device->communication.last_failure_ms = poll_timestamp_ms;
    }

    snapshot->raw_angle_valid = true;
    snapshot->raw_angle = normalize_raw_angle(raw_angle);
    snapshot->status = status;
    snapshot->magnet_detected =
        (status & AS5600_STATUS_MAGNET_DETECTED) != 0U;
    snapshot->magnet_too_weak =
        (status & AS5600_STATUS_MAGNET_TOO_WEAK) != 0U;
    snapshot->magnet_too_strong =
        (status & AS5600_STATUS_MAGNET_TOO_STRONG) != 0U;
    snapshot->diagnostics_requested = device->config.read_diagnostics;
    if (diagnostics_attempted) {
        device->diagnostics_attempted = true;
        snapshot->diagnostics_attempted = true;
        snapshot->diagnostics_valid = diagnostics_valid;
        snapshot->automatic_gain_control = automatic_gain_control;
        snapshot->magnitude = magnitude;
        snapshot->diagnostics_timestamp_ms = poll_timestamp_ms;
        snapshot->diagnostics_last_result = error;
    }
    /* Timestamp the raw sample when its contiguous STATUS+RAW read completed,
     * not after an optional later diagnostic transaction. A diagnostic read
     * must never make control feedback appear fresher than it is. */
    snapshot->sample_timestamp_ms = sample_timestamp_ms;
    snapshot->last_poll_timestamp_ms = poll_timestamp_ms;
    snapshot->last_poll_result = poll_result;
    snapshot->last_error = error;
    snapshot->online = true;
    snapshot->stale = false;
    snapshot->health = as5600_device_health_from_snapshot(snapshot);
    device->poll_in_progress = false;
    unlock_state(device);
}

bool as5600_calibration_lut_validate(const as5600_calibration_lut_t *calibration)
{
    if (calibration == NULL ||
        calibration->metadata.format_version != AS5600_CALIBRATION_FORMAT_VERSION ||
        !text_present(calibration->metadata.calibration_id) ||
        !text_present(calibration->metadata.hardware_identity) ||
        !text_present(calibration->metadata.provenance)) {
        return false;
    }

    int32_t previous = corrected_scaled(calibration, 0U);
    const int32_t first = previous;
    for (uint16_t raw_angle = 1U; raw_angle < AS5600_RAW_COUNTS_PER_TURN;
         ++raw_angle) {
        const int32_t current = corrected_scaled(calibration, raw_angle);
        if (current <= previous) {
            return false;
        }
        previous = current;
    }

    return first + AS5600_FULL_TURN_SCALED > previous;
}

float as5600_raw_angle_degrees(uint16_t raw_angle)
{
    return (float)normalize_raw_angle(raw_angle) *
           (AS5600_DEGREES_PER_TURN / (float)AS5600_RAW_COUNTS_PER_TURN);
}

float as5600_calibration_correction_degrees(
    const as5600_calibration_lut_t *calibration,
    uint16_t raw_angle)
{
    return (float)correction_scaled(calibration, raw_angle) /
           ((float)AS5600_CENTIDEGREE_SCALE * 100.0f);
}

float as5600_calibration_corrected_degrees(
    const as5600_calibration_lut_t *calibration,
    uint16_t raw_angle)
{
    const float degrees =
        (float)wrap_scaled_turn(corrected_scaled(calibration, raw_angle)) /
        ((float)AS5600_CENTIDEGREE_SCALE * 100.0f);
    /* Preserve the documented half-open cyclic range after float rounding. */
    return degrees >= AS5600_DEGREES_PER_TURN ? 0.0f : degrees;
}

bool as5600_device_init(as5600_device_t *device,
                        const as5600_device_config_t *config)
{
    if (device == NULL || config == NULL || config->device_id == 0U ||
        config->register_read.read == NULL || config->clock_ms == NULL ||
        config->response_timeout_ms == 0U || config->stale_timeout_ms == 0U ||
        !lock_callbacks_are_valid(&config->state_lock) ||
        (config->i2c_address != 0U && config->i2c_address > 0x7FU) ||
        (config->calibration != NULL &&
         !as5600_calibration_lut_validate(config->calibration))) {
        return false;
    }

    memset(device, 0, sizeof(*device));
    device->config = *config;
    if (device->config.i2c_address == 0U) {
        device->config.i2c_address = AS5600_DEFAULT_I2C_ADDRESS;
    }
    device->snapshot.diagnostics_requested = device->config.read_diagnostics;
    device->snapshot.diagnostics_attempted = false;
    device->snapshot.stale = true;
    device->snapshot.last_poll_result = AS5600_DEVICE_NOT_READY;
    device->snapshot.last_error = AS5600_DEVICE_NOT_READY;
    device->snapshot.diagnostics_last_result = AS5600_DEVICE_NOT_READY;
    device->snapshot.health = AS5600_DEVICE_HEALTH_OFFLINE;
    device->communication.last_error = AS5600_DEVICE_NOT_READY;
    device->initialized = true;
    return true;
}

void as5600_device_deinit(as5600_device_t *device)
{
    if (device != NULL) {
        memset(device, 0, sizeof(*device));
    }
}

uint16_t as5600_device_id(const as5600_device_t *device)
{
    return device != NULL && device->initialized ? device->config.device_id : 0U;
}

uint8_t as5600_device_i2c_address(const as5600_device_t *device)
{
    return device != NULL && device->initialized ? device->config.i2c_address : 0U;
}

as5600_device_result_t as5600_device_poll(as5600_device_t *device)
{
    if (device == NULL || !device->initialized) {
        return AS5600_DEVICE_INVALID_ARGUMENT;
    }
    if (!begin_poll(device)) {
        return AS5600_DEVICE_BUS_BUSY;
    }

    /* STATUS (0x0B) and RAW_ANGLE (0x0C..0x0D) are contiguous. One primary
     * transaction is essential at the 5 kHz empirical bit-bang rate: separate
     * status/raw reads exceeded the timing budget before diagnostics ran. */
    uint8_t primary[3] = {0U};
    as5600_device_result_t result = read_registers(
        device, AS5600_REGISTER_STATUS, primary, sizeof(primary));
    if (result != AS5600_DEVICE_OK) {
        finish_failed_poll(device, result);
        return result;
    }

    const uint32_t sample_timestamp_ms = now_ms(device);
    const uint8_t status = primary[0];
    const uint16_t raw_angle =
        ((uint16_t)(primary[1] & 0x0FU) << 8U) | primary[2];
    if (!device->config.read_diagnostics || device->diagnostics_attempted) {
        finish_sample(device,
                      status,
                      raw_angle,
                      sample_timestamp_ms,
                      false,
                      false,
                      0U,
                      0U,
                      AS5600_DEVICE_OK,
                      AS5600_DEVICE_OK);
        return AS5600_DEVICE_OK;
    }

    uint8_t diagnostics[3] = {0U};
    result = read_registers(device,
                            AS5600_REGISTER_AGC,
                            diagnostics,
                            sizeof(diagnostics));
    if (result != AS5600_DEVICE_OK) {
        finish_sample(device,
                      status,
                      raw_angle,
                      sample_timestamp_ms,
                      true,
                      false,
                      0U,
                      0U,
                      AS5600_DEVICE_PARTIAL,
                      result);
        return AS5600_DEVICE_PARTIAL;
    }

    const uint16_t magnitude = ((uint16_t)(diagnostics[1] & 0x0FU) << 8U) |
                               diagnostics[2];
    finish_sample(device,
                  status,
                  raw_angle,
                  sample_timestamp_ms,
                  true,
                  true,
                  diagnostics[0],
                  magnitude,
                  AS5600_DEVICE_OK,
                  AS5600_DEVICE_OK);
    return AS5600_DEVICE_OK;
}

bool as5600_device_get_snapshot(as5600_device_t *device,
                                as5600_device_snapshot_t *snapshot)
{
    if (device == NULL || !device->initialized || snapshot == NULL ||
        !lock_state(device)) {
        return false;
    }

    *snapshot = device->snapshot;
    update_timing(device, snapshot);
    unlock_state(device);
    return true;
}

bool as5600_device_get_diagnostics(
    as5600_device_t *device,
    as5600_device_diagnostics_t *diagnostics)
{
    if (device == NULL || !device->initialized || diagnostics == NULL ||
        !lock_state(device)) {
        return false;
    }

    *diagnostics = (as5600_device_diagnostics_t){
        .device_id = device->config.device_id,
        .i2c_address = device->config.i2c_address,
        .snapshot = device->snapshot,
        .communication = device->communication,
        .calibration_configured = device->config.calibration != NULL,
        .calibration_metadata = device->config.calibration != NULL
                                    ? device->config.calibration->metadata
                                    : (as5600_calibration_metadata_t){0},
    };
    /* Apply freshness to the copied snapshot only.  A serial diagnostic read
     * must not rejuvenate state or mutate the device's scheduled polling. */
    update_timing(device, &diagnostics->snapshot);
    unlock_state(device);
    return true;
}

bool as5600_device_get_position_snapshot(as5600_device_t *device,
                                         as5600_position_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    as5600_device_snapshot_t device_snapshot;
    if (!as5600_device_get_snapshot(device, &device_snapshot)) {
        return false;
    }

    *snapshot = (as5600_position_snapshot_t){
        .valid = device_snapshot.raw_angle_valid &&
                 device_snapshot.magnet_detected && !device_snapshot.stale,
        .degrees = as5600_calibration_corrected_degrees(
            device->config.calibration, device_snapshot.raw_angle),
        .raw_angle = device_snapshot.raw_angle,
        .timestamp_ms = device_snapshot.sample_timestamp_ms,
        .online = device_snapshot.online,
        .stale = device_snapshot.stale,
        .health = device_snapshot.health,
        .calibration_applied = device->config.calibration != NULL,
    };
    return true;
}

bool as5600_device_get_communication(
    as5600_device_t *device,
    as5600_device_communication_t *communication)
{
    if (device == NULL || !device->initialized || communication == NULL ||
        !lock_state(device)) {
        return false;
    }

    *communication = device->communication;
    unlock_state(device);
    return true;
}

as5600_device_health_t as5600_device_health_from_snapshot(
    const as5600_device_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return AS5600_DEVICE_HEALTH_UNKNOWN;
    }
    if (!snapshot->raw_angle_valid) {
        return AS5600_DEVICE_HEALTH_OFFLINE;
    }
    if (snapshot->stale) {
        return snapshot->last_poll_result == AS5600_DEVICE_OK
                   ? AS5600_DEVICE_HEALTH_STALE
                   : AS5600_DEVICE_HEALTH_OFFLINE;
    }
    if (!snapshot->online) {
        return AS5600_DEVICE_HEALTH_OFFLINE;
    }
    /* A requested one-shot diagnostic is part of the steering fixture's
     * initial qualification state.  Later successful STATUS+RAW polls must
     * not hide a failed AGC/MAGNITUDE read: the raw phase can remain useful
     * for L2/L3 investigation, but the device is still degraded until an
     * explicitly reviewed recovery path exists. */
    if (snapshot->last_poll_result != AS5600_DEVICE_OK ||
        (snapshot->diagnostics_requested && snapshot->diagnostics_attempted &&
         !snapshot->diagnostics_valid) ||
        !snapshot->magnet_detected || snapshot->magnet_too_weak ||
        snapshot->magnet_too_strong) {
        return AS5600_DEVICE_HEALTH_DEGRADED;
    }
    return AS5600_DEVICE_HEALTH_HEALTHY;
}
