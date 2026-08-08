from __future__ import annotations

from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import hil_runner
import serial_gateway_client
from serial_gateway_client import GatewayResponse, GatewayTimeout


MANIFEST_PATH = (
    REPO_ROOT
    / "tests"
    / "hil"
    / "specs"
    / "capabilities"
    / "single_endpoint_velocity_l4.json"
)
GIT_SHA = "a" * 40
ARTIFACT_SHA256 = "b" * 64
PROFILE = "bench_single_svd48_motor"
BOARD = "esp32s3"
CONFIRMATIONS = {
    "motion_authorized",
    "wheels_unloaded",
    "power_cutoff_ready",
}


class AdvancingClock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class FakeSerial:
    def __init__(self, clock: AdvancingClock, lines: list[bytes] | None = None) -> None:
        self.clock = clock
        self.lines = list(lines or [])
        self.timeout = 0.01
        self.closed = False
        self.writes: list[bytes] = []

    def reset_input_buffer(self) -> None:
        pass

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass

    def readline(self) -> bytes:
        self.clock.now += self.timeout
        return self.lines.pop(0) if self.lines else b""

    def close(self) -> None:
        self.closed = True


class FakeGatewayClient:
    def __init__(
        self,
        *,
        fail_command: str | None = None,
        failure=None,
        rpm_bias: float = 0.0,
        observation_stale: bool = False,
        cleanup_observation_stale: bool = False,
        ignore_motion: bool = False,
        stop_ack: str = "OK STOP ALL",
        endpoint_count: int = 1,
        endpoint_criticality: str = "DEVELOPMENT",
        rc_loss: bool = False,
        min_rpm: int = -15,
        max_rpm: int = 15,
        set_ack_id: str | None = None,
        set_ack_rpm: str | None = None,
        stop_endpoint_ack_id: str | None = None,
    ) -> None:
        self.fail_command = fail_command
        self.failure = failure
        self.failed_once = False
        self.open_count = 0
        self.close_count = 0
        self.opened = False
        self.commands: list[str] = []
        self.rpm = 0.0
        self.rpm_bias = rpm_bias
        self.observation_stale = observation_stale
        self.cleanup_observation_stale = cleanup_observation_stale
        self.ignore_motion = ignore_motion
        self.stop_ack = stop_ack
        self.endpoint_count = endpoint_count
        self.endpoint_criticality = endpoint_criticality
        self.rc_loss = rc_loss
        self.min_rpm = min_rpm
        self.max_rpm = max_rpm
        self.set_ack_id = set_ack_id
        self.set_ack_rpm = set_ack_rpm
        self.stop_endpoint_ack_id = stop_endpoint_ack_id

    def open(self):
        self.open_count += 1
        self.opened = True
        return self

    def close(self) -> None:
        self.close_count += 1
        self.opened = False

    def execute_checked(
        self, command: str, *, timeout: float | None = None
    ) -> GatewayResponse:
        if not self.opened:
            raise RuntimeError("fake connection is closed")
        self.commands.append(command)
        if command == self.fail_command and not self.failed_once:
            self.failed_once = True
            raise self.failure

        if command == "VERSION":
            lines = (f"DATA VERSION GIT_SHA:{GIT_SHA} GIT_DIRTY:0",)
        elif command == "PROFILE_STATUS":
            lines = (f"DATA PROFILE NAME:{PROFILE} BOARD:{BOARD}",)
        elif command == "ENDPOINTS":
            lines = (
                f"DATA ENDPOINTS COUNT:{self.endpoint_count}",
                "DATA ENDPOINT ID:1 NAME:bench_motor "
                "CAPABILITIES:0x00000003 VELOCITY_RPM:1 "
                "VELOCITY_OBSERVATION:1 STOPPABLE:1 AVAILABLE:1 "
                f"CRITICALITY:{self.endpoint_criticality} "
                f"MIN_RPM:{self.min_rpm} MAX_RPM:{self.max_rpm}",
            )
        elif command == "COMPOSITION_STATUS":
            lines = (
                "DATA COMPOSITION MODE:ACTIVE RUNTIME_READY:1 "
                "OUTPUTS_INITIALIZED:1",
            )
        elif command == "PLATFORM_STATUS":
            lines = (
                "DATA PLATFORM STATE:SAFE_IDLE MOTION_ACTIVE:0 FAULTED:0 "
                "TRACE:0 STREAM:0",
            )
        elif command == "SAFETY_STATUS":
            lines = (
                "DATA SAFETY TASK:RUNNING MOTOR_FAULT:0 "
                f"RC_LOSS:{1 if self.rc_loss else 0}",
            )
        elif command.startswith("GET_ENDPOINT_OBSERVATION "):
            observed_rpm = self.rpm + self.rpm_bias if self.rpm != 0.0 else 0.0
            stale = self.observation_stale or (
                self.cleanup_observation_stale
                and self.commands.count("STOP ALL") >= 2
            )
            lines = (
                "DATA ENDPOINT_OBSERVATION ID:1 TYPE:VELOCITY_RPM VALID:1 "
                f"RPM:{observed_rpm} TIMESTAMP_MS:10 "
                "SOURCE:DEVICE_FEEDBACK ONLINE:1 "
                f"STALE:{1 if stale else 0} "
                f"HEALTH:{'STALE' if stale else 'HEALTHY'}",
            )
        else:
            if command.startswith("SET_ENDPOINT_SPEED "):
                if not self.ignore_motion:
                    self.rpm = float(command.rsplit(" ", 1)[1])
            elif command == "STOP ALL" or command.startswith("STOP_ENDPOINT "):
                self.rpm = 0.0
            if command == "STOP ALL":
                lines = (self.stop_ack,)
            elif command.startswith("SET_ENDPOINT_SPEED "):
                _, endpoint_id, rpm = command.split()
                lines = (
                    "OK SET_ENDPOINT_SPEED "
                    f"ID:{self.set_ack_id or endpoint_id} "
                    f"RPM_TARGET:{self.set_ack_rpm or rpm}",
                )
            elif command.startswith("STOP_ENDPOINT "):
                _, endpoint_id = command.split()
                lines = (
                    "OK STOP_ENDPOINT "
                    f"ID:{self.stop_endpoint_ack_id or endpoint_id}",
                )
            else:
                lines = ("OK",)
        return GatewayResponse(command, lines, 0.001)


