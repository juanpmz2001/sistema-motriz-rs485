#ifndef MOTOR_MODE_PWM_H
#define MOTOR_MODE_PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A motor-mode PWM output.  Its pulse is a bounded direction/speed command;
 * it deliberately does not model a servo position or claim a position was
 * reached.  Closed-loop position belongs to a separate controller.
 */
typedef struct {
    int gpio_pin;
    uint32_t frequency_hz;
    uint8_t duty_resolution_bits;
    ledc_mode_t speed_mode;
    ledc_timer_t timer;
    ledc_channel_t channel;
    uint16_t minimum_pulse_us;
    uint16_t neutral_pulse_us;
    uint16_t maximum_pulse_us;
} motor_mode_pwm_config_t;

typedef struct {
    motor_mode_pwm_config_t config;
    uint16_t last_pulse_us;
    bool output_attached;
    bool initialized;
} motor_mode_pwm_t;

esp_err_t motor_mode_pwm_init(motor_mode_pwm_t *motor,
                              const motor_mode_pwm_config_t *config);
esp_err_t motor_mode_pwm_set_pulse_us(motor_mode_pwm_t *motor,
                                      uint16_t pulse_us);
esp_err_t motor_mode_pwm_stop(motor_mode_pwm_t *motor);
bool motor_mode_pwm_get_last_pulse_us(const motor_mode_pwm_t *motor,
                                      uint16_t *pulse_us);
void motor_mode_pwm_deinit(motor_mode_pwm_t *motor);

#ifdef __cplusplus
}
#endif

#endif
