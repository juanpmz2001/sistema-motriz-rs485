#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app_version.h"
#include "config_manager.h"
#include "control_lan.h"
#include "ibus_receiver.h"
#include "maintenance_lan.h"
#include "motion_application_service.h"
#include "nvs_flash.h"
#include "ota_announce.h"
#include "ota_manager.h"
#include "robot_control.h"
#include "robot_composition.h"
#include "robot_profile.h"
#include "robot_safety.h"
#include "serial_gateway.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static const robot_profile_t *profile = NULL;

static robot_control_handle_t robot = NULL;
static serial_gateway_handle_t gateway = NULL;
static config_manager_handle_t config_manager = NULL;
static wifi_manager_handle_t wifi_manager = NULL;
static ota_manager_handle_t ota_manager = NULL;
static ota_announce_handle_t ota_announce = NULL;
static maintenance_lan_handle_t maintenance_lan = NULL;
static ibus_receiver_handle_t ibus_receiver = NULL;
static robot_safety_handle_t robot_safety = NULL;
static motion_application_service_handle_t motion_application = NULL;
static control_lan_handle_t control_lan = NULL;
static robot_composition_t composition;

static bool motion_safety_gate(void *context,
                               char *detail,
                               size_t detail_size)
{
    robot_safety_handle_t safety = context;
    robot_safety_status_t status;
    if (!safety || robot_safety_get_status(safety, &status) != ESP_OK) {
        snprintf(detail, detail_size, "%s", "SAFETY_STATUS_UNAVAILABLE");
        return false;
    }
    if (!status.task_running) {
        snprintf(detail, detail_size, "%s", "SAFETY_TASK_NOT_RUNNING");
        return false;
    }
    if (status.motor_fault_active) {
        snprintf(detail, detail_size, "%s", "MOTOR_FAULT");
        return false;
    }
    if (status.rc_loss_active) {
        snprintf(detail, detail_size, "%s", "RC_LOSS");
        return false;
    }
    snprintf(detail, detail_size, "%s", "SAFE");
    return true;
}

static control_lan_callback_result_t control_lan_event_callback(
    control_lan_event_t event,
    void *context)
{
    motion_application_event_action_t action;
    switch (event.action) {
    case CONTROL_LAN_ACTION_ARM:
        action = MOTION_APPLICATION_EVENT_ARM;
        break;
    case CONTROL_LAN_ACTION_COMMAND:
        action = MOTION_APPLICATION_EVENT_COMMAND;
        break;
    case CONTROL_LAN_ACTION_DISARM:
        action = MOTION_APPLICATION_EVENT_DISARM;
        break;
    case CONTROL_LAN_ACTION_STOP:
        action = MOTION_APPLICATION_EVENT_STOP;
        break;
    default:
        return (control_lan_callback_result_t){
            .accepted = false,
            .detail = "INVALID_ACTION",
        };
    }
    motion_application_event_t command = {
        .action = action,
        .stream_id = event.stream_id_hash,
        .sequence = event.sequence,
        .received_at_ms = event.timestamp_us / 1000U,
        .vx_mps = event.vx_mps,
        .vy_mps = event.vy_mps,
        .wz_radps = event.wz_radps,
        .deadman = event.deadman,
    };
    motion_application_submit_result_t result =
        motion_application_service_publish(context, &command);
    control_lan_callback_result_t response = {
        .accepted = result.accepted,
    };
    snprintf(response.detail, sizeof(response.detail), "%s", result.detail);
    return response;
}

static void deinit_control_plane(void)
{
    control_lan_deinit(control_lan);
    control_lan = NULL;
    motion_application_service_deinit(motion_application);
    motion_application = NULL;
}

