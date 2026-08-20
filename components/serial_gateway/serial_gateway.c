#include "serial_gateway.h"

#include "as5600_diagnostics_port.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "serial_gateway_policy.h"

static const char *TAG = "serial_gateway";

#define GATEWAY_LINE_MAX SERIAL_GATEWAY_COMMAND_MAX
#define GATEWAY_ARG_MAX 20
#define GATEWAY_DEFAULT_STREAM_MS 200
#define SVD48_PY6514_POLE_PAIRS 10
#define SVD48_PY6514_SENSOR_HALL 1
#define SVD48_PY6514_WHEEL_DIAMETER_MM 330
#define SVD48_PY6514_MOTOR_TEETH 1
#define SVD48_PY6514_WHEEL_TEETH 5
#define SVD48_CHANNEL_ALL (-1)
#define GATEWAY_RX_IDLE_TICKS 1
#define GATEWAY_RX_DRAIN_MAX 256
#define GATEWAY_RX_TASK_STACK 12288
#define GATEWAY_STREAM_TASK_STACK 4096
#define GATEWAY_COMMAND_LOCK_TIMEOUT_MS 1000
#define GATEWAY_OUTPUT_CHUNK_MAX 768
#define AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX 128U
#define MAINTENANCE_LAN_DEFAULT_PORT 32321
#define PLATFORM_SAFE_RPM_THRESHOLD 5
#define PLATFORM_SAFE_FLOAT_THRESHOLD 0.001f
#define SVD48_MAINTENANCE_WRITE_MAX_REGISTERS 8

struct serial_gateway_t {
    serial_gateway_config_t config;
    SemaphoreHandle_t print_lock;
    SemaphoreHandle_t command_lock;
    TaskHandle_t rx_task;
    TaskHandle_t stream_task;
    TaskHandle_t active_output_task;
    serial_gateway_output_fn_t active_output;
    void *active_output_ctx;
    bool running;
    bool stream_enabled;
    uint32_t stream_period_ms;
};

static void print_locked(serial_gateway_handle_t handle, const char *fmt, ...)
{
    va_list args;
    xSemaphoreTake(handle->print_lock, portMAX_DELAY);

    bool use_callback = handle->active_output &&
                        xTaskGetCurrentTaskHandle() == handle->active_output_task;
    if (use_callback) {
        char chunk[GATEWAY_OUTPUT_CHUNK_MAX];
        va_start(args, fmt);
        vsnprintf(chunk, sizeof(chunk), fmt, args);
        va_end(args);
        handle->active_output(handle->active_output_ctx, chunk);
    } else {
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
    }

    xSemaphoreGive(handle->print_lock);
}

static void print_prompt(serial_gateway_handle_t handle)
{
    if (handle->config.print_prompt) {
        print_locked(handle, "> ");
    }
}

static char *trim(char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)*(end - 1))) {
        *(--end) = '\0';
    }
    return line;
}

static int split_args(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *cursor = line;

    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        if (argc >= max_args) {
            break;
        }

        argv[argc++] = cursor;
        char *out = cursor;
        bool in_quotes = false;
        char quote = '\0';

        while (*cursor) {
            char c = *cursor++;

            if (in_quotes) {
                if (c == '\\' && *cursor) {
                    *out++ = *cursor++;
                    continue;
                }
                if (c == quote) {
                    in_quotes = false;
                    continue;
                }
                *out++ = c;
                continue;
            }

            if (c == '\\' && *cursor) {
                *out++ = *cursor++;
                continue;
            }
            if (c == '"' || c == '\'') {
                in_quotes = true;
                quote = c;
                continue;
            }
            if (isspace((unsigned char)c)) {
                break;
            }
            *out++ = c;
        }

        if (in_quotes) {
            return -1;
        }
        *out = '\0';
    }

    return argc;
}

static bool command_allowed_for_policy(serial_gateway_command_policy_t policy, int argc, char *argv[])
{
    if (policy == SERIAL_GATEWAY_POLICY_FULL_SERIAL) {
        return true;
    }
    if (policy != SERIAL_GATEWAY_POLICY_LAN_SAFE || argc <= 0 || argc > GATEWAY_ARG_MAX) {
        return false;
    }

    const char *lan_argv[GATEWAY_ARG_MAX] = { 0 };
    for (int i = 0; i < argc; i++) {
        lan_argv[i] = argv[i];
    }
    return serial_gateway_lan_command_allowed(argc, lan_argv);
}

static size_t configured_motor_count(serial_gateway_handle_t handle)
{
    size_t count = handle && handle->config.actuation
                       ? actuation_application_legacy_motor_count(
                             handle->config.actuation)
                       : 0U;
    return count > 0U ? count
                      : handle ? robot_control_get_motor_count(handle->config.robot)
                               : 0U;
}

