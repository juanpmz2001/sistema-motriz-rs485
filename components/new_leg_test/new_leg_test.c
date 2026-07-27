#include "new_leg_test.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_timer.h"

#define SERVO_GPIO GPIO_NUM_14
#define SENSOR_GPIO GPIO_NUM_41
#define SERVO_FREQUENCY_HZ 50
#define SERVO_PERIOD_US 20000
#define SERVO_NEUTRAL_US 1500
#define SERVO_SAFE_MIN_US 1300
#define SERVO_SAFE_MAX_US 1700
#define SENSOR_MIN_PERIOD_US 900
#define SENSOR_MAX_PERIOD_US 10000
#define SENSOR_STALE_MS 250

struct new_leg_test_t {
    portMUX_TYPE lock;
    volatile int64_t last_rise_us;
    volatile uint32_t period_us;
    volatile uint32_t high_us;
    volatile int64_t sample_us;
};

static void IRAM_ATTR sensor_edge_isr(void *arg)
{
    new_leg_test_handle_t handle = (new_leg_test_handle_t)arg;
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(SENSOR_GPIO);

    portENTER_CRITICAL_ISR(&handle->lock);
    if (level) {
        if (handle->last_rise_us > 0) {
            handle->period_us = (uint32_t)(now - handle->last_rise_us);
        }
        handle->last_rise_us = now;
    } else if (handle->last_rise_us > 0) {
        handle->high_us = (uint32_t)(now - handle->last_rise_us);
        handle->sample_us = now;
    }
    portEXIT_CRITICAL_ISR(&handle->lock);
}

esp_err_t new_leg_test_set_pulse_us(new_leg_test_handle_t handle, uint32_t pulse_us)
{
    if (!handle || pulse_us < SERVO_SAFE_MIN_US || pulse_us > SERVO_SAFE_MAX_US) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t max_duty = (1U << LEDC_TIMER_14_BIT) - 1U;
    uint32_t duty = (uint32_t)(((uint64_t)pulse_us * max_duty) / SERVO_PERIOD_US);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty),
                        "new_leg", "set duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t new_leg_test_stop(new_leg_test_handle_t handle)
{
    return new_leg_test_set_pulse_us(handle, SERVO_NEUTRAL_US);
}

esp_err_t new_leg_test_init(new_leg_test_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    new_leg_test_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }
    ledc_channel_config_t channel = {
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }
    err = new_leg_test_stop(handle);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    gpio_config_t input = {
        .pin_bit_mask = 1ULL << SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    err = gpio_config(&input);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(SENSOR_GPIO, sensor_edge_isr, handle);
    }
    if (err != ESP_OK) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        free(handle);
        return err;
    }

    *out_handle = handle;
    return ESP_OK;
}

void new_leg_test_deinit(new_leg_test_handle_t handle)
{
    if (!handle) {
        return;
    }
    gpio_isr_handler_remove(SENSOR_GPIO);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    free(handle);
}

esp_err_t new_leg_test_get_sensor(new_leg_test_handle_t handle, new_leg_sensor_t *sensor)
{
    if (!handle || !sensor) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t period_us;
    uint32_t high_us;
    int64_t sample_us;
    portENTER_CRITICAL(&handle->lock);
    period_us = handle->period_us;
    high_us = handle->high_us;
    sample_us = handle->sample_us;
    portEXIT_CRITICAL(&handle->lock);

    memset(sensor, 0, sizeof(*sensor));
    int64_t now = esp_timer_get_time();
    sensor->age_ms = sample_us > 0 ? (uint32_t)((now - sample_us) / 1000) : UINT32_MAX;
    sensor->period_us = period_us;
    sensor->high_us = high_us;
    sensor->valid = sample_us > 0 &&
                    sensor->age_ms <= SENSOR_STALE_MS &&
                    period_us >= SENSOR_MIN_PERIOD_US &&
                    period_us <= SENSOR_MAX_PERIOD_US &&
                    high_us <= period_us;
    if (sensor->valid) {
        sensor->duty_percent = 100.0f * (float)high_us / (float)period_us;
        float normalized = (sensor->duty_percent - 2.9f) / (97.1f - 2.9f);
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        sensor->angle_deg = normalized * 360.0f;
    }
    return ESP_OK;
}
