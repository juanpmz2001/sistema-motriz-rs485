#ifndef WIFI_MANAGER_SOFTAP_PROFILE_H
#define WIFI_MANAGER_SOFTAP_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is deliberately a small, build-selected AP profile.  It describes no
 * station, upstream router or gateway.  The passphrase is supplied only by an
 * ignored local sdkconfig override at build time. */
#define WIFI_MANAGER_SOFTAP_IP_ADDR "192.168.4.1"
#define WIFI_MANAGER_SOFTAP_NETMASK "255.255.255.0"
#define WIFI_MANAGER_SOFTAP_GATEWAY "192.168.4.1"
#define WIFI_MANAGER_SOFTAP_DEFAULT_CHANNEL 6U
#define WIFI_MANAGER_SOFTAP_DEFAULT_MAX_CLIENTS 4U
#define WIFI_MANAGER_SOFTAP_MAX_SSID_LENGTH 32U
#define WIFI_MANAGER_SOFTAP_MIN_WPA2_PASSPHRASE_LENGTH 8U
#define WIFI_MANAGER_SOFTAP_MAX_WPA2_PASSPHRASE_LENGTH 63U

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    uint8_t max_clients;
} wifi_manager_softap_config_t;

/* Pure validation shared by the ESP-IDF AP setup and host tests.  It does not
 * retain or log the passphrase. */
bool wifi_manager_softap_config_is_valid(const wifi_manager_softap_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_SOFTAP_PROFILE_H */
