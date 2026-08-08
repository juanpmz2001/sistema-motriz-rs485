#ifndef STEERING_POSITION_CONTROLLER_H
#define STEERING_POSITION_CONTROLLER_H

/*
 * Portable steering-position closed-loop policy.
 *
 * This component owns neither GPIO nor an AS5600/I2C driver.  A caller supplies
 * corrected cyclic position observations and a bounded PWM-output callback.
 * LUT correction deliberately remains outside this controller.  After an
 * explicit reference it unwraps adjacent cyclic samples only for the normal,
 * bounded steering range.  The multi-turn 7+7 calibration procedure is a
 * separate maintenance capture/session and is not an ordinary position target.
 *
 * `at_target` means that this controller's configured observation has remained
 * inside its configured arrival band.  It is not a claim that an independent
 * physical measurement proved the linkage reached the commanded position.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES 360.0

typedef uint64_t (*steering_position_controller_clock_ms_fn)(void *context);

/*
 * Apply one PWM pulse width.  Returning false reports that the output layer
 * could not accept the requested value.  The controller then latches a fault
 * and attempts one best-effort neutral command when appropriate.
 */
typedef bool (*steering_position_controller_pwm_output_fn)(void *context,
                                                            uint16_t pulse_us);

typedef enum {
    STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY = 0,
    STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_DEGRADED,
    STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT,
    STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_OFFLINE,
} steering_position_controller_sensor_health_t;

/*
 * corrected_cyclic_position_deg is the LUT-corrected single-turn AS5600 angle
 * in [0, 360).  The controller starts UNHOMED and must never infer a logical
 * zero from this phase.  `set_reference` explicitly maps one fresh phase to a
 * logical position; later fresh samples are unwrapped relative to it.
 */
typedef struct {
    double corrected_cyclic_position_deg;
    bool valid;
    bool magnet_detected;
    steering_position_controller_sensor_health_t health;
    uint64_t timestamp_ms;
} steering_position_controller_sample_t;

/*
 * All values are profile/tuning data, not controller-family constants.
 *
 * `arrival_min_error_deg` and `arrival_max_error_deg` define an intentionally
 * unilateral arrival band.  The empirical steering policy uses [0, +3] deg:
 * a small negative error is driven back through the configured negative PWM
 * direction rather than silently treated as arrived.
 */
typedef struct {
    double minimum_position_deg;
    double maximum_position_deg;

    uint16_t neutral_pulse_us;
    uint16_t positive_far_pulse_us;
    uint16_t positive_near_pulse_us;
    uint16_t negative_far_pulse_us;
    uint16_t negative_near_pulse_us;

    double arrival_min_error_deg;
    double arrival_max_error_deg;
    double full_speed_error_deg;
    double reacquire_error_deg;
    uint32_t stable_sample_count;
    uint32_t reacquire_sample_count;

    uint32_t reversal_settle_ms;
    uint32_t sensor_stale_timeout_ms;
    uint32_t sensor_fault_timeout_ms;
    uint32_t max_command_ttl_ms;
    uint32_t move_timeout_ms;

    /* A weak-but-characterized field can be a profile-approved warning. */
    bool allow_degraded_sensor_health;
} steering_position_controller_config_t;

typedef enum {
    STEERING_POSITION_CONTROLLER_STATE_UNHOMED = 0,
    STEERING_POSITION_CONTROLLER_STATE_IDLE,
    STEERING_POSITION_CONTROLLER_STATE_TRACKING,
    STEERING_POSITION_CONTROLLER_STATE_WAITING_STABLE,
    STEERING_POSITION_CONTROLLER_STATE_HOLDING_TARGET,
    STEERING_POSITION_CONTROLLER_STATE_REVERSAL_SETTLING,
    STEERING_POSITION_CONTROLLER_STATE_COMMAND_EXPIRED,
    STEERING_POSITION_CONTROLLER_STATE_SENSOR_STALE,
    STEERING_POSITION_CONTROLLER_STATE_FAULT,
} steering_position_controller_state_t;

typedef enum {
    STEERING_POSITION_CONTROLLER_FAULT_NONE = 0,
    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT,
    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH,
    STEERING_POSITION_CONTROLLER_FAULT_POSITION_OUT_OF_RANGE,
    STEERING_POSITION_CONTROLLER_FAULT_MOVE_TIMEOUT,
    STEERING_POSITION_CONTROLLER_FAULT_OUTPUT,
} steering_position_controller_fault_t;

typedef enum {
    STEERING_POSITION_CONTROLLER_ACTION_NONE = 0,
    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE,
    STEERING_POSITION_CONTROLLER_ACTION_DRIVE_NEGATIVE,
    STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE,
    STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET,
    STEERING_POSITION_CONTROLLER_ACTION_REVERSAL_SETTLING,
    STEERING_POSITION_CONTROLLER_ACTION_COMMAND_EXPIRED,
    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE,
    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT,
    STEERING_POSITION_CONTROLLER_ACTION_MOVE_TIMEOUT,
    STEERING_POSITION_CONTROLLER_ACTION_STOPPED,
} steering_position_controller_action_t;