static bool parse_motor_arg(serial_gateway_handle_t handle,
                            const char *text,
                            uint8_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    size_t motor_count = configured_motor_count(handle);
    if (*text == '\0' || *end != '\0' || parsed < 0 ||
        (size_t)parsed >= motor_count) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool parse_drive_id_arg(const char *text, uint8_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 0);
    if (*text == '\0' || *end != '\0' || parsed <= 0 || parsed > 255) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool parse_u16_any_arg(const char *text, uint16_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 0);
    if (*text == '\0' || *end != '\0' || parsed < 0 || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool parse_endpoint_id_arg(const char *text,
                                  robot_endpoint_id_t *value)
{
    uint16_t parsed = 0U;
    if (!value || !parse_u16_any_arg(text, &parsed) || parsed == 0U) {
        return false;
    }
    *value = (robot_endpoint_id_t)parsed;
    return true;
}

static bool parse_u32_any_arg(const char *text, uint32_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (*text == '\0' || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_channel_arg(const char *text, int8_t *channel)
{
    if (!text || !channel) {
        return false;
    }
    if (strcasecmp(text, "ALL") == 0) {
        *channel = SVD48_CHANNEL_ALL;
        return true;
    }
    if (strcasecmp(text, "M1") == 0 || strcmp(text, "1") == 0) {
        *channel = 0;
        return true;
    }
    if (strcasecmp(text, "M2") == 0 || strcmp(text, "2") == 0) {
        *channel = 1;
        return true;
    }
    return false;
}

static bool parse_i16_arg(const char *text, int16_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || parsed < INT16_MIN || parsed > INT16_MAX) {
        return false;
    }
    *value = (int16_t)parsed;
    return true;
}

static bool parse_float_arg(const char *text, float *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (*text == '\0' || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_on_off_arg(const char *text, bool *enabled)
{
    if (!text || !enabled) {
        return false;
    }
    if (strcasecmp(text, "ON") == 0) {
        *enabled = true;
        return true;
    }
    if (strcasecmp(text, "OFF") == 0) {
        *enabled = false;
        return true;
    }
    return false;
}

static bool parse_rollback_test_mode_arg(const char *text, ota_manager_rollback_test_mode_t *mode)
{
    if (!text || !mode) {
        return false;
    }
    if (strcasecmp(text, "NONE") == 0 || strcasecmp(text, "CLEAR") == 0) {
        *mode = OTA_MANAGER_ROLLBACK_TEST_NONE;
        return true;
    }
    if (strcasecmp(text, "NO_CONFIRM_ONCE") == 0) {
        *mode = OTA_MANAGER_ROLLBACK_TEST_NO_CONFIRM_ONCE;
        return true;
    }
    if (strcasecmp(text, "SELF_TEST_FAIL_ONCE") == 0 || strcasecmp(text, "SELFTEST_FAIL_ONCE") == 0) {
        *mode = OTA_MANAGER_ROLLBACK_TEST_SELF_TEST_FAIL_ONCE;
        return true;
    }
    return false;
}

static bool parse_ibus_mode_arg(const char *text, ibus_receiver_mode_t *mode)
{
    if (!text || !mode) {
        return false;
    }
    if (strcasecmp(text, "IBUS") == 0) {
        *mode = IBUS_RECEIVER_MODE_IBUS;
        return true;
    }
    if (strcasecmp(text, "IBUS_INV") == 0 || strcasecmp(text, "IBUS_INVERTED") == 0) {
        *mode = IBUS_RECEIVER_MODE_IBUS_INVERTED;
        return true;
    }
    if (strcasecmp(text, "IBUS_8N2") == 0) {
        *mode = IBUS_RECEIVER_MODE_IBUS_8N2;
        return true;
    }
    if (strcasecmp(text, "IBUS_INV_8N2") == 0 || strcasecmp(text, "IBUS_INVERTED_8N2") == 0) {
        *mode = IBUS_RECEIVER_MODE_IBUS_INVERTED_8N2;
        return true;
    }
    if (strcasecmp(text, "SBUS") == 0) {
        *mode = IBUS_RECEIVER_MODE_SBUS;
        return true;
    }
    if (strcasecmp(text, "SBUS_NOINV") == 0 || strcasecmp(text, "SBUS_NONINV") == 0) {
        *mode = IBUS_RECEIVER_MODE_SBUS_NON_INVERTED;
        return true;
    }
    if (strcasecmp(text, "PPM") == 0) {
        *mode = IBUS_RECEIVER_MODE_PPM;
        return true;
    }
    return false;
}

static void print_motor_full(serial_gateway_handle_t handle, uint8_t motor)
{
    svd48_motor_telemetry_t t;
    if (!robot_control_get_motor(handle->config.robot, motor, &t)) {
        print_locked(handle, "ERR BAD_MOTOR\n");
        return;
    }

    robot_motion_command_t cmd;
    float steering_deg = 0.0f;
    if (robot_control_get_last_motion(handle->config.robot, &cmd)) {
        steering_deg = cmd.steering_deg[motor];
    }

    uint32_t exception_age_ms = t.last_exception_ms == 0
                                    ? 0
                                    : (uint32_t)(esp_timer_get_time() / 1000ULL) - t.last_exception_ms;

    print_locked(handle,
                 "DATA MOTOR_%u RPM:%d CURRENT_DA:%d STEER_DEG:%.1f STATUS:%d BUS_DV:%d MOTOR_TEMP_DC:%d MOS_TEMP_DC:%d POS:%ld ERROR:0x%08lx ONLINE:%u STALE:%u COMM_ERR:%u EXC_FUNC:0x%02X EXC_CODE:0x%02X EXC_AGE_MS:%lu\n",
                 motor,
                 t.actual_rpm,
                 t.current_deciamp,
                 steering_deg,
                 t.status,
                 t.bus_voltage_deciv,
                 t.motor_temp_decic,
                 t.mos_temp_decic,
                 (long)t.position_counts,
                 (unsigned long)t.error_code,
                 t.online ? 1 : 0,
                 t.stale ? 1 : 0,
                 (unsigned)t.last_error,
                 t.last_exception_function,
                 t.last_exception_code,
                 (unsigned long)exception_age_ms);
}

static const char *sensor_type_name(uint16_t sensor_type)
{
    switch (sensor_type) {
        case 0:
            return "ENCODER";
        case 1:
            return "HALL";
        case 2:
            return "STRING_ENCODER";
        default:
            return "UNKNOWN";
    }
}

static const char *channel_name(uint8_t channel)
{
    return channel == 0 ? "M1" : "M2";
}

static const char *safe_text(const char *value, const char *fallback)
{
    return (value && value[0] != '\0') ? value : fallback;
}

/* Calibration metadata originates in the immutable build profile, but it is
 * still text carried over a whitespace-delimited serial protocol. Keep every
 * token bounded and wire-safe so an unexpectedly long or malformed profile
 * string cannot truncate a diagnostic response line. The 128-character bound
 * preserves a full SHA-256 provenance value and ordinary calibration IDs. */
static void as5600_diagnostics_token(const char *value,
                                     const char *fallback,
                                     char output[AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX +
                                                 1U],
                                     bool *truncated,
                                     bool *sanitized)
{
    const char *source = safe_text(value, fallback);
    bool token_truncated = false;
    bool token_sanitized = false;
    size_t index = 0U;
    for (; source[index] != '\0' &&
           index < AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX;
         ++index) {
        const unsigned char character = (unsigned char)source[index];
        if (isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == ':' || character == '/' ||
            character == '@') {
            output[index] = (char)character;
        } else {
            output[index] = '_';
            token_sanitized = true;
        }
    }
    if (source[index] != '\0') {
        token_truncated = true;
    }
    output[index] = '\0';
    if (truncated != NULL) {
        *truncated = token_truncated;
    }
    if (sanitized != NULL) {
        *sanitized = token_sanitized;
    }
}

static const char *endpoint_health_name(robot_endpoint_health_t health)
{
    switch (health) {
    case ROBOT_ENDPOINT_HEALTH_HEALTHY:
        return "HEALTHY";
    case ROBOT_ENDPOINT_HEALTH_DEGRADED:
        return "DEGRADED";
    case ROBOT_ENDPOINT_HEALTH_OFFLINE:
        return "OFFLINE";
    case ROBOT_ENDPOINT_HEALTH_FAULT:
        return "FAULT";
    case ROBOT_ENDPOINT_HEALTH_STALE:
        return "STALE";
    case ROBOT_ENDPOINT_HEALTH_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

/* Concrete device diagnostic names intentionally stay out of the generic
 * endpoint/position-observation handlers below. */
static const char *as5600_device_result_name(as5600_device_result_t result)
{
    switch (result) {
    case AS5600_DEVICE_OK:
        return "OK";
    case AS5600_DEVICE_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case AS5600_DEVICE_NOT_READY:
        return "NOT_READY";
    case AS5600_DEVICE_BUS_BUSY:
        return "BUS_BUSY";
    case AS5600_DEVICE_TIMEOUT:
        return "TIMEOUT";
    case AS5600_DEVICE_IO_ERROR:
        return "IO_ERROR";
    case AS5600_DEVICE_BAD_RESPONSE:
        return "BAD_RESPONSE";
    case AS5600_DEVICE_PARTIAL:
        return "PARTIAL";
    default:
        return "UNKNOWN";
    }
}

static const char *as5600_device_health_name(as5600_device_health_t health)
{
    switch (health) {
    case AS5600_DEVICE_HEALTH_HEALTHY:
        return "HEALTHY";
    case AS5600_DEVICE_HEALTH_DEGRADED:
        return "DEGRADED";
    case AS5600_DEVICE_HEALTH_OFFLINE:
        return "OFFLINE";
    case AS5600_DEVICE_HEALTH_STALE:
        return "STALE";
    case AS5600_DEVICE_HEALTH_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *velocity_observation_source_name(
    robot_velocity_observation_source_t source)
{
    return source == ROBOT_VELOCITY_OBSERVATION_SOURCE_DEVICE_FEEDBACK
               ? "DEVICE_FEEDBACK"
               : "UNKNOWN";
}

static const char *position_observation_source_name(
    robot_position_observation_source_t source)
{
    switch (source) {
    case ROBOT_POSITION_OBSERVATION_SOURCE_DEVICE_FEEDBACK:
        return "DEVICE_FEEDBACK";
    case ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR:
        return "INDEPENDENT_SENSOR";
    case ROBOT_POSITION_OBSERVATION_SOURCE_INFERRED:
        return "INFERRED";
    case ROBOT_POSITION_OBSERVATION_SOURCE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *capability_error_name(robot_capability_error_t error)
{
    switch (error) {
    case ROBOT_CAP_OK:
        return "OK";
    case ROBOT_CAP_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case ROBOT_CAP_UNAVAILABLE:
        return "UNAVAILABLE";
    case ROBOT_CAP_UNSUPPORTED:
        return "UNSUPPORTED";
    case ROBOT_CAP_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case ROBOT_CAP_IO_ERROR:
        return "IO_ERROR";
    default:
        return "UNKNOWN";
    }
}

static const char *endpoint_criticality_name(
    robot_endpoint_criticality_t criticality)
{
    switch (criticality) {
    case ROBOT_ENDPOINT_REQUIRED:
        return "REQUIRED";
    case ROBOT_ENDPOINT_OPTIONAL:
        return "OPTIONAL";
    case ROBOT_ENDPOINT_DEVELOPMENT:
        return "DEVELOPMENT";
    default:
        return "UNKNOWN";
    }
}

static const char *application_result_name(
    actuation_application_result_t result)
{
    switch (result) {
    case ACTUATION_APPLICATION_FAILED:
        return "FAILED";
    case ACTUATION_APPLICATION_PARTIAL:
        return "PARTIAL";
    case ACTUATION_APPLICATION_TIMEOUT:
        return "TIMEOUT";
    case ACTUATION_APPLICATION_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case ACTUATION_APPLICATION_OK:
        return "OK";
    default:
        return "UNKNOWN";
    }
}

static uint32_t gateway_monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool motion_command_active(const robot_motion_command_t *command,
                                  size_t motor_count)
{
    if (!command) {
        return false;
    }
    if (fabsf(command->vx_mps) > PLATFORM_SAFE_FLOAT_THRESHOLD ||
        fabsf(command->vy_mps) > PLATFORM_SAFE_FLOAT_THRESHOLD ||
        fabsf(command->wz_radps) > PLATFORM_SAFE_FLOAT_THRESHOLD) {
        return true;
    }
    for (uint8_t motor = 0; motor < motor_count; motor++) {
        if (command->wheel_rpm[motor] != 0) {
            return true;
        }
    }
    return false;
}

static void handle_platform_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE PLATFORM_STATUS\n");
        return;
    }

    robot_motion_command_t command = {0};
    bool have_command = robot_control_get_last_motion(handle->config.robot, &command);
    size_t motor_count = configured_motor_count(handle);
    bool command_active = have_command && motion_command_active(&command,
                                                                motor_count);
    uint32_t last_age_ms = 0;
    if (have_command && command.sequence != 0) {
        last_age_ms = gateway_monotonic_ms() - command.issued_ms;
    }

    uint8_t online_count = 0;
    uint8_t stale_count = 0;
    uint8_t running_count = 0;
    uint8_t faulted_count = 0;
    for (uint8_t motor = 0; motor < motor_count; motor++) {
        svd48_motor_telemetry_t telemetry;
        if (!robot_control_get_motor(handle->config.robot, motor, &telemetry)) {
            continue;
        }
        if (telemetry.online) {
            online_count++;
        }
        if (telemetry.stale) {
            stale_count++;
        }
        if (telemetry.online && !telemetry.stale && abs(telemetry.actual_rpm) > PLATFORM_SAFE_RPM_THRESHOLD) {
            running_count++;
        }
        if (telemetry.error_code != 0) {
            faulted_count++;
        }
    }

    char safe_reason[32];
    bool safe_for_ota = robot_control_is_safe_for_ota(handle->config.robot, safe_reason, sizeof(safe_reason));
    bool motion_active = command_active || running_count > 0;
    const char *state = "SAFE_IDLE";
    if (faulted_count > 0) {
        state = "FAULT";
    } else if (motion_active) {
        state = "MOTION_ACTIVE";
    }

    print_locked(handle,
                 "DATA PLATFORM STATE:%s AUTHORITY:SERIAL_ASCII PROTOCOL:ASCII_V1 HEARTBEAT:UNSUPPORTED ESTOP:UNSUPPORTED LAST_SEQ:%lu LAST_AGE_MS:%lu MOTION_ACTIVE:%u SAFE_FOR_OTA:%u SAFE_REASON:%s ONLINE:%u STALE:%u RUNNING:%u FAULTED:%u TRACE:%u STREAM:%u\n",
                 state,
                 (unsigned long)command.sequence,
                 (unsigned long)last_age_ms,
                 motion_active ? 1 : 0,
                 safe_for_ota ? 1 : 0,
                 safe_reason,
                 online_count,
                 stale_count,
                 running_count,
                 faulted_count,
                 robot_control_get_trace_enabled(handle->config.robot) ? 1 : 0,
                 handle->stream_enabled ? 1 : 0);
}

static void handle_version(serial_gateway_handle_t handle)
{
    ota_manager_boot_state_t boot_state;
    esp_err_t state_err = ota_manager_get_boot_state(&boot_state);
    const char *partition_label = state_err == ESP_OK && boot_state.partition_label[0]
                                      ? boot_state.partition_label
                                      : "UNKNOWN";
    const char *ota_state = state_err == ESP_OK && boot_state.state_known
                                ? ota_manager_image_state_to_string(boot_state.state)
                                : "UNKNOWN";

    print_locked(handle,
                 "DATA VERSION PROJECT:%s TARGET:%s VERSION:%s BUILD_NUMBER:%lu IDF:%s PARTITION:%s OTA_STATE:%s PENDING_VERIFY:%u ROLLBACK_POSSIBLE:%u GIT_SHA:%s GIT_DIRTY:%u\n",
                 safe_text(handle->config.fw_project, "UNKNOWN"),
                 safe_text(handle->config.fw_target, "UNKNOWN"),
                 safe_text(handle->config.fw_version, "UNKNOWN"),
                 (unsigned long)handle->config.fw_build_number,
                 esp_get_idf_version(),
                 safe_text(partition_label, "UNKNOWN"),
                 ota_state,
                 state_err == ESP_OK && boot_state.pending_verify ? 1 : 0,
                 state_err == ESP_OK && boot_state.rollback_possible ? 1 : 0,
                 safe_text(handle->config.fw_git_sha, "UNKNOWN"),
                 handle->config.fw_git_dirty ? 1U : 0U);
}

static esp_err_t get_config_snapshot(serial_gateway_handle_t handle, config_manager_snapshot_t *snapshot)
{
    if (!handle->config.config_manager) {
        return ESP_ERR_INVALID_STATE;
    }
    return config_manager_get_snapshot(handle->config.config_manager, snapshot);
}

static esp_err_t get_wifi_status(serial_gateway_handle_t handle, wifi_manager_status_t *status)
{
    if (!handle->config.wifi_manager) {
        return ESP_ERR_INVALID_STATE;
    }
    return wifi_manager_get_status(handle->config.wifi_manager, status);
}

static void print_wifi_status(serial_gateway_handle_t handle, const wifi_manager_status_t *status)
{
    print_locked(handle,
                 "DATA WIFI STATUS:%s SSID:%s IP:%s RSSI:%d RETRIES:%u/%u DISCONNECT_REASON:%u LAST_ERR:0x%x AUTOCONNECT:%s PAUSED:%u RETRY_DELAY_MS:%lu\n",
                 wifi_manager_state_to_string(status->state),
                 status->ssid[0] ? status->ssid : "<empty>",
                 status->ip_addr[0] ? status->ip_addr : "<none>",
                 status->rssi,
                 status->retry_count,
                 status->max_retries,
                 status->disconnect_reason,
                 status->last_error,
                 status->auto_connect_running ? "RUNNING" : "STOPPED",
                 status->auto_connect_paused ? 1 : 0,
                 (unsigned long)status->auto_retry_delay_ms);
}

static void handle_safety_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE SAFETY_STATUS\n");
        return;
    }
    if (!handle->config.robot_safety) {
        print_locked(handle, "ERR ROBOT_SAFETY_UNAVAILABLE\n");
        return;
    }

    robot_safety_status_t status;
    esp_err_t err = robot_safety_get_status(handle->config.robot_safety, &status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR SAFETY_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA SAFETY TASK:%s RC_AVAILABLE:%u RC_SEEN:%u RC_VALID:%u RC_LOSS:%u RC_LAST_AGE_MS:%lu RC_INTERLOCK:%s RC_CH5_US:%u LAN_ELIGIBLE:%u LAN_REVOCATION_EPOCH:%lu MOTOR_FAULT:%u STOP_REQUESTS:%lu LAST_STOP_REASON:%s LAST_STOP_ERR:0x%x LOOPS:%lu\n",
                 status.task_running ? "RUNNING" : "STOPPED",
                 status.rc_available ? 1 : 0,
                 status.rc_signal_seen ? 1 : 0,
                 status.rc_signal_valid ? 1 : 0,
                 status.rc_loss_active ? 1 : 0,
                 (unsigned long)status.rc_last_frame_age_ms,
                 robot_safety_rc_lan_interlock_state_name(
                     status.rc_lan_interlock_state),
                 status.rc_lan_channel_us,
                 status.lan_control_allowed ? 1U : 0U,
                 (unsigned long)status.rc_lan_priority_epoch,
                 status.motor_fault_active ? 1 : 0,
                 (unsigned long)status.stop_requests,
                 status.last_stop_reason[0] ? status.last_stop_reason : "NONE",
                 status.last_stop_error,
                 (unsigned long)status.loop_count);
}

static void handle_control_status(serial_gateway_handle_t handle,
                                  int argc,
                                  char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE CONTROL_STATUS\n");
        return;
    }
    motion_status_snapshot_t status;
    if (!motion_status_snapshot(handle->config.motion_status, &status) ||
        !status.available) {
        print_locked(handle, "ERR CONTROL_UNAVAILABLE\n");
        return;
    }

    robot_safety_status_t safety = {0};
    bool safety_available = handle->config.robot_safety &&
                            robot_safety_get_status(handle->config.robot_safety,
                                                    &safety) == ESP_OK;

    print_locked(
        handle,
        "DATA CONTROL TASK:%s STATE:%s SOURCE:%s DEADMAN:%u TTL_MS:%lu LEASE_FRESH:%u LEASE_AGE_MS:%lu LEASE_REMAINING_MS:%lu STREAM_HASH:%016llx SEQUENCE:%llu MAX_VX_MPS:%.4f MAX_VY_MPS:%.4f MAX_WZ_RADPS:%.4f REQUESTED_VX_MPS:%.4f REQUESTED_VY_MPS:%.4f REQUESTED_WZ_RADPS:%.4f ENDPOINTS:%u DETAIL:%s\n",
        status.task_running ? "RUNNING" : "STOPPED",
        motion_control_state_name(status.state),
        motion_control_source_name(status.source),
        status.deadman ? 1U : 0U,
        (unsigned long)status.command_ttl_ms,
        status.lease_fresh ? 1U : 0U,
        (unsigned long)status.lease_age_ms,
        (unsigned long)status.lease_remaining_ms,
        (unsigned long long)status.stream_id_hash,
        (unsigned long long)status.sequence,
        (double)status.max_vx_mps,
        (double)status.max_vy_mps,
        (double)status.max_wz_radps,
        (double)status.requested_vx_mps,
        (double)status.requested_vy_mps,
        (double)status.requested_wz_radps,
        (unsigned)status.endpoint_count,
        safe_text(status.last_detail, "UNKNOWN"));
    print_locked(handle,
                 "DATA CONTROL_AUTHORITY LAN_ELIGIBLE:%u RC_INTERLOCK:%s RC_CH5_US:%u LAN_REVOCATION_EPOCH:%lu\n",
                 safety_available && safety.lan_control_allowed ? 1U : 0U,
                 safety_available ? robot_safety_rc_lan_interlock_state_name(
                                       safety.rc_lan_interlock_state)
                                  : "UNAVAILABLE",
                 safety_available ? safety.rc_lan_channel_us : 0U,
                 (unsigned long)(safety_available ? safety.rc_lan_priority_epoch : 0U));
    for (size_t index = 0U; index < status.endpoint_count; ++index) {
        const motion_status_endpoint_t *endpoint = &status.endpoints[index];
        print_locked(
            handle,
            "DATA CONTROL_ENDPOINT ID:%u NAME:%s TARGET_RPM:%d OBSERVED_VALID:%u OBSERVED_RPM:%d OBSERVATION_MS:%lu ONLINE:%u STALE:%u HEALTH:%s\n",
            (unsigned)endpoint->endpoint_id,
            safe_text(endpoint->name, "UNKNOWN"),
            endpoint->target_rpm,
            endpoint->observed_valid ? 1U : 0U,
            endpoint->observed_rpm,
            (unsigned long)endpoint->observation_timestamp_ms,
            endpoint->online ? 1U : 0U,
            endpoint->stale ? 1U : 0U,
            endpoint_health_name(endpoint->health));
    }
}

static bool reject_continuous_control_conflict(serial_gateway_handle_t handle,
                                               const char *operation)
{
    motion_control_state_t state = MOTION_CONTROL_UNAVAILABLE;
    if (!motion_status_blocks_maintenance_changes(handle->config.motion_status,
                                                   &state)) {
        return false;
    }
    print_locked(handle,
                 "ERR CONTINUOUS_CONTROL_CONFLICT OPERATION:%s STATE:%s\n",
                 operation,
                 motion_control_state_name(state));
    return true;
}

static void handle_config_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE CONFIG_STATUS\n");
        return;
    }

    config_manager_snapshot_t snapshot;
    esp_err_t err = get_config_snapshot(handle, &snapshot);
    if (err != ESP_OK) {
        print_locked(handle, "ERR CONFIG_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA CONFIG WIFI_SSID:%s WIFI_PASSWORD:%s OTA_HOST:%s OTA_PORT:%u OTA_MANIFEST:%s OTA_AUTO_CHECK:%u OTA_AUTO_INTERVAL_MS:%lu OTA_AUTO_UPDATE:%u OTA_ANNOUNCE_TOKEN:%s MAINT_LAN_TOKEN:%s\n",
                 snapshot.wifi_ssid[0] ? snapshot.wifi_ssid : "<empty>",
                 snapshot.wifi_password_set ? "<set>" : "<empty>",
                 snapshot.ota_server_host,
                 snapshot.ota_server_port,
                 snapshot.ota_manifest_path,
                 snapshot.ota_auto_check_enabled ? 1 : 0,
                 (unsigned long)snapshot.ota_auto_check_interval_ms,
                 snapshot.ota_auto_update_enabled ? 1 : 0,
                 snapshot.ota_announce_token_set ? "<set>" : "<empty>",
                 snapshot.maintenance_lan_token_set ? "<set>" : "<empty>");
}

static void handle_maintenance_lan_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE MAINT_LAN_STATUS\n");
        return;
    }

    config_manager_snapshot_t snapshot;
    esp_err_t err = get_config_snapshot(handle, &snapshot);
    if (err != ESP_OK) {
        print_locked(handle, "ERR MAINT_LAN_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA MAINT_LAN PORT:%u TOKEN:%s POLICY:LAN_SAFE\n",
                 MAINTENANCE_LAN_DEFAULT_PORT,
                 snapshot.maintenance_lan_token_set ? "<set>" : "<empty>");
}

static void handle_maintenance_lan_token_set(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE MAINT_TOKEN_SET token\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_maintenance_lan_token(handle->config.config_manager, argv[1]);
    if (err == ESP_OK) {
        print_locked(handle, "OK MAINT_TOKEN_SET TOKEN:<set>\n");
    } else {
        print_locked(handle, "ERR MAINT_TOKEN_SET_FAILED 0x%x\n", err);
    }
}

static void handle_maintenance_lan_token_clear(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE MAINT_TOKEN_CLEAR\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_clear_maintenance_lan_token(handle->config.config_manager);
    if (err == ESP_OK) {
        print_locked(handle, "OK MAINT_TOKEN_CLEAR\n");
    } else {
        print_locked(handle, "ERR MAINT_TOKEN_CLEAR_FAILED 0x%x\n", err);
    }
}

static void handle_config_clear(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE CONFIG_CLEAR\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_clear(handle->config.config_manager);
    if (err == ESP_OK) {
        if (handle->config.wifi_manager) {
            (void)wifi_manager_disconnect(handle->config.wifi_manager);
        }
        print_locked(handle, "OK CONFIG_CLEAR\n");
    } else {
        print_locked(handle, "ERR CONFIG_CLEAR_FAILED 0x%x\n", err);
    }
}

static void handle_wifi_set(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 3) {
        print_locked(handle, "ERR USAGE WIFI_SET \"ssid\" \"password\"\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_wifi(handle->config.config_manager, argv[1], argv[2]);
    if (err == ESP_OK) {
        if (handle->config.wifi_manager) {
            (void)wifi_manager_disconnect(handle->config.wifi_manager);
            (void)wifi_manager_set_auto_connect_paused(handle->config.wifi_manager, false);
        }
        print_locked(handle,
                     "OK WIFI_SET SSID:%s PASSWORD:%s\n",
                     argv[1],
                     argv[2][0] ? "<set>" : "<empty>");
    } else {
        print_locked(handle, "ERR WIFI_SET_FAILED 0x%x\n", err);
    }
}

static void handle_wifi_clear(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE WIFI_CLEAR\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_clear_wifi(handle->config.config_manager);
    if (err == ESP_OK) {
        if (handle->config.wifi_manager) {
            (void)wifi_manager_disconnect(handle->config.wifi_manager);
        }
        print_locked(handle, "OK WIFI_CLEAR\n");
    } else {
        print_locked(handle, "ERR WIFI_CLEAR_FAILED 0x%x\n", err);
    }
}

static void handle_wifi_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE WIFI_STATUS\n");
        return;
    }

    wifi_manager_status_t status;
    esp_err_t err = get_wifi_status(handle, &status);
    if (err == ESP_OK) {
        print_wifi_status(handle, &status);
    } else {
        print_locked(handle, "ERR WIFI_STATUS_FAILED 0x%x\n", err);
    }
}

static void handle_wifi_connect(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE WIFI_CONNECT\n");
        return;
    }
    if (!handle->config.wifi_manager) {
        print_locked(handle, "ERR WIFI_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = wifi_manager_connect(handle->config.wifi_manager);
    if (err == ESP_OK) {
        print_locked(handle, "OK WIFI_CONNECT STARTED\n");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        print_locked(handle, "ERR WIFI_NOT_CONFIGURED\n");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        wifi_manager_status_t status;
        if (get_wifi_status(handle, &status) == ESP_OK) {
            if (status.state == WIFI_MANAGER_STATE_CONNECTED) {
                print_locked(handle, "OK WIFI_CONNECT ALREADY_CONNECTED\n");
                return;
            }
            if (status.state == WIFI_MANAGER_STATE_CONNECTING) {
                print_locked(handle, "OK WIFI_CONNECT ALREADY_CONNECTING\n");
                return;
            }
        }
    }

    print_locked(handle, "ERR WIFI_CONNECT_FAILED 0x%x\n", err);
}

static void handle_wifi_disconnect(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE WIFI_DISCONNECT\n");
        return;
    }
    if (!handle->config.wifi_manager) {
        print_locked(handle, "ERR WIFI_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = wifi_manager_disconnect(handle->config.wifi_manager);
    if (err == ESP_OK) {
        print_locked(handle, "OK WIFI_DISCONNECT\n");
    } else {
        print_locked(handle, "ERR WIFI_DISCONNECT_FAILED 0x%x\n", err);
    }
}

static void handle_ota_set_server(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint16_t port = 0;
    if (argc != 3 || !parse_u16_any_arg(argv[2], &port) || port == 0) {
        print_locked(handle, "ERR USAGE OTA_SET_SERVER host port\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_ota_server(handle->config.config_manager, argv[1], port);
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_SET_SERVER HOST:%s PORT:%u\n", argv[1], port);
    } else {
        print_locked(handle, "ERR OTA_SET_SERVER_FAILED 0x%x\n", err);
    }
}

static void handle_ota_set_manifest(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE OTA_SET_MANIFEST path\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_ota_manifest_path(handle->config.config_manager, argv[1]);
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_SET_MANIFEST PATH:%s\n", argv[1]);
    } else {
        print_locked(handle, "ERR OTA_SET_MANIFEST_FAILED 0x%x\n", err);
    }
}

static void handle_ota_config(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_CONFIG\n");
        return;
    }

    config_manager_snapshot_t snapshot;
    esp_err_t err = get_config_snapshot(handle, &snapshot);
    if (err != ESP_OK) {
        print_locked(handle, "ERR OTA_CONFIG_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA OTA_CONFIG HOST:%s PORT:%u MANIFEST:%s AUTO_CHECK:%u AUTO_CHECK_INTERVAL_MS:%lu AUTO_UPDATE:%u ANNOUNCE_PORT:%u ANNOUNCE_TOKEN:%s\n",
                 snapshot.ota_server_host,
                 snapshot.ota_server_port,
                 snapshot.ota_manifest_path,
                 snapshot.ota_auto_check_enabled ? 1 : 0,
                 (unsigned long)snapshot.ota_auto_check_interval_ms,
                 snapshot.ota_auto_update_enabled ? 1 : 0,
                 OTA_ANNOUNCE_DEFAULT_PORT,
                 snapshot.ota_announce_token_set ? "<set>" : "<empty>");
}

static void handle_ota_announce_token_set(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE OTA_ANNOUNCE_TOKEN_SET token\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_ota_announce_token(handle->config.config_manager, argv[1]);
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_ANNOUNCE_TOKEN_SET TOKEN:<set>\n");
    } else {
        print_locked(handle, "ERR OTA_ANNOUNCE_TOKEN_SET_FAILED 0x%x\n", err);
    }
}

static void handle_ota_announce_token_clear(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_ANNOUNCE_TOKEN_CLEAR\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_clear_ota_announce_token(handle->config.config_manager);
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_ANNOUNCE_TOKEN_CLEAR\n");
    } else {
        print_locked(handle, "ERR OTA_ANNOUNCE_TOKEN_CLEAR_FAILED 0x%x\n", err);
    }
}

static void handle_ota_announce_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_ANNOUNCE_STATUS\n");
        return;
    }
    if (!handle->config.ota_announce) {
        print_locked(handle, "ERR OTA_ANNOUNCE_UNAVAILABLE\n");
        return;
    }

    ota_announce_status_t status;
    esp_err_t err = ota_announce_get_status(handle->config.ota_announce, &status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR OTA_ANNOUNCE_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA OTA_ANNOUNCE TASK:%s PORT:%u SEEN:%lu ACCEPTED:%lu REJECTED:%lu CHECKS:%lu DOWNLOAD_TESTS:%lu UPDATES:%lu LAST_SENDER:%s LAST_ACTION:%s DETAIL:%s\n",
                 status.task_running ? "RUNNING" : "STOPPED",
                 status.listen_port,
                 (unsigned long)status.packets_seen,
                 (unsigned long)status.packets_accepted,
                 (unsigned long)status.packets_rejected,
                 (unsigned long)status.checks,
                 (unsigned long)status.download_tests,
                 (unsigned long)status.updates,
                 status.last_sender[0] ? status.last_sender : "<none>",
                 status.last_action[0] ? status.last_action : "<none>",
                 status.last_detail[0] ? status.last_detail : "<none>");
}

static void handle_ota_check(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_CHECK\n");
        return;
    }
    if (!handle->config.ota_manager) {
        print_locked(handle, "ERR OTA_MANAGER_UNAVAILABLE\n");
        return;
    }

    ota_manager_check_result_t result;
    esp_err_t err = ota_manager_check(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR OTA_CHECK_FAILED 0x%x DETAIL:%s CURRENT_BUILD:%lu\n",
                     err,
                     result.detail[0] ? result.detail : "UNKNOWN",
                     (unsigned long)result.current_build_number);
        return;
    }

    print_locked(handle,
                 "DATA OTA_CHECK STATUS:%s PROJECT:%s TARGET:%s VERSION:%s BUILD_NUMBER:%lu CURRENT_BUILD:%lu MIN_SUPPORTED_BUILD:%lu SIZE:%lu SHA256:%s FILENAME:%s URL:%s\n",
                 ota_manager_check_status_to_string(result.status),
                 result.project,
                 result.target,
                 result.version,
                 (unsigned long)result.build_number,
                 (unsigned long)result.current_build_number,
                 (unsigned long)result.min_supported_build,
                 (unsigned long)result.size,
                 result.sha256,
                 result.filename,
                 result.url);
}

