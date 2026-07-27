#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "app_version.h"
#include "config_manager.h"
#include "new_leg_test.h"
#include "ota_manager.h"
#include "wifi_manager.h"

static const char *TAG = "ensayo_nueva_pata";
static new_leg_test_handle_t leg;
static config_manager_handle_t config;
static wifi_manager_handle_t wifi;
static ota_manager_handle_t ota;

static void print_sensor(void)
{
    new_leg_sensor_t s;
    if (new_leg_test_get_sensor(leg, &s) != ESP_OK || !s.valid) {
        printf("DATA AS5600 VALID:0 AGE_MS:%lu PERIOD_US:%lu HIGH_US:%lu\n",
               (unsigned long)s.age_ms, (unsigned long)s.period_us, (unsigned long)s.high_us);
        return;
    }
    printf("DATA AS5600 VALID:1 AGE_MS:%lu PERIOD_US:%lu HIGH_US:%lu DUTY:%.3f ANGLE_DEG:%.2f\n",
           (unsigned long)s.age_ms, (unsigned long)s.period_us, (unsigned long)s.high_us,
           s.duty_percent, s.angle_deg);
}

static void handle_command(char *line)
{
    char *end = strpbrk(line, "\r\n");
    if (end) *end = '\0';
    if (!strcmp(line, "PING")) {
        printf("OK PONG ENSAYO_NUEVA_PATA\n");
    } else if (!strcmp(line, "VERSION")) {
        printf("DATA VERSION PROJECT:%s TARGET:%s VERSION:%s BUILD:%d\n",
               FW_PROJECT, FW_TARGET, FW_VERSION, FW_BUILD_NUMBER);
    } else if (!strcmp(line, "AS5600")) {
        print_sensor();
    } else if (!strcmp(line, "SERVO STOP")) {
        printf(new_leg_test_stop(leg) == ESP_OK ? "OK SERVO PULSE_US:1500\n" : "ERR SERVO\n");
    } else if (!strncmp(line, "SERVO ", 6)) {
        char *tail = NULL;
        unsigned long pulse = strtoul(line + 6, &tail, 10);
        if (!tail || *tail || new_leg_test_set_pulse_us(leg, (uint32_t)pulse) != ESP_OK) {
            printf("ERR USAGE SERVO 1300..1700\n");
        } else {
            printf("OK SERVO PULSE_US:%lu\n", pulse);
        }
    } else if (!strcmp(line, "WIFI_CONNECT")) {
        esp_err_t err = wifi_manager_connect(wifi);
        if (err == ESP_OK) {
            printf("OK WIFI_CONNECT\n");
        } else {
            printf("ERR WIFI_CONNECT 0x%x\n", err);
        }
    } else if (!strcmp(line, "WIFI_STATUS")) {
        wifi_manager_status_t s;
        esp_err_t err = wifi_manager_get_status(wifi, &s);
        if (err == ESP_OK) {
            printf("DATA WIFI STATE:%s IP:%s RSSI:%d\n",
                   wifi_manager_state_to_string(s.state), s.ip_addr, s.rssi);
        } else {
            printf("ERR WIFI_STATUS 0x%x\n", err);
        }
    } else if (!strcmp(line, "OTA_CHECK")) {
        ota_manager_check_result_t r;
        esp_err_t err = ota_manager_check(ota, &r);
        if (err == ESP_OK) {
            printf("DATA OTA STATUS:%s BUILD:%lu VERSION:%s\n",
                   ota_manager_check_status_to_string(r.status),
                   (unsigned long)r.build_number, r.version);
        } else {
            printf("ERR OTA_CHECK 0x%x DETAIL:%s\n", err, r.detail);
        }
    } else if (!strcmp(line, "HELP")) {
        printf("DATA HELP PING,VERSION,AS5600,SERVO STOP,SERVO 1300..1700,WIFI_CONNECT,WIFI_STATUS,OTA_CHECK\n");
    } else if (line[0]) {
        printf("ERR UNKNOWN_COMMAND\n");
    }
    fflush(stdout);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t used = 0;
    while (1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (used) {
                line[used] = '\0';
                handle_command(line);
                used = 0;
            }
        } else if (used + 1 < sizeof(line)) {
            line[used++] = (char)c;
        } else {
            used = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting isolated new-leg test: servo GPIO14, AS5600 PWM GPIO41");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(config_manager_init(&config));
    ESP_ERROR_CHECK(wifi_manager_init(config, &wifi));
    ota_manager_config_t ota_cfg = {
        .config_manager = config,
        .wifi_manager = wifi,
        .current_project = FW_PROJECT,
        .current_target = FW_TARGET,
        .current_build_number = FW_BUILD_NUMBER,
    };
    ESP_ERROR_CHECK(ota_manager_init(&ota_cfg, &ota));
    ESP_ERROR_CHECK(new_leg_test_init(&leg));
    ESP_LOGW(TAG, "Servo held at neutral 1500 us; no automatic motion");
    ESP_LOGI(TAG, "Commands: HELP, AS5600, SERVO 1300..1700, SERVO STOP");
    xTaskCreate(console_task, "new_leg_console", 4096, NULL, 5, NULL);
}
