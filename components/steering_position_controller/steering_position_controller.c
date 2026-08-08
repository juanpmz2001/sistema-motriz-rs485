#include "steering_position_controller.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static bool is_valid_sensor_health(steering_position_controller_sensor_health_t health)
{
    return health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY ||
           health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_DEGRADED ||
           health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT ||
           health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_OFFLINE;
}

static bool sensor_health_is_accepted(
    const steering_position_controller_config_t *config,
    steering_position_controller_sensor_health_t health)
{
    return health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_HEALTHY ||
           (health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_DEGRADED &&
            config->allow_degraded_sensor_health);
}

static bool sample_is_acceptable(const steering_position_controller_t *controller,
                                 const steering_position_controller_sample_t *sample,
                                 uint64_t now_ms)
{
    if (sample == NULL || !sample->valid || !sample->magnet_detected ||
        !is_valid_sensor_health(sample->health) ||
        !sensor_health_is_accepted(&controller->config, sample->health) ||
        !isfinite(sample->corrected_cyclic_position_deg) ||
        sample->corrected_cyclic_position_deg < 0.0 ||
        sample->corrected_cyclic_position_deg >=
            STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES ||
        sample->timestamp_ms > now_ms) {
        return false;
    }
    return true;
}

static bool sample_reports_hard_fault(
    const steering_position_controller_sample_t *sample)
{
    if (sample == NULL) {
        return false;
    }

    return !sample->magnet_detected ||
           sample->health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_FAULT ||
           sample->health == STEERING_POSITION_CONTROLLER_SENSOR_HEALTH_OFFLINE ||
           !is_valid_sensor_health(sample->health);
}

static bool timestamp_is_fresh(const steering_position_controller_t *controller,
                               uint64_t now_ms)
{
    return controller->has_latest_sample &&
           now_ms >= controller->latest_sample_timestamp_ms &&
           now_ms - controller->latest_sample_timestamp_ms <=
               controller->config.sensor_stale_timeout_ms;
}

static bool timestamp_has_faulted(const steering_position_controller_t *controller,
                                  uint64_t now_ms)
{
    return !controller->has_latest_sample ||
           now_ms < controller->latest_sample_timestamp_ms ||
           now_ms - controller->latest_sample_timestamp_ms >=
               controller->config.sensor_fault_timeout_ms;
}

static bool deadline_reached(uint64_t now_ms, uint64_t deadline_ms)
{
    return now_ms >= deadline_ms;
}

static bool add_duration(uint64_t now_ms, uint32_t duration_ms, uint64_t *deadline_ms)
{
    if (deadline_ms == NULL || UINT64_MAX - now_ms < duration_ms) {
        return false;
    }
    *deadline_ms = now_ms + duration_ms;
    return true;
}

static double signed_cyclic_delta_deg(double newer_deg, double older_deg)
{
    double delta = fmod(newer_deg - older_deg,
                        STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES);

    if (delta > STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES / 2.0) {
        delta -= STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES;
    } else if (delta < -STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES / 2.0) {
        delta += STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES;
    }
    return delta;
}

static bool controller_has_fault(const steering_position_controller_t *controller)
{
    return controller->state == STEERING_POSITION_CONTROLLER_STATE_FAULT ||
           controller->fault != STEERING_POSITION_CONTROLLER_FAULT_NONE;
}

static void clear_target(steering_position_controller_t *controller)
{
    controller->target_active = false;
    controller->controller_estimated_at_target = false;
    controller->stable_sample_count = 0U;
    controller->reacquire_sample_count = 0U;
    controller->command_deadline_ms = 0U;
    controller->move_deadline_ms = 0U;
}

static void set_fault(steering_position_controller_t *controller,
                      steering_position_controller_fault_t fault)
{
    clear_target(controller);
    controller->state = STEERING_POSITION_CONTROLLER_STATE_FAULT;
    controller->fault = fault;
}