static void handle_ota_download_test(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_DOWNLOAD_TEST\n");
        return;
    }
    if (!handle->config.ota_manager) {
        print_locked(handle, "ERR OTA_MANAGER_UNAVAILABLE\n");
        return;
    }
    if (!handle->config.wifi_manager) {
        print_locked(handle, "ERR WIFI_MANAGER_UNAVAILABLE\n");
        return;
    }
    if (!handle->config.robot) {
        print_locked(handle, "ERR ROBOT_CONTROL_UNAVAILABLE\n");
        return;
    }

    wifi_manager_status_t wifi_status;
    esp_err_t err = wifi_manager_get_status(handle->config.wifi_manager, &wifi_status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR OTA_DOWNLOAD_TEST_BLOCKED WIFI_STATUS_FAILED 0x%x\n", err);
        return;
    }
    if (wifi_status.state != WIFI_MANAGER_STATE_CONNECTED) {
        print_locked(handle,
                     "ERR OTA_DOWNLOAD_TEST_BLOCKED WIFI_NOT_CONNECTED STATUS:%s\n",
                     wifi_manager_state_to_string(wifi_status.state));
        return;
    }

    char safety_reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, safety_reason, sizeof(safety_reason))) {
        print_locked(handle,
                     "ERR OTA_DOWNLOAD_TEST_BLOCKED ROBOT_NOT_SAFE REASON:%s\n",
                     safety_reason[0] ? safety_reason : "UNKNOWN");
        return;
    }

    ota_manager_download_result_t result;
    err = ota_manager_download_test(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR OTA_DOWNLOAD_TEST_FAILED 0x%x DETAIL:%s PARTITION:%s BYTES:%lu\n",
                     err,
                     result.detail[0] ? result.detail : "UNKNOWN",
                     result.partition_label[0] ? result.partition_label : "NONE",
                     (unsigned long)result.bytes_written);
        return;
    }

    print_locked(handle,
                 "DATA OTA_DOWNLOAD_TEST STATUS:VERIFIED PARTITION:%s BYTES:%lu SHA256:%s\n",
                 result.partition_label,
                 (unsigned long)result.bytes_written,
                 result.sha256);
}

static void handle_ota_update(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_UPDATE\n");
        return;
    }
    if (!handle->config.ota_manager) {
        print_locked(handle, "ERR OTA_MANAGER_UNAVAILABLE\n");
        return;
    }
    if (!handle->config.wifi_manager) {
        print_locked(handle, "ERR WIFI_MANAGER_UNAVAILABLE\n");
        return;
    }
    if (!handle->config.robot) {
        print_locked(handle, "ERR ROBOT_CONTROL_UNAVAILABLE\n");
        return;
    }

    wifi_manager_status_t wifi_status;
    esp_err_t err = wifi_manager_get_status(handle->config.wifi_manager, &wifi_status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR OTA_UPDATE_BLOCKED WIFI_STATUS_FAILED 0x%x\n", err);
        return;
    }
    if (wifi_status.state != WIFI_MANAGER_STATE_CONNECTED) {
        print_locked(handle,
                     "ERR OTA_UPDATE_BLOCKED WIFI_NOT_CONNECTED STATUS:%s\n",
                     wifi_manager_state_to_string(wifi_status.state));
        return;
    }

    char safety_reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, safety_reason, sizeof(safety_reason))) {
        print_locked(handle,
                     "ERR OTA_UPDATE_BLOCKED ROBOT_NOT_SAFE REASON:%s\n",
                     safety_reason[0] ? safety_reason : "UNKNOWN");
        return;
    }

    ota_manager_download_result_t result;
    err = ota_manager_download_to_inactive(handle->config.ota_manager, &result);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR OTA_UPDATE_FAILED 0x%x DETAIL:%s PARTITION:%s BYTES:%lu\n",
                     err,
                     result.detail[0] ? result.detail : "UNKNOWN",
                     result.partition_label[0] ? result.partition_label : "NONE",
                     (unsigned long)result.bytes_written);
        return;
    }

    err = robot_control_prepare_for_ota(handle->config.robot);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR OTA_UPDATE_PREPARE_FAILED 0x%x PARTITION:%s BYTES:%lu\n",
                     err,
                     result.partition_label,
                     (unsigned long)result.bytes_written);
        return;
    }

    err = ota_manager_set_boot_partition(result.partition_label);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR OTA_UPDATE_SET_BOOT_FAILED 0x%x PARTITION:%s BYTES:%lu\n",
                     err,
                     result.partition_label,
                     (unsigned long)result.bytes_written);
        return;
    }

    print_locked(handle,
                 "DATA OTA_UPDATE STATUS:REBOOTING PARTITION:%s BYTES:%lu SHA256:%s\n",
                 result.partition_label,
                 (unsigned long)result.bytes_written,
                 result.sha256);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void handle_ota_rollback_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_ROLLBACK_STATUS\n");
        return;
    }

    ota_manager_boot_state_t boot_state;
    esp_err_t state_err = ota_manager_get_boot_state(&boot_state);
    ota_manager_rollback_test_mode_t test_mode = OTA_MANAGER_ROLLBACK_TEST_NONE;
    esp_err_t test_err = ota_manager_get_rollback_test_mode(&test_mode);
    if (test_err != ESP_OK) {
        test_mode = OTA_MANAGER_ROLLBACK_TEST_NONE;
    }

    print_locked(handle,
                 "DATA OTA_ROLLBACK PARTITION:%s OTA_STATE:%s PENDING_VERIFY:%u ROLLBACK_POSSIBLE:%u STATE_ERR:0x%x TEST_MODE:%s TEST_ERR:0x%x\n",
                 state_err == ESP_OK && boot_state.partition_label[0] ? boot_state.partition_label : "UNKNOWN",
                 state_err == ESP_OK && boot_state.state_known ? ota_manager_image_state_to_string(boot_state.state) : "UNKNOWN",
                 state_err == ESP_OK && boot_state.pending_verify ? 1 : 0,
                 state_err == ESP_OK && boot_state.rollback_possible ? 1 : 0,
                 state_err == ESP_OK ? boot_state.state_error : state_err,
                 ota_manager_rollback_test_mode_to_string(test_mode),
                 test_err);
}

static void handle_ota_rollback_test(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE OTA_ROLLBACK_TEST NONE|NO_CONFIRM_ONCE|SELF_TEST_FAIL_ONCE\n");
        return;
    }

    ota_manager_rollback_test_mode_t mode = OTA_MANAGER_ROLLBACK_TEST_NONE;
    if (!parse_rollback_test_mode_arg(argv[1], &mode)) {
        print_locked(handle, "ERR USAGE OTA_ROLLBACK_TEST NONE|NO_CONFIRM_ONCE|SELF_TEST_FAIL_ONCE\n");
        return;
    }

    esp_err_t err = ota_manager_set_rollback_test_mode(mode);
    if (err == ESP_OK) {
        print_locked(handle,
                     "OK OTA_ROLLBACK_TEST MODE:%s\n",
                     ota_manager_rollback_test_mode_to_string(mode));
    } else {
        print_locked(handle, "ERR OTA_ROLLBACK_TEST_FAILED 0x%x\n", err);
    }
}

static void handle_ota_auto_check(serial_gateway_handle_t handle, int argc, char *argv[])
{
    bool enabled = false;
    if (argc != 2 || !parse_on_off_arg(argv[1], &enabled)) {
        print_locked(handle, "ERR USAGE OTA_AUTO_CHECK ON|OFF\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_ota_auto_check(handle->config.config_manager, enabled);
    if (err == ESP_OK) {
        if (handle->config.ota_manager) {
            (void)ota_manager_set_auto_check_runtime_enabled(handle->config.ota_manager, enabled);
        }
        print_locked(handle, "OK OTA_AUTO_CHECK %s\n", enabled ? "ON" : "OFF");
    } else {
        print_locked(handle, "ERR OTA_AUTO_CHECK_FAILED 0x%x\n", err);
    }
}

static void handle_ota_auto_interval(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 1 && argc != 2) {
        print_locked(handle, "ERR USAGE OTA_AUTO_INTERVAL [milliseconds]\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    if (argc == 1) {
        config_manager_snapshot_t snapshot;
        esp_err_t err = get_config_snapshot(handle, &snapshot);
        if (err == ESP_OK) {
            print_locked(handle, "DATA OTA_AUTO_INTERVAL MS:%lu\n", (unsigned long)snapshot.ota_auto_check_interval_ms);
        } else {
            print_locked(handle, "ERR OTA_AUTO_INTERVAL_FAILED 0x%x\n", err);
        }
        return;
    }

    uint32_t interval_ms = 0;
    if (!parse_u32_any_arg(argv[1], &interval_ms)) {
        print_locked(handle, "ERR USAGE OTA_AUTO_INTERVAL [milliseconds]\n");
        return;
    }
    if (interval_ms < CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS ||
        interval_ms > CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS) {
        print_locked(handle,
                     "ERR OTA_AUTO_INTERVAL OUT_OF_RANGE MIN:%lu MAX:%lu\n",
                     (unsigned long)CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MIN_MS,
                     (unsigned long)CONFIG_MANAGER_OTA_AUTO_CHECK_INTERVAL_MAX_MS);
        return;
    }

    esp_err_t err = config_manager_set_ota_auto_check_interval_ms(handle->config.config_manager, interval_ms);
    if (err == ESP_OK && handle->config.ota_manager) {
        err = ota_manager_set_auto_check_interval_runtime_ms(handle->config.ota_manager, interval_ms);
    }
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_AUTO_INTERVAL MS:%lu\n", (unsigned long)interval_ms);
    } else {
        print_locked(handle, "ERR OTA_AUTO_INTERVAL_FAILED 0x%x\n", err);
    }
}

static void handle_ota_auto_force_check(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_AUTO_FORCE_CHECK\n");
        return;
    }
    if (!handle->config.ota_manager) {
        print_locked(handle, "ERR OTA_MANAGER_UNAVAILABLE\n");
        return;
    }

    ota_manager_check_result_t result;
    esp_err_t err = ota_manager_force_check(handle->config.ota_manager, &result);
    if (err == ESP_OK) {
        print_locked(handle,
                     "DATA OTA_AUTO_FORCE_CHECK STATUS:%s CURRENT_BUILD:%lu REMOTE_BUILD:%lu HTTP_STATUS:%d UPDATE_AVAILABLE:%u\n",
                     ota_manager_check_status_to_string(result.status),
                     (unsigned long)result.current_build_number,
                     (unsigned long)result.build_number,
                     result.http_status,
                     result.status == OTA_MANAGER_CHECK_STATUS_UPDATE_AVAILABLE ? 1 : 0);
        return;
    }

    if (strcmp(result.detail, "WIFI_NOT_CONNECTED") == 0) {
        print_locked(handle, "ERR OTA_AUTO_FORCE_CHECK WIFI_NOT_CONNECTED\n");
        return;
    }
    if (strcmp(result.detail, "OTA_BUSY") == 0) {
        print_locked(handle, "ERR OTA_AUTO_FORCE_CHECK BUSY\n");
        return;
    }

    print_locked(handle,
                 "ERR OTA_AUTO_FORCE_CHECK_FAILED 0x%x DETAIL:%s HTTP_STATUS:%d CURRENT_BUILD:%lu\n",
                 err,
                 result.detail[0] ? result.detail : "UNKNOWN",
                 result.http_status,
                 (unsigned long)result.current_build_number);
}

static void handle_ota_auto_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE OTA_AUTO_STATUS\n");
        return;
    }
    if (!handle->config.ota_manager) {
        print_locked(handle, "ERR OTA_MANAGER_UNAVAILABLE\n");
        return;
    }

    ota_manager_auto_status_t status;
    esp_err_t err = ota_manager_get_auto_status(handle->config.ota_manager, &status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR OTA_AUTO_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA OTA_AUTO TASK:%s ENABLED:%u CHECKING:%u INTERVAL_MS:%lu BACKOFF_MS:%lu NEXT_DELAY_MS:%lu CHECK_COUNT:%lu FAILURE_COUNT:%lu LAST_RESULT:%s LAST_ERROR:0x%x LAST_HTTP_STATUS:%d LAST_CHECK_AGE_MS:%lu LAST_CHECK_TIME_MS:%lu CURRENT_BUILD:%lu LAST_REMOTE_BUILD:%lu LAST_VERSION:%s UPDATE_AVAILABLE:%u AUTO_UPDATE_ENABLED:%u DETAIL:%s LAST_URL:%s\n",
                 status.task_running ? "RUNNING" : "STOPPED",
                 status.enabled ? 1 : 0,
                 status.checking ? 1 : 0,
                 (unsigned long)status.interval_ms,
                 (unsigned long)status.backoff_ms,
                 (unsigned long)status.next_check_in_ms,
                 (unsigned long)status.checks,
                 (unsigned long)status.failures,
                 ota_manager_check_status_to_string(status.last_status),
                 status.last_error,
                 status.last_http_status,
                 (unsigned long)status.last_check_age_ms,
                 (unsigned long)status.last_check_time_ms,
                 (unsigned long)status.current_build_number,
                 (unsigned long)status.last_build_number,
                 status.last_version[0] ? status.last_version : "<none>",
                 status.update_available ? 1 : 0,
                 status.auto_update_enabled ? 1 : 0,
                 status.last_detail[0] ? status.last_detail : "<none>",
                 status.last_url[0] ? status.last_url : "<none>");
}

static void handle_ota_auto_update(serial_gateway_handle_t handle, int argc, char *argv[])
{
    bool enabled = false;
    if (argc != 2 || !parse_on_off_arg(argv[1], &enabled)) {
        print_locked(handle, "ERR USAGE OTA_AUTO_UPDATE OFF\n");
        return;
    }
    if (enabled) {
        print_locked(handle, "ERR AUTO_UPDATE_DISABLED_UNTIL_EXPLICITLY_APPROVED\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_ota_auto_update(handle->config.config_manager, false);
    if (err == ESP_OK) {
        print_locked(handle, "OK OTA_AUTO_UPDATE OFF\n");
    } else {
        print_locked(handle, "ERR OTA_AUTO_UPDATE_FAILED 0x%x\n", err);
    }
}

static void handle_read_reg(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    uint16_t reg = 0;
    uint16_t quantity = 1;
    if ((argc != 3 && argc != 4) ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_u16_any_arg(argv[2], &reg) ||
        (argc == 4 && !parse_u16_any_arg(argv[3], &quantity)) ||
        quantity == 0 || quantity > 16) {
        print_locked(handle, "ERR USAGE READ_REG drive_id reg [count]\n");
        return;
    }

    uint16_t regs[16] = { 0 };
    esp_err_t err = robot_control_read_svd48_registers(handle->config.robot, drive_id, reg, quantity, regs);
    if (err != ESP_OK) {
        print_locked(handle, "ERR READ_REG_FAILED DRIVE:%u REG:0x%04x COUNT:%u ERR:0x%x\n", drive_id, reg, quantity, err);
        return;
    }

    print_locked(handle, "DATA REG DRIVE:%u START:0x%04x COUNT:%u", drive_id, reg, quantity);
    for (uint16_t i = 0; i < quantity; i++) {
        print_locked(handle, " R%u:0x%04x/%u", i, regs[i], regs[i]);
    }
    print_locked(handle, "\n");
}

static const char *svd48_probe_result_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_ERR_TIMEOUT:
        return "TIMEOUT";
    case ESP_ERR_INVALID_CRC:
        return "CRC_ERROR";
    case ESP_ERR_INVALID_RESPONSE:
        return "BAD_RESPONSE";
    case ESP_ERR_INVALID_STATE:
        return "UNAVAILABLE";
    case ESP_ERR_INVALID_ARG:
        return "INVALID_ARGUMENT";
    default:
        return "IO_ERROR";
    }
}

static void handle_svd48_probe(serial_gateway_handle_t handle,
                               int argc,
                               char *argv[])
{
    uint8_t address = 0U;
    if (argc != 2 || !parse_drive_id_arg(argv[1], &address) ||
        address > 247U) {
        print_locked(handle, "ERR USAGE SVD48_PROBE address\n");
        return;
    }

    uint16_t bus_voltage[2] = {0U, 0U};
    esp_err_t err = robot_control_probe_svd48_address(handle->config.robot,
                                                      address,
                                                      0x540CU,
                                                      2U,
                                                      bus_voltage);
    if (err == ESP_OK) {
        print_locked(handle,
                     "DATA SVD48_PROBE ADDRESS:%u READ_OK:1 RESULT:OK REG:0x540c M1_BUS_DV:%u M2_BUS_DV:%u\n",
                     address,
                     bus_voltage[0],
                     bus_voltage[1]);
        return;
    }

    print_locked(handle,
                 "DATA SVD48_PROBE ADDRESS:%u READ_OK:0 RESULT:%s REG:0x540c ERROR:0x%x\n",
                 address,
                 svd48_probe_result_name(err),
                 err);
}

