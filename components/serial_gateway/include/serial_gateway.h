#ifndef SERIAL_GATEWAY_H
#define SERIAL_GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "actuation_application_port.h"
#include "config_manager.h"
#include "esp_err.h"
#include "ibus_receiver.h"
#include "motion_control_port.h"
#include "motion_status_port.h"
#include "ota_announce.h"
#include "ota_manager.h"
#include "robot_control.h"
#include "robot_safety.h"
#include "serial_gateway_framing.h"
#include "svd48_workspace_port.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct serial_gateway_t *serial_gateway_handle_t;
typedef struct as5600_diagnostics_port as5600_diagnostics_port_t;

typedef enum {
    SERIAL_GATEWAY_POLICY_FULL_SERIAL = 0,
    SERIAL_GATEWAY_POLICY_LAN_SAFE,
} serial_gateway_command_policy_t;

typedef void (*serial_gateway_output_fn_t)(void *ctx, const char *chunk);

/* Internal composition configuration; use designated initializers. This struct
 * is rebuilt with the firmware and does not promise a stable binary ABI. */
typedef struct {
    robot_control_handle_t robot;
    actuation_application_port_t *actuation;
    /* Optional application-level continuous-control boundaries. Maintenance
     * may observe state and revoke motion, but never publish velocity intent. */
    motion_control_port_t *motion_control;
    motion_status_port_t *motion_status;
    /* Optional typed SVD48 inventory/cached-observation boundary. */
    svd48_workspace_port_t *svd48_workspace;
    /* Optional concrete L2/L3 read-only sensor diagnostic path. It is not an
     * endpoint capability or an actuation interface. */
    as5600_diagnostics_port_t *as5600_diagnostics;
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
    const char *fw_git_sha;
    bool fw_git_dirty;
    uint32_t default_stream_period_ms;
    bool print_prompt;
    bool diagnostic_only;
    const char *profile_name;
    const char *board_name;
    bool profile_schema_valid;
    bool composition_supported;
    bool composition_runtime_ready;
    const char *composition_code;
    const char *composition_stage;
    uint16_t composition_driver_id;
    uint16_t composition_bus_id;
    uint16_t composition_device_id;
    uint16_t composition_endpoint_id;
    esp_err_t composition_error;
    size_t composition_required_storage;
    size_t composition_available_storage;
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
