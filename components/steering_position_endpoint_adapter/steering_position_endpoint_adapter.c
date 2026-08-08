#include "steering_position_endpoint_adapter.h"

#include <math.h>
#include <string.h>

#define STEERING_POSITION_PHASE_MATCH_TOLERANCE_DEG 0.001

static bool lock_callbacks_are_valid(
    const steering_position_endpoint_adapter_lock_t *lock)
{
    return lock != NULL &&
           ((lock->acquire == NULL && lock->release == NULL) ||
            (lock->acquire != NULL && lock->release != NULL));
}

static bool acquire_adapter(steering_position_endpoint_adapter_t *adapter)
{
    return adapter != NULL &&
           (adapter->lock.acquire == NULL ||
            adapter->lock.acquire(adapter->lock.context));
}

static void release_adapter(steering_position_endpoint_adapter_t *adapter)
{
    if (adapter != NULL && adapter->lock.release != NULL) {
        adapter->lock.release(adapter->lock.context);
    }
}

static robot_capability_error_t map_controller_result(
    steering_position_controller_result_t result)
{
    switch (result) {
    case STEERING_POSITION_CONTROLLER_OK:
        return ROBOT_CAP_OK;
    case STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE:
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL:
        return ROBOT_CAP_OUT_OF_RANGE;
    case STEERING_POSITION_CONTROLLER_ERROR_UNHOMED:
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_SAMPLE:
    case STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH:
    case STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED:
        return ROBOT_CAP_UNAVAILABLE;
    case STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE:
        return ROBOT_CAP_IO_ERROR;
    case STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT:
    case STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED:
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_CONFIG:
    case STEERING_POSITION_CONTROLLER_ERROR_TIME_OVERFLOW:
    default:
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
}

static bool corrected_cyclic_phase_is_valid(float degrees)
{
    return isfinite(degrees) && degrees >= 0.0f &&
           degrees < (float)STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES;
}

static bool corrected_cyclic_phases_match(float supplied_degrees,
                                           double accepted_degrees)
{
    double delta = fabs((double)supplied_degrees - accepted_degrees);
    if (delta > STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES / 2.0) {
        delta = STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES - delta;
    }
    /* This is only an equivalence check for one sample, not a mechanical
     * position tolerance. It remains much smaller than one AS5600 LSB. */
    return delta <= STEERING_POSITION_PHASE_MATCH_TOLERANCE_DEG;
}

static steering_position_controller_sensor_health_t map_sensor_health(
    as5600_device_health_t health)
{
    switch (health) {
    case AS5600_DEVICE_HEALTH_HEALTHY:
        return STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY;
    case AS5600_DEVICE_HEALTH_DEGRADED:
        return STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_DEGRADED;
    case AS5600_DEVICE_HEALTH_STALE:
    case AS5600_DEVICE_HEALTH_OFFLINE:
        return STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_OFFLINE;
    case AS5600_DEVICE_HEALTH_UNKNOWN:
    default:
        return STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT;
    }
}

static uint64_t controller_now_ms(
    const steering_position_endpoint_adapter_t *adapter)
{
    return adapter->controller.clock_ms(adapter->controller.clock_context);
}

/* Expand the AS5600's uint32_t timestamp into the controller's monotonic
 * uint64_t clock domain. Both clocks must have the same milliseconds epoch
 * modulo 2^32; composition supplies that common source. */
static uint64_t extend_as5600_timestamp(uint64_t now_ms, uint32_t timestamp_ms)
{
    const uint64_t low_mask = UINT64_C(0xffffffff);
    const uint64_t half_turn = UINT64_C(0x80000000);
    const uint64_t full_turn = UINT64_C(0x100000000);
    uint64_t candidate = (now_ms & ~low_mask) | (uint64_t)timestamp_ms;

    if (candidate > now_ms && candidate - now_ms > half_turn &&
        candidate >= full_turn) {
        candidate -= full_turn;
    } else if (now_ms > candidate && now_ms - candidate > half_turn &&
               candidate <= UINT64_MAX - full_turn) {
        candidate += full_turn;
    }
    return candidate;
}

static int32_t raw_circular_delta(uint16_t newer_raw, uint16_t older_raw)
{
    int32_t delta = (int32_t)(newer_raw & AS5600_RAW_ANGLE_MASK) -
                    (int32_t)(older_raw & AS5600_RAW_ANGLE_MASK);
    const int32_t half_turn = (int32_t)AS5600_RAW_COUNTS_PER_TURN / 2;
    if (delta > half_turn) {
        delta -= (int32_t)AS5600_RAW_COUNTS_PER_TURN;
    } else if (delta < -half_turn) {
        delta += (int32_t)AS5600_RAW_COUNTS_PER_TURN;
    }
    return delta;
}

static bool raw_step_is_accepted(
    const steering_position_endpoint_adapter_t *adapter,
    uint16_t raw_angle)
{
    if (!adapter->have_last_raw_angle) {
        return true;
    }
    int32_t delta = raw_circular_delta(raw_angle, adapter->last_raw_angle);
    if (delta < 0) {
        delta = -delta;
    }
    return (uint32_t)delta <= adapter->max_raw_circular_step_counts;
}

static bool calibration_is_approved(
    const steering_position_endpoint_adapter_t *adapter)
{
    return adapter != NULL && adapter->calibration_approved &&
           adapter->approved_calibration != NULL &&
           adapter->as5600 != NULL && adapter->as5600->initialized &&
           adapter->as5600->config.calibration == adapter->approved_calibration;
}

/* The profile exception is intentionally exact. `DEGRADED` is a summary that
 * can also mean missing magnet, MH or a partial/failed I2C operation, none of
 * which is a permitted feedback input for motor-mode steering. */
static bool snapshot_is_explicitly_permitted_weak_magnet(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    return adapter != NULL && snapshot != NULL &&
           adapter->allow_magnet_too_weak_for_development &&
           adapter->controller.config.allow_degraded_sensor_health &&
           snapshot->last_poll_result == AS5600_DEVICE_OK &&
           snapshot->magnet_detected && snapshot->magnet_too_weak &&
           !snapshot->magnet_too_strong;
}

/* AGC/MAGNITUDE are read once after the initial primary sample.  A failed
 * requested diagnostic remains cached by the device even when a later primary
 * STATUS+RAW poll succeeds; it is deliberately a persistent control NO-GO,
 * not a transient summary of only the most recent primary transaction. */
static bool snapshot_has_failed_requested_diagnostics(
    const as5600_device_snapshot_t *snapshot)
{
    return snapshot != NULL && snapshot->diagnostics_requested &&
           snapshot->diagnostics_attempted && !snapshot->diagnostics_valid;
}

static bool snapshot_is_control_usable(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    if (!calibration_is_approved(adapter) || snapshot == NULL ||
        !snapshot->raw_angle_valid || !snapshot->magnet_detected ||
        !snapshot->online || snapshot->stale ||
        snapshot->last_poll_result != AS5600_DEVICE_OK ||
        snapshot_has_failed_requested_diagnostics(snapshot) ||
        snapshot->magnet_too_strong) {
        return false;
    }
    return !snapshot->magnet_too_weak ||
           snapshot_is_explicitly_permitted_weak_magnet(adapter, snapshot);
}

static bool snapshot_has_immediate_sensor_fault(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->stale || !snapshot->raw_angle_valid ||
        !snapshot->online) {
        return false;
    }
    return !snapshot->magnet_detected || snapshot->magnet_too_strong ||
           snapshot_has_failed_requested_diagnostics(snapshot) ||
           (snapshot->magnet_too_weak &&
            !snapshot_is_explicitly_permitted_weak_magnet(adapter, snapshot)) ||
           snapshot->last_poll_result == AS5600_DEVICE_PARTIAL;
}