static void handle_write_reg(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    uint16_t reg = 0;
    uint16_t value = 0;
    if (argc != 5 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_u16_any_arg(argv[2], &reg) ||
        !parse_u16_any_arg(argv[3], &value) ||
        strcasecmp(argv[4], "CONFIRM") != 0) {
        print_locked(handle, "ERR USAGE WRITE_REG drive_id reg value CONFIRM\n");
        return;
    }

    if (svd48_register_is_runtime_actuation(reg)) {
        print_locked(handle,
                     "ERR WRITE_REG_ACTUATION_BLOCKED REG:0x%04x\n",
                     reg);
        return;
    }
    if (reject_continuous_control_conflict(handle, "WRITE_REG")) {
        return;
    }

    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        print_locked(handle, "ERR WRITE_REG_ROBOT_NOT_STOPPED REASON:%s\n", reason);
        return;
    }

    uint16_t old_value = 0;
    esp_err_t err = robot_control_read_svd48_registers(handle->config.robot,
                                                       drive_id,
                                                       reg,
                                                       1,
                                                       &old_value);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REG_PRE_READ_FAILED DRIVE:%u REG:0x%04x ERR:0x%x\n",
                     drive_id,
                     reg,
                     err);
        return;
    }

    err = robot_control_write_svd48_register(handle->config.robot, drive_id, reg, value);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REG_FAILED DRIVE:%u REG:0x%04x VALUE:0x%04x OUTCOME:UNKNOWN ERR:0x%x\n",
                     drive_id,
                     reg,
                     value,
                     err);
        return;
    }

    uint16_t readback = 0;
    err = robot_control_read_svd48_registers(handle->config.robot,
                                             drive_id,
                                             reg,
                                             1,
                                             &readback);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REG_READBACK_FAILED DRIVE:%u REG:0x%04x OLD:0x%04x VALUE:0x%04x OUTCOME:ACKED_UNVERIFIED ERR:0x%x\n",
                     drive_id,
                     reg,
                     old_value,
                     value,
                     err);
        return;
    }
    if (readback != value) {
        print_locked(handle,
                     "ERR WRITE_REG_READBACK_MISMATCH DRIVE:%u REG:0x%04x OLD:0x%04x VALUE:0x%04x READBACK:0x%04x\n",
                     drive_id,
                     reg,
                     old_value,
                     value,
                     readback);
        return;
    }

    print_locked(handle,
                 "OK WRITE_REG DRIVE:%u REG:0x%04x OLD:0x%04x/%u VALUE:0x%04x/%u READBACK:0x%04x/%u VERIFIED:1\n",
                 drive_id,
                 reg,
                 old_value,
                 old_value,
                 value,
                 value,
                 readback,
                 readback);
}

static void handle_write_regs(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    uint16_t start_reg = 0;
    uint16_t values[SVD48_MAINTENANCE_WRITE_MAX_REGISTERS] = { 0 };
    uint16_t old_values[SVD48_MAINTENANCE_WRITE_MAX_REGISTERS] = { 0 };
    uint16_t readback[SVD48_MAINTENANCE_WRITE_MAX_REGISTERS] = { 0 };
    int value_count = argc - 4;

    if (argc < 5 || value_count <= 0 ||
        value_count > SVD48_MAINTENANCE_WRITE_MAX_REGISTERS ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_u16_any_arg(argv[2], &start_reg) ||
        strcasecmp(argv[argc - 1], "CONFIRM") != 0) {
        print_locked(handle, "ERR USAGE WRITE_REGS drive_id start_reg value [value...] CONFIRM\n");
        return;
    }

    for (int i = 0; i < value_count; i++) {
        if (!parse_u16_any_arg(argv[3 + i], &values[i])) {
            print_locked(handle, "ERR WRITE_REGS_BAD_VALUE INDEX:%d\n", i);
            return;
        }
    }

    uint16_t quantity = (uint16_t)value_count;
    if (!svd48_write_multiple_range_is_valid(start_reg, quantity)) {
        print_locked(handle, "ERR WRITE_REGS_BAD_RANGE START:0x%04x COUNT:%u\n", start_reg, quantity);
        return;
    }
    if (svd48_register_range_has_runtime_actuation(start_reg, quantity)) {
        print_locked(handle,
                     "ERR WRITE_REGS_ACTUATION_BLOCKED START:0x%04x COUNT:%u\n",
                     start_reg,
                     quantity);
        return;
    }
    if (reject_continuous_control_conflict(handle, "WRITE_REGS")) {
        return;
    }

    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        print_locked(handle, "ERR WRITE_REGS_ROBOT_NOT_STOPPED REASON:%s\n", reason);
        return;
    }

    esp_err_t err = robot_control_read_svd48_registers(handle->config.robot,
                                                       drive_id,
                                                       start_reg,
                                                       quantity,
                                                       old_values);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REGS_PRE_READ_FAILED DRIVE:%u START:0x%04x COUNT:%u ERR:0x%x\n",
                     drive_id,
                     start_reg,
                     quantity,
                     err);
        return;
    }

    err = robot_control_write_svd48_registers(handle->config.robot,
                                              drive_id,
                                              start_reg,
                                              values,
                                              quantity);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REGS_FAILED DRIVE:%u START:0x%04x COUNT:%u OUTCOME:UNKNOWN ERR:0x%x\n",
                     drive_id,
                     start_reg,
                     quantity,
                     err);
        return;
    }

    err = robot_control_read_svd48_registers(handle->config.robot,
                                             drive_id,
                                             start_reg,
                                             quantity,
                                             readback);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR WRITE_REGS_READBACK_FAILED DRIVE:%u START:0x%04x COUNT:%u OUTCOME:ACKED_UNVERIFIED ERR:0x%x\n",
                     drive_id,
                     start_reg,
                     quantity,
                     err);
        return;
    }

    for (uint16_t i = 0; i < quantity; i++) {
        if (readback[i] != values[i]) {
            print_locked(handle,
                         "ERR WRITE_REGS_READBACK_MISMATCH DRIVE:%u START:0x%04x INDEX:%u OLD:0x%04x VALUE:0x%04x READBACK:0x%04x\n",
                         drive_id,
                         start_reg,
                         i,
                         old_values[i],
                         values[i],
                         readback[i]);
            return;
        }
    }

    print_locked(handle,
                 "OK WRITE_REGS DRIVE:%u START:0x%04x COUNT:%u VERIFIED:1",
                 drive_id,
                 start_reg,
                 quantity);
    for (uint16_t i = 0; i < quantity; i++) {
        print_locked(handle,
                     " O%u:0x%04x N%u:0x%04x R%u:0x%04x",
                     i,
                     old_values[i],
                     i,
                     values[i],
                     i,
                     readback[i]);
    }
    print_locked(handle, "\n");
}

static void handle_save_svd48_config(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    if (argc != 3 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        strcasecmp(argv[2], "CONFIRM") != 0) {
        print_locked(handle, "ERR USAGE SAVE_SVD48_CONFIG drive_id CONFIRM\n");
        return;
    }
    if (reject_continuous_control_conflict(handle, "SAVE_SVD48_CONFIG")) {
        return;
    }

    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        print_locked(handle, "ERR SAVE_SVD48_CONFIG_ROBOT_NOT_STOPPED REASON:%s\n", reason);
        return;
    }

    esp_err_t err = robot_control_write_svd48_register(handle->config.robot,
                                                        drive_id,
                                                        0x3100,
                                                        1);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR SAVE_SVD48_CONFIG_FAILED DRIVE:%u OUTCOME:UNKNOWN ERR:0x%x\n",
                     drive_id,
                     err);
        return;
    }

    print_locked(handle,
                 "OK SAVE_SVD48_CONFIG DRIVE:%u REG:0x3100 VALUE:1 OUTCOME:ACKED_UNVERIFIED WRITE_ONLY:1\n",
                 drive_id);
}

static void handle_set_svd48_gear_ratio(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    uint16_t motor_teeth = 0;
    uint16_t wheel_teeth = 0;
    if (argc != 5 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_u16_any_arg(argv[2], &motor_teeth) ||
        !parse_u16_any_arg(argv[3], &wheel_teeth) ||
        motor_teeth == 0 || wheel_teeth == 0 ||
        strcasecmp(argv[4], "CONFIRM") != 0) {
        print_locked(handle,
                     "ERR USAGE SET_SVD48_GEAR_RATIO drive_id motor_teeth wheel_teeth CONFIRM\n");
        return;
    }
    if (reject_continuous_control_conflict(handle, "SET_SVD48_GEAR_RATIO")) {
        return;
    }

    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        print_locked(handle, "ERR SET_SVD48_GEAR_RATIO_ROBOT_NOT_STOPPED REASON:%s\n", reason);
        return;
    }

    uint16_t values[2] = { motor_teeth, wheel_teeth };
    esp_err_t err = robot_control_write_svd48_registers(handle->config.robot,
                                                         drive_id,
                                                         0x2202,
                                                         values,
                                                         2);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR SET_SVD48_GEAR_RATIO_FAILED DRIVE:%u MOTOR_TEETH:%u WHEEL_TEETH:%u OUTCOME:UNKNOWN ERR:0x%x\n",
                     drive_id,
                     motor_teeth,
                     wheel_teeth,
                     err);
        return;
    }

    uint16_t readback[2] = { 0 };
    err = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x2202, 2, readback);
    if (err != ESP_OK) {
        print_locked(handle,
                     "OK SET_SVD48_GEAR_RATIO DRIVE:%u MOTOR_TEETH:%u WHEEL_TEETH:%u RATIO:%.3f OUTCOME:ACKED_UNVERIFIED READBACK_ERR:0x%x\n",
                     drive_id,
                     motor_teeth,
                     wheel_teeth,
                     (double)wheel_teeth / (double)motor_teeth,
                     err);
        return;
    }

    if (readback[0] != motor_teeth || readback[1] != wheel_teeth) {
        print_locked(handle,
                     "ERR SET_SVD48_GEAR_RATIO_READBACK_MISMATCH DRIVE:%u MOTOR_TEETH:%u WHEEL_TEETH:%u READBACK:%u/%u\n",
                     drive_id,
                     motor_teeth,
                     wheel_teeth,
                     readback[0],
                     readback[1]);
        return;
    }

    print_locked(handle,
                 "OK SET_SVD48_GEAR_RATIO DRIVE:%u MOTOR_TEETH:%u WHEEL_TEETH:%u RATIO:%.3f VERIFIED:1\n",
                 drive_id,
                 motor_teeth,
                 wheel_teeth,
                 (double)wheel_teeth / (double)motor_teeth);
}

static void handle_svd48_identify_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    int8_t channel = SVD48_CHANNEL_ALL;
    if (argc != 3 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_channel_arg(argv[2], &channel) ||
        channel == SVD48_CHANNEL_ALL) {
        print_locked(handle, "ERR USAGE SVD48_IDENTIFY_STATUS drive_id M1|M2\n");
        return;
    }

    const uint16_t offset = channel == 0 ? 0U : 1U;
    uint16_t state = 0;
    uint16_t rs = 0;
    uint16_t ld = 0;
    uint16_t lq = 0;
    esp_err_t err = robot_control_read_svd48_registers(handle->config.robot,
                                                       drive_id,
                                                       (uint16_t)(0x5710U + offset),
                                                       1,
                                                       &state);
    if (err == ESP_OK) {
        err = robot_control_read_svd48_registers(handle->config.robot,
                                                 drive_id,
                                                 (uint16_t)(0x5714U + offset),
                                                 1,
                                                 &rs);
    }
    if (err == ESP_OK) {
        err = robot_control_read_svd48_registers(handle->config.robot,
                                                 drive_id,
                                                 (uint16_t)(0x5718U + offset),
                                                 1,
                                                 &ld);
    }
    if (err == ESP_OK) {
        err = robot_control_read_svd48_registers(handle->config.robot,
                                                 drive_id,
                                                 (uint16_t)(0x571CU + offset),
                                                 1,
                                                 &lq);
    }
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR SVD48_IDENTIFY_STATUS_FAILED DRIVE:%u MOTOR:M%u ERR:0x%x\n",
                     drive_id,
                     (unsigned)(channel + 1),
                     err);
        return;
    }

    print_locked(handle,
                 "DATA SVD48_IDENTIFY DRIVE:%u MOTOR:M%u STATE:%u RS_RAW:%u LD_RAW:%u LQ_RAW:%u SCALE:UNVERIFIED KV:NOT_IDENTIFIED\n",
                 drive_id,
                 (unsigned)(channel + 1),
                 state,
                 rs,
                 ld,
                 lq);
}

static void handle_svd48_identify(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    int8_t channel = SVD48_CHANNEL_ALL;
    bool start = false;
    if (argc != 5 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_channel_arg(argv[2], &channel) ||
        channel == SVD48_CHANNEL_ALL ||
        (strcasecmp(argv[3], "START") != 0 && strcasecmp(argv[3], "STOP") != 0) ||
        strcasecmp(argv[4], "CONFIRM") != 0) {
        print_locked(handle, "ERR USAGE SVD48_IDENTIFY drive_id M1|M2 START|STOP CONFIRM\n");
        return;
    }
    start = strcasecmp(argv[3], "START") == 0;

    char reason[48] = { 0 };
    if (!robot_control_is_safe_for_ota(handle->config.robot, reason, sizeof(reason))) {
        print_locked(handle, "ERR SVD48_IDENTIFY_ROBOT_NOT_STOPPED REASON:%s\n", reason);
        return;
    }

    const uint16_t reg = (uint16_t)(0x5700U + (channel == 0 ? 0U : 1U));
    const uint16_t value = start ? 1U : 0U;
    esp_err_t err = robot_control_write_svd48_registers(handle->config.robot,
                                                         drive_id,
                                                         reg,
                                                         &value,
                                                         1);
    if (err != ESP_OK) {
        print_locked(handle,
                     "ERR SVD48_IDENTIFY_%s_FAILED DRIVE:%u MOTOR:M%u ERR:0x%x OUTCOME:UNKNOWN\n",
                     start ? "START" : "STOP",
                     drive_id,
                     (unsigned)(channel + 1),
                     err);
        return;
    }

    print_locked(handle,
                 "OK SVD48_IDENTIFY DRIVE:%u MOTOR:M%u ACTION:%s OUTCOME:ACKED WRITE_ONLY:1 APPLY:MANUAL SAVE:MANUAL\n",
                 drive_id,
                 (unsigned)(channel + 1),
                 start ? "START" : "STOP");
}

static void handle_get_svd48_config(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    int8_t requested_channel = SVD48_CHANNEL_ALL;
    if ((argc != 2 && argc != 3) ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        (argc == 3 && !parse_channel_arg(argv[2], &requested_channel))) {
        print_locked(handle, "ERR USAGE GET_SVD48_CONFIG drive_id [M1|M2|ALL]\n");
        return;
    }

    uint16_t poles[2] = { 0 };
    uint16_t sensors[2] = { 0 };
    uint16_t wheel_diameter_mm = 0;
    uint16_t motor_teeth = 0;
    uint16_t wheel_teeth = 0;
    bool has_motor_teeth = false;
    bool has_wheel_teeth = false;
    uint16_t hall_install[2] = { 0 };
    uint16_t hall_status[2] = { 0 };
    uint16_t hall_angle[2] = { 0 };

    esp_err_t err = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x5018, 2, poles);
    if (err != ESP_OK) {
        print_locked(handle, "ERR GET_SVD48_CONFIG_FAILED DRIVE:%u REG:0x5018 ERR:0x%x\n", drive_id, err);
        return;
    }
    err = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x502C, 2, sensors);
    if (err != ESP_OK) {
        print_locked(handle, "ERR GET_SVD48_CONFIG_FAILED DRIVE:%u REG:0x502c ERR:0x%x\n", drive_id, err);
        return;
    }
    err = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x2201, 1, &wheel_diameter_mm);
    if (err != ESP_OK) {
        print_locked(handle, "ERR GET_SVD48_CONFIG_FAILED DRIVE:%u REG:0x2201 ERR:0x%x\n", drive_id, err);
        return;
    }
    has_motor_teeth = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x2202, 1, &motor_teeth) == ESP_OK;
    has_wheel_teeth = robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x2203, 1, &wheel_teeth) == ESP_OK;
    (void)robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x5620, 2, hall_install);
    (void)robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x5688, 2, hall_status);
    (void)robot_control_read_svd48_registers(handle->config.robot, drive_id, 0x568C, 2, hall_angle);

    float gear_ratio = motor_teeth == 0 ? 0.0f : (float)wheel_teeth / (float)motor_teeth;
    if (requested_channel >= 0) {
        uint8_t channel = (uint8_t)requested_channel;
        print_locked(handle,
                     "DATA SVD48_CONFIG DRIVE:%u CHANNEL:%s POLES:%u SENSOR:%u/%s WHEEL_DIAM_MM:%u MOTOR_TEETH:%s%u WHEEL_TEETH:%s%u GEAR_RATIO:%s%.3f HALL_INSTALL:%u HALL_STATUS:%u HALL_ANGLE:%u\n",
                     drive_id,
                     channel_name(channel),
                     poles[channel],
                     sensors[channel],
                     sensor_type_name(sensors[channel]),
                     wheel_diameter_mm,
                     has_motor_teeth ? "" : "NA/",
                     motor_teeth,
                     has_wheel_teeth ? "" : "NA/",
                     wheel_teeth,
                     (has_motor_teeth && has_wheel_teeth && motor_teeth != 0) ? "" : "NA/",
                     gear_ratio,
                     hall_install[channel],
                     hall_status[channel],
                     hall_angle[channel]);
        return;
    }

    print_locked(handle,
                 "DATA SVD48_CONFIG DRIVE:%u M1_POLES:%u M2_POLES:%u M1_SENSOR:%u/%s M2_SENSOR:%u/%s WHEEL_DIAM_MM:%u MOTOR_TEETH:%s%u WHEEL_TEETH:%s%u GEAR_RATIO:%s%.3f M1_HALL_INSTALL:%u M2_HALL_INSTALL:%u M1_HALL_STATUS:%u M2_HALL_STATUS:%u M1_HALL_ANGLE:%u M2_HALL_ANGLE:%u\n",
                 drive_id,
                 poles[0],
                 poles[1],
                 sensors[0],
                 sensor_type_name(sensors[0]),
                 sensors[1],
                 sensor_type_name(sensors[1]),
                 wheel_diameter_mm,
                 has_motor_teeth ? "" : "NA/",
                 motor_teeth,
                 has_wheel_teeth ? "" : "NA/",
                 wheel_teeth,
                 (has_motor_teeth && has_wheel_teeth && motor_teeth != 0) ? "" : "NA/",
                 gear_ratio,
                 hall_install[0],
                 hall_install[1],
                 hall_status[0],
                 hall_status[1],
                 hall_angle[0],
                 hall_angle[1]);
}