static bool write_pulse(steering_position_controller_t *controller,
                        uint16_t pulse_us,
                        bool force)
{
    if (!force && controller->output_known &&
        controller->output_pulse_us == pulse_us) {
        return true;
    }

    if (!controller->pwm_output(controller->pwm_output_context, pulse_us)) {
        controller->output_known = false;
        return false;
    }

    controller->output_known = true;
    controller->output_pulse_us = pulse_us;
    return true;
}

static bool command_neutral(steering_position_controller_t *controller,
                            bool force,
                            uint64_t now_ms)
{
    /* A held PWM pulse remains an active drive even when no new callback was
     * needed on the preceding scheduler tick.  Record the actual transition
     * to neutral, rather than the last time tick happened to re-evaluate it. */
    const bool was_non_neutral =
        !controller->output_known ||
        controller->output_pulse_us != controller->config.neutral_pulse_us;
    if (write_pulse(controller, controller->config.neutral_pulse_us, force)) {
        if (was_non_neutral) {
            controller->last_neutral_transition_ms = now_ms;
            controller->has_neutral_transition = true;
        }
        return true;
    }

    set_fault(controller, STEERING_POSITION_CONTROLLER_FAULT_OUTPUT);
    return false;
}

static bool latch_fault_and_neutral(steering_position_controller_t *controller,
                                    steering_position_controller_fault_t fault,
                                    uint64_t now_ms)
{
    set_fault(controller, fault);
    if (command_neutral(controller, true, now_ms)) {
        return true;
    }
    return false;
}

static bool current_position(const steering_position_controller_t *controller,
                             double *position_deg)
{
    if (position_deg == NULL || !controller->homed ||
        !controller->has_latest_sample) {
        return false;
    }

    double position = controller->reference_position_deg +
                      controller->unwrapped_offset_deg;
    if (!isfinite(position)) {
        return false;
    }
    *position_deg = position;
    return true;
}

static bool current_position_in_bounds(const steering_position_controller_t *controller,
                                       double position_deg)
{
    return position_deg >= controller->config.minimum_position_deg &&
           position_deg <= controller->config.maximum_position_deg;
}

static void accept_sample(steering_position_controller_t *controller,
                          const steering_position_controller_sample_t *sample)
{
    if (controller->has_latest_sample) {
        double delta = signed_cyclic_delta_deg(sample->corrected_cyclic_position_deg,
                                                controller->latest_cyclic_position_deg);
        controller->unwrapped_offset_deg += delta;
    }

    controller->latest_cyclic_position_deg = sample->corrected_cyclic_position_deg;
    controller->latest_sample_timestamp_ms = sample->timestamp_ms;
    controller->has_latest_sample = true;
}

static bool accept_new_sample(steering_position_controller_t *controller,
                              const steering_position_controller_sample_t *sample,
                              uint64_t now_ms,
                              bool *is_new_sample)
{
    if (is_new_sample != NULL) {
        *is_new_sample = false;
    }
    if (!sample_is_acceptable(controller, sample, now_ms)) {
        return false;
    }
    if (controller->has_latest_sample &&
        sample->timestamp_ms < controller->latest_sample_timestamp_ms) {
        return false;
    }
    if (controller->has_latest_sample &&
        sample->timestamp_ms == controller->latest_sample_timestamp_ms) {
        return true;
    }

    accept_sample(controller, sample);
    if (is_new_sample != NULL) {
        *is_new_sample = true;
    }
    return true;
}

static uint16_t interpolated_drive_pulse(uint16_t near_pulse_us,
                                         uint16_t far_pulse_us,
                                         double absolute_error_deg,
                                         const steering_position_controller_config_t *config)
{
    if (absolute_error_deg <= config->arrival_max_error_deg) {
        return near_pulse_us;
    }
    if (absolute_error_deg >= config->full_speed_error_deg) {
        return far_pulse_us;
    }

    double ratio = (absolute_error_deg - config->arrival_max_error_deg) /
                   (config->full_speed_error_deg -
                    config->arrival_max_error_deg);
    double pulse = (double)near_pulse_us +
                   ((double)far_pulse_us - (double)near_pulse_us) * ratio;
    return (uint16_t)(pulse + 0.5);
}