static steering_position_controller_sample_t sample_from_snapshot(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    const bool approved = calibration_is_approved(adapter);
    const bool fresh = snapshot->raw_angle_valid && snapshot->online &&
                       !snapshot->stale;
    const uint64_t now_ms = controller_now_ms(adapter);

    /* Freshness is handled by the controller's stale-neutral and later
     * fault-latch windows.  Do not translate a retained, expired snapshot
     * into an immediate OFFLINE hard fault: doing so would bypass the
     * profile's intentional stale-before-fault policy. */
    steering_position_controller_sensor_health_t health =
        snapshot->stale ? STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY
                        : map_sensor_health(snapshot->health);
    if (snapshot_has_immediate_sensor_fault(adapter, snapshot)) {
        health = STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT;
    } else if (snapshot_is_explicitly_permitted_weak_magnet(adapter, snapshot)) {
        health = STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_DEGRADED;
    } else if (snapshot->last_poll_result != AS5600_DEVICE_OK) {
        /* A failed primary read retains an old sample temporarily. Let the
         * controller's stale-neutral/fault windows govern that loss rather
         * than turning a transport timeout into an immediate hard fault. */
        health = STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY;
    }

    return (steering_position_controller_sample_t){
        .corrected_cyclic_position_deg =
            (double)as5600_calibration_corrected_degrees(
                adapter->approved_calibration, snapshot->raw_angle),
        .valid = approved && fresh &&
                 snapshot_is_control_usable(adapter, snapshot),
        .magnet_detected = snapshot->magnet_detected,
        .health = health,
        .timestamp_ms = extend_as5600_timestamp(now_ms,
                                                 snapshot->sample_timestamp_ms),
    };
}