static void handle_apply_py6514_config(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    int8_t requested_channel = SVD48_CHANNEL_ALL;
    const char *confirm = NULL;
    if (argc == 3) {
        confirm = argv[2];
    } else if (argc == 4) {
        confirm = argv[3];
    }

    if ((argc != 3 && argc != 4) ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        (argc == 4 && !parse_channel_arg(argv[2], &requested_channel)) ||
        !confirm ||
        strcasecmp(confirm, "CONFIRM") != 0) {
        print_locked(handle, "ERR USAGE APPLY_PY6514_CONFIG drive_id [M1|M2|ALL] CONFIRM\n");
        return;
    }

    if (reject_continuous_control_conflict(handle, "APPLY_PY6514_CONFIG")) {
        return;
    }

    bool gear_written = true;
    uint8_t first_channel = requested_channel == SVD48_CHANNEL_ALL ? 0 : (uint8_t)requested_channel;
    uint8_t last_channel = requested_channel == SVD48_CHANNEL_ALL ? 1 : (uint8_t)requested_channel;

    for (uint8_t channel = first_channel; channel <= last_channel; channel++) {
        uint16_t pole_reg = channel == 0 ? 0x5018 : 0x5019;
        uint16_t sensor_reg = channel == 0 ? 0x502C : 0x502D;
        esp_err_t err = robot_control_write_svd48_register(handle->config.robot, drive_id, pole_reg, SVD48_PY6514_POLE_PAIRS);
        if (err != ESP_OK) {
            print_locked(handle, "ERR APPLY_PY6514_CONFIG_FAILED DRIVE:%u REG:0x%04x VALUE:%u ERR:0x%x\n",
                         drive_id,
                         pole_reg,
                         SVD48_PY6514_POLE_PAIRS,
                         err);
            return;
        }
        err = robot_control_write_svd48_register(handle->config.robot, drive_id, sensor_reg, SVD48_PY6514_SENSOR_HALL);
        if (err != ESP_OK) {
            print_locked(handle, "ERR APPLY_PY6514_CONFIG_FAILED DRIVE:%u REG:0x%04x VALUE:%u ERR:0x%x\n",
                         drive_id,
                         sensor_reg,
                         SVD48_PY6514_SENSOR_HALL,
                         err);
            return;
        }
    }

    esp_err_t err = robot_control_write_svd48_register(handle->config.robot, drive_id, 0x2201, SVD48_PY6514_WHEEL_DIAMETER_MM);
    if (err != ESP_OK) {
        print_locked(handle, "ERR APPLY_PY6514_CONFIG_FAILED DRIVE:%u REG:0x2201 VALUE:%u ERR:0x%x\n",
                     drive_id,
                     SVD48_PY6514_WHEEL_DIAMETER_MM,
                     err);
        return;
    }

    if (robot_control_write_svd48_register(handle->config.robot, drive_id, 0x2202, SVD48_PY6514_MOTOR_TEETH) != ESP_OK ||
        robot_control_write_svd48_register(handle->config.robot, drive_id, 0x2203, SVD48_PY6514_WHEEL_TEETH) != ESP_OK) {
        gear_written = false;
    }

    print_locked(handle, "OK APPLY_PY6514_CONFIG DRIVE:%u CHANNEL:%s POLES:10 SENSOR:HALL WHEEL_DIAM_MM:330 MOTOR_TEETH:%s WHEEL_TEETH:%s GEAR_RATIO:%s\n",
                 drive_id,
                 requested_channel == SVD48_CHANNEL_ALL ? "ALL" : channel_name((uint8_t)requested_channel),
                 gear_written ? "1" : "UNSUPPORTED",
                 gear_written ? "5" : "UNSUPPORTED",
                 gear_written ? "5.000" : "UNSUPPORTED");
}

static void print_help(serial_gateway_handle_t handle)
{
    print_locked(handle,
                 "DATA HELP COMMANDS:PING,VERSION,PROFILE_STATUS,COMPOSITION_STATUS,PLATFORM_STATUS,SAFETY_STATUS,CONTROL_STATUS,HELP,CONFIG_STATUS,CONFIG_CLEAR,WIFI_SET \"ssid\" \"password\",WIFI_CLEAR,WIFI_STATUS,WIFI_CONNECT,WIFI_DISCONNECT,MAINT_LAN_STATUS,MAINT_TOKEN_SET token,MAINT_TOKEN_CLEAR,OTA_CONFIG,OTA_SET_SERVER host port,OTA_SET_MANIFEST path,OTA_ANNOUNCE_TOKEN_SET token,OTA_ANNOUNCE_TOKEN_CLEAR,OTA_ANNOUNCE_STATUS,OTA_CHECK,OTA_DOWNLOAD_TEST,OTA_UPDATE,OTA_ROLLBACK_STATUS,OTA_ROLLBACK_TEST NONE|NO_CONFIRM_ONCE|SELF_TEST_FAIL_ONCE,OTA_AUTO_STATUS,OTA_AUTO_FORCE_CHECK,OTA_AUTO_INTERVAL [ms],OTA_AUTO_CHECK ON|OFF,OTA_AUTO_UPDATE OFF,TRACE ON|OFF|STATUS,POLL_ONCE,SVD48_INVENTORY,GET_SVD48_CHANNEL_TELEMETRY device_id M1|M2,SVD48_BENCH_SET_SPEED device_id M1|M2 rpm,SVD48_BENCH_HOLD device_id M1|M2,SVD48_BENCH_DISABLE device_id M1|M2,SVD48_BENCH_STOP device_id M1|M2,SVD48_PROBE address,READ_REG drive reg [count],WRITE_REG drive reg value CONFIRM,WRITE_REGS drive start value [value...] CONFIRM,SAVE_SVD48_CONFIG drive CONFIRM,SET_SVD48_GEAR_RATIO drive motor_teeth wheel_teeth CONFIRM,SVD48_IDENTIFY_STATUS drive M1|M2,SVD48_IDENTIFY drive M1|M2 START|STOP CONFIRM,GET_SVD48_CONFIG drive [M1|M2|ALL],APPLY_PY6514_CONFIG drive [M1|M2|ALL] CONFIRM,IBUS_MODE [mode],IBUS_STATUS,IBUS_CHANNELS,IBUS_RAW,IBUS_PIN,PPM_CAPTURE [duration_ms] [interval_us],GET_SPEED n,GET_MOTOR n,SET_SPEED n rpm,ENABLE n|ALL,STOP n|ALL,CLEAR_FAULT n|ALL,MOVE_VEL vx vy wz,ENDPOINTS,SET_ENDPOINT_SPEED id rpm,SET_ENDPOINT_POSITION id degrees,SET_ENDPOINT_POSITION_REFERENCE id degrees CONFIRM,STOP_ENDPOINT id,GET_ENDPOINT_OBSERVATION id,GET_ENDPOINT_POSITION_OBSERVATION id,GET_AS5600_DIAGNOSTICS device_id,STREAM ON|OFF [period_ms]\n");
}

static void print_diagnostic_help(serial_gateway_handle_t handle)
{
    print_locked(
        handle,
        "DATA HELP MODE:DIAGNOSTIC_ONLY COMMANDS:PING,VERSION,HELP,PLATFORM_STATUS,CONFIG_STATUS,WIFI_STATUS,PROFILE_STATUS,COMPOSITION_STATUS,STOP ALL\n");
}

static void handle_profile_status(serial_gateway_handle_t handle)
{
    print_locked(handle,
                 "DATA PROFILE NAME:%s SCHEMA_VALID:%u COMPOSITION_SUPPORTED:%u BOARD:%s\n",
                 safe_text(handle->config.profile_name, "UNKNOWN"),
                 handle->config.profile_schema_valid ? 1U : 0U,
                 handle->config.composition_supported ? 1U : 0U,
                 safe_text(handle->config.board_name, "UNKNOWN"));
}

static bool find_svd48_workspace_channel(
    serial_gateway_handle_t handle,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel_id,
    svd48_workspace_channel_info_t *channel)
{
    size_t count = svd48_workspace_controller_count(
        handle ? handle->config.svd48_workspace : NULL);
    for (size_t index = 0U; index < count; ++index) {
        svd48_workspace_controller_info_t controller;
        if (!svd48_workspace_controller_at(handle->config.svd48_workspace,
                                           index,
                                           &controller) ||
            controller.device_id != device_id ||
            channel_id >= controller.channel_count) {
            continue;
        }
        if (channel) {
            *channel = controller.channels[channel_id];
        }
        return true;
    }
    return false;
}

static bool parse_svd48_workspace_target(int argc,
                                         char *argv[],
                                         int expected_argc,
                                         uint16_t *device_id,
                                         svd48_workspace_channel_id_t *channel)
{
    int8_t parsed_channel = SVD48_CHANNEL_ALL;
    return argc == expected_argc &&
           parse_u16_any_arg(argv[1], device_id) && *device_id != 0U &&
           parse_channel_arg(argv[2], &parsed_channel) && parsed_channel >= 0 &&
           parsed_channel < (int8_t)SVD48_WORKSPACE_CHANNEL_COUNT &&
           ((*channel = (svd48_workspace_channel_id_t)parsed_channel), true);
}

static void handle_svd48_inventory(serial_gateway_handle_t handle,
                                   int argc,
                                   char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE SVD48_INVENTORY\n");
        return;
    }
    size_t count = svd48_workspace_controller_count(
        handle->config.svd48_workspace);
    print_locked(handle,
                 "DATA SVD48_INVENTORY CONTROLLERS:%u\n",
                 (unsigned)count);
    for (size_t index = 0U; index < count; ++index) {
        svd48_workspace_controller_info_t controller;
        if (!svd48_workspace_controller_at(handle->config.svd48_workspace,
                                           index,
                                           &controller)) {
            print_locked(handle,
                         "ERR SVD48_INVENTORY_ENUMERATION_FAILED INDEX:%u\n",
                         (unsigned)index);
            return;
        }
        print_locked(
            handle,
            "DATA SVD48_CONTROLLER DEVICE_ID:%u BUS_ID:%u ADDRESS:%u DRIVER:%s AVAILABLE:%u HEALTH:%s CHANNELS:%u\n",
            (unsigned)controller.device_id,
            (unsigned)controller.bus_id,
            (unsigned)controller.address,
            safe_text(controller.driver, "UNKNOWN"),
            controller.available ? 1U : 0U,
            endpoint_health_name(controller.health),
            (unsigned)controller.channel_count);
        for (size_t channel_index = 0U;
             channel_index < controller.channel_count;
             ++channel_index) {
            const svd48_workspace_channel_info_t *channel =
                &controller.channels[channel_index];
            print_locked(
                handle,
                "DATA SVD48_CHANNEL DEVICE_ID:%u CHANNEL:%s ENDPOINT_BOUND:%u ENDPOINT_ID:%u ENDPOINT_NAME:%s AVAILABLE:%u HEALTH:%s CAPABILITIES:0x%08lx MIN_RPM:%d MAX_RPM:%d\n",
                (unsigned)controller.device_id,
                channel_name((uint8_t)channel->channel),
                channel->endpoint_bound ? 1U : 0U,
                (unsigned)channel->endpoint_id,
                safe_text(channel->endpoint_name, "NONE"),
                channel->available ? 1U : 0U,
                endpoint_health_name(channel->health),
                (unsigned long)channel->capabilities,
                channel->min_rpm,
                channel->max_rpm);
        }
    }
}

static void handle_get_svd48_channel_telemetry(serial_gateway_handle_t handle,
                                               int argc,
                                               char *argv[])
{
    uint16_t device_id = 0U;
    svd48_workspace_channel_id_t channel = SVD48_WORKSPACE_CHANNEL_M1;
    if (!parse_svd48_workspace_target(argc, argv, 3, &device_id, &channel)) {
        print_locked(
            handle,
            "ERR USAGE GET_SVD48_CHANNEL_TELEMETRY device_id M1|M2\n");
        return;
    }
    svd48_workspace_channel_telemetry_t telemetry;
    if (!svd48_workspace_get_channel_telemetry(handle->config.svd48_workspace,
                                               device_id,
                                               channel,
                                               &telemetry)) {
        print_locked(handle,
                     "ERR SVD48_CHANNEL_UNAVAILABLE DEVICE_ID:%u CHANNEL:%s\n",
                     (unsigned)device_id,
                     channel_name((uint8_t)channel));
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t exception_age_ms = telemetry.last_exception_ms == 0U
                                    ? 0U
                                    : now_ms - telemetry.last_exception_ms;
    print_locked(
        handle,
        "DATA SVD48_CHANNEL_TELEMETRY DEVICE_ID:%u CHANNEL:%s ENDPOINT_BOUND:%u ENDPOINT_ID:%u STATUS:%d RPM:%d CURRENT_DA:%d BUS_DV:%d MOTOR_TEMP_DC:%d MOS_TEMP_DC:%d POS:%ld ERROR:0x%08lx ONLINE:%u STALE:%u HEALTH:%s VALID_MASK:0x%08lx FAILED_MASK:0x%08lx STALE_MASK:0x%08lx COMM_ERR:%u EXC_FUNC:0x%02X EXC_CODE:0x%02X EXC_AGE_MS:%lu\n",
        (unsigned)telemetry.device_id,
        channel_name((uint8_t)telemetry.channel),
        telemetry.endpoint_bound ? 1U : 0U,
        (unsigned)telemetry.endpoint_id,
        telemetry.status,
        telemetry.observed_speed_rpm,
        telemetry.current_deciamp,
        telemetry.bus_voltage_deciv,
        telemetry.motor_temp_decic,
        telemetry.mos_temp_decic,
        (long)telemetry.position_counts,
        (unsigned long)telemetry.error_code,
        telemetry.online ? 1U : 0U,
        telemetry.stale ? 1U : 0U,
        endpoint_health_name(telemetry.health),
        (unsigned long)telemetry.valid_observations,
        (unsigned long)telemetry.failed_observations,
        (unsigned long)telemetry.stale_observations,
        (unsigned)telemetry.communication_error,
        telemetry.last_exception_function,
        telemetry.last_exception_code,
        (unsigned long)exception_age_ms);
}

typedef enum {
    SVD48_BENCH_SET_SPEED,
    SVD48_BENCH_HOLD,
    SVD48_BENCH_DISABLE,
    SVD48_BENCH_STOP,
} svd48_bench_operation_t;

static void handle_svd48_bench_operation(serial_gateway_handle_t handle,
                                         int argc,
                                         char *argv[],
                                         svd48_bench_operation_t operation)
{
    const bool takes_rpm = operation == SVD48_BENCH_SET_SPEED;
    uint16_t device_id = 0U;
    svd48_workspace_channel_id_t channel_id = SVD48_WORKSPACE_CHANNEL_M1;
    int16_t rpm = 0;
    if (!parse_svd48_workspace_target(argc,
                                     argv,
                                     takes_rpm ? 4 : 3,
                                     &device_id,
                                     &channel_id) ||
        (takes_rpm && !parse_i16_arg(argv[3], &rpm))) {
        const char *usage = takes_rpm
                                ? "SVD48_BENCH_SET_SPEED device_id M1|M2 rpm"
                                : operation == SVD48_BENCH_HOLD
                                      ? "SVD48_BENCH_HOLD device_id M1|M2"
                                      : operation == SVD48_BENCH_DISABLE
                                            ? "SVD48_BENCH_DISABLE device_id M1|M2"
                                            : "SVD48_BENCH_STOP device_id M1|M2";
        print_locked(handle, "ERR USAGE %s\n", usage);
        return;
    }

    if ((operation == SVD48_BENCH_SET_SPEED ||
         operation == SVD48_BENCH_HOLD) &&
        reject_continuous_control_conflict(handle,
                                           operation == SVD48_BENCH_SET_SPEED
                                               ? "SVD48_BENCH_SET_SPEED"
                                               : "SVD48_BENCH_HOLD")) {
        return;
    }

    svd48_workspace_channel_info_t channel;
    if (!find_svd48_workspace_channel(handle,
                                      device_id,
                                      channel_id,
                                      &channel) ||
        !channel.endpoint_bound) {
        print_locked(handle,
                     "ERR SVD48_CHANNEL_UNBOUND DEVICE_ID:%u CHANNEL:%s\n",
                     (unsigned)device_id,
                     channel_name((uint8_t)channel_id));
        return;
    }
    if (!channel.available &&
        (operation == SVD48_BENCH_SET_SPEED ||
         operation == SVD48_BENCH_HOLD)) {
        print_locked(handle,
                     "ERR SVD48_CHANNEL_UNAVAILABLE DEVICE_ID:%u CHANNEL:%s\n",
                     (unsigned)device_id,
                     channel_name((uint8_t)channel_id));
        return;
    }
    if ((operation == SVD48_BENCH_SET_SPEED ||
         operation == SVD48_BENCH_HOLD) &&
        channel.health != ROBOT_ENDPOINT_HEALTH_HEALTHY) {
        print_locked(
            handle,
            "ERR SVD48_CHANNEL_NOT_HEALTHY DEVICE_ID:%u CHANNEL:%s HEALTH:%s\n",
            (unsigned)device_id,
            channel_name((uint8_t)channel_id),
            endpoint_health_name(channel.health));
        return;
    }
    if ((channel.capabilities & ROBOT_CAPABILITY_STOPPABLE) == 0U ||
        ((takes_rpm || operation == SVD48_BENCH_HOLD) &&
         (channel.capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) == 0U)) {
        print_locked(handle,
                     "ERR SVD48_CHANNEL_CAPABILITY_UNSUPPORTED DEVICE_ID:%u CHANNEL:%s\n",
                     (unsigned)device_id,
                     channel_name((uint8_t)channel_id));
        return;
    }
    if ((takes_rpm || operation == SVD48_BENCH_HOLD) &&
        (rpm < channel.min_rpm || rpm > channel.max_rpm)) {
        print_locked(
            handle,
            "ERR SVD48_BENCH_SPEED_OUT_OF_RANGE DEVICE_ID:%u CHANNEL:%s REQUESTED:%d MIN_RPM:%d MAX_RPM:%d\n",
            (unsigned)device_id,
            channel_name((uint8_t)channel_id),
            rpm,
            channel.min_rpm,
            channel.max_rpm);
        return;
    }

    actuation_application_result_t result =
        operation == SVD48_BENCH_SET_SPEED || operation == SVD48_BENCH_HOLD
            ? actuation_application_set_endpoint_speed_rpm(
                  handle->config.actuation, channel.endpoint_id, rpm)
            : actuation_application_stop_endpoint(handle->config.actuation,
                                                  channel.endpoint_id);
    const char *operation_name = operation == SVD48_BENCH_SET_SPEED
                                     ? "SET_SPEED"
                                     : operation == SVD48_BENCH_HOLD
                                           ? "HOLD"
                                           : operation == SVD48_BENCH_DISABLE
                                                 ? "DISABLE"
                                                 : "STOP";
    if (result == ACTUATION_APPLICATION_OK) {
        print_locked(
            handle,
            "OK SVD48_BENCH_%s DEVICE_ID:%u CHANNEL:%s ENDPOINT_ID:%u RPM_TARGET:%d MODE:%s\n",
            operation_name,
            (unsigned)device_id,
            channel_name((uint8_t)channel_id),
            (unsigned)channel.endpoint_id,
            rpm,
            operation == SVD48_BENCH_SET_SPEED || operation == SVD48_BENCH_HOLD
                ? "ACTIVE"
                : "FREEWHEEL");
        return;
    }
    print_locked(
        handle,
        "ERR SVD48_BENCH_%s_FAILED DEVICE_ID:%u CHANNEL:%s RESULT:%s\n",
        operation_name,
        (unsigned)device_id,
        channel_name((uint8_t)channel_id),
        application_result_name(result));
}

static void handle_endpoints(serial_gateway_handle_t handle,
                             int argc,
                             char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE ENDPOINTS\n");
        return;
    }
    size_t count = actuation_application_endpoint_count(
        handle->config.actuation);
    print_locked(handle, "DATA ENDPOINTS COUNT:%u\n", (unsigned)count);
    for (size_t index = 0; index < count; ++index) {
        actuation_application_endpoint_info_t endpoint;
        if (!actuation_application_endpoint_at(handle->config.actuation,
                                               index,
                                               &endpoint)) {
            print_locked(handle,
                         "ERR ENDPOINT_ENUMERATION_FAILED INDEX:%u\n",
                         (unsigned)index);
            return;
        }
        print_locked(
            handle,
            "DATA ENDPOINT ID:%u NAME:%s CRITICALITY:%s AVAILABLE:%u CAPABILITIES:0x%08lx VELOCITY_RPM:%u VELOCITY_OBSERVATION:%u STOPPABLE:%u MIN_RPM:%d MAX_RPM:%d POSITION:%u POSITION_REFERENCE:%u POSITION_OBSERVATION:%u MIN_POSITION_DEG:%.3f MAX_POSITION_DEG:%.3f\n",
            (unsigned)endpoint.id,
            safe_text(endpoint.name, "UNKNOWN"),
            endpoint_criticality_name(endpoint.criticality),
            endpoint.available ? 1U : 0U,
            (unsigned long)endpoint.capabilities,
            (endpoint.capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) != 0U
                ? 1U
                : 0U,
            endpoint.velocity_observation_supported ? 1U : 0U,
            (endpoint.capabilities & ROBOT_CAPABILITY_STOPPABLE) != 0U ? 1U
                                                                        : 0U,
            endpoint.min_rpm,
            endpoint.max_rpm,
            (endpoint.capabilities & ROBOT_CAPABILITY_POSITION) != 0U ? 1U
                                                                        : 0U,
            (endpoint.capabilities & ROBOT_CAPABILITY_POSITION_REFERENCE) != 0U
                ? 1U
                : 0U,
            endpoint.position_observation_supported ? 1U : 0U,
            (double)endpoint.min_position_degrees,
            (double)endpoint.max_position_degrees);
    }
}

