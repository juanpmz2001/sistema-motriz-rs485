#include "host_test.h"
#include "wifi_manager_softap_profile.h"

#include <string.h>

static wifi_manager_softap_config_t valid_config(void)
{
    return (wifi_manager_softap_config_t){
        .ssid = "RAFA-CONTROL",
        .passphrase = "abcdefgh",
        .channel = WIFI_MANAGER_SOFTAP_DEFAULT_CHANNEL,
        .max_clients = WIFI_MANAGER_SOFTAP_DEFAULT_MAX_CLIENTS,
    };
}

static bool softap_profile_is_static_ap_only_and_dhcp_ready(void)
{
    const wifi_manager_softap_config_t config = valid_config();
    HOST_TEST_CHECK(wifi_manager_softap_config_is_valid(&config));
    HOST_TEST_CHECK(strcmp(WIFI_MANAGER_SOFTAP_IP_ADDR, "192.168.4.1") == 0);
    HOST_TEST_CHECK(strcmp(WIFI_MANAGER_SOFTAP_NETMASK, "255.255.255.0") == 0);
    HOST_TEST_CHECK(strcmp(WIFI_MANAGER_SOFTAP_GATEWAY, "192.168.4.1") == 0);
    HOST_TEST_CHECK(config.channel == 6U);
    HOST_TEST_CHECK(config.max_clients > 0U);
    return true;
}

static bool softap_profile_rejects_unusable_wpa2_or_channel(void)
{
    wifi_manager_softap_config_t config = valid_config();
    config.passphrase = "short";
    HOST_TEST_CHECK(!wifi_manager_softap_config_is_valid(&config));
    config = valid_config();
    config.channel = 2U;
    HOST_TEST_CHECK(!wifi_manager_softap_config_is_valid(&config));
    config = valid_config();
    config.max_clients = 0U;
    HOST_TEST_CHECK(!wifi_manager_softap_config_is_valid(&config));
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(softap_profile_is_static_ap_only_and_dhcp_ready),
        HOST_TEST_CASE(softap_profile_rejects_unusable_wpa2_or_channel),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
