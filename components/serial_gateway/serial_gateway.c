#include "serial_gateway.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "serial_gateway";

#define GATEWAY_LINE_MAX 160
#define GATEWAY_ARG_MAX 10
#define GATEWAY_DEFAULT_STREAM_MS 200
#define SVD48_PY6514_POLE_PAIRS 10
#define SVD48_PY6514_SENSOR_HALL 1
#define SVD48_PY6514_WHEEL_DIAMETER_MM 330
#define SVD48_PY6514_MOTOR_TEETH 1
#define SVD48_PY6514_WHEEL_TEETH 5
#define SVD48_CHANNEL_ALL (-1)
#define GATEWAY_RX_IDLE_TICKS 1
#define GATEWAY_RX_DRAIN_MAX 256

struct serial_gateway_t {
    serial_gateway_config_t config;
    SemaphoreHandle_t print_lock;
    TaskHandle_t rx_task;
    TaskHandle_t stream_task;
    bool running;
    bool stream_enabled;
    uint32_t stream_period_ms;
};

static void print_locked(serial_gateway_handle_t handle, const char *fmt, ...)
{
    va_list args;
    xSemaphoreTake(handle->print_lock, portMAX_DELAY);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
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
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save);
         tok && argc < max_args;
         tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    return argc;
}

static bool parse_u8_arg(const char *text, uint8_t *value)
{
    if (!text || !value) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || parsed < 0 || parsed >= SVD48_MOTOR_COUNT) {
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

    print_locked(handle,
                 "DATA MOTOR_%u RPM:%d CURRENT_DA:%d STEER_DEG:%.1f STATUS:%d BUS_DV:%d MOTOR_TEMP_DC:%d MOS_TEMP_DC:%d POS:%ld ERROR:0x%08lx ONLINE:%u STALE:%u\n",
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
                 t.stale ? 1 : 0);
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

static void handle_version(serial_gateway_handle_t handle)
{
    const esp_partition_t *partition = esp_ota_get_running_partition();
    const char *partition_label = partition ? partition->label : "UNKNOWN";

    print_locked(handle,
                 "DATA VERSION PROJECT:%s TARGET:%s VERSION:%s BUILD_NUMBER:%lu IDF:%s PARTITION:%s\n",
                 safe_text(handle->config.fw_project, "UNKNOWN"),
                 safe_text(handle->config.fw_target, "UNKNOWN"),
                 safe_text(handle->config.fw_version, "UNKNOWN"),
                 (unsigned long)handle->config.fw_build_number,
                 esp_get_idf_version(),
                 safe_text(partition_label, "UNKNOWN"));
}

static esp_err_t get_config_snapshot(serial_gateway_handle_t handle, config_manager_snapshot_t *snapshot)
{
    if (!handle->config.config_manager) {
        return ESP_ERR_INVALID_STATE;
    }
    return config_manager_get_snapshot(handle->config.config_manager, snapshot);
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
                 "DATA CONFIG WIFI_SSID:%s WIFI_PASSWORD:%s OTA_HOST:%s OTA_PORT:%u OTA_MANIFEST:%s OTA_AUTO_CHECK:%u OTA_AUTO_UPDATE:%u\n",
                 snapshot.wifi_ssid[0] ? snapshot.wifi_ssid : "<empty>",
                 snapshot.wifi_password_set ? "<set>" : "<empty>",
                 snapshot.ota_server_host,
                 snapshot.ota_server_port,
                 snapshot.ota_manifest_path,
                 snapshot.ota_auto_check_enabled ? 1 : 0,
                 snapshot.ota_auto_update_enabled ? 1 : 0);
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
        print_locked(handle, "OK CONFIG_CLEAR\n");
    } else {
        print_locked(handle, "ERR CONFIG_CLEAR_FAILED 0x%x\n", err);
    }
}