static steering_position_controller_sample_t raw_jump_fault_sample(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    steering_position_controller_sample_t sample =
        sample_from_snapshot(adapter, snapshot);
    sample.valid = false;
    sample.health = STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT;
    return sample;
}

static bool snapshot_has_usable_raw_position(
    const steering_position_endpoint_adapter_t *adapter,
    const as5600_device_snapshot_t *snapshot)
{
    return snapshot_is_control_usable(adapter, snapshot);
}

static robot_capability_error_t set_position_port(
    robot_position_port_t *port,
    float degrees)
{
    steering_position_endpoint_adapter_t *adapter =
        port != NULL ? port->context : NULL;
    return steering_position_endpoint_adapter_set_position_degrees(adapter, degrees);
}

static robot_capability_error_t set_position_reference_port(
    robot_position_reference_port_t *port,
    float degrees)
{
    steering_position_endpoint_adapter_t *adapter =
        port != NULL ? port->context : NULL;
    return steering_position_endpoint_adapter_set_reference(adapter, degrees);
}

static robot_capability_error_t stop_port(robot_stoppable_port_t *port)
{
    steering_position_endpoint_adapter_t *adapter =
        port != NULL ? port->context : NULL;
    return steering_position_endpoint_adapter_stop(adapter);
}

bool steering_position_endpoint_adapter_init(
    steering_position_endpoint_adapter_t *adapter,
    const steering_position_endpoint_adapter_config_t *config)
{
    static const robot_position_ops_t position_ops = {
        .set_position_degrees = set_position_port,
    };
    static const robot_position_reference_ops_t position_reference_ops = {
        .set_reference_degrees = set_position_reference_port,
    };
    static const robot_stoppable_ops_t stoppable_ops = {
        .stop = stop_port,
    };

    if (adapter == NULL || config == NULL || config->as5600 == NULL ||
        !config->as5600->initialized || config->approved_calibration == NULL ||
        config->as5600->config.calibration != config->approved_calibration ||
        !as5600_calibration_lut_validate(config->approved_calibration) ||
        config->endpoint_id == 0U || config->endpoint_name == NULL ||
        config->endpoint_name[0] == '\0' || config->clock_ms == NULL ||
        config->pwm_output == NULL || config->position_command_ttl_ms == 0U ||
        config->position_command_ttl_ms > config->controller_config.max_command_ttl_ms ||
        !isfinite((float)config->controller_config.minimum_position_deg) ||
        !isfinite((float)config->controller_config.maximum_position_deg) ||
        config->max_raw_circular_step_counts == 0U ||
        config->max_raw_circular_step_counts >=
            AS5600_RAW_COUNTS_PER_TURN / 2U ||
        config->allow_magnet_too_weak_for_development !=
            config->controller_config.allow_degraded_sensor_health ||
        !lock_callbacks_are_valid(&config->lock)) {
        return false;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->as5600 = config->as5600;
    adapter->approved_calibration = config->approved_calibration;
    adapter->lock = config->lock;
    adapter->max_raw_circular_step_counts = config->max_raw_circular_step_counts;
    adapter->position_command_ttl_ms = config->position_command_ttl_ms;
    adapter->allow_magnet_too_weak_for_development =
        config->allow_magnet_too_weak_for_development;
    adapter->calibration_approved = true;
    if (steering_position_controller_init(&adapter->controller,
                                          &config->controller_config,
                                          config->clock_ms,
                                          config->clock_context,
                                          config->pwm_output,
                                          config->pwm_output_context) !=
        STEERING_POSITION_CONTROLLER_OK) {
        memset(adapter, 0, sizeof(*adapter));
        return false;
    }

    adapter->position.ops = &position_ops;
    adapter->position.context = adapter;
    adapter->position.min_degrees = (float)config->controller_config.minimum_position_deg;
    adapter->position.max_degrees = (float)config->controller_config.maximum_position_deg;
    adapter->position_reference.ops = &position_reference_ops;
    adapter->position_reference.context = adapter;
    adapter->position_reference.min_degrees =
        (float)config->controller_config.minimum_position_deg;
    adapter->position_reference.max_degrees =
        (float)config->controller_config.maximum_position_deg;
    adapter->stoppable.ops = &stoppable_ops;
    adapter->stoppable.context = adapter;
    adapter->endpoint = (robot_endpoint_t){
        .id = config->endpoint_id,
        .name = config->endpoint_name,
        .criticality = config->criticality,
        .available = true,
        .position = &adapter->position,
        .position_reference = &adapter->position_reference,
        .stoppable = &adapter->stoppable,
    };
    adapter->initialized = true;
    return true;
}

void steering_position_endpoint_adapter_deinit(
    steering_position_endpoint_adapter_t *adapter)
{
    if (adapter == NULL || !adapter->initialized) {
        return;
    }
    if (!acquire_adapter(adapter)) {
        return;
    }
    (void)steering_position_controller_stop(&adapter->controller, NULL);
    release_adapter(adapter);
    memset(adapter, 0, sizeof(*adapter));
}

robot_capability_error_t steering_position_endpoint_adapter_set_position_degrees(
    steering_position_endpoint_adapter_t *adapter,
    float degrees)
{
    if (adapter == NULL || !adapter->initialized || !isfinite(degrees)) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    if (!acquire_adapter(adapter)) {
        return ROBOT_CAP_IO_ERROR;
    }
    steering_position_controller_result_t result =
        steering_position_controller_set_target(&adapter->controller,
                                                (double)degrees,
                                                adapter->position_command_ttl_ms,
                                                NULL);
    release_adapter(adapter);
    return map_controller_result(result);
}

robot_capability_error_t steering_position_endpoint_adapter_stop(
    steering_position_endpoint_adapter_t *adapter)
{
    if (adapter == NULL || !adapter->initialized) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    if (!acquire_adapter(adapter)) {
        return ROBOT_CAP_IO_ERROR;
    }
    steering_position_controller_result_t result =
        steering_position_controller_stop(&adapter->controller, NULL);
    release_adapter(adapter);
    return map_controller_result(result);
}

robot_capability_error_t steering_position_endpoint_adapter_set_reference(
    steering_position_endpoint_adapter_t *adapter,
    float logical_degrees)
{
    if (adapter == NULL || !adapter->initialized || !isfinite(logical_degrees)) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    if (!acquire_adapter(adapter)) {
        return ROBOT_CAP_IO_ERROR;
    }

    as5600_device_snapshot_t snapshot;
    if (!as5600_device_get_snapshot(adapter->as5600, &snapshot)) {
        release_adapter(adapter);
        return ROBOT_CAP_IO_ERROR;
    }
    if (!snapshot_has_usable_raw_position(adapter, &snapshot)) {
        release_adapter(adapter);
        return ROBOT_CAP_UNAVAILABLE;
    }

    steering_position_controller_sample_t sample =
        sample_from_snapshot(adapter, &snapshot);
    steering_position_controller_result_t result =
        steering_position_controller_set_reference(&adapter->controller,
                                                   &sample,
                                                   (double)logical_degrees,
                                                   NULL);
    if (result == STEERING_POSITION_CONTROLLER_OK) {
        adapter->last_raw_angle = snapshot.raw_angle;
        adapter->have_last_raw_angle = true;
    }
    release_adapter(adapter);
    return map_controller_result(result);
}

robot_capability_error_t steering_position_endpoint_adapter_project_cyclic_phase(
    steering_position_endpoint_adapter_t *adapter,
    float corrected_cyclic_degrees,
    uint32_t timestamp_ms,
    float *logical_degrees)
{
    if (adapter == NULL || logical_degrees == NULL || !adapter->initialized ||
        !corrected_cyclic_phase_is_valid(corrected_cyclic_degrees)) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    if (!acquire_adapter(adapter)) {
        return ROBOT_CAP_IO_ERROR;
    }

    steering_position_controller_snapshot_t snapshot;
    const steering_position_controller_result_t result =
        steering_position_controller_snapshot(&adapter->controller, &snapshot);
    const bool usable = result == STEERING_POSITION_CONTROLLER_OK &&
                        snapshot.homed && snapshot.sample_fresh &&
                        snapshot.current_position_valid &&
                        snapshot.fault == STEERING_POSITION_CONTROLLER_FAULT_NONE &&
                        (uint32_t)snapshot.latest_sample_timestamp_ms ==
                            timestamp_ms &&
                        corrected_cyclic_phases_match(
                            corrected_cyclic_degrees,
                            adapter->controller.latest_cyclic_position_deg);
    if (usable) {
        *logical_degrees = (float)snapshot.current_position_deg;
    }
    release_adapter(adapter);
    if (result != STEERING_POSITION_CONTROLLER_OK) {
        return map_controller_result(result);
    }
    return usable ? ROBOT_CAP_OK : ROBOT_CAP_UNAVAILABLE;
}

robot_capability_error_t steering_position_endpoint_adapter_tick(
    steering_position_endpoint_adapter_t *adapter,
    steering_position_controller_report_t *report)
{
    if (adapter == NULL || !adapter->initialized) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    if (!acquire_adapter(adapter)) {
        return ROBOT_CAP_IO_ERROR;
    }

    as5600_device_snapshot_t snapshot;
    if (!as5600_device_get_snapshot(adapter->as5600, &snapshot)) {
        steering_position_controller_result_t controller_result =
            steering_position_controller_tick(&adapter->controller, NULL, report);
        release_adapter(adapter);
        if (controller_result != STEERING_POSITION_CONTROLLER_OK) {
            return map_controller_result(controller_result);
        }
        return ROBOT_CAP_IO_ERROR;
    }

    steering_position_controller_sample_t sample =
        sample_from_snapshot(adapter, &snapshot);
    if (snapshot_has_usable_raw_position(adapter, &snapshot)) {
        if (!raw_step_is_accepted(adapter, snapshot.raw_angle)) {
            sample = raw_jump_fault_sample(adapter, &snapshot);
        } else {
            adapter->last_raw_angle = snapshot.raw_angle;
            adapter->have_last_raw_angle = true;
        }
    }

    steering_position_controller_result_t result =
        steering_position_controller_tick(&adapter->controller, &sample, report);
    release_adapter(adapter);
    if (result != STEERING_POSITION_CONTROLLER_OK) {
        return map_controller_result(result);
    }
    return adapter->controller.fault == STEERING_POSITION_CONTROLLER_FAULT_NONE
               ? ROBOT_CAP_OK
               : ROBOT_CAP_UNAVAILABLE;
}
