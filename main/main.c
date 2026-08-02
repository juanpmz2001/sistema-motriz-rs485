#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app_version.h"
#include "config_manager.h"
#include "ibus_receiver.h"
#include "maintenance_lan.h"
#include "nvs_flash.h"
#include "ota_announce.h"
#include "ota_manager.h"
#include "robot_control.h"
#include "robot_composition.h"
#include "robot_profile.h"
#include "robot_safety.h"
#include "serial_gateway.h"
#include "svd48.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static const robot_profile_t *profile = NULL;

static svd48_handle_t svd48 = NULL;
static robot_control_handle_t robot = NULL;
static serial_gateway_handle_t gateway = NULL;
static config_manager_handle_t config_manager = NULL;
static wifi_manager_handle_t wifi_manager = NULL;
static ota_manager_handle_t ota_manager = NULL;
static ota_announce_handle_t ota_announce = NULL;
static maintenance_lan_handle_t maintenance_lan = NULL;
static ibus_receiver_handle_t ibus_receiver = NULL;
static robot_safety_handle_t robot_safety = NULL;
static robot_composition_t composition;

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

    profile = robot_profile_current();
    if (robot_profile_validate(profile) != ROBOT_PROFILE_VALID) {
        handle_startup_failure("robot_profile", ESP_ERR_INVALID_ARG, pending_verify);
        return;
    }
    svd48_config_t svd48_config = {
        .uart_port = (uart_port_t)profile->rs485.uart_port,
        .tx_pin = profile->rs485.tx_pin, .rx_pin = profile->rs485.rx_pin,
        .rts_pin = profile->rs485.rts_pin, .use_rs485_half_duplex = profile->rs485.half_duplex,
        .baud_rate = profile->rs485.baud_rate,
        .drive_ids = { profile->rs485.drive_ids[0], profile->rs485.drive_ids[1] },
        .response_timeout_ms = profile->rs485.response_timeout_ms, .retries = profile->rs485.retries,
        .telemetry_period_ms = profile->rs485.telemetry_period_ms, .stale_timeout_ms = profile->rs485.stale_timeout_ms,
    };

    svd48 = svd48_init(&svd48_config);
    if (!svd48) {
        handle_startup_failure("svd48", ESP_FAIL, pending_verify);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    robot_control_config_t robot_config = {
        .svd48 = svd48,
        .wheelbase_m = profile->wheelbase_m, .track_width_m = profile->track_width_m,
        .wheel_radius_m = profile->wheel_radius_m, .max_wheel_rpm = profile->max_wheel_rpm,
        .enable_steering_servos = profile->steering_servos_enabled,
        .steering_servo_pins = { profile->steering_servo_pins[0], profile->steering_servo_pins[1], profile->steering_servo_pins[2], profile->steering_servo_pins[3] },
        .servo_min_us = profile->servo_min_us, .servo_center_us = profile->servo_center_us,
        .servo_max_us = profile->servo_max_us, .servo_min_deg = profile->servo_min_deg,
        .servo_max_deg = profile->servo_max_deg,
    };

    robot = robot_control_init(&robot_config);
    if (!robot) {
        handle_startup_failure("robot_control", ESP_FAIL, pending_verify);
        svd48_deinit(svd48);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    err = robot_composition_init(&composition, profile, robot);
    if (err != ESP_OK) { handle_startup_failure("robot_composition", err, pending_verify); return; }
    actuation_report_t boot_stop_report;
    err = actuation_coordinator_stop_all(&composition.coordinator, &boot_stop_report) == ACTUATION_RESULT_SUCCESS ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Boot stop was not acknowledged by every configured motor, err=0x%x; startup continues",
                 err);
    }

    if (svd48_start_polling(svd48) != ESP_OK) {
        handle_startup_failure("svd48_polling", ESP_FAIL, pending_verify);
        robot_control_deinit(robot);
        svd48_deinit(svd48);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    ibus_receiver_config_t ibus_config = {
        .uart_port = (uart_port_t)profile->rc_uart_port,
        .rx_pin = profile->rc_rx_pin,
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
                 profile->rc_rx_pin,
                 err);
        ibus_receiver = NULL;
    }

    robot_safety_config_t safety_config = {
        .robot = robot,
        .actuation = &composition.coordinator,
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
        robot_control_deinit(robot);
        svd48_deinit(svd48);
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
        robot_control_deinit(robot);
        svd48_deinit(svd48);
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
        .actuation = &composition.coordinator,
        .profile = profile,
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
        .default_stream_period_ms = 200,
        .print_prompt = false,
    };

    gateway = serial_gateway_init(&gateway_config);
    if (!gateway) {
        handle_startup_failure("serial_gateway_init", ESP_FAIL, pending_verify);
        ota_announce_deinit(ota_announce);
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_control_deinit(robot);
        svd48_deinit(svd48);
        ota_manager_deinit(ota_manager);
        wifi_manager_deinit(wifi_manager);
        config_manager_deinit(config_manager);
        return;
    }

    if (serial_gateway_start(gateway) != ESP_OK) {
        handle_startup_failure("serial_gateway_start", ESP_FAIL, pending_verify);
        serial_gateway_deinit(gateway);
        ota_announce_deinit(ota_announce);
        robot_safety_deinit(robot_safety);
        ibus_receiver_deinit(ibus_receiver);
        robot_control_deinit(robot);
        svd48_deinit(svd48);
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
