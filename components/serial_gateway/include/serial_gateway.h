#ifndef SERIAL_GATEWAY_H
#define SERIAL_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include "actuation_application_port.h"
#include "config_manager.h"
#include "esp_err.h"
#include "ibus_receiver.h"
#include "ota_announce.h"
#include "ota_manager.h"
#include "robot_control.h"
#include "robot_safety.h"
#include "serial_gateway_framing.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct serial_gateway_t *serial_gateway_handle_t;

typedef enum {
    SERIAL_GATEWAY_POLICY_FULL_SERIAL = 0,
    SERIAL_GATEWAY_POLICY_LAN_SAFE,
} serial_gateway_command_policy_t;

typedef void (*serial_gateway_output_fn_t)(void *ctx, const char *chunk);

typedef struct {
    robot_control_handle_t robot;
    actuation_application_port_t *actuation;
    config_manager_handle_t config_manager;
    wifi_manager_handle_t wifi_manager;
    ota_manager_handle_t ota_manager;
    ota_announce_handle_t ota_announce;
    ibus_receiver_handle_t ibus_receiver;
    robot_safety_handle_t robot_safety;
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
esp_err_t serial_gateway_execute_command(serial_gateway_handle_t handle,
                                         const char *line,
                                         serial_gateway_command_policy_t policy,
                                         serial_gateway_output_fn_t output_fn,
                                         void *output_ctx);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_GATEWAY_H