def expected_gates(**overrides) -> hil_runner.GateExpectations:
    values = {
        "git_sha": GIT_SHA,
        "git_dirty": False,
        "profile": PROFILE,
        "board": BOARD,
    }
    values.update(overrides)
    return hil_runner.GateExpectations(**values)


class HilRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = hil_runner.load_manifest(MANIFEST_PATH)
        self.variables = hil_runner.resolve_variables(
            self.manifest, {"test_rpm": "1"}
        )

    def run_fake(
        self,
        client: FakeGatewayClient,
        evidence: Path,
        *,
        gates: hil_runner.GateExpectations | None = None,
        confirmations: set[str] = CONFIRMATIONS,
        firmware_artifact_sha256: str = ARTIFACT_SHA256,
        firmware_artifact_uri: str | None = "artifact://fixture/build.bin",
    ):
        return hil_runner.HilRunner(client, sleep=lambda _seconds: None).run(
            self.manifest,
            expected=gates or expected_gates(),
            variables=self.variables,
            confirmations=confirmations,
            output_path=evidence,
            manifest_path=MANIFEST_PATH,
            hardware_id="fixture-001",
            pcb_revision="rev-a",
            firmware_artifact_sha256=firmware_artifact_sha256,
            firmware_artifact_uri=firmware_artifact_uri,
            port="fake://gateway",
            baudrate=115200,
        )

    def test_validate_is_offline_and_manifest_has_no_device_details(self) -> None:
        manifest_text = MANIFEST_PATH.read_text(encoding="utf-8").lower()
        # The existing build-profile name is transitional and contains "svd48";
        # executable test logic still contains no concrete driver dependency.
        for forbidden in ("modbus", "uart", "register", "motor_index", "drive_id"):
            self.assertNotIn(forbidden, manifest_text)

        with mock.patch.object(
            serial_gateway_client.importlib,
            "import_module",
            side_effect=AssertionError("validate imported pyserial"),
        ), redirect_stdout(io.StringIO()):
            self.assertEqual(hil_runner.main(["validate", str(MANIFEST_PATH)]), 0)

    def test_success_uses_one_connection_and_stop_all_brackets_steps(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "success.json"
            result = self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        self.assertEqual(client.open_count, 1)
        self.assertEqual(client.close_count, 1)
        self.assertEqual(client.commands.count("STOP ALL"), 2)
        first_stop = client.commands.index("STOP ALL")
        motion = client.commands.index("SET_ENDPOINT_SPEED 1 1")
        self.assertLess(first_stop, motion)
        self.assertEqual(
            client.commands[-2:],
            ["STOP ALL", "GET_ENDPOINT_OBSERVATION 1"],
        )
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(stored["status"], "PASS")
        self.assertEqual(stored["hardware"]["id"], "fixture-001")
        self.assertEqual(stored["hardware"]["pcb_revision"], "rev-a")
        self.assertEqual(stored["firmware_artifact"]["sha256"], ARTIFACT_SHA256)
        self.assertEqual(stored["selected_endpoint"]["min_rpm"], -15)
        self.assertEqual(stored["selected_endpoint"]["max_rpm"], 15)
        self.assertEqual(stored["selected_endpoint"]["manifest_min_rpm"], -5)
        self.assertEqual(stored["selected_endpoint"]["manifest_max_rpm"], 5)
        self.assertEqual(stored["connection"], {"port": "fake://gateway", "baudrate": 115200})
        self.assertEqual(stored["safety"]["confirmations"], sorted(CONFIRMATIONS))
        self.assertTrue(stored["safety"]["software_cleanup_is_best_effort"])
        self.assertIn("completed_at", stored)
        self.assertTrue(stored["assertions"])
        self.assertTrue(all(item["passed"] for item in stored["assertions"]))
        self.assertTrue(stored["cleanup_observation"]["passed"])
        offsets = [item["offset_seconds"] for item in stored["commands"]]
        self.assertEqual(offsets, sorted(offsets))
        self.assertEqual(
            [item["command"] for item in stored["commands"][:3]],
            ["VERSION", "PROFILE_STATUS", "ENDPOINTS"],
        )

    def test_motion_confirmations_are_required_before_connecting(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            with self.assertRaises(hil_runner.SafetyConfirmationError):
                self.run_fake(client, Path(directory) / "rejected.json", confirmations=set())
        self.assertEqual(client.open_count, 0)
        self.assertEqual(client.commands, [])

    def test_identity_gate_failure_still_attempts_final_stop(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "gate-failure.json"
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(
                    client,
                    evidence_path,
                    gates=expected_gates(board="wrong-board"),
                )
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        self.assertEqual(client.commands[-1], "STOP ALL")
        self.assertEqual(client.commands.count("STOP ALL"), 1)
        self.assertFalse(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertEqual(client.close_count, 1)

    def test_identity_timeout_attempt_is_evidence_and_aborts_before_motion(self) -> None:
        client = FakeGatewayClient(
            fail_command="VERSION",
            failure=GatewayTimeout("VERSION", ("DATA PARTIAL",)),
        )
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "identity-timeout.json"
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        attempt = stored["commands"][0]
        self.assertEqual(attempt["command"], "VERSION")
        self.assertEqual(attempt["partial_lines"], ["DATA PARTIAL"])
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertFalse(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )
        self.assertEqual(client.commands[-1], "STOP ALL")

    def test_error_timeout_interrupt_and_signal_all_cleanup(self) -> None:
        cases = (
            (RuntimeError("step failed"), RuntimeError, "INCONCLUSIVE"),
            (
                GatewayTimeout("SET_ENDPOINT_SPEED 1 1"),
                GatewayTimeout,
                "INCONCLUSIVE",
            ),
            (KeyboardInterrupt(), KeyboardInterrupt, "ABORTED_FOR_SAFETY"),
            (
                hil_runner.HilInterrupted(signal.SIGTERM),
                hil_runner.HilInterrupted,
                "ABORTED_FOR_SAFETY",
            ),
        )
        for failure, error_type, status in cases:
            with self.subTest(error_type=error_type.__name__):
                client = FakeGatewayClient(
                    fail_command="SET_ENDPOINT_SPEED 1 1",
                    failure=failure,
                )
                with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
                    evidence_path = Path(directory) / "failure.json"
                    with self.assertRaises(error_type):
                        self.run_fake(client, evidence_path)
                    stored = json.loads(evidence_path.read_text(encoding="utf-8"))

                self.assertEqual(client.commands.count("STOP ALL"), 2)
                self.assertEqual(
                    client.commands[-2:],
                    ["STOP ALL", "GET_ENDPOINT_OBSERVATION 1"],
                )
                self.assertEqual(client.close_count, 1)
                self.assertEqual(stored["status"], status)

    def test_failed_observation_assertion_cannot_report_pass(self) -> None:
        client = FakeGatewayClient(rpm_bias=10.0)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "assertion-failure.json"
            with self.assertRaises(hil_runner.AssertionFailure):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        self.assertEqual(stored["status"], "FAIL")
        self.assertFalse(stored["assertions"][-1]["passed"])
        self.assertEqual(
            client.commands[-2:],
            ["STOP ALL", "GET_ENDPOINT_OBSERVATION 1"],
        )

    def test_unverified_final_stop_aborts_for_safety(self) -> None:
        client = FakeGatewayClient(cleanup_observation_stale=True)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "cleanup-stale.json"
            with self.assertRaises(hil_runner.CleanupError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertTrue(stored["cleanup_errors"])
        self.assertEqual(client.close_count, 1)

    def test_stale_baseline_aborts_before_motion(self) -> None:
        client = FakeGatewayClient(observation_stale=True)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "stale.json"
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))

        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertFalse(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )
        self.assertEqual(client.commands[-1], "STOP ALL")

    def test_motion_variable_is_bounded(self) -> None:
        self.assertEqual(
            hil_runner.resolve_variables(self.manifest, {"test_rpm": "1"})["test_rpm"],
            1,
        )
        for value in ("-6", "6", "0"):
            with self.subTest(value=value), self.assertRaises(hil_runner.ManifestError):
                hil_runner.resolve_variables(self.manifest, {"test_rpm": value})

    def test_evidence_must_be_outside_repository(self) -> None:
        client = FakeGatewayClient()
        with self.assertRaises(hil_runner.EvidenceError):
            self.run_fake(client, REPO_ROOT / "hil-evidence.json")
        self.assertEqual(client.open_count, 0)

    def test_zero_response_cannot_satisfy_one_rpm_claim(self) -> None:
        client = FakeGatewayClient(ignore_motion=True)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "no-response.json"
            with self.assertRaises(hil_runner.AssertionFailure):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertEqual(stored["status"], "FAIL")
        observed = next(
            item for item in stored["assertions"] if item["step_id"] == "observe-after"
        )
        self.assertEqual(observed["observed"], 0)
        self.assertFalse(observed["passed"])

    def test_manifest_rejects_unbounded_numbers_and_missing_response(self) -> None:
        mutations = []
        manifest = copy.deepcopy(self.manifest)
        manifest["safety"]["maximum_test_duration_seconds"] = float("inf")
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["cleanup"]["observation"]["tolerance"] = float("inf")
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        next(
            step for step in manifest["steps"] if step["id"] == "observe-after"
        )["assert"]["tolerance"] = 1.0
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["steps"] = [
            step for step in manifest["steps"] if step["id"] != "observe-after"
        ]
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        del manifest["endpoint_selector"]
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["level"] = "L3"
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["evidence_class"] = "E3"
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["endpoint_selector"]["exact_inventory_count"] = 2
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        del manifest["cleanup"]["observation"]["required_fields"]["HEALTH"]
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        next(
            step for step in manifest["steps"] if step["id"] == "command-velocity"
        )["command"] = "SET_ENDPOINT_SPEED {endpoint_id} 1"
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["variables"]["other_rpm"] = copy.deepcopy(
            manifest["variables"]["test_rpm"]
        )
        next(
            step for step in manifest["steps"] if step["id"] == "observe-after"
        )["assert"]["expected_variable"] = "other_rpm"
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["variables"]["test_rpm"]["default"] = 1
        mutations.append(manifest)
        manifest = copy.deepcopy(self.manifest)
        del next(
            step for step in manifest["steps"] if step["id"] == "observe-after"
        )["assert"]["required_fields"]["HEALTH"]
        mutations.append(manifest)
        for invalid in mutations:
            with self.subTest(invalid=invalid), self.assertRaises(hil_runner.ManifestError):
                hil_runner.validate_manifest(invalid)

    def test_safety_gate_block_must_be_immediately_before_set(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        set_index = next(
            index
            for index, step in enumerate(manifest["steps"])
            if step.get("command", "").startswith("SET_ENDPOINT_SPEED ")
        )
        manifest["steps"].insert(
            set_index,
            {
                "id": "unsafe-delay-after-gates",
                "sleep_seconds": 4,
            },
        )
        with self.assertRaisesRegex(
            hil_runner.ManifestError,
            "contiguous block immediately before SET_ENDPOINT_SPEED",
        ):
            hil_runner.validate_manifest(manifest)

    def test_endpoint_inventory_and_criticality_are_exact_gates(self) -> None:
        for client in (
            FakeGatewayClient(endpoint_count=2),
            FakeGatewayClient(endpoint_criticality="REQUIRED"),
        ):
            with self.subTest(client=client), tempfile.TemporaryDirectory(
                prefix="hil-evidence-"
            ) as directory:
                with self.assertRaises((hil_runner.IdentityError, hil_runner.GateError)):
                    self.run_fake(client, Path(directory) / "gate.json")
                self.assertFalse(
                    any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
                )

    def test_manifest_rpm_range_must_fit_runtime_endpoint_limits(self) -> None:
        client = FakeGatewayClient(min_rpm=-2, max_rpm=2)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "range.json"
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertFalse(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )

    def test_rc_loss_safety_gate_aborts_before_set(self) -> None:
        client = FakeGatewayClient(rc_loss=True)
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "rc-loss.json"
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertFalse(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )

    def test_any_safety_gate_protocol_failure_aborts_for_safety(self) -> None:
        failures = (
            GatewayTimeout("COMPOSITION_STATUS"),
            hil_runner.StepError("malformed response"),
            hil_runner.StepError("missing prefix"),
        )
        for failure in failures:
            client = FakeGatewayClient(
                fail_command="COMPOSITION_STATUS",
                failure=failure,
            )
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory(
                prefix="hil-evidence-"
            ) as directory:
                evidence_path = Path(directory) / "safety-gate.json"
                with self.assertRaises(hil_runner.GateError):
                    self.run_fake(client, evidence_path)
                stored = json.loads(evidence_path.read_text(encoding="utf-8"))
            self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
            self.assertFalse(
                any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
            )

    def test_stop_all_requires_exact_acknowledgement(self) -> None:
        client = FakeGatewayClient(stop_ack="DATA UNRELATED VALUE:1")
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "bad-stop-ack.json"
            with self.assertRaises(hil_runner.CleanupError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertTrue(stored["cleanup_errors"])

    def test_endpoint_motion_acknowledgements_are_exact_and_correlated(self) -> None:
        cases = (
            FakeGatewayClient(set_ack_id="other"),
            FakeGatewayClient(set_ack_rpm="2"),
            FakeGatewayClient(stop_endpoint_ack_id="other"),
        )
        for index, client in enumerate(cases):
            with self.subTest(index=index), tempfile.TemporaryDirectory(
                prefix="hil-evidence-"
            ) as directory:
                evidence_path = Path(directory) / "bad-endpoint-ack.json"
                with self.assertRaises(hil_runner.StepError):
                    self.run_fake(client, evidence_path)
                stored = json.loads(evidence_path.read_text(encoding="utf-8"))
            self.assertEqual(stored["status"], "INCONCLUSIVE")
            self.assertNotEqual(stored["status"], "PASS")
            self.assertEqual(client.commands[-2:], [
                "STOP ALL",
                "GET_ENDPOINT_OBSERVATION 1",
            ])

    def test_failed_final_stop_ack_cannot_preserve_pass(self) -> None:
        class FinalAckFailureClient(FakeGatewayClient):
            def execute_checked(self, command: str, *, timeout: float | None = None):
                if command == "STOP ALL" and self.commands.count("STOP ALL") == 1:
                    self.stop_ack = "DATA UNRELATED VALUE:1"
                return super().execute_checked(command, timeout=timeout)

        client = FinalAckFailureClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "bad-final-stop-ack.json"
            with self.assertRaises(hil_runner.CleanupError):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertTrue(
            any(command.startswith("SET_ENDPOINT_SPEED") for command in client.commands)
        )
        self.assertEqual(stored["status"], "ABORTED_FOR_SAFETY")
        self.assertTrue(stored["cleanup_errors"])

    def test_timeout_attempt_is_recorded_before_cleanup(self) -> None:
        client = FakeGatewayClient(
            fail_command="SET_ENDPOINT_SPEED 1 1",
            failure=GatewayTimeout("SET_ENDPOINT_SPEED 1 1", ("DATA PARTIAL",)),
        )
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "timeout.json"
            with self.assertRaises(GatewayTimeout):
                self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        attempted = next(
            item
            for item in stored["commands"]
            if item["command"] == "SET_ENDPOINT_SPEED 1 1"
        )
        self.assertEqual(attempted["partial_lines"], ["DATA PARTIAL"])

    def test_evidence_refuses_to_overwrite_existing_record(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "existing.json"
            evidence_path.write_text("preserve\n", encoding="utf-8")
            with self.assertRaises(hil_runner.EvidenceError):
                self.run_fake(client, evidence_path)
            self.assertEqual(evidence_path.read_text(encoding="utf-8"), "preserve\n")
        self.assertEqual(client.open_count, 0)

    def test_evidence_is_reserved_before_serial_open(self) -> None:
        class ReservationAwareClient(FakeGatewayClient):
            evidence_path: Path

            def open(self):
                self.assert_final_absent()
                reservation_path = self.evidence_path.with_name(
                    f".{self.evidence_path.name}.reserved"
                )
                marker = json.loads(reservation_path.read_text(encoding="utf-8"))
                if marker.get("status") != "RESERVED":
                    raise AssertionError("evidence was not reserved before serial open")
                if marker.get("manifest_id") != "single-endpoint-velocity-l4":
                    raise AssertionError("reservation lacks run provenance")
                return super().open()

            def assert_final_absent(self) -> None:
                if self.evidence_path.exists():
                    raise AssertionError("final evidence appeared before serial open")

        client = ReservationAwareClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "reserved.json"
            client.evidence_path = evidence_path
            result = self.run_fake(client, evidence_path)
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
            reservation_path = evidence_path.with_name(
                f".{evidence_path.name}.reserved"
            )
            self.assertFalse(reservation_path.exists())
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(stored["status"], "PASS")

    def test_unwritable_evidence_destination_never_connects(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "denied.json"
            with mock.patch.object(
                hil_runner.os,
                "open",
                side_effect=PermissionError("denied"),
            ), self.assertRaises(hil_runner.EvidenceError):
                self.run_fake(client, evidence_path)
        self.assertEqual(client.open_count, 0)
        self.assertEqual(client.commands, [])

    def test_evidence_reservation_race_never_overwrites_intruder(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "race.json"
            reservation = hil_runner.EvidenceReservation.reserve(evidence_path)
            evidence_path.write_text("intruder\n", encoding="utf-8")
            with self.assertRaises(hil_runner.EvidenceError):
                reservation.finalize({"status": "PASS"})
            self.assertEqual(evidence_path.read_text(encoding="utf-8"), "intruder\n")
            reservation_path = evidence_path.with_name(
                f".{evidence_path.name}.reserved"
            )
            marker = json.loads(reservation_path.read_text(encoding="utf-8"))
            self.assertEqual(marker["status"], "FINALIZE_FAILED")

    def test_existing_reservation_is_preserved_and_never_connects(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "reserved.json"
            first = hil_runner.EvidenceReservation.reserve(evidence_path)
            reservation_path = first.reservation_path
            original = reservation_path.read_text(encoding="utf-8")
            try:
                with self.assertRaises(hil_runner.EvidenceError):
                    self.run_fake(client, evidence_path)
                self.assertEqual(
                    reservation_path.read_text(encoding="utf-8"),
                    original,
                )
            finally:
                first.close(remove_reservation=True)
        self.assertEqual(client.open_count, 0)
        self.assertEqual(client.commands, [])

    def test_unsupported_atomic_publish_is_rejected_before_connect(self) -> None:
        client = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "unsupported.json"
            with mock.patch.object(
                hil_runner.os,
                "link",
                side_effect=OSError("hard links unsupported"),
            ), self.assertRaises(hil_runner.EvidenceError):
                self.run_fake(client, evidence_path)
            reservation_path = evidence_path.with_name(
                f".{evidence_path.name}.reserved"
            )
            self.assertFalse(reservation_path.exists())
        self.assertEqual(client.open_count, 0)
        self.assertEqual(client.commands, [])

    def test_artifact_provenance_is_validated_before_connect_and_normalized(self) -> None:
        rejected = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            with self.assertRaises(hil_runner.GateError):
                self.run_fake(
                    rejected,
                    Path(directory) / "bad-sha.json",
                    firmware_artifact_sha256="not-a-sha",
                )
        self.assertEqual(rejected.open_count, 0)

        accepted = FakeGatewayClient()
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            evidence_path = Path(directory) / "uppercase.json"
            self.run_fake(
                accepted,
                evidence_path,
                firmware_artifact_sha256=ARTIFACT_SHA256.upper(),
            )
            stored = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertEqual(stored["firmware_artifact"]["sha256"], ARTIFACT_SHA256)

    def test_cli_does_not_report_non_pass_as_success(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hil-evidence-") as directory:
            args = hil_runner.build_parser().parse_args(
                [
                    "run",
                    str(MANIFEST_PATH),
                    "--port",
                    "fake://gateway",
                    "--evidence",
                    str(Path(directory) / "unused.json"),
                    "--expect-git-sha",
                    GIT_SHA,
                    "--expect-git-dirty",
                    "0",
                    "--expect-profile",
                    PROFILE,
                    "--expect-board",
                    BOARD,
                    "--hardware-id",
                    "fixture-001",
                    "--pcb-revision",
                    "rev-a",
                    "--firmware-artifact-sha256",
                    ARTIFACT_SHA256,
                    "--set",
                    "test_rpm=1",
                    "--authorize-motion",
                    "--confirm-unloaded",
                    "--confirm-cutoff",
                ]
            )
            with mock.patch.object(
                hil_runner.HilRunner,
                "run",
                return_value={"status": "INCONCLUSIVE"},
            ), redirect_stdout(io.StringIO()) as stdout, mock.patch(
                "sys.stderr", new_callable=io.StringIO
            ) as stderr:
                result = hil_runner.cmd_run(args)
        self.assertEqual(result, 3)
        self.assertNotIn("OK HIL", stdout.getvalue())
        self.assertIn("INCONCLUSIVE", stderr.getvalue())

    def test_serial_timeout_configuration_must_be_finite_and_bounded(self) -> None:
        for timeout in (float("nan"), float("inf"), 0, 11):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                serial_gateway_client.SerialGatewayClient(
                    "fake://gateway", command_timeout=timeout
                )

    def test_serial_client_enforces_monotonic_deadline_and_preserves_partial_lines(self) -> None:
        clock = AdvancingClock()
        serial = FakeSerial(clock, [b"DATA PARTIAL VALUE:1\n"])
        client = serial_gateway_client.SerialGatewayClient(
            "fake://gateway",
            command_timeout=0.04,
            response_idle=0.05,
            poll_interval=0.01,
            serial_factory=lambda **_kwargs: serial,
            clock=clock,
        )
        client.open()
        with self.assertRaises(GatewayTimeout) as captured:
            client.execute("PING")
        client.close()
        self.assertEqual(captured.exception.partial_lines, ("DATA PARTIAL VALUE:1",))
        self.assertGreaterEqual(clock.now, 0.04)
        self.assertEqual(serial.writes, [b"PING\n"])
        self.assertTrue(serial.closed)

    def test_termination_guard_converts_first_signal_to_cleanup_exception(self) -> None:
        guard = hil_runner.TerminationSignalGuard()
        with self.assertRaises(hil_runner.HilInterrupted) as captured:
            guard.handle(signal.SIGTERM, None)
        self.assertEqual(captured.exception.signum, signal.SIGTERM)
        guard.handle(signal.SIGTERM, None)

    def test_all_versioned_manifests_validate(self) -> None:
        manifests = sorted((REPO_ROOT / "tests/hil").glob("**/*.json"))
        self.assertTrue(manifests)
        for path in manifests:
            with self.subTest(path=path.relative_to(REPO_ROOT)):
                hil_runner.load_manifest(path)


if __name__ == "__main__":
    unittest.main()
