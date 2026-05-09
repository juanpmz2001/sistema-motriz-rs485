#ifndef SERIAL_GATEWAY_H
#define SERIAL_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "robot_control.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct serial_gateway_t *serial_gateway_handle_t;

typedef struct {
    robot_control_handle_t robot;
    config_manager_handle_t config_manager;
    wifi_manager_handle_t wifi_manager;
    const char *fw_project;
    const char *fw_target;
    const char *fw_version;
    uint32_t fw_build_number;
    uint32_t default_stream_period_ms;
    bool print_prompt;
} serial_gateway_config_t;

serial_gateway_handle_t serial_gateway_init(const serial_gateway_config_t *config);
void serial_gateway_deinit(serial_gateway_handle_t handle);
esp_err_t serial_gateway_start(serial_gateway_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_GATEWAY_H