static esp_err_t start_control_plane(void)
{
    if (profile->application.kind != ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY) {
        ESP_LOGW(TAG,
                 "Continuous LAN control disabled: profile has no qualified motion geometry");
        return ESP_OK;
    }
    const motion_application_service_config_t motion_config = {
        .profile = profile,
        .actuation = &composition.application_port,
        .period_ms = MOTION_APPLICATION_DEFAULT_PERIOD_MS,
        .task_priority = MOTION_APPLICATION_DEFAULT_TASK_PRIORITY,
        .safety_gate = motion_safety_gate,
        .safety_context = robot_safety,
    };
    esp_err_t error = motion_application_service_init(&motion_config,
                                                       &motion_application);
    if (error != ESP_OK) {
        return error;
    }
    error = motion_application_service_start(motion_application);
    if (error != ESP_OK) {
        deinit_control_plane();
        return error;
    }

    float max_vx_mps;
    float max_vy_mps;
    float max_wz_radps;
    if (!motion_application_service_limits(motion_application,
                                           &max_vx_mps,
                                           &max_vy_mps,
                                           &max_wz_radps)) {
        deinit_control_plane();
        return ESP_ERR_INVALID_STATE;
    }
    const control_lan_config_t control_config = {
        .config_manager = config_manager,
        .listen_port = CONTROL_LAN_DEFAULT_PORT,
        .task_priority = CONTROL_LAN_DEFAULT_TASK_PRIORITY,
        .max_abs_vx_mps = max_vx_mps,
        .max_abs_vy_mps = max_vy_mps,
        .max_abs_wz_radps = max_wz_radps,
        .event_callback = control_lan_event_callback,
        .callback_context = motion_application,
    };
    error = control_lan_init(&control_config, &control_lan);
    if (error == ESP_OK) {
        error = control_lan_start(control_lan);
    }
    if (error != ESP_OK) {
        deinit_control_plane();
        return error;
    }
    ESP_LOGI(TAG,
             "Continuous LAN control active on UDP:%u ttl:%lums",
             CONTROL_LAN_DEFAULT_PORT,
             (unsigned long)profile->application.control_ttl_ms);
    return ESP_OK;
}