static void handle_wifi_set(serial_gateway_handle_t handle, int argc, char *argv[])
{
    if (argc != 3) {
        print_locked(handle, "ERR USAGE WIFI_SET ssid password\n");
        return;
    }
    if (!handle->config.config_manager) {
        print_locked(handle, "ERR CONFIG_MANAGER_UNAVAILABLE\n");
        return;
    }

    esp_err_t err = config_manager_set_wifi(handle->config.config_manager, argv[1], argv[2]);
    if (err == ESP_OK) {
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
        print_locked(handle, "OK WIFI_CLEAR\n");
    } else {
        print_locked(handle, "ERR WIFI_CLEAR_FAILED 0x%x\n", err);
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
                 "DATA OTA_CONFIG HOST:%s PORT:%u MANIFEST:%s AUTO_CHECK:%u AUTO_UPDATE:%u\n",
                 snapshot.ota_server_host,
                 snapshot.ota_server_port,
                 snapshot.ota_manifest_path,
                 snapshot.ota_auto_check_enabled ? 1 : 0,
                 snapshot.ota_auto_update_enabled ? 1 : 0);
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
        print_locked(handle, "OK OTA_AUTO_CHECK %s\n", enabled ? "ON" : "OFF");
    } else {
        print_locked(handle, "ERR OTA_AUTO_CHECK_FAILED 0x%x\n", err);
    }
}

