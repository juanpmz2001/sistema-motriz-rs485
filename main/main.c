#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "app_version.h"
#include "config_manager.h"
#include "nvs_flash.h"
#include "robot_control.h"
#include "serial_gateway.h"
#include "svd48.h"

static const char *TAG = "main";

// ESP32-S3 <-> UART-RS485 converter <-> two SVD48 drives.
#define RS485_UART_PORT UART_NUM_2
#define RS485_TX_PIN 17
#define RS485_RX_PIN 16
#define RS485_RTS_PIN UART_PIN_NO_CHANGE

// Steering servo defaults. Change these to match the robot harness.
#define STEER_FL_PIN 4
#define STEER_FR_PIN 5
#define STEER_RL_PIN 6
#define STEER_RR_PIN 7

static svd48_handle_t svd48 = NULL;
static robot_control_handle_t robot = NULL;
static serial_gateway_handle_t gateway = NULL;
static config_manager_handle_t config_manager = NULL;

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

void app_main(void)
{
    ESP_LOGI(TAG, "SVD48 robot framework starting");
    ESP_LOGI(TAG,
             "Firmware project:%s target:%s version:%s build:%d",
             FW_PROJECT,
             FW_TARGET,
             FW_VERSION,
             FW_BUILD_NUMBER);
    ESP_LOGI(TAG, "Read docs/skills/SVD48B50A_SKILL.md before changing RS485 behavior");

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS, err=0x%x", err);
        return;
    }

    err = config_manager_init(&config_manager);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config manager, err=0x%x", err);
        return;
    }

    svd48_config_t svd48_config = {
        .uart_port = RS485_UART_PORT,
        .tx_pin = RS485_TX_PIN,
        .rx_pin = RS485_RX_PIN,
        .rts_pin = RS485_RTS_PIN,
        .use_rs485_half_duplex = false,
        .baud_rate = 115200,
        .drive_ids = { 1, 2 },
        .response_timeout_ms = 100,
        .retries = 2,
        .telemetry_period_ms = 30,
        .stale_timeout_ms = 1000,
    };

    svd48 = svd48_init(&svd48_config);
    if (!svd48) {
        ESP_LOGE(TAG, "Failed to initialize SVD48 bus");
        config_manager_deinit(config_manager);
        return;
    }

    robot_control_config_t robot_config = {
        .svd48 = svd48,
        .wheelbase_m = 0.50f,
        .track_width_m = 0.40f,
        .wheel_radius_m = 0.10f,
        .max_wheel_rpm = 1000.0f,
        .enable_steering_servos = true,
        .steering_servo_pins = { STEER_FL_PIN, STEER_FR_PIN, STEER_RL_PIN, STEER_RR_PIN },
        .servo_min_us = 1000,
        .servo_center_us = 1500,
        .servo_max_us = 2000,
        .servo_min_deg = -90.0f,
        .servo_max_deg = 90.0f,
    };

    robot = robot_control_init(&robot_config);
    if (!robot) {
        ESP_LOGE(TAG, "Failed to initialize robot control");
        svd48_deinit(svd48);
        config_manager_deinit(config_manager);
        return;
    }

    if (svd48_start_polling(svd48) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SVD48 telemetry polling");
        robot_control_deinit(robot);
        svd48_deinit(svd48);
        config_manager_deinit(config_manager);
        return;
    }

    serial_gateway_config_t gateway_config = {
        .robot = robot,
        .config_manager = config_manager,
        .fw_project = FW_PROJECT,
        .fw_target = FW_TARGET,
        .fw_version = FW_VERSION,
        .fw_build_number = FW_BUILD_NUMBER,
        .default_stream_period_ms = 200,
        .print_prompt = false,
    };

    gateway = serial_gateway_init(&gateway_config);
    if (!gateway) {
        ESP_LOGE(TAG, "Failed to initialize serial gateway");
        robot_control_deinit(robot);
        svd48_deinit(svd48);
        config_manager_deinit(config_manager);
        return;
    }

    if (serial_gateway_start(gateway) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start serial gateway");
        serial_gateway_deinit(gateway);
        robot_control_deinit(robot);
        svd48_deinit(svd48);
        config_manager_deinit(config_manager);
        return;
    }

    ESP_LOGI(TAG, "Ready. Try: PING, GET_SPEED 0, GET_MOTOR 0, MOVE_VEL 1.0 0.0 0.5");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
