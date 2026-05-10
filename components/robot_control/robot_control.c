#include "robot_control.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "robot_control";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_MODE LEDC_LOW_SPEED_MODE
#define SERVO_DUTY_RES LEDC_TIMER_14_BIT
#define SERVO_FREQ_HZ 50
#define SERVO_PERIOD_US 20000
#define OTA_SAFE_RPM_THRESHOLD 5
#define OTA_SAFE_FLOAT_THRESHOLD 0.001f

struct robot_control_t {
    robot_control_config_t config;
    SemaphoreHandle_t lock;
    robot_motion_command_t last_command;
};

static const ledc_channel_t SERVO_CHANNELS[SVD48_MOTOR_COUNT] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
};

static float clampf_local(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int16_t clamp_rpm(float rpm, float max_abs_rpm)
{
    rpm = clampf_local(rpm, -max_abs_rpm, max_abs_rpm);
    return (int16_t)lrintf(rpm);
}

static void set_reason(char *reason, size_t reason_size, const char *value)
{
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", value ? value : "UNKNOWN");
    }
}

static void clear_last_command(robot_control_handle_t handle)
{
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    memset(&handle->last_command, 0, sizeof(handle->last_command));
    xSemaphoreGive(handle->lock);
}

static void set_last_motor_rpm(robot_control_handle_t handle, uint8_t motor, int16_t rpm)
{
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->last_command.vx_mps = 0.0f;
    handle->last_command.vy_mps = 0.0f;
    handle->last_command.wz_radps = 0.0f;
    handle->last_command.wheel_rpm[motor] = rpm;
    xSemaphoreGive(handle->lock);
}

static esp_err_t steering_set_angle(robot_control_handle_t handle, uint8_t motor, float angle_deg)
{
    if (!handle || motor >= SVD48_MOTOR_COUNT || !handle->config.enable_steering_servos) {
        return ESP_OK;
    }
    if (handle->config.steering_servo_pins[motor] < 0) {
        return ESP_OK;
    }

    angle_deg = clampf_local(angle_deg, handle->config.servo_min_deg, handle->config.servo_max_deg);
    float span_deg = handle->config.servo_max_deg - handle->config.servo_min_deg;
    float t = span_deg == 0.0f ? 0.5f : (angle_deg - handle->config.servo_min_deg) / span_deg;
    int pulse_us = (int)lrintf(handle->config.servo_min_us +
                               t * (float)(handle->config.servo_max_us - handle->config.servo_min_us));
    uint32_t max_duty = (1U << 14) - 1U;
    uint32_t duty = (uint32_t)((uint64_t)pulse_us * max_duty / SERVO_PERIOD_US);

    ESP_RETURN_ON_ERROR(ledc_set_duty(SERVO_MODE, SERVO_CHANNELS[motor], duty), TAG, "set servo duty failed");
    return ledc_update_duty(SERVO_MODE, SERVO_CHANNELS[motor]);
}

static esp_err_t steering_init(robot_control_handle_t handle)
{
    if (!handle->config.enable_steering_servos) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = SERVO_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "servo timer failed");

    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        if (handle->config.steering_servo_pins[motor] < 0) {
            continue;
        }

        ledc_channel_config_t channel = {
            .gpio_num = handle->config.steering_servo_pins[motor],
            .speed_mode = SERVO_MODE,
            .channel = SERVO_CHANNELS[motor],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = SERVO_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "servo channel failed");
        ESP_RETURN_ON_ERROR(steering_set_angle(handle, motor, 0.0f), TAG, "servo center failed");
    }

    return ESP_OK;
}

robot_control_handle_t robot_control_init(const robot_control_config_t *config)
{
    if (!config || !config->svd48) {
        return NULL;
    }

    robot_control_handle_t handle = calloc(1, sizeof(struct robot_control_t));
    if (!handle) {
        return NULL;
    }

    handle->config = *config;
    if (handle->config.wheelbase_m <= 0.0f) {
        handle->config.wheelbase_m = 0.50f;
    }
    if (handle->config.track_width_m <= 0.0f) {
        handle->config.track_width_m = 0.40f;
    }
    if (handle->config.wheel_radius_m <= 0.0f) {
        handle->config.wheel_radius_m = 0.10f;
    }
    if (handle->config.max_wheel_rpm <= 0.0f) {
        handle->config.max_wheel_rpm = 1000.0f;
    }
    if (handle->config.servo_min_us == 0) {
        handle->config.servo_min_us = 1000;
    }
    if (handle->config.servo_center_us == 0) {
        handle->config.servo_center_us = 1500;
    }
    if (handle->config.servo_max_us == 0) {
        handle->config.servo_max_us = 2000;
    }
    if (handle->config.servo_min_deg == 0.0f && handle->config.servo_max_deg == 0.0f) {
        handle->config.servo_min_deg = -90.0f;
        handle->config.servo_max_deg = 90.0f;
    }

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return NULL;
    }

    if (steering_init(handle) != ESP_OK) {
        robot_control_deinit(handle);
        return NULL;
    }

    ESP_LOGI(TAG, "robot geometry wheelbase=%.3fm track=%.3fm radius=%.3fm max=%.1frpm",
             handle->config.wheelbase_m,
             handle->config.track_width_m,
             handle->config.wheel_radius_m,
             handle->config.max_wheel_rpm);
    return handle;
}