static void handle_ota_auto_update(serial_gateway_handle_t handle, int argc, char *argv[])
{
    bool enabled = false;
    if (argc != 2 || !parse_on_off_arg(argv[1], &enabled)) {
        print_locked(handle, "ERR USAGE OTA_AUTO_UPDATE OFF\n");
        return;
    }
    if (enabled) {
        print_locked(handle, "ERR AUTO_UPDATE_DISABLED_UNTIL_MANUAL_OTA_VALIDATED\n");
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

static void handle_write_reg(serial_gateway_handle_t handle, int argc, char *argv[])
{
    uint8_t drive_id = 0;
    uint16_t reg = 0;
    uint16_t value = 0;
    if (argc != 4 ||
        !parse_drive_id_arg(argv[1], &drive_id) ||
        !parse_u16_any_arg(argv[2], &reg) ||
        !parse_u16_any_arg(argv[3], &value)) {
        print_locked(handle, "ERR USAGE WRITE_REG drive_id reg value\n");
        return;
    }

    esp_err_t err = robot_control_write_svd48_register(handle->config.robot, drive_id, reg, value);
    if (err == ESP_OK) {
        print_locked(handle, "OK WRITE_REG DRIVE:%u REG:0x%04x VALUE:0x%04x/%u\n", drive_id, reg, value, value);
    } else {
        print_locked(handle, "ERR WRITE_REG_FAILED DRIVE:%u REG:0x%04x VALUE:0x%04x ERR:0x%x\n", drive_id, reg, value, err);
    }
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
                 "DATA HELP COMMANDS:PING,VERSION,HELP,CONFIG_STATUS,CONFIG_CLEAR,WIFI_SET ssid password,WIFI_CLEAR,OTA_CONFIG,OTA_SET_SERVER host port,OTA_SET_MANIFEST path,OTA_AUTO_CHECK ON|OFF,OTA_AUTO_UPDATE OFF,TRACE ON|OFF|STATUS,POLL_ONCE,READ_REG drive reg [count],WRITE_REG drive reg value,GET_SVD48_CONFIG drive [M1|M2|ALL],APPLY_PY6514_CONFIG drive [M1|M2|ALL] CONFIRM,GET_SPEED n,GET_MOTOR n,SET_SPEED n rpm,ENABLE n|ALL,STOP n|ALL,CLEAR_FAULT n|ALL,MOVE_VEL vx vy wz,STREAM ON|OFF [period_ms]\n");
}

static esp_err_t command_each_motor(serial_gateway_handle_t handle, const char *target, esp_err_t (*fn)(robot_control_handle_t, uint8_t))
{
    if (strcasecmp(target, "ALL") == 0) {
        esp_err_t first_error = ESP_OK;
        for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
            esp_err_t err = fn(handle->config.robot, motor);
            if (err != ESP_OK && first_error == ESP_OK) {
                first_error = err;
            }
        }
        return first_error;
    }

    uint8_t motor = 0;
    if (!parse_u8_arg(target, &motor)) {
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
        esp_err_t err = robot_control_stop_all(handle->config.robot);
        if (err == ESP_OK) {
            print_locked(handle, "OK STOP ALL\n");
        } else {
            print_locked(handle, "ERR STOP_FAILED 0x%x\n", err);
        }
        return;
    }

    uint8_t motor = 0;
    if (!parse_u8_arg(argv[1], &motor)) {
        print_locked(handle, "ERR BAD_MOTOR\n");
        return;
    }
    esp_err_t err = robot_control_stop_motor(handle->config.robot, motor);
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
        for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
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
    if (!parse_u8_arg(argv[1], &motor)) {
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

    print_locked(handle, "TRACE PC_RX ASCII:\"%s\"\n", original);
}

static void handle_command(serial_gateway_handle_t handle, char *line)
{
    char original[GATEWAY_LINE_MAX];
    snprintf(original, sizeof(original), "%s", line);

    char *argv[GATEWAY_ARG_MAX];
    int argc = split_args(line, argv, GATEWAY_ARG_MAX);
    if (argc == 0) {
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
    } else if (strcasecmp(argv[0], "CONFIG_STATUS") == 0) {
        handle_config_status(handle, argc, argv);
    } else if (strcasecmp(argv[0], "CONFIG_CLEAR") == 0) {
        handle_config_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_SET") == 0) {
        handle_wifi_set(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WIFI_CLEAR") == 0) {
        handle_wifi_clear(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_SET_SERVER") == 0) {
        handle_ota_set_server(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_SET_MANIFEST") == 0) {
        handle_ota_set_manifest(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_CONFIG") == 0) {
        handle_ota_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_CHECK") == 0) {
        handle_ota_auto_check(handle, argc, argv);
    } else if (strcasecmp(argv[0], "OTA_AUTO_UPDATE") == 0) {
        handle_ota_auto_update(handle, argc, argv);
    } else if (strcasecmp(argv[0], "HELP") == 0) {
        print_help(handle);
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
    } else if (strcasecmp(argv[0], "READ_REG") == 0) {
        handle_read_reg(handle, argc, argv);
    } else if (strcasecmp(argv[0], "WRITE_REG") == 0) {
        handle_write_reg(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_SVD48_CONFIG") == 0) {
        handle_get_svd48_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "APPLY_PY6514_CONFIG") == 0) {
        handle_apply_py6514_config(handle, argc, argv);
    } else if (strcasecmp(argv[0], "GET_SPEED") == 0) {
        uint8_t motor = 0;
        if (argc != 2 || !parse_u8_arg(argv[1], &motor)) {
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
        if (argc != 2 || !parse_u8_arg(argv[1], &motor)) {
            print_locked(handle, "ERR USAGE GET_MOTOR n\n");
            return;
        }
        print_motor_full(handle, motor);
    } else if (strcasecmp(argv[0], "SET_SPEED") == 0) {
        uint8_t motor = 0;
        int16_t rpm = 0;
        if (argc != 3 || !parse_u8_arg(argv[1], &motor) || !parse_i16_arg(argv[2], &rpm)) {
            print_locked(handle, "ERR USAGE SET_SPEED n rpm\n");
            return;
        }
        esp_err_t err = robot_control_set_motor_speed(handle->config.robot, motor, rpm);
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
    char line[GATEWAY_LINE_MAX];
    size_t line_len = 0;

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    }

    print_locked(handle, "OK READY SVD48_GATEWAY\n");
    print_help(handle);
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
            if (ch == '\r' || ch == '\n') {
                if (line_len == 0) {
                    continue;
                }
                line[line_len] = '\0';
                char *clean = trim(line);
                handle_command(handle, clean);
                print_prompt(handle);
                line_len = 0;
                continue;
            }

            if (line_len >= sizeof(line) - 1) {
                line_len = 0;
                print_locked(handle, "ERR LINE_TOO_LONG\n");
                print_prompt(handle);
                continue;
            }

            line[line_len++] = ch;
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
            for (uint8_t motor = 0; motor < SVD48_MOTOR_COUNT; motor++) {
                print_motor_full(handle, motor);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(handle->stream_period_ms));
    }
    vTaskDelete(NULL);
}

serial_gateway_handle_t serial_gateway_init(const serial_gateway_config_t *config)
{
    if (!config || !config->robot) {
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
    if (xTaskCreate(gateway_rx_task, "serial_gateway", 4096, handle, 6, &handle->rx_task) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(gateway_stream_task, "gateway_stream", 4096, handle, 4, &handle->stream_task) != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial gateway started");
    return ESP_OK;
}