typedef enum {
    STEERING_POSITION_CONTROLLER_OK = 0,
    STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT,
    STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED,
    STEERING_POSITION_CONTROLLER_ERROR_INVALID_CONFIG,
    STEERING_POSITION_CONTROLLER_ERROR_INVALID_SAMPLE,
    STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH,
    STEERING_POSITION_CONTROLLER_ERROR_UNHOMED,
    STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE,
    STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL,
    STEERING_POSITION_CONTROLLER_ERROR_TIME_OVERFLOW,
    STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED,
    STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE,
} steering_position_controller_result_t;

typedef struct {
    steering_position_controller_action_t action;
    steering_position_controller_state_t state;
    steering_position_controller_fault_t fault;
    uint64_t now_ms;
    bool target_active;
    bool controller_estimated_at_target;
    bool current_position_valid;
    double current_position_deg;
    double target_position_deg;
    double error_deg;
    bool sample_fresh;
    bool output_known;
    uint16_t output_pulse_us;
} steering_position_controller_report_t;

typedef struct {
    bool initialized;
    bool homed;
    bool target_active;
    bool controller_estimated_at_target;
    bool current_position_valid;
    bool sample_fresh;
    steering_position_controller_state_t state;
    steering_position_controller_fault_t fault;
    double reference_position_deg;
    double reference_cyclic_position_deg;
    double current_position_deg;
    double target_position_deg;
    double error_deg;
    uint64_t latest_sample_timestamp_ms;
    uint64_t command_deadline_ms;
    uint64_t move_deadline_ms;
    uint16_t output_pulse_us;
    bool output_known;
    int last_drive_direction;
} steering_position_controller_snapshot_t;

/*
 * The structure is intentionally stack-allocatable.  Fields after the callback
 * bindings are controller-owned state; callers must modify it only through the
 * functions below.
 */
typedef struct {
    steering_position_controller_config_t config;
    steering_position_controller_clock_ms_fn clock_ms;
    void *clock_context;
    steering_position_controller_pwm_output_fn pwm_output;
    void *pwm_output_context;

    bool initialized;
    bool homed;
    bool target_active;
    bool controller_estimated_at_target;
    bool has_latest_sample;
    bool output_known;
    steering_position_controller_state_t state;
    steering_position_controller_fault_t fault;
    double reference_position_deg;
    double reference_cyclic_position_deg;
    double latest_cyclic_position_deg;
    double unwrapped_offset_deg;
    double target_position_deg;
    uint64_t latest_sample_timestamp_ms;
    uint64_t command_deadline_ms;
    uint64_t move_deadline_ms;
    /* The instant at which the output most recently transitioned from a
     * drive pulse to neutral.  A reverse drive cannot start until the
     * configured settle interval has elapsed from this point. */
    uint64_t last_neutral_transition_ms;
    bool has_neutral_transition;
    uint16_t output_pulse_us;
    uint32_t stable_sample_count;
    uint32_t reacquire_sample_count;
    int last_drive_direction;
} steering_position_controller_t;

steering_position_controller_result_t steering_position_controller_init(
    steering_position_controller_t *controller,
    const steering_position_controller_config_t *config,
    steering_position_controller_clock_ms_fn clock_ms,
    void *clock_context,
    steering_position_controller_pwm_output_fn pwm_output,
    void *pwm_output_context);

/*
 * Establish an explicit logical position from a fresh observation.  It rejects
 * every latched fault: reference is not a fault-clear or re-arm operation.
 * The caller remains responsible for authorizing this maintenance operation.
 */
steering_position_controller_result_t steering_position_controller_set_reference(
    steering_position_controller_t *controller,
    const steering_position_controller_sample_t *sample,
    double reference_position_deg,
    steering_position_controller_report_t *report);

/*
 * Start or refresh a bounded target.  A fresh command expires at ttl_ms even
 * after arrival; callers must renew it to retain active closed-loop holding.
 */
steering_position_controller_result_t steering_position_controller_set_target(
    steering_position_controller_t *controller,
    double target_position_deg,
    uint32_t ttl_ms,
    steering_position_controller_report_t *report);

/*
 * Process the latest observation and enforce expiry, sensor freshness and the
 * profile-tuned drive policy.  A NULL sample represents no newly supplied
 * observation; the last accepted sample remains usable only until stale.
 */
steering_position_controller_result_t steering_position_controller_tick(
    steering_position_controller_t *controller,
    const steering_position_controller_sample_t *sample,
    steering_position_controller_report_t *report);

/* Stop is idempotent, clears the current target and commands neutral. */
steering_position_controller_result_t steering_position_controller_stop(
    steering_position_controller_t *controller,
    steering_position_controller_report_t *report);

steering_position_controller_result_t steering_position_controller_snapshot(
    const steering_position_controller_t *controller,
    steering_position_controller_snapshot_t *snapshot);

const char *steering_position_controller_result_name(
    steering_position_controller_result_t result);
const char *steering_position_controller_state_name(
    steering_position_controller_state_t state);
const char *steering_position_controller_fault_name(
    steering_position_controller_fault_t fault);
const char *steering_position_controller_action_name(
    steering_position_controller_action_t action);

#ifdef __cplusplus
}
#endif

#endif
