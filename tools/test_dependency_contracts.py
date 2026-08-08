#!/usr/bin/env python3
"""Static dependency checks for host-testable architecture boundaries."""

import ast
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source_text(*relative_paths: str) -> str:
    return "\n".join((ROOT / path).read_text(encoding="utf-8") for path in relative_paths)


def imported_modules(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    modules: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            modules.add(node.module)
    return modules


def manifest_level(document: dict[str, object]) -> int:
    level = document.get("level")
    if not isinstance(level, str) or len(level) != 2 or level[0] != "L" or not level[1].isdigit():
        raise ValueError(f"invalid HIL level: {level!r}")
    numeric = int(level[1])
    if numeric > 7:
        raise ValueError(f"invalid HIL level: {level!r}")
    return numeric


def nested_keys(value: object) -> set[str]:
    if isinstance(value, dict):
        keys = {str(key).lower() for key in value}
        for nested in value.values():
            keys.update(nested_keys(nested))
        return keys
    if isinstance(value, list):
        keys: set[str] = set()
        for nested in value:
            keys.update(nested_keys(nested))
        return keys
    return set()


def nested_strings(value: object, *, parent: str = "") -> list[tuple[str, str]]:
    if isinstance(value, dict):
        strings: list[tuple[str, str]] = []
        for key, nested in value.items():
            strings.extend(nested_strings(nested, parent=str(key).lower()))
        return strings
    if isinstance(value, list):
        strings: list[tuple[str, str]] = []
        for nested in value:
            strings.extend(nested_strings(nested, parent=parent))
        return strings
    return [(parent, value.lower())] if isinstance(value, str) else []


FORBIDDEN_HIL_VALUE_TOKENS = (
    "read_reg",
    "write_reg",
    "register",
    "svd48",
    "modbus",
    "rs485",
    "can_id",
    "can bus",
    "canbus",
    "pwm_pin",
    "pwm pin",
    "uart_id",
    "uart",
    "/dev/tty",
)


def concrete_hil_values(document: object) -> list[str]:
    return [
        value
        for parent, value in nested_strings(document)
        if parent != "profile"
        and any(token in value for token in FORBIDDEN_HIL_VALUE_TOKENS)
    ]


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

    def test_composition_preserves_dependencies_until_polling_is_quiesced(self) -> None:
        composition = source_text("components/robot_composition/robot_composition.c")
        quiescence = composition[
            composition.index("static bool polling_task_is_quiesced") :
            composition.index("static bool cleanup_runtime")
        ]
        self.assertIn("!task->service", quiescence)

        cleanup = composition[
            composition.index("static bool cleanup_runtime") :
            composition.index("static esp_err_t fail_after_cleanup")
        ]
        self.assertIn("composition->polling_task.service", cleanup)
        quiescence_guard = cleanup.index("polling_task_is_quiesced")
        device_destroy = cleanup.index("factory->ops->destroy")
        transport_destroy = cleanup.index("rs485_transport_deinit")
        self.assertLess(quiescence_guard, device_destroy)
        self.assertLess(quiescence_guard, transport_destroy)
        self.assertIn("return false;", cleanup[quiescence_guard:device_destroy])

        deinit = composition[composition.index("void robot_composition_deinit") :]
        cleanup_call = deinit.index("cleanup_runtime")
        reset = deinit.index("memset(composition")
        self.assertLess(cleanup_call, reset)
        self.assertIn("return;", deinit[cleanup_call:reset])

    def test_poll_task_stop_waits_for_completion_after_a_timeout(self) -> None:
        poll_task = source_text("components/svd48/svd48_poll_task.c")
        start = poll_task[
            poll_task.index("esp_err_t svd48_poll_task_start") :
            poll_task.index("esp_err_t svd48_poll_task_stop")
        ]
        for pending_state in (
            "task->service",
            "task->task",
            "task->stopped",
            "task->stop_requested",
            "task->running",
        ):
            with self.subTest(pending_state=pending_state):
                self.assertIn(pending_state, start)

        semaphore_failure = start[
            start.index("if (!task->stopped)") :
            start.index("BaseType_t created")
        ]
        self.assertIn("memset(task, 0, sizeof(*task))", semaphore_failure)

        stop = poll_task[poll_task.index("esp_err_t svd48_poll_task_stop") :]
        completion_semaphore = stop.index("if (!task->stopped)")
        stop_request = stop.index("task->stop_requested = true")
        completion_wait = stop.index("xSemaphoreTake(task->stopped")
        reset = stop.index("memset(task", completion_wait)
        self.assertLess(completion_semaphore, stop_request)
        self.assertLess(stop_request, completion_wait)
        self.assertLess(completion_wait, reset)
        self.assertNotIn("if (!task->task && !task->running)", stop)

        composition = source_text("components/robot_composition/robot_composition.c")
        composition_stop = composition[
            composition.index("esp_err_t robot_composition_stop") :
            composition.index("void robot_composition_deinit")
        ]
        self.assertIn("composition->polling_task.service", composition_stop)
        self.assertIn("composition->polling_task.stopped", composition_stop)
        self.assertIn("composition->polling_task.stop_requested", composition_stop)

        composition_start = composition[
            composition.index("esp_err_t robot_composition_start") :
            composition.index("esp_err_t robot_composition_stop")
        ]
        quiescence_guard = composition_start.index("polling_task_is_quiesced")
        device_start = composition_start.index("factory->ops->start")
        self.assertLess(quiescence_guard, device_start)

    def test_diagnostic_guard_precedes_legacy_command_access(self) -> None:
        gateway = source_text("components/serial_gateway/serial_gateway.c")
        command_dispatch = gateway[gateway.index("static void handle_command") :]
        guard = command_dispatch.index("handle->config.diagnostic_only")
        legacy_trace = command_dispatch.index("robot_control_get_trace_enabled")
        self.assertLess(guard, legacy_trace)

        main = source_text("main/main.c")
        self.assertIn("start_safe_diagnostic_gateway", main)
        self.assertIn("!diagnostics->composition_supported", main)

    def test_hil_runner_has_no_concrete_driver_imports(self) -> None:
        paths = set((ROOT / "tools").glob("hil*.py"))
        paths.add(ROOT / "tools/serial_gateway_client.py")
        paths.update((ROOT / "tests/hil").glob("**/*.py"))
        forbidden_parts = {
            "svd48",
            "modbus",
            "rs485",
            "driver",
            "uart",
            "pwm",
            "can",
        }
        for path in sorted(paths):
            self.assertTrue(path.is_file(), f"missing HIL Python source: {path}")
            for module in imported_modules(path):
                with self.subTest(path=path.relative_to(ROOT), module=module):
                    lowered = module.lower()
                    if path.name == "serial_gateway_client.py" and lowered == "serial":
                        continue
                    if lowered == "serial":
                        self.fail(f"concrete serial import in {path}: {module}")
                    parts = {
                        part
                        for dotted in lowered.split(".")
                        for part in dotted.replace("-", "_").split("_")
                    }
                    self.assertFalse(
                        parts & forbidden_parts,
                        f"concrete HIL import in {path}: {module}",
                    )
                    self.assertNotIn("robot_control", lowered)

    def test_hil_manifests_respect_level_boundaries(self) -> None:
        hil_root = ROOT / "tests/hil"
        manifests = sorted(hil_root.glob("**/*.json"))
        self.assertTrue(manifests, "at least one versioned HIL manifest is required")
        forbidden_l4_keys = {
            "raw_command",
            "register",
            "registers",
            "modbus_address",
            "slave_id",
            "can_id",
            "pwm_pin",
            "uart_id",
            "motor_index",
            "drive_id",
            "driver",
            "transport",
        }
        expected_directory = {
            2: "bringup",
            3: "devices",
            4: "capabilities",
            5: "closed_loop",
            6: "mobility",
        }
        for path in manifests:
            with self.subTest(path=path.relative_to(ROOT)):
                document = json.loads(path.read_text(encoding="utf-8"))
                self.assertIsInstance(document, dict)
                level = manifest_level(document)
                directory = expected_directory.get(level)
                if directory:
                    self.assertIn(directory, path.relative_to(hil_root).parts)
                if level >= 4:
                    keys = nested_keys(document)
                    normalized_keys = {
                        key.replace("-", "_") for key in keys
                    }
                    concrete_keys = {
                        key
                        for key in normalized_keys
                        if any(token in key for token in forbidden_l4_keys)
                    }
                    self.assertFalse(
                        concrete_keys,
                        f"L{level} manifest exposes concrete details: {sorted(concrete_keys)}",
                    )
                    concrete_values = concrete_hil_values(document)
                    self.assertFalse(
                        concrete_values,
                        f"L{level} manifest embeds concrete command/details: {concrete_values}",
                    )
                    target = document.get("target")
                    self.assertIsInstance(target, dict)
                    self.assertIn("endpoint_id", target)
                    self.assertIn("endpoint_name", target)
                    self.assertIn("capability", target)

    def test_hil_value_classifier_rejects_concrete_driver_details(self) -> None:
        self.assertFalse(
            concrete_hil_values({"profile": "bench_single_svd48_motor"}),
            "the build-selected profile name is the sole concrete-name exception",
        )
        for value in ("svd48_register", "READ_REG", "modbus_slave"):
            with self.subTest(value=value):
                self.assertEqual(
                    concrete_hil_values(
                        {
                            "profile": "bench_single_svd48_motor",
                            "description": value,
                        }
                    ),
                    [value.lower()],
                )

    def test_every_hil_manifest_passes_runner_validation(self) -> None:
        import sys

        tools_path = str(ROOT / "tools")
        if tools_path not in sys.path:
            sys.path.insert(0, tools_path)
        from hil_runner import load_manifest

        manifests = sorted((ROOT / "tests/hil").glob("**/*.json"))
        self.assertTrue(manifests)
        for path in manifests:
            with self.subTest(path=path.relative_to(ROOT)):
                load_manifest(path)


if __name__ == "__main__":
    unittest.main()
