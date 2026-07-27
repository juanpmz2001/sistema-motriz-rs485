#ifndef NEW_LEG_TEST_H
#define NEW_LEG_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct new_leg_test_t *new_leg_test_handle_t;

typedef struct {
    bool valid;
    uint32_t age_ms;
    uint32_t period_us;
    uint32_t high_us;
    float duty_percent;
    float angle_deg;
} new_leg_sensor_t;

esp_err_t new_leg_test_init(new_leg_test_handle_t *out_handle);
void new_leg_test_deinit(new_leg_test_handle_t handle);
esp_err_t new_leg_test_set_pulse_us(new_leg_test_handle_t handle, uint32_t pulse_us);
esp_err_t new_leg_test_stop(new_leg_test_handle_t handle);
esp_err_t new_leg_test_get_sensor(new_leg_test_handle_t handle, new_leg_sensor_t *sensor);

#endif
