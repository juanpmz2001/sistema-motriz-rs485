#!/usr/bin/env python3
"""Static dependency checks for host-testable Iteration 4 boundaries."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source_text(*relative_paths: str) -> str:
    return "\n".join((ROOT / path).read_text(encoding="utf-8") for path in relative_paths)


class DependencyContracts(unittest.TestCase):
    def assert_absent(self, text: str, forbidden: tuple[str, ...]) -> None:
        for token in forbidden:
            with self.subTest(token=token):
                self.assertNotIn(token, text)

    def test_serial_gateway_does_not_depend_on_composition_layers(self) -> None:
        text = source_text(
            "components/serial_gateway/include/serial_gateway.h",
            "components/serial_gateway/serial_gateway.c",
            "components/serial_gateway/CMakeLists.txt",
        )
        self.assert_absent(
            text,
            ("robot_profile", "robot_composition", "actuation_coordinator"),
        )

    def test_svd48_device_is_transport_only(self) -> None:
        text = source_text(
            "components/svd48/include/svd48_device.h",
            "components/svd48/svd48_device.c",
        )
        self.assert_absent(
            text,
            (
                "driver/uart.h",
                "uart_",
                "robot_control",
                "freertos/",
                "esp_err.h",
            ),
        )

    def test_bus_transport_has_no_svd48_dependency(self) -> None:
        text = source_text(
            "components/bus_transport/include/bus_transport.h",
            "components/bus_transport/bus_transport.c",
            "components/bus_transport/CMakeLists.txt",
        ).lower()
        self.assertNotIn("svd48", text)

    def test_robot_capabilities_is_platform_independent(self) -> None:
        text = source_text(
            "components/robot_capabilities/include/robot_capabilities.h",
            "components/robot_capabilities/robot_capabilities.c",
            "components/robot_capabilities/CMakeLists.txt",
        )
        self.assert_absent(
            text,
            ("esp_err.h", "freertos/", "driver/", "esp-idf"),
        )

    def test_direct_svd48_adapter_does_not_use_legacy_robot_control(self) -> None:
        text = source_text(
            "components/svd48_channel_endpoint_adapter/include/svd48_channel_endpoint_adapter.h",
            "components/svd48_channel_endpoint_adapter/svd48_channel_endpoint_adapter.c",
            "components/svd48_channel_endpoint_adapter/CMakeLists.txt",
        )
        self.assertNotIn("robot_control", text)

    def test_composition_does_not_select_bus_by_array_position(self) -> None:
        text = source_text("components/robot_composition/robot_composition.c")
        self.assertNotIn("profile->buses[0]", text)

    def test_composition_registers_the_direct_svd48_factory(self) -> None:
        text = source_text(
            "components/robot_composition/robot_composition.c",
            "components/robot_composition/CMakeLists.txt",
        )
        self.assertIn("EXECUTABLE_FACTORIES", text)
        self.assertIn(".driver_id = ROBOT_DRIVER_SVD48", text)
        self.assertIn("svd48_factory_construct", text)
        self.assertIn("svd48_factory_create_endpoint", text)
        self.assertIn("svd48_channel_endpoint_adapter", text)
        self.assertNotIn("robot_control_endpoint_adapter", text)

    def test_diagnostic_guard_precedes_legacy_command_access(self) -> None:
        gateway = source_text("components/serial_gateway/serial_gateway.c")
        command_dispatch = gateway[gateway.index("static void handle_command") :]
        guard = command_dispatch.index("handle->config.diagnostic_only")
        legacy_trace = command_dispatch.index("robot_control_get_trace_enabled")
        self.assertLess(guard, legacy_trace)

        main = source_text("main/main.c")
        self.assertIn("start_safe_diagnostic_gateway", main)
        self.assertIn("!diagnostics->composition_supported", main)


if __name__ == "__main__":
    unittest.main()