static void handle_set_endpoint_speed(serial_gateway_handle_t handle,
                                      int argc,
                                      char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    int16_t rpm = 0;
    if (argc != 3 || !parse_endpoint_id_arg(argv[1], &endpoint_id) ||
        !parse_i16_arg(argv[2], &rpm)) {
        print_locked(handle, "ERR USAGE SET_ENDPOINT_SPEED id rpm\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    if (!endpoint.available) {
        print_locked(handle,
                     "ERR ENDPOINT_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    if ((endpoint.capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) == 0U) {
        print_locked(
            handle,
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:VELOCITY_RPM\n",
            (unsigned)endpoint_id);
        return;
    }
    if (rpm < endpoint.min_rpm || rpm > endpoint.max_rpm) {
        print_locked(
            handle,
            "ERR ENDPOINT_SPEED_OUT_OF_RANGE ID:%u REQUESTED:%d MIN_RPM:%d MAX_RPM:%d\n",
            (unsigned)endpoint_id,
            rpm,
            endpoint.min_rpm,
            endpoint.max_rpm);
        return;
    }
    actuation_application_result_t result =
        actuation_application_set_endpoint_speed_rpm(
            handle->config.actuation, endpoint_id, rpm);
    if (result == ACTUATION_APPLICATION_OK) {
        print_locked(handle,
                     "OK SET_ENDPOINT_SPEED ID:%u RPM_TARGET:%d\n",
                     (unsigned)endpoint_id,
                     rpm);
        return;
    }
    print_locked(handle,
                 "ERR SET_ENDPOINT_SPEED_FAILED ID:%u RESULT:%s\n",
                 (unsigned)endpoint_id,
                 application_result_name(result));
}

static void handle_set_endpoint_position(serial_gateway_handle_t handle,
                                         int argc,
                                         char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    float degrees = 0.0f;
    if (argc != 3 || !parse_endpoint_id_arg(argv[1], &endpoint_id) ||
        !parse_float_arg(argv[2], &degrees) || !isfinite(degrees)) {
        print_locked(handle, "ERR USAGE SET_ENDPOINT_POSITION id degrees\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    if (!endpoint.available) {
        print_locked(handle,
                     "ERR ENDPOINT_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    if ((endpoint.capabilities & ROBOT_CAPABILITY_POSITION) == 0U) {
        print_locked(
            handle,
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:POSITION\n",
            (unsigned)endpoint_id);
        return;
    }
    if (!isfinite(endpoint.min_position_degrees) ||
        !isfinite(endpoint.max_position_degrees) ||
        endpoint.min_position_degrees > endpoint.max_position_degrees) {
        print_locked(handle,
                     "ERR ENDPOINT_POSITION_RANGE_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    if (degrees < endpoint.min_position_degrees ||
        degrees > endpoint.max_position_degrees) {
        print_locked(
            handle,
            "ERR ENDPOINT_POSITION_OUT_OF_RANGE ID:%u REQUESTED:%.3f MIN_POSITION_DEG:%.3f MAX_POSITION_DEG:%.3f\n",
            (unsigned)endpoint_id,
            (double)degrees,
            (double)endpoint.min_position_degrees,
            (double)endpoint.max_position_degrees);
        return;
    }
    actuation_application_result_t result =
        actuation_application_set_endpoint_position_degrees(
            handle->config.actuation, endpoint_id, degrees);
    if (result == ACTUATION_APPLICATION_OK) {
        print_locked(handle,
                     "OK SET_ENDPOINT_POSITION ID:%u POSITION_TARGET_DEG:%.3f\n",
                     (unsigned)endpoint_id,
                     (double)degrees);
        return;
    }
    print_locked(handle,
                 "ERR SET_ENDPOINT_POSITION_FAILED ID:%u RESULT:%s\n",
                 (unsigned)endpoint_id,
                 application_result_name(result));
}

static void handle_set_endpoint_position_reference(
    serial_gateway_handle_t handle,
    int argc,
    char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    float degrees = 0.0f;
    if (argc != 4 || !parse_endpoint_id_arg(argv[1], &endpoint_id) ||
        !parse_float_arg(argv[2], &degrees) || !isfinite(degrees) ||
        strcasecmp(argv[3], "CONFIRM") != 0) {
        print_locked(
            handle,
            "ERR USAGE SET_ENDPOINT_POSITION_REFERENCE id degrees CONFIRM\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    if (!endpoint.available) {
        print_locked(handle,
                     "ERR ENDPOINT_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    if ((endpoint.capabilities & ROBOT_CAPABILITY_POSITION_REFERENCE) == 0U ||
        (endpoint.capabilities & ROBOT_CAPABILITY_STOPPABLE) == 0U) {
        print_locked(
            handle,
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:POSITION_REFERENCE\n",
            (unsigned)endpoint_id);
        return;
    }
    if (!isfinite(endpoint.min_position_degrees) ||
        !isfinite(endpoint.max_position_degrees) ||
        endpoint.min_position_degrees > endpoint.max_position_degrees) {
        print_locked(handle,
                     "ERR ENDPOINT_POSITION_RANGE_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    if (degrees < endpoint.min_position_degrees ||
        degrees > endpoint.max_position_degrees) {
        print_locked(
            handle,
            "ERR ENDPOINT_POSITION_OUT_OF_RANGE ID:%u REQUESTED:%.3f MIN_POSITION_DEG:%.3f MAX_POSITION_DEG:%.3f\n",
            (unsigned)endpoint_id,
            (double)degrees,
            (double)endpoint.min_position_degrees,
            (double)endpoint.max_position_degrees);
        return;
    }
    actuation_application_result_t result =
        actuation_application_set_endpoint_position_reference_degrees(
            handle->config.actuation, endpoint_id, degrees);
    if (result == ACTUATION_APPLICATION_OK) {
        print_locked(
            handle,
            "OK SET_ENDPOINT_POSITION_REFERENCE ID:%u REFERENCE_DEG:%.3f\n",
            (unsigned)endpoint_id,
            (double)degrees);
        return;
    }
    print_locked(
        handle,
        "ERR SET_ENDPOINT_POSITION_REFERENCE_FAILED ID:%u RESULT:%s\n",
        (unsigned)endpoint_id,
        application_result_name(result));
}

static void handle_stop_endpoint(serial_gateway_handle_t handle,
                                 int argc,
                                 char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    if (argc != 2 || !parse_endpoint_id_arg(argv[1], &endpoint_id)) {
        print_locked(handle, "ERR USAGE STOP_ENDPOINT id\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    if ((endpoint.capabilities & ROBOT_CAPABILITY_STOPPABLE) == 0U) {
        print_locked(
            handle,
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:STOPPABLE\n",
            (unsigned)endpoint_id);
        return;
    }
    actuation_application_result_t result = actuation_application_stop_endpoint(
        handle->config.actuation, endpoint_id);
    if (result == ACTUATION_APPLICATION_OK) {
        print_locked(handle,
                     "OK STOP_ENDPOINT ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    print_locked(handle,
                 "ERR STOP_ENDPOINT_FAILED ID:%u RESULT:%s\n",
                 (unsigned)endpoint_id,
                 application_result_name(result));
}

static void handle_get_endpoint_observation(serial_gateway_handle_t handle,
                                            int argc,
                                            char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    if (argc != 2 || !parse_endpoint_id_arg(argv[1], &endpoint_id)) {
        print_locked(handle, "ERR USAGE GET_ENDPOINT_OBSERVATION id\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    robot_velocity_observation_t observation;
    if (!actuation_application_get_endpoint_velocity_observation(
            handle->config.actuation, endpoint_id, &observation)) {
        print_locked(handle,
                     "ERR ENDPOINT_OBSERVATION_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    print_locked(
        handle,
        "DATA ENDPOINT_OBSERVATION ID:%u TYPE:VELOCITY_RPM VALID:%u RPM:%d TIMESTAMP_MS:%lu SOURCE:%s ONLINE:%u STALE:%u HEALTH:%s HEALTH_AVAILABLE:%u\n",
        (unsigned)endpoint_id,
        observation.valid ? 1U : 0U,
        observation.rpm,
        (unsigned long)observation.timestamp_ms,
        velocity_observation_source_name(observation.source),
        observation.online ? 1U : 0U,
        observation.stale ? 1U : 0U,
        endpoint_health_name(observation.health),
        observation.health != ROBOT_ENDPOINT_HEALTH_UNKNOWN ? 1U : 0U);
}

static void handle_get_endpoint_position_observation(
    serial_gateway_handle_t handle,
    int argc,
    char *argv[])
{
    robot_endpoint_id_t endpoint_id = 0U;
    if (argc != 2 || !parse_endpoint_id_arg(argv[1], &endpoint_id)) {
        print_locked(handle,
                     "ERR USAGE GET_ENDPOINT_POSITION_OBSERVATION id\n");
        return;
    }
    actuation_application_endpoint_info_t endpoint;
    if (!actuation_application_find_endpoint(handle->config.actuation,
                                             endpoint_id,
                                             &endpoint)) {
        print_locked(handle, "ERR BAD_ENDPOINT ID:%u\n", (unsigned)endpoint_id);
        return;
    }
    robot_position_observation_t observation;
    if (!actuation_application_get_endpoint_position_observation(
            handle->config.actuation, endpoint_id, &observation)) {
        print_locked(handle,
                     "ERR ENDPOINT_POSITION_OBSERVATION_UNAVAILABLE ID:%u\n",
                     (unsigned)endpoint_id);
        return;
    }
    print_locked(
        handle,
        "DATA ENDPOINT_POSITION_OBSERVATION ID:%u TYPE:POSITION_DEGREES VALID:%u CALIBRATED:%u REFERENCED:%u DEGREES:%.3f TIMESTAMP_MS:%lu SOURCE_ENDPOINT_ID:%u SOURCE:%s ONLINE:%u STALE:%u HEALTH:%s HEALTH_AVAILABLE:%u STATUS:%s\n",
        (unsigned)endpoint_id,
        observation.valid ? 1U : 0U,
        observation.calibrated ? 1U : 0U,
        observation.referenced ? 1U : 0U,
        (double)observation.degrees,
        (unsigned long)observation.timestamp_ms,
        (unsigned)observation.source_endpoint_id,
        position_observation_source_name(observation.source),
        observation.online ? 1U : 0U,
        observation.stale ? 1U : 0U,
        endpoint_health_name(observation.health),
        observation.health != ROBOT_ENDPOINT_HEALTH_UNKNOWN ? 1U : 0U,
        capability_error_name(observation.status));
}

/* L2/L3-only, profile-device-scoped diagnostics. This reads the last cached
 * AS5600 snapshot through an injected port; it never polls I2C or affects any
 * actuator/controller state. */
static void handle_get_as5600_diagnostics(serial_gateway_handle_t handle,
                                          int argc,
                                          char *argv[])
{
    uint16_t device_id = 0U;
    if (argc != 2 || !parse_u16_any_arg(argv[1], &device_id) ||
        device_id == 0U) {
        print_locked(handle,
                     "ERR USAGE GET_AS5600_DIAGNOSTICS device_id\n");
        return;
    }

    as5600_device_diagnostics_t diagnostics;
    if (!as5600_diagnostics_port_read(handle->config.as5600_diagnostics,
                                      device_id,
                                      &diagnostics)) {
        print_locked(handle,
                     "ERR AS5600_DIAGNOSTICS_UNAVAILABLE DEVICE_ID:%u\n",
                     (unsigned)device_id);
        return;
    }

    char calibration_id[AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX + 1U];
    char calibration_hardware[AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX + 1U];
    char calibration_provenance[AS5600_DIAGNOSTICS_METADATA_TOKEN_MAX + 1U];
    bool calibration_id_truncated = false;
    bool calibration_hardware_truncated = false;
    bool calibration_provenance_truncated = false;
    bool calibration_id_sanitized = false;
    bool calibration_hardware_sanitized = false;
    bool calibration_provenance_sanitized = false;
    as5600_diagnostics_token(
        diagnostics.calibration_configured
            ? diagnostics.calibration_metadata.calibration_id
            : NULL,
        "NONE",
        calibration_id,
        &calibration_id_truncated,
        &calibration_id_sanitized);
    as5600_diagnostics_token(
        diagnostics.calibration_configured
            ? diagnostics.calibration_metadata.hardware_identity
            : NULL,
        "NONE",
        calibration_hardware,
        &calibration_hardware_truncated,
        &calibration_hardware_sanitized);
    as5600_diagnostics_token(
        diagnostics.calibration_configured
            ? diagnostics.calibration_metadata.provenance
            : NULL,
        "NONE",
        calibration_provenance,
        &calibration_provenance_truncated,
        &calibration_provenance_sanitized);

    const as5600_device_snapshot_t *snapshot = &diagnostics.snapshot;
    const as5600_device_communication_t *communication =
        &diagnostics.communication;
    print_locked(
        handle,
        "DATA AS5600_DIAGNOSTICS DEVICE_ID:%u ADDRESS:0x%02X RAW_VALID:%u RAW_ANGLE:%u STATUS:0x%02X MAGNET_DETECTED:%u MAGNET_TOO_WEAK:%u MAGNET_TOO_STRONG:%u SAMPLE_TIMESTAMP_MS:%lu LAST_POLL_TIMESTAMP_MS:%lu ONLINE:%u STALE:%u HEALTH:%s LAST_POLL_RESULT:%s LAST_ERROR:%s DIAGNOSTICS_REQUESTED:%u DIAGNOSTICS_ATTEMPTED:%u DIAGNOSTICS_VALID:%u AGC:%u MAGNITUDE:%u DIAGNOSTICS_TIMESTAMP_MS:%lu DIAGNOSTICS_RESULT:%s CALIBRATION_CONFIGURED:%u CALIBRATION_FORMAT:%u\n",
        (unsigned)diagnostics.device_id,
        (unsigned)diagnostics.i2c_address,
        snapshot->raw_angle_valid ? 1U : 0U,
        (unsigned)snapshot->raw_angle,
        (unsigned)snapshot->status,
        snapshot->magnet_detected ? 1U : 0U,
        snapshot->magnet_too_weak ? 1U : 0U,
        snapshot->magnet_too_strong ? 1U : 0U,
        (unsigned long)snapshot->sample_timestamp_ms,
        (unsigned long)snapshot->last_poll_timestamp_ms,
        snapshot->online ? 1U : 0U,
        snapshot->stale ? 1U : 0U,
        as5600_device_health_name(snapshot->health),
        as5600_device_result_name(snapshot->last_poll_result),
        as5600_device_result_name(snapshot->last_error),
        snapshot->diagnostics_requested ? 1U : 0U,
        snapshot->diagnostics_attempted ? 1U : 0U,
        snapshot->diagnostics_valid ? 1U : 0U,
        (unsigned)snapshot->automatic_gain_control,
        (unsigned)snapshot->magnitude,
        (unsigned long)snapshot->diagnostics_timestamp_ms,
        as5600_device_result_name(snapshot->diagnostics_last_result),
        diagnostics.calibration_configured ? 1U : 0U,
        (unsigned)diagnostics.calibration_metadata.format_version);
    print_locked(
        handle,
        "DATA AS5600_COMMUNICATION DEVICE_ID:%u POLLS:%lu SUCCESSFUL_SAMPLES:%lu FAILED_POLLS:%lu CONSECUTIVE_FAILURES:%lu LAST_SUCCESS_MS:%lu LAST_FAILURE_MS:%lu LAST_ERROR:%s\n",
        (unsigned)diagnostics.device_id,
        (unsigned long)communication->polls,
        (unsigned long)communication->successful_samples,
        (unsigned long)communication->failed_polls,
        (unsigned long)communication->consecutive_failures,
        (unsigned long)communication->last_success_ms,
        (unsigned long)communication->last_failure_ms,
        as5600_device_result_name(communication->last_error));
    print_locked(
        handle,
        "DATA AS5600_CALIBRATION DEVICE_ID:%u ID:%s ID_TRUNCATED:%u ID_SANITIZED:%u HARDWARE:%s HARDWARE_TRUNCATED:%u HARDWARE_SANITIZED:%u PROVENANCE:%s PROVENANCE_TRUNCATED:%u PROVENANCE_SANITIZED:%u\n",
        (unsigned)diagnostics.device_id,
        calibration_id,
        calibration_id_truncated ? 1U : 0U,
        calibration_id_sanitized ? 1U : 0U,
        calibration_hardware,
        calibration_hardware_truncated ? 1U : 0U,
        calibration_hardware_sanitized ? 1U : 0U,
        calibration_provenance,
        calibration_provenance_truncated ? 1U : 0U,
        calibration_provenance_sanitized ? 1U : 0U);
}

static void handle_composition_status(serial_gateway_handle_t handle)
{
    print_locked(
        handle,
        "DATA COMPOSITION MODE:%s RUNTIME_READY:%u CODE:%s STAGE:%s DRIVER:%u BUS:%u DEVICE:%u ENDPOINT:%u ERROR:0x%x REQUIRED_STORAGE:%u AVAILABLE_STORAGE:%u OUTPUTS_INITIALIZED:%u\n",
        handle->config.diagnostic_only ? "DIAGNOSTIC_ONLY" : "ACTIVE",
        handle->config.composition_runtime_ready ? 1U : 0U,
        safe_text(handle->config.composition_code, "UNKNOWN"),
        safe_text(handle->config.composition_stage, "UNKNOWN"),
        (unsigned)handle->config.composition_driver_id,
        (unsigned)handle->config.composition_bus_id,
        (unsigned)handle->config.composition_device_id,
        (unsigned)handle->config.composition_endpoint_id,
        handle->config.composition_error,
        (unsigned)handle->config.composition_required_storage,
        (unsigned)handle->config.composition_available_storage,
        handle->config.composition_runtime_ready ? 1U : 0U);
}

static void handle_diagnostic_command(serial_gateway_handle_t handle,
                                      int argc,
                                      char *argv[])
{
    const char *diagnostic_argv[GATEWAY_ARG_MAX] = {0};
    for (int index = 0; index < argc && index < GATEWAY_ARG_MAX; ++index) {
        diagnostic_argv[index] = argv[index];
    }
    if (!serial_gateway_diagnostic_command_allowed(argc, diagnostic_argv)) {
        print_locked(handle,
                     "ERR DIAGNOSTIC_MODE_COMMAND_BLOCKED %s\n",
                     argv[0]);
        return;
    }

    if (strcasecmp(argv[0], "PING") == 0) {
        print_locked(handle, "OK PONG\n");
    } else if (strcasecmp(argv[0], "VERSION") == 0) {
        handle_version(handle);
    } else if (strcasecmp(argv[0], "HELP") == 0) {
        print_diagnostic_help(handle);
    } else if (strcasecmp(argv[0], "PLATFORM_STATUS") == 0) {
        print_locked(
            handle,
            "DATA PLATFORM STATE:DIAGNOSTIC_ONLY AUTHORITY:SERIAL_ASCII PROTOCOL:ASCII_V1 OUTPUTS_INITIALIZED:0 MOTION_ACTIVE:0 SAFE_FOR_OTA:0 SAFE_REASON:COMPOSITION_UNAVAILABLE TRACE:0 STREAM:0\n");
    } else if (strcasecmp(argv[0], "CONFIG_STATUS") == 0) {
        handle_config_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_STATUS") == 0) {
        handle_wifi_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "PROFILE_STATUS") == 0) {
        handle_profile_status(handle);
    } else if (strcasecmp(argv[0], "COMPOSITION_STATUS") == 0) {
        handle_composition_status(handle);
    } else {
        print_locked(handle, "ERR STOP_UNAVAILABLE OUTPUTS_NOT_INITIALIZED\n");
    }
}

static void print_ibus_status(serial_gateway_handle_t handle, bool include_channels)
{
    if (!handle->config.ibus_receiver) {
        print_locked(handle, "ERR IBUS_UNAVAILABLE\n");
        return;
    }

    ibus_receiver_status_t status;
    esp_err_t err = ibus_receiver_get_status(handle->config.ibus_receiver, &status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR IBUS_STATUS_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA IBUS STATUS:%s MODE:%s UART:%d RX_GPIO:%d BAUD:%lu STALE_TIMEOUT_MS:%lu PULSE_MIN_US:%u PULSE_MAX_US:%u LAST_AGE_MS:%lu BYTES_OR_EDGES:%lu FRAMES:%lu VALID:%lu BAD_HEADER:%lu BAD_CHECKSUM:%lu FRAME_CHANNELS:%u INVALID_PULSES:%lu INCOMPLETE:%lu OVERFLOW:%lu",
                 status.signal_valid ? "OK" : "NO_SIGNAL",
                 ibus_receiver_mode_to_string(status.mode),
                 status.uart_port,
                 status.rx_pin,
                 (unsigned long)status.baud_rate,
                 (unsigned long)status.stale_timeout_ms,
                 status.ppm_min_pulse_us,
                 status.ppm_max_pulse_us,
                 (unsigned long)status.last_frame_age_ms,
                 (unsigned long)status.bytes_received,
                 (unsigned long)status.frames_seen,
                 (unsigned long)status.valid_frames,
                 (unsigned long)status.bad_header_frames,
                 (unsigned long)status.bad_checksum_frames,
                 status.frame_channel_count,
                 (unsigned long)status.invalid_pulses,
                 (unsigned long)status.incomplete_frames,
                 (unsigned long)status.overflow_pulses);
    if (include_channels) {
        for (uint8_t i = 0; i < IBUS_RECEIVER_CHANNELS; i++) {
            print_locked(handle, " CH%u:%u", i + 1, status.channels[i]);
        }
    }
    print_locked(handle, "\n");
}

static void handle_ibus_status(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE IBUS_STATUS\n");
        return;
    }
    print_ibus_status(handle, true);
}

static void handle_ibus_mode(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (!handle->config.ibus_receiver) {
        print_locked(handle, "ERR IBUS_UNAVAILABLE\n");
        return;
    }
    if (argc != 1 && argc != 2) {
        print_locked(handle, "ERR USAGE IBUS_MODE [PPM|IBUS|IBUS_INV|IBUS_8N2|IBUS_INV_8N2|SBUS|SBUS_NOINV]\n");
        return;
    }

    if (argc == 1) {
        ibus_receiver_status_t status;
        esp_err_t err = ibus_receiver_get_status(handle->config.ibus_receiver, &status);
        if (err == ESP_OK) {
            print_locked(handle, "DATA IBUS_MODE MODE:%s\n", ibus_receiver_mode_to_string(status.mode));
        } else {
            print_locked(handle, "ERR IBUS_MODE_FAILED 0x%x\n", err);
        }
        return;
    }

    ibus_receiver_mode_t mode = IBUS_RECEIVER_MODE_IBUS;
    if (!parse_ibus_mode_arg(argv[1], &mode)) {
        print_locked(handle, "ERR USAGE IBUS_MODE [PPM|IBUS|IBUS_INV|IBUS_8N2|IBUS_INV_8N2|SBUS|SBUS_NOINV]\n");
        return;
    }

    esp_err_t err = ibus_receiver_set_mode(handle->config.ibus_receiver, mode);
    if (err == ESP_OK) {
        print_locked(handle, "OK IBUS_MODE MODE:%s\n", ibus_receiver_mode_to_string(mode));
    } else {
        print_locked(handle, "ERR IBUS_MODE_FAILED 0x%x MODE:%s\n", err, ibus_receiver_mode_to_string(mode));
    }
}

static void handle_ibus_channels(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE IBUS_CHANNELS\n");
        return;
    }
    print_ibus_status(handle, true);
}

static void handle_ibus_raw(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE IBUS_RAW\n");
        return;
    }
    if (!handle->config.ibus_receiver) {
        print_locked(handle, "ERR IBUS_UNAVAILABLE\n");
        return;
    }

    ibus_receiver_status_t status;
    esp_err_t err = ibus_receiver_get_status(handle->config.ibus_receiver, &status);
    if (err != ESP_OK) {
        print_locked(handle, "ERR IBUS_RAW_FAILED 0x%x\n", err);
        return;
    }

    print_locked(handle,
                 "DATA IBUS_RAW COUNT:%u BYTES:%lu HEX:",
                 status.raw_sample_count,
                 (unsigned long)status.bytes_received);
    for (uint8_t i = 0; i < status.raw_sample_count; i++) {
        print_locked(handle, "%s%02X", i == 0 ? "" : " ", status.raw_sample[i]);
    }
    print_locked(handle, "\n");
}

static void handle_ibus_pin(serial_gateway_handle_t handle, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        print_locked(handle, "ERR USAGE IBUS_PIN\n");
        return;
    }
    if (!handle->config.ibus_receiver) {
        print_locked(handle, "ERR IBUS_UNAVAILABLE\n");
        return;
    }

    ibus_receiver_pin_sample_t sample;
    esp_err_t err = ibus_receiver_sample_pin(handle->config.ibus_receiver, 2000, 50, &sample);
    if (err != ESP_OK) {
        print_locked(handle, "ERR IBUS_PIN_FAILED 0x%x\n", err);
        return;
    }

    uint32_t high_permille = sample.samples == 0 ? 0 : (sample.high_count * 1000UL) / sample.samples;
    print_locked(handle,
                 "DATA IBUS_PIN RX_GPIO:%d SAMPLES:%lu HIGH:%lu LOW:%lu HIGH_PERMILLE:%lu TRANSITIONS:%lu LAST:%d\n",
                 sample.rx_pin,
                 (unsigned long)sample.samples,
                 (unsigned long)sample.high_count,
                 (unsigned long)sample.low_count,
                 (unsigned long)high_permille,
                 (unsigned long)sample.transitions,
                 sample.last_level);
}

static void handle_ppm_capture(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc > 3) {
        print_locked(handle, "ERR USAGE PPM_CAPTURE [duration_ms] [interval_us]\n");
        return;
    }
    if (!handle->config.ibus_receiver) {
        print_locked(handle, "ERR PPM_UNAVAILABLE\n");
        return;
    }

    uint32_t duration_ms = 120;
    uint32_t interval_us = 20;
    if (argc >= 2 && (!parse_u32_any_arg(argv[1], &duration_ms) || duration_ms == 0 || duration_ms > 1000)) {
        print_locked(handle, "ERR USAGE PPM_CAPTURE [duration_ms<=1000] [interval_us]\n");
        return;
    }
    if (argc >= 3 && (!parse_u32_any_arg(argv[2], &interval_us) || interval_us > 1000)) {
        print_locked(handle, "ERR USAGE PPM_CAPTURE [duration_ms] [interval_us<=1000]\n");
        return;
    }

    ibus_receiver_ppm_capture_t capture;
    esp_err_t err = ibus_receiver_capture_ppm(handle->config.ibus_receiver,
                                              duration_ms * 1000UL,
                                              interval_us,
                                              &capture);
    if (err != ESP_OK) {
        print_locked(handle, "ERR PPM_CAPTURE_FAILED 0x%x\n", err);
        return;
    }

    uint32_t high_permille = capture.samples == 0 ? 0 : (capture.high_count * 1000UL) / capture.samples;
    print_locked(handle,
                 "DATA PPM_CAPTURE RX_GPIO:%d REQUEST_US:%lu ELAPSED_US:%lu INTERVAL_US:%lu SAMPLES:%lu HIGH:%lu LOW:%lu HIGH_PERMILLE:%lu TRANSITIONS:%lu RISES:%lu FALLS:%lu INITIAL:%d LAST:%d EDGES:%u OVERFLOW:%u\n",
                 capture.rx_pin,
                 (unsigned long)capture.requested_duration_us,
                 (unsigned long)capture.elapsed_us,
                 (unsigned long)capture.interval_us,
                 (unsigned long)capture.samples,
                 (unsigned long)capture.high_count,
                 (unsigned long)capture.low_count,
                 (unsigned long)high_permille,
                 (unsigned long)capture.transitions,
                 (unsigned long)capture.rising_edges,
                 (unsigned long)capture.falling_edges,
                 capture.initial_level,
                 capture.last_level,
                 capture.edge_count,
                 capture.edge_overflow ? 1 : 0);

    print_locked(handle, "DATA PPM_EDGES FORMAT:time_us:level:since_prev_us VALUES:");
    for (uint8_t i = 0; i < capture.edge_count; i++) {
        const ibus_receiver_pin_edge_t *edge = &capture.edges[i];
        print_locked(handle,
                     "%s%lu:%d:%lu",
                     i == 0 ? "" : ",",
                     (unsigned long)edge->time_us,
                     edge->level,
                     (unsigned long)edge->duration_since_previous_us);
    }
    print_locked(handle, "\n");

    uint32_t last_rise_us = 0;
    uint16_t frame_channels[12] = { 0 };
    uint16_t current_channels[12] = { 0 };
    uint8_t frame_channel_count = 0;
    uint8_t current_channel_count = 0;
    uint32_t decoded_frames = 0;
    uint32_t sync_gaps = 0;
    print_locked(handle, "DATA PPM_RISE_DELTAS_US VALUES:");
    bool first_delta = true;
    for (uint8_t i = 0; i < capture.edge_count; i++) {
        const ibus_receiver_pin_edge_t *edge = &capture.edges[i];
        if (edge->level != 1) {
            continue;
        }
        if (last_rise_us != 0) {
            uint32_t delta_us = edge->time_us - last_rise_us;
            print_locked(handle, "%s%lu", first_delta ? "" : ",", (unsigned long)delta_us);
            first_delta = false;
            if (delta_us > 3000) {
                sync_gaps++;
                if (current_channel_count > 0) {
                    memcpy(frame_channels, current_channels, sizeof(frame_channels));
                    frame_channel_count = current_channel_count;
                    decoded_frames++;
                }
                memset(current_channels, 0, sizeof(current_channels));
                current_channel_count = 0;
            } else if (delta_us >= 800 && delta_us <= 2200 && current_channel_count < 12) {
                current_channels[current_channel_count++] = (uint16_t)delta_us;
            }
        }
        last_rise_us = edge->time_us;
    }
    print_locked(handle, "\n");

    print_locked(handle,
                 "DATA PPM_DECODE SYNC_GAPS:%lu FRAMES:%lu CHANNELS:%u",
                 (unsigned long)sync_gaps,
                 (unsigned long)decoded_frames,
                 frame_channel_count);
    for (uint8_t i = 0; i < frame_channel_count; i++) {
        print_locked(handle, " CH%u:%u", i + 1, frame_channels[i]);
    }
    print_locked(handle, "\n");
}

static esp_err_t command_each_motor(serial_gateway_handle_t handle, const char *target, esp_err_t (*fn)(robot_control_handle_t, uint8_t))
{
    if (strcasecmp(target, "ALL") == 0) {
        esp_err_t first_error = ESP_OK;
        size_t motor_count = configured_motor_count(handle);
        for (uint8_t motor = 0; motor < motor_count; motor++) {
            esp_err_t err = fn(handle->config.robot, motor);
            if (err != ESP_OK && first_error == ESP_OK) {
                first_error = err;
            }
        }
        return first_error;
    }

    uint8_t motor = 0;
    if (!parse_motor_arg(handle, target, &motor)) {
        return ESP_ERR_INVALID_ARG;
    }
    return fn(handle->config.robot, motor);
}

static esp_err_t enable_one(robot_control_handle_t robot, uint8_t motor)
{
    return robot_control_set_motor_speed(robot, motor, 0);
}

static void handle_enable(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE ENABLE n|ALL\n");
        return;
    }

    if (strcasecmp(argv[1], "ALL") == 0) {
        esp_err_t err = robot_control_enable_all(handle->config.robot);
        if (err == ESP_OK) {
            print_locked(handle, "OK ENABLE ALL\n");
        } else {
            print_locked(handle, "ERR ENABLE_FAILED 0x%x\n", err);
        }
        return;
    }

    esp_err_t err = command_each_motor(handle, argv[1], enable_one);
    if (err == ESP_OK) {
        print_locked(handle, "OK ENABLE %s\n", argv[1]);
    } else {
        print_locked(handle, "ERR ENABLE_FAILED 0x%x\n", err);
    }
}

static void handle_stop(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE STOP n|ALL\n");
        return;
    }

    if (strcasecmp(argv[1], "ALL") == 0) {
        char motion_detail[MOTION_STATUS_DETAIL_MAX] = {0};
        bool control_stopped = !handle->config.motion_control ||
                               motion_control_stop_all(
                                   handle->config.motion_control,
                                   motion_detail,
                                   sizeof(motion_detail));
        esp_err_t err = actuation_application_stop_all(handle->config.actuation) == ACTUATION_APPLICATION_OK ? ESP_OK : ESP_FAIL;
        if (err != ESP_OK) {
            print_locked(handle, "ERR STOP_FAILED 0x%x\n", err);
        } else if (!control_stopped) {
            /* The physical STOP must never be skipped because the semantic
             * authority port was briefly busy.  Report the partial result so
             * callers do not confuse it with a fully revoked session. */
            print_locked(handle,
                         "ERR CONTROL_STOP_FAILED %s PHYSICAL_STOP:OK\n",
                         safe_text(motion_detail, "UNKNOWN"));
        } else {
            print_locked(handle, "OK STOP ALL\n");
        }
        return;
    }

    uint8_t motor = 0;
    if (!parse_motor_arg(handle, argv[1], &motor)) {
        print_locked(handle, "ERR BAD_MOTOR\n");
        return;
    }
    esp_err_t err = actuation_application_stop_legacy_motor(handle->config.actuation, motor) ==
                            ACTUATION_APPLICATION_OK
                        ? ESP_OK
                        : ESP_FAIL;
    if (err == ESP_OK) {
        print_locked(handle, "OK STOP %u\n", motor);
    } else {
        print_locked(handle, "ERR STOP_FAILED 0x%x\n", err);
    }
}

static void handle_clear_fault(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 2) {
        print_locked(handle, "ERR USAGE CLEAR_FAULT n|ALL\n");
        return;
    }

    if (strcasecmp(argv[1], "ALL") == 0) {
        esp_err_t first_error = ESP_OK;
        size_t motor_count = configured_motor_count(handle);
        for (uint8_t motor = 0; motor < motor_count; motor++) {
            esp_err_t err = robot_control_clear_motor_alarm(handle->config.robot, motor);
            if (err != ESP_OK && first_error == ESP_OK) {
                first_error = err;
            }
        }
        if (first_error == ESP_OK) {
            print_locked(handle, "OK CLEAR_FAULT ALL\n");
        } else {
            print_locked(handle, "ERR CLEAR_FAULT_FAILED 0x%x\n", first_error);
        }
        return;
    }

    uint8_t motor = 0;
    if (!parse_motor_arg(handle, argv[1], &motor)) {
        print_locked(handle, "ERR BAD_MOTOR\n");
        return;
    }
    esp_err_t err = robot_control_clear_motor_alarm(handle->config.robot, motor);
    if (err == ESP_OK) {
        print_locked(handle, "OK CLEAR_FAULT %u\n", motor);
    } else {
        print_locked(handle, "ERR CLEAR_FAULT_FAILED 0x%x\n", err);
    }
}

static void print_pc_rx_trace(serial_gateway_handle_t handle, const char *original)
{
    char copy[GATEWAY_LINE_MAX];
    snprintf(copy, sizeof(copy), "%s", original);

    char *argv[GATEWAY_ARG_MAX];
    int argc = split_args(copy, argv, GATEWAY_ARG_MAX);
    if (argc > 0 && strcasecmp(argv[0], "WIFI_SET") == 0) {
        if (argc >= 2) {
            print_locked(handle, "TRACE PC_RX ASCII:\"WIFI_SET %s <redacted>\"\n", argv[1]);
        } else {
            print_locked(handle, "TRACE PC_RX ASCII:\"WIFI_SET <redacted>\"\n");
        }
        return;
    }
    if (argc > 0 && strcasecmp(argv[0], "OTA_ANNOUNCE_TOKEN_SET") == 0) {
        print_locked(handle, "TRACE PC_RX ASCII:\"OTA_ANNOUNCE_TOKEN_SET <redacted>\"\n");
        return;
    }
    if (argc > 0 && strcasecmp(argv[0], "MAINT_TOKEN_SET") == 0) {
        print_locked(handle, "TRACE PC_RX ASCII:\"MAINT_TOKEN_SET <redacted>\"\n");
        return;
    }

    print_locked(handle, "TRACE PC_RX ASCII:\"%s\"\n", original);
}

static void handle_command(serial_gateway_handle_t handle, char *line, serial_gateway_command_policy_t policy)
{
    char original[GATEWAY_LINE_MAX];
    snprintf(original, sizeof(original), "%s", line);

    char *argv[GATEWAY_ARG_MAX];
    int argc = split_args(line, argv, GATEWAY_ARG_MAX);
    if (argc < 0) {
        print_locked(handle, "ERR BAD_COMMAND_SYNTAX\n");
        return;
    }
    if (argc == 0) {
        return;
    }

    if (!command_allowed_for_policy(policy, argc, argv)) {
        print_locked(handle, "ERR LAN_COMMAND_BLOCKED %s\n", argv[0]);
        return;
    }

    if (handle->config.diagnostic_only) {
        handle_diagnostic_command(handle, argc, argv);
        return;
    }

    if (robot_control_get_trace_enabled(handle->config.robot)) {
        print_pc_rx_trace(handle, original);
    }

    if (strcasecmp(argv[0], "PING") == 0) {
        print_locked(handle, "OK PONG\n");
    } else if (strcasecmp(argv[0], "VERSION") == 0) {
        if (argc != 1) {
            print_locked(handle, "ERR USAGE VERSION\n");
            return;
        }
        handle_version(handle);
    } else if (strcasecmp(argv[0], "PROFILE_STATUS") == 0) {
        if (argc != 1) {
            print_locked(handle, "ERR USAGE PROFILE_STATUS\n");
            return;
        }
        handle_profile_status(handle);
    } else if (strcasecmp(argv[0], "COMPOSITION_STATUS") == 0) {
        if (argc != 1) {
            print_locked(handle, "ERR USAGE COMPOSITION_STATUS\n");
            return;
        }
        handle_composition_status(handle);
    } else if (strcasecmp(argv[0], "PLATFORM_STATUS") == 0) {
        handle_platform_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SAFETY_STATUS") == 0) {
        handle_safety_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "CONTROL_STATUS") == 0) {
        handle_control_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "CONFIG_STATUS") == 0) {
        handle_config_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "CONFIG_CLEAR") == 0) {
        handle_config_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_SET") == 0) {
        handle_wifi_set(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_CLEAR") == 0) {
        handle_wifi_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_STATUS") == 0) {
        handle_wifi_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_CONNECT") == 0) {
        handle_wifi_connect(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_DISCONNECT") == 0) {
        handle_wifi_disconnect(handle, argc, argv);
    } else if (strcasecmp(argv[0], "MAINT_LAN_STATUS") == 0) {
        handle_maintenance_lan_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "MAINT_TOKEN_SET") == 0) {
        handle_maintenance_lan_token_set(handle, argc, argv);
    } else if (strcasecmp(argv[0], "MAINT_TOKEN_CLEAR") == 0) {
        handle_maintenance_lan_token_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_SET_SERVER") == 0) {
        handle_ota_set_server(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_SET_MANIFEST") == 0) {
        handle_ota_set_manifest(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_CONFIG") == 0) {
        handle_ota_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_ANNOUNCE_TOKEN_SET") == 0) {
        handle_ota_announce_token_set(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_ANNOUNCE_TOKEN_CLEAR") == 0) {
        handle_ota_announce_token_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_ANNOUNCE_STATUS") == 0) {
        handle_ota_announce_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_CHECK") == 0) {
        handle_ota_check(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_DOWNLOAD_TEST") == 0) {
        handle_ota_download_test(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_UPDATE") == 0) {
        handle_ota_update(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_ROLLBACK_STATUS") == 0) {
        handle_ota_rollback_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_ROLLBACK_TEST") == 0) {
        handle_ota_rollback_test(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_STATUS") == 0) {
        handle_ota_auto_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_FORCE_CHECK") == 0) {
        handle_ota_auto_force_check(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_INTERVAL") == 0) {
        handle_ota_auto_interval(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_CHECK") == 0) {
        handle_ota_auto_check(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_UPDATE") == 0) {
        handle_ota_auto_update(handle, argc, argv);
    } else if (strcasecmp(argv[0], "HELP") == 0) {
        print_help(handle);
    } else if (strcasecmp(argv[0], "IBUS_STATUS") == 0) {
        handle_ibus_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "IBUS_MODE") == 0) {
        handle_ibus_mode(handle, argc, argv);
    } else if (strcasecmp(argv[0], "IBUS_CHANNELS") == 0) {
        handle_ibus_channels(handle, argc, argv);
    } else if (strcasecmp(argv[0], "IBUS_RAW") == 0) {
        handle_ibus_raw(handle, argc, argv);
    } else if (strcasecmp(argv[0], "IBUS_PIN") == 0) {
        handle_ibus_pin(handle, argc, argv);
    } else if (strcasecmp(argv[0], "PPM_CAPTURE") == 0) {
        handle_ppm_capture(handle, argc, argv);
    } else if (strcasecmp(argv[0], "TRACE") == 0) {
        if (argc != 2) {
            print_locked(handle, "ERR USAGE TRACE ON|OFF|STATUS\n");
            return;
        }
        if (strcasecmp(argv[1], "ON") == 0) {
            robot_control_set_trace_enabled(handle->config.robot, true);
            print_locked(handle, "OK TRACE ON\n");
        } else if (strcasecmp(argv[1], "OFF") == 0) {
            robot_control_set_trace_enabled(handle->config.robot, false);
            print_locked(handle, "OK TRACE OFF\n");
        } else if (strcasecmp(argv[1], "STATUS") == 0) {
            print_locked(handle,
                         "DATA TRACE ENABLED:%u CRC_INIT:0xFFFF CRC_POLY:0xA001 CRC_ORDER:HIGH_LOW\n",
                         robot_control_get_trace_enabled(handle->config.robot) ? 1 : 0);
        } else {
            print_locked(handle, "ERR USAGE TRACE ON|OFF|STATUS\n");
        }
    } else if (strcasecmp(argv[0], "POLL_ONCE") == 0) {
        if (argc != 1) {
            print_locked(handle, "ERR USAGE POLL_ONCE\n");
            return;
        }
        esp_err_t err = robot_control_poll_once(handle->config.robot);
        if (err == ESP_OK) {
            print_locked(handle, "OK POLL_ONCE\n");
        } else {
            print_locked(handle, "ERR POLL_ONCE_FAILED 0x%x\n", err);
        }
    } else if (strcasecmp(argv[0], "SVD48_INVENTORY") == 0) {
        handle_svd48_inventory(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_SVD48_CHANNEL_TELEMETRY") == 0) {
        handle_get_svd48_channel_telemetry(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SVD48_BENCH_SET_SPEED") == 0) {
        handle_svd48_bench_operation(
            handle, argc, argv, SVD48_BENCH_SET_SPEED);
    } else if (strcasecmp(argv[0], "SVD48_BENCH_HOLD") == 0) {
        handle_svd48_bench_operation(handle, argc, argv, SVD48_BENCH_HOLD);
    } else if (strcasecmp(argv[0], "SVD48_BENCH_DISABLE") == 0) {
        handle_svd48_bench_operation(handle, argc, argv, SVD48_BENCH_DISABLE);
    } else if (strcasecmp(argv[0], "SVD48_BENCH_STOP") == 0) {
        handle_svd48_bench_operation(handle, argc, argv, SVD48_BENCH_STOP);
    } else if (strcasecmp(argv[0], "READ_REG") == 0) {
        handle_read_reg(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SVD48_PROBE") == 0) {
        handle_svd48_probe(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WRITE_REG") == 0) {
        handle_write_reg(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WRITE_REGS") == 0) {
        handle_write_regs(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SAVE_SVD48_CONFIG") == 0) {
        handle_save_svd48_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SET_SVD48_GEAR_RATIO") == 0) {
        handle_set_svd48_gear_ratio(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SVD48_IDENTIFY_STATUS") == 0) {
        handle_svd48_identify_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SVD48_IDENTIFY") == 0) {
        handle_svd48_identify(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_SVD48_CONFIG") == 0) {
        handle_get_svd48_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "APPLY_PY6514_CONFIG") == 0) {
        handle_apply_py6514_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "ENDPOINTS") == 0) {
        handle_endpoints(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SET_ENDPOINT_SPEED") == 0) {
        handle_set_endpoint_speed(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SET_ENDPOINT_POSITION") == 0) {
        handle_set_endpoint_position(handle, argc, argv);
    } else if (strcasecmp(argv[0], "SET_ENDPOINT_POSITION_REFERENCE") == 0) {
        handle_set_endpoint_position_reference(handle, argc, argv);
    } else if (strcasecmp(argv[0], "STOP_ENDPOINT") == 0) {
        handle_stop_endpoint(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_ENDPOINT_OBSERVATION") == 0) {
        handle_get_endpoint_observation(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_ENDPOINT_POSITION_OBSERVATION") == 0) {
        handle_get_endpoint_position_observation(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_AS5600_DIAGNOSTICS") == 0) {
        handle_get_as5600_diagnostics(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_SPEED") == 0) {
        uint8_t motor = 0;
        if (argc != 2 || !parse_motor_arg(handle, argv[1], &motor)) {
            print_locked(handle, "ERR USAGE GET_SPEED n\n");
            return;
        }
        svd48_motor_telemetry_t t;
        if (robot_control_get_motor(handle->config.robot, motor, &t)) {
            print_locked(handle, "DATA MOTOR_%u RPM:%d STALE:%u ONLINE:%u\n", motor, t.actual_rpm, t.stale ? 1 : 0, t.online ? 1 : 0);
        } else {
            print_locked(handle, "ERR BAD_MOTOR\n");
        }
    } else if (strcasecmp(argv[0], "GET_MOTOR") == 0) {
        uint8_t motor = 0;
        if (argc != 2 || !parse_motor_arg(handle, argv[1], &motor)) {
            print_locked(handle, "ERR USAGE GET_MOTOR n\n");
            return;
        }
        print_motor_full(handle, motor);
    } else if (strcasecmp(argv[0], "SET_SPEED") == 0) {
        uint8_t motor = 0;
        int16_t rpm = 0;
        if (argc != 3 || !parse_motor_arg(handle, argv[1], &motor) || !parse_i16_arg(argv[2], &rpm)) {
            print_locked(handle, "ERR USAGE SET_SPEED n rpm\n");
            return;
        }
        int16_t min_rpm = 0;
        int16_t max_rpm = 0;
        if (!actuation_application_legacy_motor_limits_rpm(handle->config.actuation,
                                                           motor,
                                                           &min_rpm,
                                                           &max_rpm)) {
            print_locked(handle, "ERR BAD_MOTOR\n");
            return;
        }
        float max_abs_rpm = fmaxf(fabsf((float)min_rpm),
                                  fabsf((float)max_rpm));
        if (rpm < min_rpm || rpm > max_rpm) {
            print_locked(handle,
                         "ERR SET_SPEED_OUT_OF_RANGE REQUESTED:%d MAX_RPM:%.1f\n",
                         rpm,
                         (double)max_abs_rpm);
            return;
        }
        esp_err_t err = actuation_application_set_legacy_motor_speed_rpm(
                            handle->config.actuation, motor, rpm) == ACTUATION_APPLICATION_OK
                            ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            print_locked(handle, "OK MOTOR_%u RPM_TARGET:%d\n", motor, rpm);
        } else {
            print_locked(handle, "ERR SET_SPEED_FAILED 0x%x\n", err);
        }
    } else if (strcasecmp(argv[0], "ENABLE") == 0) {
        handle_enable(handle, argc, argv);
    } else if (strcasecmp(argv[0], "STOP") == 0) {
        handle_stop(handle, argc, argv);
    } else if (strcasecmp(argv[0], "CLEAR_FAULT") == 0) {
        handle_clear_fault(handle, argc, argv);
    } else if (strcasecmp(argv[0], "MOVE_VEL") == 0) {
        float vx = 0.0f;
        float vy = 0.0f;
        float wz = 0.0f;
        if (argc != 4 || !parse_float_arg(argv[1], &vx) || !parse_float_arg(argv[2], &vy) || !parse_float_arg(argv[3], &wz)) {
            print_locked(handle, "ERR USAGE MOVE_VEL vx vy wz\n");
            return;
        }
        esp_err_t err = robot_control_move_vel(handle->config.robot, vx, vy, wz);
        if (err != ESP_OK) {
            print_locked(handle, "ERR MOVE_VEL_FAILED 0x%x\n", err);
            return;
        }
        robot_motion_command_t cmd;
        (void)robot_control_get_last_motion(handle->config.robot, &cmd);
        print_locked(handle,
                     "OK MOVE_VEL VX:%.3f VY:%.3f WZ:%.3f M0:%d/%.1f M1:%d/%.1f M2:%d/%.1f M3:%d/%.1f\n",
                     cmd.vx_mps,
                     cmd.vy_mps,
                     cmd.wz_radps,
                     cmd.wheel_rpm[0], cmd.steering_deg[0],
                     cmd.wheel_rpm[1], cmd.steering_deg[1],
                     cmd.wheel_rpm[2], cmd.steering_deg[2],
                     cmd.wheel_rpm[3], cmd.steering_deg[3]);
    } else if (strcasecmp(argv[0], "STREAM") == 0) {
        if (argc < 2 || argc > 3) {
            print_locked(handle, "ERR USAGE STREAM ON|OFF [period_ms]\n");
            return;
        }
        if (strcasecmp(argv[1], "ON") == 0) {
            if (argc == 3) {
                char *end = NULL;
                long period = strtol(argv[2], &end, 10);
                if (*argv[2] == '\0' || *end != '\0' || period < 50 || period > 10000) {
                    print_locked(handle, "ERR BAD_PERIOD\n");
                    return;
                }
                handle->stream_period_ms = (uint32_t)period;
            }
            handle->stream_enabled = true;
            print_locked(handle, "OK STREAM ON PERIOD_MS:%lu\n", (unsigned long)handle->stream_period_ms);
        } else if (strcasecmp(argv[1], "OFF") == 0) {
            handle->stream_enabled = false;
            print_locked(handle, "OK STREAM OFF\n");
        } else {
            print_locked(handle, "ERR USAGE STREAM ON|OFF [period_ms]\n");
        }
    } else {
        print_locked(handle, "ERR UNKNOWN_COMMAND\n");
    }
}

static void gateway_rx_task(void *arg)
{
    serial_gateway_handle_t handle = (serial_gateway_handle_t)arg;
    serial_gateway_line_framer_t framer;
    serial_gateway_line_framer_init(&framer);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    }

    print_locked(handle, "OK READY SVD48_GATEWAY\n");
    if (handle->config.diagnostic_only) {
        print_diagnostic_help(handle);
    } else {
        print_help(handle);
    }
    print_prompt(handle);

    while (handle->running) {
        bool had_input = false;

        for (int drained = 0; drained < GATEWAY_RX_DRAIN_MAX; drained++) {
            char ch = '\0';
            ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
            if (bytes_read <= 0) {
                if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                break;
            }

            had_input = true;
            serial_gateway_frame_event_t event = serial_gateway_line_framer_feed(&framer, ch);
            if (event == SERIAL_GATEWAY_FRAME_LINE_READY) {
                char *clean = trim(framer.line);
                (void)serial_gateway_execute_command(handle,
                                                     clean,
                                                     SERIAL_GATEWAY_POLICY_FULL_SERIAL,
                                                     NULL,
                                                     NULL);
                print_prompt(handle);
                continue;
            }
            if (event == SERIAL_GATEWAY_FRAME_LINE_TOO_LONG) {
                print_locked(handle, "ERR LINE_TOO_LONG\n");
                print_prompt(handle);
            }
        }

        if (!had_input) {
            vTaskDelay(GATEWAY_RX_IDLE_TICKS);
        }
    }

    vTaskDelete(NULL);
}

static void gateway_stream_task(void *arg)
{
    serial_gateway_handle_t handle = (serial_gateway_handle_t)arg;
    while (handle->running) {
        if (handle->stream_enabled) {
            size_t motor_count = configured_motor_count(handle);
            for (uint8_t motor = 0; motor < motor_count; motor++) {
                print_motor_full(handle, motor);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(handle->stream_period_ms));
    }
    vTaskDelete(NULL);
}

esp_err_t serial_gateway_execute_command(serial_gateway_handle_t handle,
                                         const char *line,
                                         serial_gateway_command_policy_t policy,
                                         serial_gateway_output_fn_t output_fn,
                                         void *output_ctx)
{
    if (!handle || !line) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strnlen(line, GATEWAY_LINE_MAX) >= GATEWAY_LINE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char copy[GATEWAY_LINE_MAX];
    snprintf(copy, sizeof(copy), "%s", line);
    char *clean = trim(copy);
    if (clean[0] == '\0') {
        return ESP_OK;
    }

    if (xSemaphoreTake(handle->command_lock, pdMS_TO_TICKS(GATEWAY_COMMAND_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    handle->active_output = output_fn;
    handle->active_output_ctx = output_ctx;
    handle->active_output_task = output_fn ? xTaskGetCurrentTaskHandle() : NULL;

    handle_command(handle, clean, policy);

    handle->active_output = NULL;
    handle->active_output_ctx = NULL;
    handle->active_output_task = NULL;

    xSemaphoreGive(handle->command_lock);
    return ESP_OK;
}

serial_gateway_handle_t serial_gateway_init(const serial_gateway_config_t *config)
{
    if (!config ||
        (!config->diagnostic_only && (!config->robot || !config->actuation))) {
        return NULL;
    }

    serial_gateway_handle_t handle = calloc(1, sizeof(struct serial_gateway_t));
    if (!handle) {
        return NULL;
    }

    handle->config = *config;
    handle->stream_period_ms = config->default_stream_period_ms == 0 ?
        GATEWAY_DEFAULT_STREAM_MS : config->default_stream_period_ms;
    handle->print_lock = xSemaphoreCreateMutex();
    if (!handle->print_lock) {
        free(handle);
        return NULL;
    }
    handle->command_lock = xSemaphoreCreateMutex();
    if (!handle->command_lock) {
        vSemaphoreDelete(handle->print_lock);
        free(handle);
        return NULL;
    }

    return handle;
}

void serial_gateway_deinit(serial_gateway_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->running = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (handle->print_lock) {
        vSemaphoreDelete(handle->print_lock);
    }
    if (handle->command_lock) {
        vSemaphoreDelete(handle->command_lock);
    }
    free(handle);
}

esp_err_t serial_gateway_start(serial_gateway_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        return ESP_OK;
    }

    handle->running = true;
    if (xTaskCreate(gateway_rx_task, "serial_gateway", GATEWAY_RX_TASK_STACK, handle, 6, &handle->rx_task) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    if (!handle->config.diagnostic_only &&
        xTaskCreate(gateway_stream_task,
                    "gateway_stream",
                    GATEWAY_STREAM_TASK_STACK,
                    handle,
                    4,
                    &handle->stream_task) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial gateway started");
    return ESP_OK;
}
