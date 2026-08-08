#include "motor_mode_pwm.h"

#include <string.h>

#include "driver/gpio.h"

static bool configuration_valid(const motor_mode_pwm_config_t *config)
{
    return config && config->gpio_pin >= 0 && config->frequency_hz > 0U &&
           config->duty_resolution_bits >= 10U &&
           config->duty_resolution_bits <= 20U &&
           config->maximum_pulse_us < UINT32_C(1000000) / config->frequency_hz &&
           config->minimum_pulse_us < config->neutral_pulse_us &&
           config->neutral_pulse_us < config->maximum_pulse_us;
}

static uint32_t pulse_to_duty(const motor_mode_pwm_config_t *config,
                              uint16_t pulse_us)
{
    const uint32_t period_us = UINT32_C(1000000) / config->frequency_hz;
    const uint32_t max_duty =
        (UINT32_C(1) << config->duty_resolution_bits) - 1U;
    return (uint32_t)(((uint64_t)pulse_us * max_duty) / period_us);
}

/* LEDC has no channel deconfigure call. First stop its generated waveform,
 * then release the GPIO matrix routing to the reset/high-impedance state.
 * Neutral remains the commanded safety value while the device is live; the
 * final detached electrical state is deliberately a separate L3 hardware
 * characterization item, not assumed to be a servo-safe pulse. */
static void release_output_hardware(motor_mode_pwm_t *motor)
{
    if (motor == NULL || !motor->output_attached) {
        return;
    }
    (void)ledc_stop(motor->config.speed_mode, motor->config.channel, 0U);
    (void)gpio_reset_pin((gpio_num_t)motor->config.gpio_pin);
    motor->output_attached = false;
}

static esp_err_t apply_pulse(motor_mode_pwm_t *motor, uint16_t pulse_us)
{
    if (!motor || !motor->initialized ||
        pulse_us < motor->config.minimum_pulse_us ||
        pulse_us > motor->config.maximum_pulse_us) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t duty = pulse_to_duty(&motor->config, pulse_us);
    esp_err_t error = ledc_set_duty(motor->config.speed_mode,
                                    motor->config.channel,
                                    duty);
    if (error != ESP_OK) {
        return error;
    }
    error = ledc_update_duty(motor->config.speed_mode, motor->config.channel);
    if (error == ESP_OK) {
        motor->last_pulse_us = pulse_us;
    }
    return error;
}

esp_err_t motor_mode_pwm_init(motor_mode_pwm_t *motor,
                              const motor_mode_pwm_config_t *config)
{
    if (!motor || !configuration_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(motor, 0, sizeof(*motor));
    motor->config = *config;
    ledc_timer_config_t timer = {
        .speed_mode = config->speed_mode,
        .duty_resolution = (ledc_timer_bit_t)config->duty_resolution_bits,
        .timer_num = config->timer,
        .freq_hz = config->frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer);
    if (error != ESP_OK) {
        memset(motor, 0, sizeof(*motor));
        return error;
    }
    ledc_channel_config_t channel = {
        .gpio_num = config->gpio_pin,
        .speed_mode = config->speed_mode,
        .channel = config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = config->timer,
        /* Attach the pin with neutral already programmed. Do not briefly
         * publish a 0-duty waveform while constructing a motor-mode output. */
        .duty = pulse_to_duty(config, config->neutral_pulse_us),
        .hpoint = 0U,
    };
    error = ledc_channel_config(&channel);
    if (error != ESP_OK) {
        /* The driver may have routed the pin before returning its failure. */
        (void)gpio_reset_pin((gpio_num_t)config->gpio_pin);
        memset(motor, 0, sizeof(*motor));
        return error;
    }
    motor->output_attached = true;
    motor->initialized = true;
    motor->last_pulse_us = config->neutral_pulse_us;
    return ESP_OK;
}

esp_err_t motor_mode_pwm_set_pulse_us(motor_mode_pwm_t *motor,
                                      uint16_t pulse_us)
{
    return apply_pulse(motor, pulse_us);
}

esp_err_t motor_mode_pwm_stop(motor_mode_pwm_t *motor)
{
    if (!motor || !motor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return apply_pulse(motor, motor->config.neutral_pulse_us);
}

bool motor_mode_pwm_get_last_pulse_us(const motor_mode_pwm_t *motor,
                                      uint16_t *pulse_us)
{
    if (!motor || !motor->initialized || !pulse_us) {
        return false;
    }
    *pulse_us = motor->last_pulse_us;
    return true;
}

void motor_mode_pwm_deinit(motor_mode_pwm_t *motor)
{
    if (!motor) {
        return;
    }
    if (motor->initialized) {
        (void)motor_mode_pwm_stop(motor);
    }
    release_output_hardware(motor);
    memset(motor, 0, sizeof(*motor));
}
