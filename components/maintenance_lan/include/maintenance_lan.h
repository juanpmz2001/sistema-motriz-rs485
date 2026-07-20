#ifndef MAINTENANCE_LAN_H
#define MAINTENANCE_LAN_H

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "serial_gateway.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAINTENANCE_LAN_DEFAULT_PORT 32321
#define MAINTENANCE_LAN_ACTION_MAX 16
#define MAINTENANCE_LAN_DETAIL_MAX 64
#define MAINTENANCE_LAN_SENDER_MAX 16

typedef struct maintenance_lan_t *maintenance_lan_handle_t;

typedef struct {
    config_manager_handle_t config_manager;
    wifi_manager_handle_t wifi_manager;
    serial_gateway_handle_t gateway;
    uint16_t listen_port;
} maintenance_lan_config_t;

typedef struct {
    bool task_running;
    uint16_t listen_port;
    uint32_t packets_seen;
    uint32_t packets_accepted;
    uint32_t packets_rejected;
    uint32_t commands;
    char last_sender[MAINTENANCE_LAN_SENDER_MAX];
    char last_action[MAINTENANCE_LAN_ACTION_MAX];
    char last_detail[MAINTENANCE_LAN_DETAIL_MAX];
} maintenance_lan_status_t;

esp_err_t maintenance_lan_init(const maintenance_lan_config_t *config, maintenance_lan_handle_t *out_handle);
void maintenance_lan_deinit(maintenance_lan_handle_t handle);
esp_err_t maintenance_lan_start(maintenance_lan_handle_t handle);
esp_err_t maintenance_lan_get_status(maintenance_lan_handle_t handle, maintenance_lan_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // MAINTENANCE_LAN_H