static bool reversal_settle_required(const steering_position_controller_t *controller,
                                     int requested_direction,
                                     uint64_t now_ms)
{
    if (controller->config.reversal_settle_ms == 0U ||
        controller->last_drive_direction == 0 ||
        requested_direction == controller->last_drive_direction) {
        return false;
    }

    /* The caller will issue neutral first if a direction pulse is still
     * applied.  If its state is unknown, treat it conservatively as an
     * unsettled output and request neutral again. */
    if (!controller->output_known ||
        controller->output_pulse_us != controller->config.neutral_pulse_us ||
        !controller->has_neutral_transition ||
        now_ms < controller->last_neutral_transition_ms) {
        return true;
    }
    return now_ms - controller->last_neutral_transition_ms <
           controller->config.reversal_settle_ms;
}

static void fill_report(const steering_position_controller_t *controller,
                        uint64_t now_ms,
                        steering_position_controller_action_t action,
                        steering_position_controller_report_t *report)
{
    if (report == NULL) {
        return;
    }

    double position_deg = 0.0;
    bool position_valid = current_position(controller, &position_deg) &&
                          timestamp_is_fresh(controller, now_ms);
    double error_deg = 0.0;
    if (position_valid && controller->target_active) {
        error_deg = controller->target_position_deg - position_deg;
    }

    *report = (steering_position_controller_report_t){
        .action = action,
        .state = controller->state,
        .fault = controller->fault,
        .now_ms = now_ms,
        .target_active = controller->target_active,
        .controller_estimated_at_target = controller->controller_estimated_at_target,
        .current_position_valid = position_valid,
        .current_position_deg = position_deg,
        .target_position_deg = controller->target_position_deg,
        .error_deg = error_deg,
        .sample_fresh = timestamp_is_fresh(controller, now_ms),
        .output_known = controller->output_known,
        .output_pulse_us = controller->output_pulse_us,
    };
}

static steering_position_controller_action_t action_for_fault(
    steering_position_controller_fault_t fault)
{
    switch (fault) {
    case STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT:
    case STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH:
        return STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT;
    case STEERING_POSITION_CONTROLLER_FAULT_MOVE_TIMEOUT:
        return STEERING_POSITION_CONTROLLER_ACTION_MOVE_TIMEOUT;
    case STEERING_POSITION_CONTROLLER_FAULT_POSITION_OUT_OF_RANGE:
    case STEERING_POSITION_CONTROLLER_FAULT_OUTPUT:
    case STEERING_POSITION_CONTROLLER_FAULT_NONE:
    default:
        return STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL;
    }
}

static bool config_is_valid(const steering_position_controller_config_t *config)
{
    if (config == NULL || !isfinite(config->minimum_position_deg) ||
        !isfinite(config->maximum_position_deg) ||
        config->minimum_position_deg >= config->maximum_position_deg ||
        config->maximum_position_deg - config->minimum_position_deg >
            STEERING_POSITION_CONTROLLER_CYCLIC_DEGREES / 2.0 ||
        config->neutral_pulse_us == 0U || config->positive_far_pulse_us == 0U ||
        config->positive_near_pulse_us == 0U ||
        config->negative_far_pulse_us == 0U ||
        config->negative_near_pulse_us == 0U ||
        config->positive_far_pulse_us == config->neutral_pulse_us ||
        config->positive_near_pulse_us == config->neutral_pulse_us ||
        config->negative_far_pulse_us == config->neutral_pulse_us ||
        config->negative_near_pulse_us == config->neutral_pulse_us ||
        !isfinite(config->arrival_min_error_deg) ||
        !isfinite(config->arrival_max_error_deg) ||
        !isfinite(config->full_speed_error_deg) ||
        !isfinite(config->reacquire_error_deg) ||
        config->arrival_min_error_deg < 0.0 ||
        config->arrival_max_error_deg < config->arrival_min_error_deg ||
        config->full_speed_error_deg <= config->arrival_max_error_deg ||
        config->reacquire_error_deg <= config->arrival_max_error_deg ||
        config->stable_sample_count == 0U || config->reacquire_sample_count == 0U ||
        config->sensor_stale_timeout_ms == 0U ||
        config->sensor_fault_timeout_ms <= config->sensor_stale_timeout_ms ||
        config->max_command_ttl_ms == 0U || config->move_timeout_ms == 0U) {
        return false;
    }
    return true;
}

