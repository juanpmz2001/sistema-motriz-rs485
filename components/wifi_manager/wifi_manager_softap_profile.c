#include "wifi_manager_softap_profile.h"

#include <string.h>

bool wifi_manager_softap_config_is_valid(const wifi_manager_softap_config_t *config)
{
    if (!config || !config->ssid || !config->passphrase) {
        return false;
    }

    const size_t ssid_length = strlen(config->ssid);
    const size_t passphrase_length = strlen(config->passphrase);
    return ssid_length > 0U && ssid_length <= WIFI_MANAGER_SOFTAP_MAX_SSID_LENGTH &&
           passphrase_length >= WIFI_MANAGER_SOFTAP_MIN_WPA2_PASSPHRASE_LENGTH &&
           passphrase_length <= WIFI_MANAGER_SOFTAP_MAX_WPA2_PASSPHRASE_LENGTH &&
           (config->channel == 1U || config->channel == 6U || config->channel == 11U) &&
           config->max_clients > 0U;
}
