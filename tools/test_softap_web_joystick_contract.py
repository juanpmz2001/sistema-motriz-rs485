#!/usr/bin/env python3
"""Small source-contract checks for the branch-only AP-only experiment.

These do not claim RF/DHCP/iPhone evidence. They prevent accidental reintroduction
of STA/APSTA or a separate browser-to-driver control path into this projection.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent.parent


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"{label}: missing {fragment!r}")


def reject(text: str, fragment: str, label: str) -> None:
    if fragment in text:
        raise AssertionError(f"{label}: unexpected {fragment!r}")


def source_section(text: str, start: str, end: str) -> str:
    begin = text.find(start)
    finish = text.find(end, begin)
    if begin < 0 or finish < 0:
        raise AssertionError(f"cannot isolate {start!r}")
    return text[begin:finish]


def main() -> int:
    wifi = (ROOT / "components/wifi_manager/wifi_manager.c").read_text(encoding="utf-8")
    init_softap = source_section(wifi, "static esp_err_t configure_softap_ip", "static void wifi_supervisor_task")
    for fragment in (
        "esp_netif_create_default_wifi_ap()",
        "esp_wifi_set_mode(WIFI_MODE_AP)",
        "esp_wifi_set_config(WIFI_IF_AP",
        "esp_netif_dhcps_stop",
        "esp_netif_dhcps_start",
        "WIFI_MANAGER_SOFTAP_IP_ADDR",
        "WIFI_BW_HT20",
    ):
        require(init_softap, fragment, "SoftAP initialization")
    for fragment in (
        "esp_netif_create_default_wifi_sta",
        "WIFI_MODE_STA",
        "WIFI_MODE_APSTA",
        "esp_wifi_connect()",
        "load_credentials(",
    ):
        reject(init_softap, fragment, "SoftAP initialization")

    for start, end, label in (
        ("esp_err_t wifi_manager_connect", "esp_err_t wifi_manager_disconnect", "connect"),
        ("esp_err_t wifi_manager_disconnect", "esp_err_t wifi_manager_start_auto_connect_task", "disconnect"),
        ("esp_err_t wifi_manager_start_auto_connect_task", "esp_err_t wifi_manager_set_auto_connect_paused", "reconnect supervisor"),
    ):
        operation = source_section(wifi, start, end)
        require(operation, "WIFI_MANAGER_MODE_SOFTAP", f"SoftAP {label} guard")
        require(operation, "ESP_ERR_NOT_SUPPORTED", f"SoftAP {label} guard")

    main_c = (ROOT / "main/main.c").read_text(encoding="utf-8")
    require(main_c, "wifi_manager_init_softap(&softap_config, &wifi_manager)", "main SoftAP selection")
    require(main_c, "if (wifi_manager && !wifi_manager_is_softap(wifi_manager))", "no reconnect in AP")
    require(main_c, "if (ota_manager && !wifi_manager_is_softap(wifi_manager))", "no auto OTA check in AP")

    kconfig = (ROOT / "components/robot_profile/Kconfig").read_text(encoding="utf-8")
    require(kconfig, "config BOTFARMS_RAFA_SOFTAP_WEB_JOYSTICK_EXPERIMENTAL", "Kconfig")
    require(kconfig, "choice BOTFARMS_RAFA_WEB_JOYSTICK_EXPERIMENT_MODE", "Kconfig mutual exclusion")
    require(kconfig, "!BOTFARMS_RAFA_LAN_ONLY_DIAGNOSTIC", "Kconfig LAN-only exclusion")

    page = (ROOT / "components/web_direct_control/web_ui.html").read_text(encoding="utf-8")
    require(page, "ws://${location.host}/control", "browser WebSocket")
    reject(page, "fetch(", "browser control transport")

    web_server = (ROOT / "components/web_direct_control/web_direct_control.c").read_text(encoding="utf-8")
    start = source_section(web_server, "esp_err_t web_direct_control_start", "void web_direct_control_deinit")
    require(start, "httpd_start", "embedded HTTP server")
    require(start, ".uri = \"/control\"", "embedded WebSocket route")
    require(start, "config.close_fn = server_close_handler", "WebSocket close hook")
    close_hook = source_section(web_server, "static void server_close_handler", "static void send_status_async")
    require(close_hook, "handle->active_fd = -1", "WebSocket close hook")
    require(close_hook, "WEB_SOCKET_DISCONNECTED", "WebSocket close log")

    print("softap web joystick source contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"softap web joystick source contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