steering_position_controller_result_t steering_position_controller_init(
    steering_position_controller_t *controller,
    const steering_position_controller_config_t *config,
    steering_position_controller_clock_ms_fn clock_ms,
    void *clock_context,
    steering_position_controller_pwm_output_fn pwm_output,
    void *pwm_output_context)
{
    if (controller == NULL || config == NULL || clock_ms == NULL ||
        pwm_output == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!config_is_valid(config)) {
        return STEERING_POSITION_CONTROLLER_ERROR_INVALID_CONFIG;
    }

    memset(controller, 0, sizeof(*controller));
    controller->config = *config;
    controller->clock_ms = clock_ms;
    controller->clock_context = clock_context;
    controller->pwm_output = pwm_output;
    controller->pwm_output_context = pwm_output_context;
    controller->state = STEERING_POSITION_CONTROLLER_STATE_UNHOMED;
    controller->fault = STEERING_POSITION_CONTROLLER_FAULT_NONE;

    const uint64_t now_ms = controller->clock_ms(controller->clock_context);
    if (!command_neutral(controller, true, now_ms)) {
        return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
    }

    controller->initialized = true;
    return STEERING_POSITION_CONTROLLER_OK;
}

steering_position_controller_result_t steering_position_controller_set_reference(
    steering_position_controller_t *controller,
    const steering_position_controller_sample_t *sample,
    double reference_position_deg,
    steering_position_controller_report_t *report)
{
    if (controller == NULL || sample == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!controller->initialized) {
        return STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED;
    }

    uint64_t now_ms = controller->clock_ms(controller->clock_context);
    /*
     * A reference maps a known physical pose into the logical coordinate
     * system; it is not a fault-clear or re-arm operation.  In particular,
     * accepting it after an output, timeout, sensor or range fault would let
     * the maintenance command make a subsequent position request movable
     * again without an explicitly reviewed recovery policy.
     */
    if (controller_has_fault(controller)) {
        fill_report(controller,
                    now_ms,
                    action_for_fault(controller->fault),
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED;
    }
    if (!isfinite(reference_position_deg) ||
        reference_position_deg < controller->config.minimum_position_deg ||
        reference_position_deg > controller->config.maximum_position_deg) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE;
    }
    if (!sample_is_acceptable(controller, sample, now_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_INVALID_SAMPLE;
    }
    if (now_ms - sample->timestamp_ms > controller->config.sensor_stale_timeout_ms) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH;
    }
    if (!command_neutral(controller, true, now_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
    }

    controller->homed = true;
    controller->has_latest_sample = true;
    controller->reference_position_deg = reference_position_deg;
    controller->reference_cyclic_position_deg = sample->corrected_cyclic_position_deg;
    controller->latest_cyclic_position_deg = sample->corrected_cyclic_position_deg;
    controller->unwrapped_offset_deg = 0.0;
    controller->latest_sample_timestamp_ms = sample->timestamp_ms;
    controller->state = STEERING_POSITION_CONTROLLER_STATE_IDLE;
    clear_target(controller);

    fill_report(controller,
                now_ms,
                STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                report);
    return STEERING_POSITION_CONTROLLER_OK;
}

steering_position_controller_result_t steering_position_controller_set_target(
    steering_position_controller_t *controller,
    double target_position_deg,
    uint32_t ttl_ms,
    steering_position_controller_report_t *report)
{
    if (controller == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!controller->initialized) {
        return STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED;
    }

    uint64_t now_ms = controller->clock_ms(controller->clock_context);
    if (controller_has_fault(controller)) {
        fill_report(controller,
                    now_ms,
                    action_for_fault(controller->fault),
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED;
    }
    if (!controller->homed) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_UNHOMED;
    }
    /* A command must not refresh an active lease while its feedback has
     * already expired.  This forces the same neutral behavior as tick(),
     * including for a same-target TTL refresh that otherwise would not write
     * a new PWM pulse. */
    if (!timestamp_is_fresh(controller, now_ms)) {
        controller->state = STEERING_POSITION_CONTROLLER_STATE_SENSOR_STALE;
        if (!command_neutral(controller, true, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH;
    }
    if (!isfinite(target_position_deg) ||
        target_position_deg < controller->config.minimum_position_deg ||
        target_position_deg > controller->config.maximum_position_deg) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE;
    }
    if (ttl_ms == 0U || ttl_ms > controller->config.max_command_ttl_ms) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL;
    }

    uint64_t command_deadline_ms;
    if (!add_duration(now_ms, ttl_ms, &command_deadline_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_TIME_OVERFLOW;
    }

    bool target_is_refresh = controller->target_active &&
                             controller->target_position_deg == target_position_deg;
    uint64_t move_deadline_ms = controller->move_deadline_ms;
    if (!target_is_refresh &&
        !add_duration(now_ms, controller->config.move_timeout_ms, &move_deadline_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_TIME_OVERFLOW;
    }

    if (!target_is_refresh && !command_neutral(controller, true, now_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
    }

    controller->target_position_deg = target_position_deg;
    controller->target_active = true;
    controller->command_deadline_ms = command_deadline_ms;
    if (!target_is_refresh) {
        controller->move_deadline_ms = move_deadline_ms;
        controller->controller_estimated_at_target = false;
        controller->stable_sample_count = 0U;
        controller->reacquire_sample_count = 0U;
        controller->state = STEERING_POSITION_CONTROLLER_STATE_TRACKING;
    }

    fill_report(controller,
                now_ms,
                target_is_refresh ? STEERING_POSITION_CONTROLLER_ACTION_NONE
                                  : STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                report);
    return STEERING_POSITION_CONTROLLER_OK;
}

steering_position_controller_result_t steering_position_controller_tick(
    steering_position_controller_t *controller,
    const steering_position_controller_sample_t *sample,
    steering_position_controller_report_t *report)
{
    if (controller == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!controller->initialized) {
        return STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED;
    }

    uint64_t now_ms = controller->clock_ms(controller->clock_context);
    bool hard_sensor_fault = sample_reports_hard_fault(sample);
    bool new_acceptable_sample = false;
    (void)accept_new_sample(controller,
                            sample,
                            now_ms,
                            &new_acceptable_sample);

    if (controller_has_fault(controller)) {
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller, now_ms, action_for_fault(controller->fault), report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    /* A hard sensor fault must latch even before reference.  Otherwise an
     * initial diagnostic failure could be overwritten by a later healthy
     * primary sample and the subsequent reference operation would silently
     * make the actuator movable again. */
    if (hard_sensor_fault) {
        if (!latch_fault_and_neutral(
                controller,
                STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH,
                now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    if (!controller->homed) {
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        controller->state = STEERING_POSITION_CONTROLLER_STATE_UNHOMED;
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    if (!timestamp_is_fresh(controller, now_ms)) {
        if (timestamp_has_faulted(controller, now_ms)) {
            if (!latch_fault_and_neutral(
                    controller,
                    STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT,
                    now_ms)) {
                fill_report(controller,
                            now_ms,
                            STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                            report);
                return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
            }
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT,
                        report);
            return STEERING_POSITION_CONTROLLER_OK;
        }

        controller->state = STEERING_POSITION_CONTROLLER_STATE_SENSOR_STALE;
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    double position_deg = 0.0;
    if (!current_position(controller, &position_deg) ||
        !current_position_in_bounds(controller, position_deg)) {
        if (!latch_fault_and_neutral(
                controller,
                STEERING_POSITION_CONTROLLER_FAULT_POSITION_OUT_OF_RANGE,
                now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    if (!controller->target_active) {
        controller->state = STEERING_POSITION_CONTROLLER_STATE_IDLE;
        controller->controller_estimated_at_target = false;
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    if (deadline_reached(now_ms, controller->command_deadline_ms)) {
        clear_target(controller);
        controller->state = STEERING_POSITION_CONTROLLER_STATE_COMMAND_EXPIRED;
        if (!command_neutral(controller, true, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_COMMAND_EXPIRED,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    if (!controller->controller_estimated_at_target &&
        deadline_reached(now_ms, controller->move_deadline_ms)) {
        if (!latch_fault_and_neutral(
                controller,
                STEERING_POSITION_CONTROLLER_FAULT_MOVE_TIMEOUT,
                now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_MOVE_TIMEOUT,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    double error_deg = controller->target_position_deg - position_deg;
    if (controller->controller_estimated_at_target) {
        if (fabs(error_deg) <= controller->config.reacquire_error_deg) {
            controller->reacquire_sample_count = 0U;
            controller->state = STEERING_POSITION_CONTROLLER_STATE_HOLDING_TARGET;
            if (!command_neutral(controller, false, now_ms)) {
                fill_report(controller,
                            now_ms,
                            STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                            report);
                return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
            }
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET,
                        report);
            return STEERING_POSITION_CONTROLLER_OK;
        }

        /* Reacquisition is evidence-based: do not turn one retained AS5600
         * snapshot into several samples merely because the scheduler ticks
         * faster than the sensor poller. */
        if (new_acceptable_sample &&
            controller->reacquire_sample_count < UINT32_MAX) {
            controller->reacquire_sample_count++;
        }
        if (controller->reacquire_sample_count <
            controller->config.reacquire_sample_count) {
            controller->state = STEERING_POSITION_CONTROLLER_STATE_HOLDING_TARGET;
            if (!command_neutral(controller, false, now_ms)) {
                fill_report(controller,
                            now_ms,
                            STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                            report);
                return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
            }
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET,
                        report);
            return STEERING_POSITION_CONTROLLER_OK;
        }

        controller->controller_estimated_at_target = false;
        controller->stable_sample_count = 0U;
        controller->reacquire_sample_count = 0U;
    }

    if (error_deg >= controller->config.arrival_min_error_deg &&
        error_deg <= controller->config.arrival_max_error_deg) {
        /* Arrival stability also needs distinct fresh observations.  The
         * controller still keeps neutral while waiting for the next one. */
        if (new_acceptable_sample &&
            controller->stable_sample_count < UINT32_MAX) {
            controller->stable_sample_count++;
        }
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        if (controller->stable_sample_count >= controller->config.stable_sample_count) {
            controller->controller_estimated_at_target = true;
            controller->reacquire_sample_count = 0U;
            controller->state = STEERING_POSITION_CONTROLLER_STATE_HOLDING_TARGET;
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET,
                        report);
        } else {
            controller->state = STEERING_POSITION_CONTROLLER_STATE_WAITING_STABLE;
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE,
                        report);
        }
        return STEERING_POSITION_CONTROLLER_OK;
    }

    controller->stable_sample_count = 0U;
    int direction = error_deg > controller->config.arrival_max_error_deg ? 1 : -1;
    if (reversal_settle_required(controller, direction, now_ms)) {
        controller->state = STEERING_POSITION_CONTROLLER_STATE_REVERSAL_SETTLING;
        if (!command_neutral(controller, false, now_ms)) {
            fill_report(controller,
                        now_ms,
                        STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                        report);
            return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
        }
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_REVERSAL_SETTLING,
                    report);
        return STEERING_POSITION_CONTROLLER_OK;
    }

    uint16_t pulse_us;
    if (direction > 0) {
        pulse_us = interpolated_drive_pulse(controller->config.positive_near_pulse_us,
                                             controller->config.positive_far_pulse_us,
                                             fabs(error_deg),
                                             &controller->config);
    } else {
        pulse_us = interpolated_drive_pulse(controller->config.negative_near_pulse_us,
                                             controller->config.negative_far_pulse_us,
                                             fabs(error_deg),
                                             &controller->config);
    }

    if (!write_pulse(controller, pulse_us, false)) {
        set_fault(controller, STEERING_POSITION_CONTROLLER_FAULT_OUTPUT);
        (void)write_pulse(controller, controller->config.neutral_pulse_us, true);
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
    }

    controller->last_drive_direction = direction;
    controller->state = STEERING_POSITION_CONTROLLER_STATE_TRACKING;
    fill_report(controller,
                now_ms,
                direction > 0 ? STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE
                              : STEERING_POSITION_CONTROLLER_ACTION_DRIVE_NEGATIVE,
                report);
    return STEERING_POSITION_CONTROLLER_OK;
}

steering_position_controller_result_t steering_position_controller_stop(
    steering_position_controller_t *controller,
    steering_position_controller_report_t *report)
{
    if (controller == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!controller->initialized) {
        return STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED;
    }

    uint64_t now_ms = controller->clock_ms(controller->clock_context);
    clear_target(controller);
    if (!command_neutral(controller, true, now_ms)) {
        fill_report(controller,
                    now_ms,
                    STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL,
                    report);
        return STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE;
    }
    if (!controller_has_fault(controller)) {
        controller->state = controller->homed
                                ? STEERING_POSITION_CONTROLLER_STATE_IDLE
                                : STEERING_POSITION_CONTROLLER_STATE_UNHOMED;
    }

    fill_report(controller,
                now_ms,
                STEERING_POSITION_CONTROLLER_ACTION_STOPPED,
                report);
    return STEERING_POSITION_CONTROLLER_OK;
}

steering_position_controller_result_t steering_position_controller_snapshot(
    const steering_position_controller_t *controller,
    steering_position_controller_snapshot_t *snapshot)
{
    if (controller == NULL || snapshot == NULL) {
        return STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT;
    }
    if (!controller->initialized) {
        return STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED;
    }

    uint64_t now_ms = controller->clock_ms(controller->clock_context);
    double position_deg = 0.0;
    bool position_valid = current_position(controller, &position_deg) &&
                          timestamp_is_fresh(controller, now_ms);
    double error_deg = position_valid && controller->target_active
                           ? controller->target_position_deg - position_deg
                           : 0.0;

    *snapshot = (steering_position_controller_snapshot_t){
        .initialized = controller->initialized,
        .homed = controller->homed,
        .target_active = controller->target_active,
        .controller_estimated_at_target = controller->controller_estimated_at_target,
        .current_position_valid = position_valid,
        .sample_fresh = timestamp_is_fresh(controller, now_ms),
        .state = controller->state,
        .fault = controller->fault,
        .reference_position_deg = controller->reference_position_deg,
        .reference_cyclic_position_deg = controller->reference_cyclic_position_deg,
        .current_position_deg = position_deg,
        .target_position_deg = controller->target_position_deg,
        .error_deg = error_deg,
        .latest_sample_timestamp_ms = controller->latest_sample_timestamp_ms,
        .command_deadline_ms = controller->command_deadline_ms,
        .move_deadline_ms = controller->move_deadline_ms,
        .output_pulse_us = controller->output_pulse_us,
        .output_known = controller->output_known,
        .last_drive_direction = controller->last_drive_direction,
    };
    return STEERING_POSITION_CONTROLLER_OK;
}

const char *steering_position_controller_result_name(
    steering_position_controller_result_t result)
{
    switch (result) {
    case STEERING_POSITION_CONTROLLER_OK:
        return "OK";
    case STEERING_POSITION_CONTROLLER_ERROR_NULL_ARGUMENT:
        return "NULL_ARGUMENT";
    case STEERING_POSITION_CONTROLLER_ERROR_NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_SAMPLE:
        return "INVALID_SAMPLE";
    case STEERING_POSITION_CONTROLLER_ERROR_SAMPLE_NOT_FRESH:
        return "SAMPLE_NOT_FRESH";
    case STEERING_POSITION_CONTROLLER_ERROR_UNHOMED:
        return "UNHOMED";
    case STEERING_POSITION_CONTROLLER_ERROR_TARGET_OUT_OF_RANGE:
        return "TARGET_OUT_OF_RANGE";
    case STEERING_POSITION_CONTROLLER_ERROR_INVALID_TTL:
        return "INVALID_TTL";
    case STEERING_POSITION_CONTROLLER_ERROR_TIME_OVERFLOW:
        return "TIME_OVERFLOW";
    case STEERING_POSITION_CONTROLLER_ERROR_FAULT_LATCHED:
        return "FAULT_LATCHED";
    case STEERING_POSITION_CONTROLLER_ERROR_OUTPUT_FAILURE:
        return "OUTPUT_FAILURE";
    default:
        return "UNKNOWN";
    }
}

const char *steering_position_controller_state_name(
    steering_position_controller_state_t state)
{
    switch (state) {
    case STEERING_POSITION_CONTROLLER_STATE_UNHOMED:
        return "UNHOMED";
    case STEERING_POSITION_CONTROLLER_STATE_IDLE:
        return "IDLE";
    case STEERING_POSITION_CONTROLLER_STATE_TRACKING:
        return "TRACKING";
    case STEERING_POSITION_CONTROLLER_STATE_WAITING_STABLE:
        return "WAITING_STABLE";
    case STEERING_POSITION_CONTROLLER_STATE_HOLDING_TARGET:
        return "HOLDING_TARGET";
    case STEERING_POSITION_CONTROLLER_STATE_REVERSAL_SETTLING:
        return "REVERSAL_SETTLING";
    case STEERING_POSITION_CONTROLLER_STATE_COMMAND_EXPIRED:
        return "COMMAND_EXPIRED";
    case STEERING_POSITION_CONTROLLER_STATE_SENSOR_STALE:
        return "SENSOR_STALE";
    case STEERING_POSITION_CONTROLLER_STATE_FAULT:
        return "FAULT";
    default:
        return "UNKNOWN";
    }
}

const char *steering_position_controller_fault_name(
    steering_position_controller_fault_t fault)
{
    switch (fault) {
    case STEERING_POSITION_CONTROLLER_FAULT_NONE:
        return "NONE";
    case STEERING_POSITION_CONTROLLER_FAULT_SENSOR_TIMEOUT:
        return "SENSOR_TIMEOUT";
    case STEERING_POSITION_CONTROLLER_FAULT_SENSOR_HEALTH:
        return "SENSOR_HEALTH";
    case STEERING_POSITION_CONTROLLER_FAULT_POSITION_OUT_OF_RANGE:
        return "POSITION_OUT_OF_RANGE";
    case STEERING_POSITION_CONTROLLER_FAULT_MOVE_TIMEOUT:
        return "MOVE_TIMEOUT";
    case STEERING_POSITION_CONTROLLER_FAULT_OUTPUT:
        return "OUTPUT";
    default:
        return "UNKNOWN";
    }
}

const char *steering_position_controller_action_name(
    steering_position_controller_action_t action)
{
    switch (action) {
    case STEERING_POSITION_CONTROLLER_ACTION_NONE:
        return "NONE";
    case STEERING_POSITION_CONTROLLER_ACTION_NEUTRAL:
        return "NEUTRAL";
    case STEERING_POSITION_CONTROLLER_ACTION_DRIVE_POSITIVE:
        return "DRIVE_POSITIVE";
    case STEERING_POSITION_CONTROLLER_ACTION_DRIVE_NEGATIVE:
        return "DRIVE_NEGATIVE";
    case STEERING_POSITION_CONTROLLER_ACTION_WAITING_STABLE:
        return "WAITING_STABLE";
    case STEERING_POSITION_CONTROLLER_ACTION_HOLDING_TARGET:
        return "HOLDING_TARGET";
    case STEERING_POSITION_CONTROLLER_ACTION_REVERSAL_SETTLING:
        return "REVERSAL_SETTLING";
    case STEERING_POSITION_CONTROLLER_ACTION_COMMAND_EXPIRED:
        return "COMMAND_EXPIRED";
    case STEERING_POSITION_CONTROLLER_ACTION_SENSOR_STALE:
        return "SENSOR_STALE";
    case STEERING_POSITION_CONTROLLER_ACTION_SENSOR_FAULT:
        return "SENSOR_FAULT";
    case STEERING_POSITION_CONTROLLER_ACTION_MOVE_TIMEOUT:
        return "MOVE_TIMEOUT";
    case STEERING_POSITION_CONTROLLER_ACTION_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}