static float profile_max_abs_rpm(const robot_profile_t *selected_profile)
{
    int16_t maximum = 0;
    for (size_t index = 0; selected_profile && index < selected_profile->endpoint_count;
         ++index) {
        const robot_endpoint_profile_t *endpoint =
            &selected_profile->endpoints[index];
        int16_t negative = endpoint->min_rpm < 0
                               ? (int16_t)-endpoint->min_rpm
                               : endpoint->min_rpm;
        int16_t positive = endpoint->max_rpm < 0
                               ? (int16_t)-endpoint->max_rpm
                               : endpoint->max_rpm;
        if (negative > maximum) {
            maximum = negative;
        }
        if (positive > maximum) {
            maximum = positive;
        }
    }
    return maximum > 0 ? (float)maximum : 1.0f;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase before first use, err=0x%x", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void rollback_pending_app(const char *stage, esp_err_t err)
{
    ESP_LOGE(TAG, "Pending OTA app failed self-test stage:%s err=0x%x; rolling back", stage, err);
    esp_err_t rollback_err = ota_manager_mark_app_invalid_and_rollback();
    ESP_LOGE(TAG, "Rollback request failed, err=0x%x", rollback_err);
}

static void handle_startup_failure(const char *stage, esp_err_t err, bool pending_verify)
{
    ESP_LOGE(TAG, "Startup failed at %s, err=0x%x", stage, err);
    if (pending_verify) {
        rollback_pending_app(stage, err);
    }
}

static void confirm_pending_app_after_self_test(void)
{
    ota_manager_rollback_test_mode_t test_mode = OTA_MANAGER_ROLLBACK_TEST_NONE;
    esp_err_t err = ota_manager_consume_rollback_test_mode(&test_mode);
    if (err != ESP_OK) {
        rollback_pending_app("rollback_test_mode", err);
        return;
    }

    if (test_mode == OTA_MANAGER_ROLLBACK_TEST_NO_CONFIRM_ONCE) {
        ESP_LOGW(TAG, "Rollback test mode NO_CONFIRM_ONCE consumed; rebooting before app validation");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    if (test_mode == OTA_MANAGER_ROLLBACK_TEST_SELF_TEST_FAIL_ONCE) {
        rollback_pending_app("forced_self_test_failure", ESP_FAIL);
        return;
    }

    err = ota_manager_mark_app_valid();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Pending OTA app marked valid after subsystem self-test");
        return;
    }

    rollback_pending_app("mark_app_valid", err);
}

static esp_err_t start_safe_diagnostic_gateway(
    const robot_composition_diagnostics_t *diagnostics,
    esp_err_t composition_error)
{
    if (!diagnostics || !diagnostics->schema_valid ||
        diagnostics->composition_supported) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_manager_deinit(ota_manager);
    ota_manager = NULL;
    robot_composition_deinit(&composition);

    serial_gateway_config_t gateway_config = {
        .config_manager = config_manager,
        .wifi_manager = wifi_manager,
        .fw_project = FW_PROJECT,
        .fw_target = FW_TARGET,
        .fw_version = FW_VERSION,
        .fw_build_number = FW_BUILD_NUMBER,
        .fw_git_sha = FW_GIT_SHA,
        .fw_git_dirty = FW_GIT_DIRTY != 0,
        .default_stream_period_ms = 200,
        .print_prompt = false,
        .diagnostic_only = true,
        .profile_name = profile ? profile->name : robot_profile_selected_name(),
        .board_name = profile && profile->board ? profile->board->id : "UNKNOWN",
        .profile_schema_valid = diagnostics->schema_valid,
        .composition_supported = diagnostics->composition_supported,
        .composition_runtime_ready = false,
        .composition_code =
            robot_composition_diagnostic_code_name(diagnostics->code),
        .composition_stage = robot_composition_stage_name(diagnostics->stage),
        .composition_driver_id = (uint16_t)diagnostics->driver_id,
        .composition_bus_id = diagnostics->bus_id,
        .composition_device_id = diagnostics->device_id,
        .composition_endpoint_id = diagnostics->endpoint_id,
        .composition_error = composition_error,
        .composition_required_storage = diagnostics->required_storage,
        .composition_available_storage = diagnostics->available_storage,
    };
    gateway = serial_gateway_init(&gateway_config);
    if (!gateway) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = serial_gateway_start(gateway);
    if (error != ESP_OK) {
        serial_gateway_deinit(gateway);
        gateway = NULL;
    }
    return error;
}

void app_main(void)
{
    ESP_LOGI(TAG, "SVD48 robot framework starting");
    ESP_LOGI(TAG,
             "Firmware project:%s target:%s version:%s build:%d",
             FW_PROJECT,
             FW_TARGET,
             FW_VERSION,
             FW_BUILD_NUMBER);
    ESP_LOGI(TAG, "Read docs/SVD48.md before changing RS485 behavior");

    ota_manager_boot_state_t boot_state;
    esp_err_t boot_state_err = ota_manager_get_boot_state(&boot_state);
    bool pending_verify = boot_state_err == ESP_OK && boot_state.pending_verify;
    if (boot_state_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Boot partition:%s ota_state:%s pending_verify:%u rollback_possible:%u",
                 boot_state.partition_label[0] ? boot_state.partition_label : "UNKNOWN",
                 boot_state.state_known ? ota_manager_image_state_to_string(boot_state.state) : "UNKNOWN",
                 boot_state.pending_verify ? 1 : 0,
                 boot_state.rollback_possible ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "Failed to read OTA boot state, err=0x%x", boot_state_err);
    }

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        handle_startup_failure("nvs", err, pending_verify);
        return;
    }

    err = config_manager_init(&config_manager);
    if (err != ESP_OK) {
        handle_startup_failure("config_manager", err, pending_verify);
        return;
    }

    err = wifi_manager_init(config_manager, &wifi_manager);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi manager unavailable, err=0x%x; robot startup continues", err);
        wifi_manager = NULL;
    }

    if (wifi_manager) {
        ota_manager_config_t ota_config = {
            .config_manager = config_manager,
            .wifi_manager = wifi_manager,
            .current_project = FW_PROJECT,
            .current_target = FW_TARGET,
            .current_build_number = FW_BUILD_NUMBER,
        };
        err = ota_manager_init(&ota_config, &ota_manager);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA check manager unavailable, err=0x%x; robot startup continues", err);
            ota_manager = NULL;
        }
    } else {
        ESP_LOGW(TAG, "OTA check manager disabled because Wi-Fi manager is unavailable");
    }

    profile = robot_profile_selected();
    ESP_LOGI(TAG, "Selected robot profile:%s", robot_profile_selected_name());
    const robot_bus_profile_t *rc_bus = robot_profile_find_bus_type(profile, ROBOT_BUS_GPIO);
    err = robot_composition_init(&composition, profile);
    if (err != ESP_OK) {
        const robot_composition_diagnostics_t *diagnostics =
            robot_composition_get_diagnostics(&composition);
        robot_composition_diagnostics_t diagnostic_snapshot = {0};
        if (diagnostics) {
            diagnostic_snapshot = *diagnostics;
        }
        ESP_LOGE(TAG,
                 "Composition unavailable schema=%u supported=%u code=%s stage=%s driver=%u bus=%u device=%u endpoint=%u",
                 diagnostics && diagnostics->schema_valid ? 1U : 0U,
                 diagnostics && diagnostics->composition_supported ? 1U : 0U,
                 diagnostics ? robot_composition_diagnostic_code_name(diagnostics->code)
                             : "UNKNOWN",
                 diagnostics ? robot_composition_stage_name(diagnostics->stage)
                             : "UNKNOWN",
                 diagnostics ? (unsigned)diagnostics->driver_id : 0U,
                 diagnostics ? diagnostics->bus_id : 0U,
                 diagnostics ? diagnostics->device_id : 0U,
                 diagnostics ? diagnostics->endpoint_id : 0U);
        if (diagnostics && diagnostics->schema_valid &&
            !diagnostics->composition_supported && !pending_verify) {
            esp_err_t diagnostic_error = start_safe_diagnostic_gateway(
                &diagnostic_snapshot, err);
            if (diagnostic_error == ESP_OK) {
                ESP_LOGW(TAG,
                         "Safe diagnostic mode active; outputs and automatic network/OTA tasks are disabled");
                while (1) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
            handle_startup_failure("diagnostic_gateway",
                                   diagnostic_error,
                                   false);
            wifi_manager_deinit(wifi_manager);
            config_manager_deinit(config_manager);
            return;
        }
        handle_startup_failure("robot_composition", err, pending_verify);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    robot_control_config_t robot_config = {
        .svd48 = robot_composition_legacy_svd48(&composition),
        .wheelbase_m = profile->application.wheelbase_m, .track_width_m = profile->application.track_width_m,
        .wheel_radius_m = profile->application.wheel_radius_m,
        .max_wheel_rpm = profile_max_abs_rpm(profile),
        .motion_kinematics_enabled =
            profile->application.kind == ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY,
        .enable_steering_servos = false,
        .steering_servo_pins = { -1, -1, -1, -1 },
        .servo_min_us = 1000, .servo_center_us = 1500,
        .servo_max_us = 2000, .servo_min_deg = -90.0f,
        .servo_max_deg = 90.0f,
    };

    robot = robot_control_init(&robot_config);
    if (!robot) {
        handle_startup_failure("robot_control", ESP_FAIL, pending_verify);
        robot_composition_deinit(&composition);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    robot_composition_attach_legacy_robot(&composition, robot);
    err = actuation_application_stop_all(&composition.application_port) == ACTUATION_APPLICATION_OK
              ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Boot stop was not acknowledged by every configured motor, err=0x%x; startup continues",
                 err);
    }

    if (robot_composition_start(&composition) != ESP_OK) {
        handle_startup_failure("svd48_polling", ESP_FAIL, pending_verify);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    if (rc_bus) {
        ibus_receiver_config_t ibus_config = {
            .uart_port = (uart_port_t)rc_bus->peripheral,
            .rx_pin = rc_bus->pins[0],
            .tx_pin = UART_PIN_NO_CHANGE,
            .baud_rate = 0,
            .stale_timeout_ms = 300,
            .invert_rx = false,
            .mode = IBUS_RECEIVER_MODE_PPM,
            .ppm_channel_count = 10,
            .ppm_min_frame_channels = 4,
            .ppm_sync_threshold_us = 3000,
            .ppm_min_pulse_us = 750,
            .ppm_max_pulse_us = 2250,
        };
        err = ibus_receiver_init(&ibus_config, &ibus_receiver);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "RC receiver unavailable on GPIO%d, err=0x%x; robot startup continues",
                     rc_bus->pins[0],
                     err);
            ibus_receiver = NULL;
        }
    } else {
        ESP_LOGW(TAG, "RC bus omitted by profile; RC receiver disabled");
    }

    robot_safety_config_t safety_config = {
        .robot = robot,
        .stop_port = &composition.application_port,
        .ibus_receiver = ibus_receiver,
        .period_ms = 20,
        .rc_loss_timeout_ms = 150,
        .stop_repeat_ms = 500,
        .stop_on_rc_loss = true,
        .stop_on_motor_fault = true,
    };
    err = robot_safety_init(&safety_config, &robot_safety);
    if (err != ESP_OK) {
        handle_startup_failure("robot_safety_init", err, pending_verify);
        ibus_receiver_deinit(ibus_receiver);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }
    err = robot_safety_start(robot_safety);
    if (err != ESP_OK) {
        handle_startup_failure("robot_safety_start", err, pending_verify);
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    err = start_control_plane();
    if (err != ESP_OK) {
        handle_startup_failure("control_plane", err, pending_verify);
        deinit_control_plane();
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    if (ota_manager && wifi_manager) {
        ota_announce_config_t announce_config = {
            .config_manager = config_manager,
            .wifi_manager = wifi_manager,
            .ota_manager = ota_manager,
            .robot = robot,
            .listen_port = OTA_ANNOUNCE_DEFAULT_PORT,
        };
        err = ota_announce_init(&announce_config, &ota_announce);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA LAN announce listener unavailable, err=0x%x; robot startup continues", err);
            ota_announce = NULL;
        }
    }

    serial_gateway_config_t gateway_config = {
        .robot = robot,
        .actuation = &composition.application_port,
        .motion_control = motion_application_service_control_port(
            motion_application),
        .motion_status = motion_application_service_status_port(
            motion_application),
        .svd48_workspace =
            robot_composition_svd48_workspace_port(&composition),
        .as5600_diagnostics =
            robot_composition_as5600_diagnostics_port(&composition),
        .config_manager = config_manager,
        .wifi_manager = wifi_manager,
        .ota_manager = ota_manager,
        .ota_announce = ota_announce,
        .ibus_receiver = ibus_receiver,
        .robot_safety = robot_safety,
        .fw_project = FW_PROJECT,
        .fw_target = FW_TARGET,
        .fw_version = FW_VERSION,
        .fw_build_number = FW_BUILD_NUMBER,
        .fw_git_sha = FW_GIT_SHA,
        .fw_git_dirty = FW_GIT_DIRTY != 0,
        .default_stream_period_ms = 200,
        .print_prompt = false,
        .profile_name = profile->name,
        .board_name = profile->board->id,
        .profile_schema_valid = true,
        .composition_supported = true,
        .composition_runtime_ready = composition.diagnostics.runtime_ready,
        .composition_code = robot_composition_diagnostic_code_name(
            composition.diagnostics.code),
        .composition_stage = robot_composition_stage_name(
            composition.diagnostics.stage),
        .composition_required_storage =
            composition.diagnostics.required_storage,
        .composition_available_storage =
            composition.diagnostics.available_storage,
    };

    gateway = serial_gateway_init(&gateway_config);
    if (!gateway) {
        handle_startup_failure("serial_gateway_init", ESP_FAIL, pending_verify);
        ota_announce_deinit(ota_announce);
        deinit_control_plane();
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    if (serial_gateway_start(gateway) != ESP_OK) {
        handle_startup_failure("serial_gateway_start", ESP_FAIL, pending_verify);
        serial_gateway_deinit(gateway);
        ota_announce_deinit(ota_announce);
        deinit_control_plane();
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_composition_deinit(&composition);
        robot_control_deinit(robot);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    maintenance_lan_config_t maintenance_config = {
        .config_manager = config_manager,
        .wifi_manager = wifi_manager,
        .gateway = gateway,
        .listen_port = MAINTENANCE_LAN_DEFAULT_PORT,
    };
    err = maintenance_lan_init(&maintenance_config, &maintenance_lan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Maintenance LAN listener unavailable, err=0x%x; robot startup continues", err);
        maintenance_lan = NULL;
    }

    if (pending_verify) {
        confirm_pending_app_after_self_test();
    }

    if (wifi_manager) {
        err = wifi_manager_start_auto_connect_task(wifi_manager);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Wi-Fi reconnect supervisor unavailable, err=0x%x", err);
        }
    }

    if (ota_manager) {
        err = ota_manager_start_auto_check_task(ota_manager);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Automatic OTA_CHECK task unavailable, err=0x%x", err);
        }
    }

    if (ota_announce) {
        err = ota_announce_start(ota_announce);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "OTA LAN announce listener failed to start, err=0x%x", err);
        }
    }

    if (maintenance_lan) {
        err = maintenance_lan_start(maintenance_lan);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Maintenance LAN listener failed to start, err=0x%x", err);
        }
    }

    ESP_LOGI(TAG, "Ready for diagnostics. Try: VERSION, SAFETY_STATUS, IBUS_STATUS, READ_REG 1 0x5018 1");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