void robot_control_deinit(robot_control_handle_t handle)
{
    if (!handle) {
        return;
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t robot_control_enable_all(robot_control_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_error = ESP_OK;
    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        esp_err_t err = svd48_enable_motor(handle->config.svd48, motor);
        if (err != ESP_OK && first_error == ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}

esp_err_t robot_control_stop_all(robot_control_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_last_command(handle);

    return svd48_stop_all(handle->config.svd48);
}

esp_err_t robot_control_stop_motor(robot_control_handle_t handle, uint8_t motor)
{
    if (!handle || motor >= SVD48_MOTOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = svd48_stop_motor(handle->config.svd48, motor);
    if (err == ESP_OK) {
        set_last_motor_rpm(handle, motor, 0);
    }
    return err;
}

esp_err_t robot_control_clear_motor_alarm(robot_control_handle_t handle, uint8_t motor)
{
    if (!handle || motor >= SVD48_MOTOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return svd48_clear_motor_alarm(handle->config.svd48, motor);
}

esp_err_t robot_control_set_motor_speed(robot_control_handle_t handle, uint8_t motor, int16_t rpm)
{
    if (!handle || motor >= SVD48_MOTOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(svd48_enable_motor(handle->config.svd48, motor), TAG, "enable motor failed");
    esp_err_t err = svd48_set_motor_speed(handle->config.svd48, motor, rpm);
    if (err == ESP_OK) {
        set_last_motor_rpm(handle, motor, rpm);
    }
    return err;
}

esp_err_t robot_control_move_vel(robot_control_handle_t handle, float vx_mps, float vy_mps, float wz_radps)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    const float half_l = handle->config.wheelbase_m * 0.5f;
    const float half_w = handle->config.track_width_m * 0.5f;
    const float xs[SVD48_MOTOR_COUNT] = { half_l, half_l, -half_l, -half_l };
    const float ys[SVD48_MOTOR_COUNT] = { half_w, -half_w, half_w, -half_w };
    float wheel_rpm[SVD48_MOTOR_COUNT];
    float steering_deg[SVD48_MOTOR_COUNT];
    float max_abs_rpm = 0.0f;

    for (uint8_t i = 0; i < SVD48_MOTOR_COUNT; i++) {
        float wheel_vx = vx_mps - wz_radps * ys[i];
        float wheel_vy = vy_mps + wz_radps * xs[i];
        float speed_mps = hypotf(wheel_vx, wheel_vy);
        float angle_rad = speed_mps < 0.0001f ? 0.0f : atan2f(wheel_vy, wheel_vx);
        float rpm = speed_mps * 60.0f / (2.0f * (float)M_PI * handle->config.wheel_radius_m);

        while (angle_rad > (float)M_PI) {
            angle_rad -= 2.0f * (float)M_PI;
        }
        while (angle_rad < -(float)M_PI) {
            angle_rad += 2.0f * (float)M_PI;
        }

        if (angle_rad > (float)M_PI_2) {
            angle_rad -= (float)M_PI;
            rpm = -rpm;
        } else if (angle_rad < -(float)M_PI_2) {
            angle_rad += (float)M_PI;
            rpm = -rpm;
        }

        wheel_rpm[i] = rpm;
        steering_deg[i] = angle_rad * 180.0f / (float)M_PI;
        if (fabsf(rpm) > max_abs_rpm) {
            max_abs_rpm = fabsf(rpm);
        }
    }

    float scale = 1.0f;
    if (max_abs_rpm > handle->config.max_wheel_rpm) {
        scale = handle->config.max_wheel_rpm / max_abs_rpm;
    }

    robot_motion_command_t command = {
        .vx_mps = vx_mps,
        .vy_mps = vy_mps,
        .wz_radps = wz_radps,
    };

    ESP_RETURN_ON_ERROR(robot_control_enable_all(handle), TAG, "enable all failed");

    for (uint8_t i = 0; i < SVD48_MOTOR_COUNT; i++) {
        command.wheel_rpm[i] = clamp_rpm(wheel_rpm[i] * scale, handle->config.max_wheel_rpm);
        command.steering_deg[i] = steering_deg[i];
        ESP_RETURN_ON_ERROR(steering_set_angle(handle, i, command.steering_deg[i]), TAG, "steering failed");
        ESP_RETURN_ON_ERROR(svd48_set_motor_speed(handle->config.svd48, i, command.wheel_rpm[i]), TAG, "set speed failed");
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->last_command = command;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t robot_control_poll_once(robot_control_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return svd48_poll_once(handle->config.svd48);
}

void robot_control_set_trace_enabled(robot_control_handle_t handle, bool enabled)
{
    if (!handle) {
        return;
    }
    svd48_set_trace_enabled(handle->config.svd48, enabled);
}

bool robot_control_get_trace_enabled(robot_control_handle_t handle)
{
    if (!handle) {
        return false;
    }
    return svd48_get_trace_enabled(handle->config.svd48);
}

bool robot_control_get_motor(robot_control_handle_t handle, uint8_t motor, svd48_motor_telemetry_t *telemetry)
{
    if (!handle) {
        return false;
    }
    return svd48_get_motor_telemetry(handle->config.svd48, motor, telemetry);
}

bool robot_control_get_last_motion(robot_control_handle_t handle, robot_motion_command_t *command)
{
    if (!handle || !command) {
        return false;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    *command = handle->last_command;
    xSemaphoreGive(handle->lock);
    return true;
}

bool robot_control_is_safe_for_ota(robot_control_handle_t handle, char *reason, size_t reason_size)
{
    if (!handle) {
        set_reason(reason, reason_size, "NO_ROBOT");
        return false;
    }

    robot_motion_command_t command;
    if (!robot_control_get_last_motion(handle, &command)) {
        set_reason(reason, reason_size, "COMMAND_READ_FAILED");
        return false;
    }

    if (fabsf(command.vx_mps) > OTA_SAFE_FLOAT_THRESHOLD ||
        fabsf(command.vy_mps) > OTA_SAFE_FLOAT_THRESHOLD ||
        fabsf(command.wz_radps) > OTA_SAFE_FLOAT_THRESHOLD) {
        set_reason(reason, reason_size, "COMMAND_ACTIVE");
        return false;
    }

    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        if (command.wheel_rpm[motor] != 0) {
            set_reason(reason, reason_size, "MOTOR_COMMAND_ACTIVE");
            return false;
        }
    }

    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        svd48_motor_telemetry_t telemetry;
        if (!robot_control_get_motor(handle, motor, &telemetry)) {
            continue;
        }
        if (!telemetry.online || telemetry.stale) {
            continue;
        }
        if (abs(telemetry.actual_rpm) > OTA_SAFE_RPM_THRESHOLD) {
            set_reason(reason, reason_size, "MOTOR_RUNNING");
            return false;
        }
    }

    set_reason(reason, reason_size, "SAFE");
    return true;
}

esp_err_t robot_control_prepare_for_ota(robot_control_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    robot_motion_command_t command;
    memset(&command, 0, sizeof(command));
    (void)robot_control_get_last_motion(handle, &command);

    esp_err_t first_error = ESP_OK;
    for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
        svd48_motor_telemetry_t telemetry;
        bool online = robot_control_get_motor(handle, motor, &telemetry) && telemetry.online && !telemetry.stale;
        bool commanded = command.wheel_rpm[motor] != 0;
        if (!online && !commanded) {
            continue;
        }

        esp_err_t err = robot_control_stop_motor(handle, motor);
        if (err != ESP_OK && first_error == ESP_OK) {
            first_error = err;
        }
    }

    if (first_error == ESP_OK) {
        clear_last_command(handle);
    }
    return first_error;
}

esp_err_t robot_control_read_svd48_registers(robot_control_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t quantity, uint16_t *out_regs)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return svd48_read_registers_by_id(handle->config.svd48, drive_id, reg, quantity, out_regs);
}

esp_err_t robot_control_write_svd48_register(robot_control_handle_t handle, uint8_t drive_id, uint16_t reg, uint16_t value)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return svd48_write_register_by_id(handle->config.svd48, drive_id, reg, value);
}
