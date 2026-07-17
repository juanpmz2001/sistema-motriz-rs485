#ifndef OTA_ANNOUNCE_H
#define OTA_ANNOUNCE_H

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "ota_manager.h"
#include "robot_control.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_ANNOUNCE_DEFAULT_PORT 32320
#define OTA_ANNOUNCE_ACTION_MAX 16
#define OTA_ANNOUNCE_DETAIL_MAX 64
#define OTA_ANNOUNCE_SENDER_MAX 16

typedef struct ota_announce_t *ota_announce_handle_t;

typedef struct {
    config_manager_handle_t config_manager;
    wifi_manager_handle_t wifi_manager;
    ota_manager_handle_t ota_manager;
    robot_control_handle_t robot;
    uint16_t listen_port;
} ota_announce_config_t;

typedef struct {
    bool task_running;
    uint16_t listen_port;
    uint32_t packets_seen;
    uint32_t packets_accepted;
    uint32_t packets_rejected;
    uint32_t checks;
    uint32_t download_tests;
    uint32_t updates;
    char last_sender[OTA_ANNOUNCE_SENDER_MAX];
    char last_action[OTA_ANNOUNCE_ACTION_MAX];
    char last_detail[OTA_ANNOUNCE_DETAIL_MAX];
} ota_announce_status_t;

esp_err_t ota_announce_init(const ota_announce_config_t *config, ota_announce_handle_t *out_handle);
void ota_announce_deinit(ota_announce_handle_t handle);
esp_err_t ota_announce_start(ota_announce_handle_t handle);
esp_err_t ota_announce_get_status(ota_announce_handle_t handle, ota_announce_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // OTA_ANNOUNCE_H
