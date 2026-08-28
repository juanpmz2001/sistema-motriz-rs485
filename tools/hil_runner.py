#!/usr/bin/env python3
"""Manifest-driven, fail-safe host HIL runner for the serial gateway."""

from __future__ import annotations

import argparse
from contextlib import AbstractContextManager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import string
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Protocol

from serial_gateway_client import (
    DEFAULT_BAUDRATE,
    GatewayClientError,
    GatewayCommandError,
    GatewayResponse,
    GatewayTimeout,
    MAX_COMMAND_TIMEOUT_SECONDS,
    SerialGatewayClient,
)


REPO_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
MAX_TEST_DURATION_SECONDS = 300.0
REQUIRED_GATES = {"git_sha", "git_dirty", "profile", "board"}
REQUIRED_MOTION_CONFIRMATIONS = {
    "motion_authorized",
    "wheels_unloaded",
    "power_cutoff_ready",
}
SAFE_COMMANDS = {
    "PING",
    "COMPOSITION_STATUS",
    "PLATFORM_STATUS",
    "SAFETY_STATUS",
    "GET_ENDPOINT_OBSERVATION",
    "SET_ENDPOINT_SPEED",
    "STOP_ENDPOINT",
}
IDENTIFIER_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]*$")
ENDPOINT_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]+$")


class HilError(RuntimeError):
    pass


class ManifestError(HilError):
    pass


class IdentityError(HilError):
    pass


class GateError(HilError):
    pass


class SafetyConfirmationError(HilError):
    pass


class StepError(HilError):
    pass


class AssertionFailure(StepError):
    pass


class CleanupError(HilError):
    pass


class EvidenceError(HilError):
    pass


class HilInterrupted(HilError):
    def __init__(self, signum: int) -> None:
        super().__init__(f"interrupted by signal {signum}")
        self.signum = signum


class Gateway(Protocol):
    def open(self) -> Any: ...

    def close(self) -> None: ...

    def execute_checked(
        self, command: str, *, timeout: float | None = None
    ) -> GatewayResponse: ...


@dataclass(frozen=True)
class Endpoint:
    endpoint_id: str
    name: str
    capabilities: frozenset[str]
    available: bool
    criticality: str
    min_rpm: int
    max_rpm: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.endpoint_id,
            "name": self.name,
            "capabilities": sorted(self.capabilities),
            "available": self.available,
            "criticality": self.criticality,
            "min_rpm": self.min_rpm,
            "max_rpm": self.max_rpm,
        }


@dataclass(frozen=True)
class DeviceIdentity:
    version: dict[str, str]
    profile: dict[str, str]
    endpoints: tuple[Endpoint, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "version": dict(self.version),
            "profile": dict(self.profile),
            "endpoints": [endpoint.to_dict() for endpoint in self.endpoints],
        }


@dataclass(frozen=True)
class GateExpectations:
    git_sha: str
    git_dirty: bool
    profile: str
    board: str


class TerminationSignalGuard(AbstractContextManager["TerminationSignalGuard"]):
    """Turn manageable termination signals into exceptions so finally can stop."""

    def __init__(self) -> None:
        self.triggered = False
        self._previous: dict[int, Any] = {}

    def __enter__(self) -> "TerminationSignalGuard":
        if threading.current_thread() is not threading.main_thread():
            return self
        for name in ("SIGINT", "SIGTERM", "SIGHUP"):
            signum = getattr(signal, name, None)
            if signum is None:
                continue
            self._previous[signum] = signal.getsignal(signum)
            signal.signal(signum, self.handle)
        return self

    def handle(self, signum: int, _frame: Any) -> None:
        if self.triggered:
            return
        self.triggered = True
        raise HilInterrupted(signum)

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        for signum, previous in self._previous.items():
            signal.signal(signum, previous)
        self._previous.clear()


def _fields(line: str, records: tuple[str, ...]) -> dict[str, str] | None:
    try:
        tokens = shlex.split(line)
    except ValueError as exc:
        raise IdentityError(f"malformed response line: {line}") from exc
    if len(tokens) < 2 or tokens[0] != "DATA" or tokens[1] not in records:
        return None
    parsed: dict[str, str] = {}
    for token in tokens[2:]:
        if ":" not in token:
            continue
        key, value = token.split(":", 1)
        parsed[key.upper()] = value
    return parsed


def _single_record(
    response: GatewayResponse, records: tuple[str, ...]
) -> dict[str, str]:
    matches = [parsed for line in response.lines if (parsed := _fields(line, records))]
    if len(matches) != 1:
        raise IdentityError(
            f"{response.command} expected one {'/'.join(records)} record, got {len(matches)}"
        )
    return matches[0]


def _parse_bool(value: str, field: str) -> bool:
    normalized = value.strip().upper()
    if normalized in {"0", "FALSE", "NO"}:
        return False
    if normalized in {"1", "TRUE", "YES"}:
        return True
    raise IdentityError(f"{field} must be 0 or 1, got {value!r}")


def identify_gateway(
    client: Gateway,
    *,
    execute: Callable[[str], GatewayResponse] | None = None,
) -> DeviceIdentity:
    execute_command = execute or client.execute_checked
    version_response = execute_command("VERSION")
    profile_response = execute_command("PROFILE_STATUS")
    endpoint_response = execute_command("ENDPOINTS")
    version = _single_record(version_response, ("VERSION",))
    profile = _single_record(profile_response, ("PROFILE", "PROFILE_STATUS"))
    endpoint_summary = _single_record(endpoint_response, ("ENDPOINTS",))

    for required in ("GIT_SHA", "GIT_DIRTY"):
        if required not in version:
            raise IdentityError(f"VERSION is missing {required}")
    for required in ("NAME", "BOARD"):
        if required not in profile:
            raise IdentityError(f"PROFILE_STATUS is missing {required}")
    _parse_bool(version["GIT_DIRTY"], "GIT_DIRTY")
    try:
        expected_endpoint_count = int(endpoint_summary["COUNT"])
    except (KeyError, ValueError) as exc:
        raise IdentityError("ENDPOINTS is missing a valid COUNT") from exc
    if expected_endpoint_count < 0:
        raise IdentityError("ENDPOINTS COUNT cannot be negative")

    endpoints: list[Endpoint] = []
    seen: set[str] = set()
    for line in endpoint_response.lines:
        parsed = _fields(line, ("ENDPOINT",))
        if parsed is None:
            continue
        endpoint_id = parsed.get("ID", "")
        if not ENDPOINT_ID_RE.fullmatch(endpoint_id) or endpoint_id in seen:
            raise IdentityError(f"invalid or duplicate endpoint ID {endpoint_id!r}")
        seen.add(endpoint_id)
        raw_capabilities = parsed.get("CAPABILITIES", parsed.get("CAPS", ""))
        named_capabilities = {
            item.strip().upper()
            for item in re.split(r"[,|]", raw_capabilities)
            if item.strip()
            and item.strip().upper() != "NONE"
            and not re.fullmatch(r"(?:0X)?[0-9A-F]+", item.strip(), re.IGNORECASE)
        }
        for capability_name in (
            "VELOCITY_RPM",
            "VELOCITY_OBSERVATION",
            "STOPPABLE",
        ):
            if capability_name in parsed:
                if _parse_bool(parsed[capability_name], capability_name):
                    named_capabilities.add(capability_name)
                else:
                    named_capabilities.discard(capability_name)
        capabilities = frozenset(named_capabilities)
        if "AVAILABLE" not in parsed or "CRITICALITY" not in parsed:
            raise IdentityError(
                f"endpoint {endpoint_id} is missing AVAILABLE or CRITICALITY"
            )
        criticality = parsed["CRITICALITY"].upper()
        if criticality not in {"DEVELOPMENT", "OPTIONAL", "REQUIRED"}:
            raise IdentityError(
                f"endpoint {endpoint_id} has invalid CRITICALITY {criticality!r}"
            )
        try:
            min_rpm = int(parsed["MIN_RPM"])
            max_rpm = int(parsed["MAX_RPM"])
        except (KeyError, ValueError) as exc:
            raise IdentityError(
                f"endpoint {endpoint_id} is missing valid MIN_RPM/MAX_RPM"
            ) from exc
        if min_rpm > max_rpm:
            raise IdentityError(f"endpoint {endpoint_id} has inverted RPM limits")
        endpoints.append(
            Endpoint(
                endpoint_id=endpoint_id,
                name=parsed.get("NAME", endpoint_id),
                capabilities=capabilities,
                available=_parse_bool(parsed["AVAILABLE"], "AVAILABLE"),
                criticality=criticality,
                min_rpm=min_rpm,
                max_rpm=max_rpm,
            )
        )
    if len(endpoints) != expected_endpoint_count:
        raise IdentityError(
            f"ENDPOINTS COUNT declared {expected_endpoint_count}, got {len(endpoints)} records"
        )
    return DeviceIdentity(version, profile, tuple(endpoints))


def verify_gates(identity: DeviceIdentity, expected: GateExpectations) -> None:
    observed_dirty = _parse_bool(identity.version["GIT_DIRTY"], "GIT_DIRTY")
    comparisons = {
        "git_sha": (identity.version["GIT_SHA"].lower(), expected.git_sha.lower()),
        "git_dirty": (observed_dirty, expected.git_dirty),
        "profile": (identity.profile["NAME"], expected.profile),
        "board": (identity.profile["BOARD"], expected.board),
    }
    mismatches = [
        f"{name}: observed={observed!r} expected={wanted!r}"
        for name, (observed, wanted) in comparisons.items()
        if observed != wanted
    ]
    if mismatches:
        raise GateError("identity/profile gate failed: " + "; ".join(mismatches))


def _require_object(value: Any, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ManifestError(f"{location} must be an object")
    return value


def _validate_v1_safety_gates(steps: list[dict[str, Any]]) -> None:
    expected = (
        (
            "COMPOSITION_STATUS",
            "DATA COMPOSITION",
            "COMPOSITION",
            "RUNTIME_READY",
            1,
            {"MODE": "ACTIVE", "OUTPUTS_INITIALIZED": "1"},
        ),
        (
            "PLATFORM_STATUS",
            "DATA PLATFORM",
            "PLATFORM",
            "MOTION_ACTIVE",
            0,
            {
                "STATE": "SAFE_IDLE",
                "FAULTED": "0",
                "TRACE": "0",
                "STREAM": "0",
            },
        ),
        (
            "SAFETY_STATUS",
            "DATA SAFETY",
            "SAFETY",
            "MOTOR_FAULT",
            0,
            {"TASK": "RUNNING", "RC_LOSS": "0"},
        ),
        (
            "GET_ENDPOINT_OBSERVATION {endpoint_id}",
            "DATA ENDPOINT_OBSERVATION",
            "ENDPOINT_OBSERVATION",
            "RPM",
            0,
            {
                "TYPE": "VELOCITY_RPM",
                "VALID": "1",
                "SOURCE": "DEVICE_FEEDBACK",
                "ONLINE": "1",
                "STALE": "0",
                "HEALTH": "HEALTHY",
            },
        ),
    )
    if len(steps) != len(expected):
        raise ManifestError("schema version 1 requires exactly four pre-motion safety gates")
    for index, (step, contract) in enumerate(zip(steps, expected, strict=True)):
        command, prefix, record, field, value, required = contract
        assertion = step.get("assert")
        if (
            step.get("command") != command
            or step.get("expect_prefix") != prefix
            or not isinstance(assertion, dict)
            or assertion.get("record") != record
            or assertion.get("field") != field
            or assertion.get("expected_value") != value
            or "expected_variable" in assertion
            or {
                str(key).upper(): field_value
                for key, field_value in assertion.get("required_fields", {}).items()
            }
            != required
        ):
            raise ManifestError(f"safety gate {index + 1} does not match schema-v1 contract")
        tolerance = assertion.get("tolerance")
        if index < 3:
            if tolerance != 0:
                raise ManifestError(f"safety gate {index + 1} requires zero tolerance")
        elif (
            isinstance(tolerance, bool)
            or not isinstance(tolerance, (int, float))
            or not math.isfinite(tolerance)
            or tolerance < 0
            or tolerance >= 1
        ):
            raise ManifestError("baseline RPM gate requires finite tolerance below 1")


def validate_manifest(manifest: dict[str, Any]) -> None:
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ManifestError(f"schema_version must be {SCHEMA_VERSION}")
    manifest_id = manifest.get("id")
    if not isinstance(manifest_id, str) or not IDENTIFIER_RE.fullmatch(manifest_id):
        raise ManifestError("id must be a stable identifier")
    description = manifest.get("description")
    if not isinstance(description, str) or not description.strip():
        raise ManifestError("description is required")
    level = manifest.get("level")
    if level != "L4":
        raise ManifestError("schema version 1 supports exactly level L4")
    profile = manifest.get("profile")
    if not isinstance(profile, str) or not IDENTIFIER_RE.fullmatch(profile):
        raise ManifestError("profile must be a stable profile name")
    if manifest.get("evidence_class") != "E2":
        raise ManifestError("schema version 1 supports exactly evidence class E2")
    verification_limits = manifest.get("verification_limits")
    if not isinstance(verification_limits, list) or not verification_limits or not all(
        isinstance(item, str) and item.strip() for item in verification_limits
    ):
        raise ManifestError("verification_limits must be a non-empty string array")

    gates = manifest.get("required_gates")
    if not isinstance(gates, list) or set(gates) != REQUIRED_GATES or len(gates) != 4:
        raise ManifestError(
            "required_gates must contain git_sha, git_dirty, profile and board"
        )

    motion = _require_object(manifest.get("motion"), "motion")
    if not isinstance(motion.get("enabled"), bool):
        raise ManifestError("motion.enabled must be boolean")
    confirmations = motion.get("required_confirmations")
    if not isinstance(confirmations, list) or not all(
        isinstance(item, str) for item in confirmations
    ):
        raise ManifestError("motion.required_confirmations must be an array")
    if motion["enabled"] and set(confirmations) != REQUIRED_MOTION_CONFIRMATIONS:
        raise ManifestError("motion manifests require all three safety confirmations")
    if not motion["enabled"] and confirmations:
        raise ManifestError("non-motion manifests cannot request motion confirmations")
    if motion["enabled"] is not True:
        raise ManifestError("schema version 1 requires a bounded motion test")
    motion_variable = motion.get("variable")
    if not isinstance(motion_variable, str) or not IDENTIFIER_RE.fullmatch(
        motion_variable
    ):
        raise ManifestError("motion.variable must name the bounded RPM variable")

    variables = _require_object(manifest.get("variables", {}), "variables")
    for name, raw_definition in variables.items():
        if not IDENTIFIER_RE.fullmatch(name):
            raise ManifestError(f"invalid variable name {name!r}")
        definition = _require_object(raw_definition, f"variables.{name}")
        if definition.get("type") not in {"integer", "string"}:
            raise ManifestError(f"variables.{name}.type must be integer or string")
        if "required" in definition and not isinstance(definition["required"], bool):
            raise ManifestError(f"variables.{name}.required must be boolean")
        if definition["type"] == "integer":
            for bound in ("minimum", "maximum", "example"):
                if bound in definition and (
                    isinstance(definition[bound], bool)
                    or not isinstance(definition[bound], int)
                ):
                    raise ManifestError(f"variables.{name}.{bound} must be an integer")
            minimum = definition.get("minimum")
            maximum = definition.get("maximum")
            example = definition.get("example")
            if minimum is not None and maximum is not None and minimum > maximum:
                raise ManifestError(f"variables.{name} has inverted limits")
            if example is not None and (
                (minimum is not None and example < minimum)
                or (maximum is not None and example > maximum)
            ):
                raise ManifestError(f"variables.{name}.example is outside its limits")

    target = _require_object(manifest.get("target"), "target")
    for field in ("endpoint_id", "endpoint_name", "capability"):
        value = target.get(field)
        if not isinstance(value, str) or not value.strip():
            raise ManifestError(f"target.{field} is required")
    if not ENDPOINT_ID_RE.fullmatch(target["endpoint_id"]):
        raise ManifestError("target.endpoint_id is invalid")
    if not IDENTIFIER_RE.fullmatch(target["capability"]):
        raise ManifestError("target.capability is invalid")
    if target["capability"].upper() != "VELOCITY_RPM":
        raise ManifestError("schema version 1 supports only VELOCITY_RPM")
    motion_definition = variables.get(motion_variable)
    if not isinstance(motion_definition, dict):
        raise ManifestError("motion.variable must reference a declared variable")
    if (
        motion_definition.get("type") != "integer"
        or motion_definition.get("required") is not True
        or motion_definition.get("nonzero") is not True
        or not isinstance(motion_definition.get("minimum"), int)
        or isinstance(motion_definition.get("minimum"), bool)
        or not isinstance(motion_definition.get("maximum"), int)
        or isinstance(motion_definition.get("maximum"), bool)
    ):
        raise ManifestError(
            "motion.variable must be a required non-zero bounded integer"
        )
    if "default" in motion_definition:
        raise ManifestError(
            "motion.variable cannot have a default; every motion run needs an explicit value"
        )

    safety = _require_object(manifest.get("safety"), "safety")
    if safety.get("unloaded_required") is not True:
        raise ManifestError("safety.unloaded_required must be true")
    if safety.get("physical_cutoff_required") is not True:
        raise ManifestError("safety.physical_cutoff_required must be true")
    maximum_duration = safety.get("maximum_test_duration_seconds")
    if (
        isinstance(maximum_duration, bool)
        or not isinstance(maximum_duration, (int, float))
        or not math.isfinite(maximum_duration)
        or maximum_duration <= 0
        or maximum_duration > MAX_TEST_DURATION_SECONDS
    ):
        raise ManifestError(
            "safety.maximum_test_duration_seconds must be finite and between "
            f"0 and {MAX_TEST_DURATION_SECONDS:g}"
        )

    cleanup = _require_object(manifest.get("cleanup"), "cleanup")
    if (
        cleanup.get("stop_all") is not True
        or cleanup.get("verify_ack") is not True
        or cleanup.get("verify_stopped") is not True
    ):
        raise ManifestError(
            "cleanup must require STOP ALL, acknowledgement and stopped observation"
        )
    cleanup_settle = cleanup.get("settle_seconds")
    if (
        isinstance(cleanup_settle, bool)
        or not isinstance(cleanup_settle, (int, float))
        or not math.isfinite(cleanup_settle)
        or cleanup_settle < 0
        or cleanup_settle > 2
    ):
        raise ManifestError("cleanup.settle_seconds must be between 0 and 2")
    cleanup_observation = _require_object(
        cleanup.get("observation"), "cleanup.observation"
    )
    for field in ("record", "field"):
        value = cleanup_observation.get(field)
        if not isinstance(value, str) or not IDENTIFIER_RE.fullmatch(value):
            raise ManifestError(f"cleanup.observation.{field} is invalid")
    expected_stopped = cleanup_observation.get("expected_value")
    cleanup_tolerance = cleanup_observation.get("tolerance")
    if expected_stopped != 0 or (
        isinstance(cleanup_tolerance, bool)
        or not isinstance(cleanup_tolerance, (int, float))
        or not math.isfinite(cleanup_tolerance)
        or cleanup_tolerance < 0
        or cleanup_tolerance >= 1
    ):
        raise ManifestError(
            "cleanup observation must distinguish zero RPM with finite tolerance below 1"
        )
    cleanup_required_fields = cleanup_observation.get("required_fields")
    if not isinstance(cleanup_required_fields, dict) or not all(
        isinstance(key, str)
        and IDENTIFIER_RE.fullmatch(key)
        and isinstance(value, str)
        and value
        for key, value in cleanup_required_fields.items()
    ):
        raise ManifestError(
            "cleanup.observation.required_fields must map field names to values"
        )
    if (
        cleanup_observation["record"].upper() != "ENDPOINT_OBSERVATION"
        or cleanup_observation["field"].upper() != "RPM"
        or {
            key.upper(): value for key, value in cleanup_required_fields.items()
        }
        != {
            "TYPE": "VELOCITY_RPM",
            "VALID": "1",
            "SOURCE": "DEVICE_FEEDBACK",
            "ONLINE": "1",
            "STALE": "0",
            "HEALTH": "HEALTHY",
        }
    ):
        raise ManifestError(
            "schema version 1 cleanup requires fresh healthy target velocity feedback"
        )

    selector = manifest.get("endpoint_selector")
    if motion["enabled"] and selector is None:
        raise ManifestError("motion manifests require endpoint_selector")
    if selector is not None:
        selector = _require_object(selector, "endpoint_selector")
        capabilities = selector.get("capabilities")
        if not isinstance(capabilities, list) or not capabilities or not all(
            isinstance(item, str) and IDENTIFIER_RE.fullmatch(item)
            for item in capabilities
        ):
            raise ManifestError("endpoint_selector.capabilities must be non-empty")
        if selector.get("exact_count") != 1:
            raise ManifestError("endpoint_selector.exact_count must be 1")
        inventory_count = selector.get("exact_inventory_count")
        if inventory_count != 1:
            raise ManifestError(
                "schema version 1 requires endpoint_selector.exact_inventory_count == 1"
            )
        target_criticality = selector.get("target_criticality")
        if target_criticality not in {"DEVELOPMENT", "OPTIONAL", "REQUIRED"}:
            raise ManifestError(
                "endpoint_selector.target_criticality must be DEVELOPMENT, OPTIONAL or REQUIRED"
            )
        if target["capability"].upper() not in {
            item.upper() for item in capabilities
        }:
            raise ManifestError("target capability must be required by endpoint_selector")

    steps = manifest.get("steps")
    if not isinstance(steps, list) or not steps:
        raise ManifestError("steps must be a non-empty array")
    step_ids: set[str] = set()
    allowed_fields = set(variables)
    if selector is not None:
        allowed_fields.add("endpoint_id")
    formatter = string.Formatter()
    has_motion_step = False
    has_assertion = False
    cleanup_verification_count = 0
    pending_motion_observation = False
    post_motion_observation_count = 0
    stopped_after_motion = False
    motion_step_count = 0
    motion_step_index: int | None = None
    safety_gate_steps: list[dict[str, Any]] = []
    safety_gate_indices: list[int] = []
    for index, raw_step in enumerate(steps):
        step = _require_object(raw_step, f"steps[{index}]")
        step_id = step.get("id")
        if (
            not isinstance(step_id, str)
            or not IDENTIFIER_RE.fullmatch(step_id)
            or step_id in step_ids
        ):
            raise ManifestError(f"steps[{index}].id is invalid or duplicate")
        step_ids.add(step_id)
        has_command = "command" in step
        has_sleep = "sleep_seconds" in step
        if has_command == has_sleep:
            raise ManifestError(f"steps[{index}] needs exactly one command or sleep")
        if has_sleep:
            delay = step["sleep_seconds"]
            if (
                isinstance(delay, bool)
                or not isinstance(delay, (int, float))
                or not math.isfinite(delay)
                or delay < 0
                or delay > maximum_duration
            ):
                raise ManifestError(f"steps[{index}].sleep_seconds must be non-negative")
            continue

        command = step["command"]
        if not isinstance(command, str) or "\n" in command or "\r" in command:
            raise ManifestError(f"steps[{index}].command must be one line")
        verb = command.split(maxsplit=1)[0].upper() if command.split() else ""
        if verb not in SAFE_COMMANDS:
            raise ManifestError(f"steps[{index}] uses unsupported command {verb!r}")
        command_tokens = command.split()
        if verb in {
            "GET_ENDPOINT_OBSERVATION",
            "STOP_ENDPOINT",
        } and command_tokens != [verb, "{endpoint_id}"]:
            raise ManifestError(f"{verb} must address exactly {{endpoint_id}}")
        if verb == "SET_ENDPOINT_SPEED" and (
            command_tokens
            != [verb, "{endpoint_id}", "{" + motion_variable + "}"]
        ):
            raise ManifestError(
                "SET_ENDPOINT_SPEED must address {endpoint_id} using motion.variable"
            )
        expected_prefix = step.get("expect_prefix")
        if not isinstance(expected_prefix, str) or not expected_prefix:
            raise ManifestError(f"steps[{index}].expect_prefix is required")
        required_prefix = {
            "SET_ENDPOINT_SPEED": (
                "OK SET_ENDPOINT_SPEED ID:{endpoint_id} "
                f"RPM_TARGET:{{{motion_variable}}}"
            ),
            "STOP_ENDPOINT": "OK STOP_ENDPOINT ID:{endpoint_id}",
        }.get(verb)
        if required_prefix is not None and expected_prefix != required_prefix:
            raise ManifestError(
                f"{verb} expect_prefix must be {required_prefix!r}"
            )
        step_motion = step.get("motion", False)
        if not isinstance(step_motion, bool):
            raise ManifestError(f"steps[{index}].motion must be boolean")
        if verb == "SET_ENDPOINT_SPEED" and not step_motion:
            raise ManifestError("SET_ENDPOINT_SPEED must be marked as motion")
        if step_motion and verb != "SET_ENDPOINT_SPEED":
            raise ManifestError("only SET_ENDPOINT_SPEED may be marked as motion")
        if step_motion:
            if pending_motion_observation:
                raise ManifestError(
                    "each motion command needs an observation before another command"
                )
            has_motion_step = True
            motion_step_count += 1
            motion_step_index = index
            pending_motion_observation = True
            stopped_after_motion = False
        if verb == "STOP_ENDPOINT" and pending_motion_observation:
            raise ManifestError("motion response must be observed before STOP_ENDPOINT")
        if verb == "STOP_ENDPOINT" and has_motion_step:
            stopped_after_motion = True
        assertion = step.get("assert")
        if assertion is not None:
            assertion = _require_object(assertion, f"steps[{index}].assert")
            record = assertion.get("record")
            if not isinstance(record, str) or not IDENTIFIER_RE.fullmatch(record):
                raise ManifestError(f"steps[{index}].assert.record is invalid")
            field = assertion.get("field")
            if not isinstance(field, str) or not IDENTIFIER_RE.fullmatch(field):
                raise ManifestError(f"steps[{index}].assert.field is invalid")
            expected_variable = assertion.get("expected_variable")
            has_expected_value = "expected_value" in assertion
            if (expected_variable is None) == (not has_expected_value):
                raise ManifestError(
                    f"steps[{index}].assert needs one expected value or variable"
                )
            if expected_variable is not None and expected_variable not in variables:
                raise ManifestError(
                    f"steps[{index}].assert references an unknown variable"
                )
            expected_value = assertion.get("expected_value")
            if has_expected_value and (
                isinstance(expected_value, bool)
                or not isinstance(expected_value, (int, float))
                or not math.isfinite(expected_value)
            ):
                raise ManifestError(
                    f"steps[{index}].assert.expected_value must be numeric"
                )
            tolerance = assertion.get("tolerance")
            if (
                isinstance(tolerance, bool)
                or not isinstance(tolerance, (int, float))
                or not math.isfinite(tolerance)
                or tolerance < 0
            ):
                raise ManifestError(
                    f"steps[{index}].assert.tolerance must be non-negative"
                )
            required_fields = assertion.get("required_fields", {})
            if not isinstance(required_fields, dict) or not all(
                isinstance(key, str)
                and IDENTIFIER_RE.fullmatch(key)
                and isinstance(value, str)
                and value
                for key, value in required_fields.items()
            ):
                raise ManifestError(
                    f"steps[{index}].assert.required_fields must map field names to values"
                )
            has_assertion = True
            if (
                pending_motion_observation
                and verb == "GET_ENDPOINT_OBSERVATION"
                and not step.get("cleanup_verification", False)
            ):
                if expected_variable != motion_variable:
                    raise ManifestError(
                        "post-motion observation must compare against motion.variable"
                    )
                if assertion["record"].upper() != "ENDPOINT_OBSERVATION" or assertion[
                    "field"
                ].upper() != "RPM":
                    raise ManifestError(
                        "velocity response must assert ENDPOINT_OBSERVATION RPM"
                    )
                required_semantics = {
                    "TYPE": "VELOCITY_RPM",
                    "VALID": "1",
                    "SOURCE": "DEVICE_FEEDBACK",
                    "ONLINE": "1",
                    "STALE": "0",
                    "HEALTH": "HEALTHY",
                }
                normalized_required = {
                    key.upper(): value for key, value in required_fields.items()
                }
                if normalized_required != required_semantics:
                    raise ManifestError(
                        "post-motion observation requires exact fresh, healthy "
                        "device-feedback semantics"
                    )
                definition = variables[expected_variable]
                if definition.get("type") != "integer" or definition.get("nonzero") is not True:
                    raise ManifestError("motion response variable must be a non-zero integer")
                minimum = definition.get("minimum")
                maximum = definition.get("maximum")
                if not isinstance(minimum, int) or not isinstance(maximum, int):
                    raise ManifestError("motion response variable needs integer minimum/maximum")
                if minimum <= 0 <= maximum:
                    smallest_nonzero = 1
                elif minimum > 0:
                    smallest_nonzero = minimum
                else:
                    smallest_nonzero = abs(maximum)
                if tolerance >= smallest_nonzero:
                    raise ManifestError(
                        "post-motion tolerance must distinguish the smallest setpoint from zero"
                    )
                pending_motion_observation = False
                post_motion_observation_count += 1
        step_timeout = step.get("timeout_seconds")
        if step_timeout is not None and (
            isinstance(step_timeout, bool)
            or not isinstance(step_timeout, (int, float))
            or not math.isfinite(step_timeout)
            or step_timeout <= 0
            or step_timeout > MAX_COMMAND_TIMEOUT_SECONDS
        ):
            raise ManifestError(
                f"steps[{index}].timeout_seconds must be finite and between 0 and "
                f"{MAX_COMMAND_TIMEOUT_SECONDS:g}"
            )
        safety_gate = step.get("safety_gate", False)
        if not isinstance(safety_gate, bool):
            raise ManifestError(f"steps[{index}].safety_gate must be boolean")
        if safety_gate and assertion is None:
            raise ManifestError(f"steps[{index}].safety_gate requires an assertion")
        if safety_gate:
            if has_motion_step:
                raise ManifestError("all safety gates must precede SET_ENDPOINT_SPEED")
            safety_gate_steps.append(step)
            safety_gate_indices.append(index)
        cleanup_verification = step.get("cleanup_verification", False)
        if not isinstance(cleanup_verification, bool):
            raise ManifestError(
                f"steps[{index}].cleanup_verification must be boolean"
            )
        if cleanup_verification:
            cleanup_verification_count += 1
            if assertion is None or assertion.get("expected_value") != 0:
                raise ManifestError(
                    "cleanup verification must assert a zero-valued observation"
                )
            if verb != "GET_ENDPOINT_OBSERVATION" or not stopped_after_motion:
                raise ManifestError(
                    "cleanup verification must observe the endpoint after STOP_ENDPOINT"
                )
        for template_name, template in (
            ("command", command),
            ("expect_prefix", expected_prefix),
        ):
            for _, field_name, format_spec, conversion in formatter.parse(template):
                if field_name is None:
                    continue
                if field_name not in allowed_fields or format_spec or conversion:
                    raise ManifestError(
                        f"steps[{index}].{template_name} has unsupported "
                        f"placeholder {field_name!r}"
                    )
    if has_motion_step != motion["enabled"]:
        raise ManifestError("motion.enabled must match the manifest steps")
    if level == "L4" and not has_assertion:
        raise ManifestError("L4 manifests need an observation assertion")
    if motion["enabled"] and cleanup_verification_count != 1:
        raise ManifestError(
            "motion manifests need exactly one stopped-observation cleanup verification"
        )
    if motion["enabled"] and (pending_motion_observation or post_motion_observation_count == 0):
        raise ManifestError("motion manifests require a post-actuation observation")
    if motion_step_count != 1:
        raise ManifestError("schema version 1 requires exactly one SET_ENDPOINT_SPEED")
    _validate_v1_safety_gates(safety_gate_steps)
    if motion_step_index is None:  # Defensive: motion_step_count already enforces this.
        raise ManifestError("schema version 1 is missing SET_ENDPOINT_SPEED")
    required_gate_indices = list(
        range(motion_step_index - len(safety_gate_steps), motion_step_index)
    )
    if safety_gate_indices != required_gate_indices:
        raise ManifestError(
            "schema version 1 requires the four safety gates as the contiguous "
            "block immediately before SET_ENDPOINT_SPEED"
        )


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ManifestError(f"invalid JSON manifest {path}: {exc}") from exc
    manifest = _require_object(manifest, "manifest")
    validate_manifest(manifest)
    return manifest


def resolve_variables(
    manifest: dict[str, Any], supplied: dict[str, str]
) -> dict[str, Any]:
    definitions = manifest.get("variables", {})
    unknown = set(supplied) - set(definitions)
    if unknown:
        raise ManifestError(f"unknown variables: {', '.join(sorted(unknown))}")
    resolved: dict[str, Any] = {}
    for name, definition in definitions.items():
        if name in supplied:
            raw = supplied[name]
        elif definition.get("required", False):
            raise ManifestError(f"missing required variable {name}")
        elif "default" in definition:
            raw = definition["default"]
        else:
            continue
        try:
            value = int(raw) if definition["type"] == "integer" else str(raw)
        except (TypeError, ValueError) as exc:
            raise ManifestError(f"variable {name} must be an integer") from exc
        if definition.get("nonzero", False) and value == 0:
            raise ManifestError(f"variable {name} must be non-zero")
        if "minimum" in definition and value < definition["minimum"]:
            raise ManifestError(
                f"variable {name} must be >= {definition['minimum']}"
            )
        if "maximum" in definition and value > definition["maximum"]:
            raise ManifestError(
                f"variable {name} must be <= {definition['maximum']}"
            )
        resolved[name] = value
    return resolved


def evidence_path(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if resolved == REPO_ROOT or REPO_ROOT in resolved.parents:
        raise EvidenceError("evidence path must be outside the source repository")
    if resolved.suffix.lower() != ".json":
        raise EvidenceError("evidence path must end in .json")
    return resolved


def _write_all(descriptor: int, payload: bytes) -> None:
    remaining = memoryview(payload)
    while remaining:
        written = os.write(descriptor, remaining)
        if written <= 0:
            raise OSError("short write while recording HIL evidence")
        remaining = remaining[written:]


def _fsync_directory(directory: Path) -> None:
    # Windows does not expose POSIX directory descriptors, so opening a
    # directory with os.open() fails before fsync can be attempted.  Individual
    # evidence and reservation files are still fsynced; directory durability is
    # unavailable on that platform rather than silently approximated.
    if os.name == "nt":
        return
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(directory, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _probe_atomic_evidence_publish(directory: Path, name: str) -> None:
    """Prove same-filesystem no-clobber publication before serial is opened."""

    source_name: str | None = None
    link_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=directory,
            prefix=f".{name}.publish-probe.",
            delete=False,
        ) as probe:
            probe.write(b"HIL evidence publication probe\n")
            probe.flush()
            os.fsync(probe.fileno())
            source_name = probe.name
        link_name = source_name + ".link"
        os.link(source_name, link_name)
        _fsync_directory(directory)
    finally:
        cleanup_error: OSError | None = None
        for candidate in (link_name, source_name):
            if candidate is None:
                continue
            try:
                os.unlink(candidate)
            except FileNotFoundError:
                pass
            except OSError as exc:
                cleanup_error = cleanup_error or exc
        try:
            _fsync_directory(directory)
        except OSError as exc:
            cleanup_error = cleanup_error or exc
        if cleanup_error is not None:
            raise cleanup_error


class EvidenceReservation:
    """Reserve and safely publish evidence before any serial I/O occurs."""

    def __init__(
        self,
        path: Path,
        reservation_path: Path,
        descriptor: int,
        device: int,
        inode: int,
        metadata: dict[str, Any],
    ) -> None:
        self.path = path
        self.reservation_path = reservation_path
        self._descriptor = descriptor
        self._device = device
        self._inode = inode
        self._metadata = dict(metadata)
        self._closed = False

    @classmethod
    def reserve(
        cls,
        path: Path,
        *,
        metadata: dict[str, Any] | None = None,
    ) -> "EvidenceReservation":
        resolved = evidence_path(path)
        reservation_path = resolved.with_name(f".{resolved.name}.reserved")
        reservation_metadata = dict(metadata or {})
        descriptor: int | None = None
        try:
            resolved.parent.mkdir(parents=True, exist_ok=True)
            descriptor = os.open(
                reservation_path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
            )
            marker = (
                json.dumps(
                    {
                        **reservation_metadata,
                        "evidence_schema_version": 1,
                        "status": "RESERVED",
                        "detail": "HIL run has not completed",
                        "destination": str(resolved),
                    },
                    sort_keys=True,
                )
                + "\n"
            ).encode("utf-8")
            _write_all(descriptor, marker)
            os.fsync(descriptor)
            reserved = os.fstat(descriptor)
            _fsync_directory(resolved.parent)
            if os.path.lexists(resolved):
                raise FileExistsError(str(resolved))
            _probe_atomic_evidence_publish(resolved.parent, resolved.name)
            if os.path.lexists(resolved):
                raise FileExistsError(str(resolved))
            return cls(
                resolved,
                reservation_path,
                descriptor,
                reserved.st_dev,
                reserved.st_ino,
                reservation_metadata,
            )
        except FileExistsError as exc:
            if descriptor is not None:
                os.close(descriptor)
                try:
                    reservation_path.unlink()
                    _fsync_directory(resolved.parent)
                except OSError:
                    pass
            raise EvidenceError(
                "evidence path already exists; refusing to overwrite"
            ) from exc
        except OSError as exc:
            if descriptor is not None:
                os.close(descriptor)
                try:
                    reservation_path.unlink()
                    _fsync_directory(resolved.parent)
                except OSError:
                    pass
            raise EvidenceError(f"cannot reserve evidence {resolved}: {exc}") from exc

    def _still_owns_reservation(self) -> bool:
        try:
            current = os.stat(self.reservation_path, follow_symlinks=False)
        except OSError:
            return False
        return current.st_dev == self._device and current.st_ino == self._inode

    def _write_marker(self, payload: dict[str, Any]) -> None:
        encoded = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode(
            "utf-8"
        )
        os.lseek(self._descriptor, 0, os.SEEK_SET)
        os.ftruncate(self._descriptor, 0)
        _write_all(self._descriptor, encoded)
        os.fsync(self._descriptor)

    def _mark_finalize_failed(
        self,
        error: BaseException,
        payload: dict[str, Any],
    ) -> None:
        if self._closed or not self._still_owns_reservation():
            return
        marker = {
            **self._metadata,
            "evidence_schema_version": 1,
            "status": "FINALIZE_FAILED",
            "detail": str(error),
            "destination": str(self.path),
            "attempted_status": payload.get("status"),
            "started_at": payload.get(
                "started_at", self._metadata.get("started_at")
            ),
            "completed_at": payload.get("completed_at"),
        }
        manifest = payload.get("manifest")
        if isinstance(manifest, dict):
            marker["manifest_id"] = manifest.get("id")
        try:
            self._write_marker(marker)
        except OSError:
            # The durable RESERVED marker is still preferable to deleting the trail.
            pass

    def finalize(self, payload: dict[str, Any]) -> None:
        temporary_name: str | None = None
        try:
            encoded = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode(
                "utf-8"
            )
            with tempfile.NamedTemporaryFile(
                mode="wb",
                dir=self.path.parent,
                prefix=f".{self.path.name}.",
                delete=False,
            ) as temporary:
                temporary.write(encoded)
                temporary.flush()
                os.fsync(temporary.fileno())
                temporary_name = temporary.name
            if not self._still_owns_reservation():
                raise EvidenceError(
                    "evidence reservation was replaced; refusing to overwrite"
                )
            try:
                os.link(temporary_name, self.path)
            except FileExistsError as exc:
                raise EvidenceError(
                    "evidence destination appeared during run; refusing to overwrite"
                ) from exc
            _fsync_directory(self.path.parent)
            try:
                os.unlink(temporary_name)
                temporary_name = None
            except OSError:
                # The published inode is already durable; orphan cleanup is best-effort.
                pass
            if not self._still_owns_reservation():
                raise EvidenceError(
                    "evidence reservation changed during finalization"
                )
            # Windows does not permit unlinking a file with this process's
            # descriptor still open.  Closing here is final: the evidence has
            # already been fsynced and published, and ownership is checked
            # again immediately before the reservation is removed.
            if os.name == "nt":
                self.close(remove_reservation=False)
            try:
                self.reservation_path.unlink()
            except OSError as exc:
                raise EvidenceError(
                    f"cannot release evidence reservation {self.reservation_path}: {exc}"
                ) from exc
            try:
                _fsync_directory(self.path.parent)
            except OSError:
                # The final evidence entry was already fsynced before reservation removal.
                pass
            self.close(remove_reservation=False)
        except BaseException as exc:
            error = (
                exc
                if isinstance(exc, EvidenceError)
                else EvidenceError(f"cannot finalize evidence {self.path}: {exc}")
            )
            self._mark_finalize_failed(error, payload)
            self.close(remove_reservation=False)
            if isinstance(exc, (HilInterrupted, KeyboardInterrupt)):
                raise
            if error is exc:
                raise
            raise error from exc
        finally:
            if temporary_name:
                try:
                    os.unlink(temporary_name)
                except OSError:
                    pass

    def close(self, *, remove_reservation: bool) -> None:
        if self._closed:
            return
        os.close(self._descriptor)
        self._closed = True
        if remove_reservation and self._still_owns_reservation():
            try:
                self.reservation_path.unlink()
                _fsync_directory(self.path.parent)
            except OSError:
                pass


def _error_payload(error: BaseException) -> dict[str, str]:
    return {"type": type(error).__name__, "message": str(error)}


def _numeric_observation(response: GatewayResponse, field: str) -> float:
    values: list[float] = []
    wanted = field.upper()
    for line in response.lines:
        try:
            tokens = shlex.split(line)
        except ValueError as exc:
            raise StepError(f"malformed observation line: {line}") from exc
        if len(tokens) < 2 or tokens[0] != "DATA":
            continue
        for token in tokens[2:]:
            if ":" not in token:
                continue
            key, raw_value = token.split(":", 1)
            if key.upper() != wanted:
                continue
            try:
                values.append(float(raw_value))
            except ValueError as exc:
                raise StepError(
                    f"observation field {field} is not numeric: {raw_value!r}"
                ) from exc
    if len(values) != 1:
        raise StepError(
            f"expected one numeric observation field {field}, got {len(values)}"
        )
    return values[0]


def _assert_required_fields(
    response: GatewayResponse,
    record: str,
    required_fields: dict[str, str],
    *,
    expected_endpoint_id: str | None = None,
) -> dict[str, str]:
    matches = [
        parsed
        for line in response.lines
        if (parsed := _fields(line, (record.upper(),)))
    ]
    if len(matches) != 1:
        raise StepError(
            f"expected one {record.upper()} record, got {len(matches)}"
        )
    observed = matches[0]
    if (
        expected_endpoint_id is not None
        and observed.get("ID") != expected_endpoint_id
    ):
        raise AssertionFailure(
            "observation endpoint mismatch: "
            f"observed={observed.get('ID')!r} expected={expected_endpoint_id!r}"
        )
    mismatches = [
        f"{key}: observed={observed.get(key.upper())!r} expected={expected!r}"
        for key, expected in required_fields.items()
        if observed.get(key.upper()) != expected
    ]
    if mismatches:
        raise AssertionFailure(
            "observation validity gate failed: " + "; ".join(mismatches)
        )
    selected = {key.upper(): observed[key.upper()] for key in required_fields}
    if expected_endpoint_id is not None:
        selected["ID"] = expected_endpoint_id
    return selected


class HilRunner:
    def __init__(
        self,
        client: Gateway,
        *,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self.client = client
        self.clock = clock
        self.sleep = sleep

    def _record_command(
        self,
        evidence: dict[str, Any],
        phase: str,
        command: str,
        *,
        timeout: float | None = None,
    ) -> GatewayResponse:
        started = self.clock()
        offset_seconds = max(
            0.0,
            started - float(evidence.get("_monotonic_origin", started)),
        )
        try:
            response = self.client.execute_checked(command, timeout=timeout)
        except GatewayCommandError as exc:
            evidence["commands"].append(
                {
                    "phase": phase,
                    "offset_seconds": round(offset_seconds, 6),
                    **exc.response.to_dict(),
                }
            )
            raise
        except BaseException as exc:
            attempted: dict[str, Any] = {
                "phase": phase,
                "offset_seconds": round(offset_seconds, 6),
                "command": command,
                "error": _error_payload(exc),
            }
            if isinstance(exc, GatewayTimeout):
                attempted["partial_lines"] = list(exc.partial_lines)
            evidence["commands"].append(attempted)
            raise
        evidence["commands"].append(
            {
                "phase": phase,
                "offset_seconds": round(offset_seconds, 6),
                **response.to_dict(),
            }
        )
        return response

    def _stop_all(
        self,
        evidence: dict[str, Any],
        phase: str,
        *,
        timeout: float | None = None,
    ) -> list[BaseException]:
        errors: list[BaseException] = []
        try:
            response = self._record_command(
                evidence,
                phase,
                "STOP ALL",
                timeout=timeout,
            )
            if not response.lines or response.lines[-1] != "OK STOP ALL":
                raise CleanupError("STOP ALL did not return its exact acknowledgement")
        except BaseException as exc:
            errors.append(exc)
            evidence["cleanup_errors"].append(_error_payload(exc))
        return errors

    def _verify_final_stop(
        self,
        evidence: dict[str, Any],
        cleanup: dict[str, Any],
        endpoint_id: str,
    ) -> list[BaseException]:
        errors: list[BaseException] = []
        try:
            self.sleep(float(cleanup["settle_seconds"]))
            assertion = cleanup["observation"]
            response = self._record_command(
                evidence,
                "final_stop_observation",
                f"GET_ENDPOINT_OBSERVATION {endpoint_id}",
            )
            observed = _numeric_observation(response, assertion["field"])
            required_observed = _assert_required_fields(
                response,
                assertion["record"],
                assertion["required_fields"],
                expected_endpoint_id=endpoint_id,
            )
            expected = float(assertion["expected_value"])
            tolerance = float(assertion["tolerance"])
            passed = abs(observed - expected) <= tolerance
            evidence["cleanup_observation"] = {
                "field": assertion["field"],
                "observed": observed,
                "expected": expected,
                "tolerance": tolerance,
                "required_fields": required_observed,
                "passed": passed,
            }
            if not passed:
                raise CleanupError(
                    f"final stopped observation {observed} outside "
                    f"{expected} +/- {tolerance}"
                )
        except BaseException as exc:
            error = exc if isinstance(exc, CleanupError) else CleanupError(str(exc))
            errors.append(error)
            evidence["cleanup_errors"].append(_error_payload(error))
        return errors

    def run(
        self,
        manifest: dict[str, Any],
        *,
        expected: GateExpectations,
        variables: dict[str, Any],
        confirmations: set[str],
        output_path: Path,
        manifest_path: Path,
        hardware_id: str,
        pcb_revision: str,
        firmware_artifact_sha256: str,
        firmware_artifact_uri: str | None,
        port: str,
        baudrate: int,
    ) -> dict[str, Any]:
        validate_manifest(manifest)
        output_path = evidence_path(output_path)
        for name, value in (
            ("hardware_id", hardware_id),
            ("pcb_revision", pcb_revision),
            ("port", port),
        ):
            if not isinstance(value, str) or not value.strip() or "\n" in value or "\r" in value:
                raise GateError(f"{name} must be a non-empty single-line value")
        if isinstance(baudrate, bool) or not isinstance(baudrate, int) or baudrate <= 0:
            raise GateError("baudrate must be a positive integer")
        if not isinstance(firmware_artifact_sha256, str) or not re.fullmatch(
            r"[0-9a-fA-F]{64}", firmware_artifact_sha256
        ):
            raise GateError("firmware_artifact_sha256 must be exactly 64 hex characters")
        firmware_artifact_sha256 = firmware_artifact_sha256.lower()
        if firmware_artifact_uri is not None and (
            not isinstance(firmware_artifact_uri, str)
            or not firmware_artifact_uri.strip()
            or "\n" in firmware_artifact_uri
            or "\r" in firmware_artifact_uri
        ):
            raise GateError("firmware_artifact_uri must be a non-empty single-line value")
        if expected.profile != manifest["profile"]:
            raise GateError(
                "expected profile does not match manifest: "
                f"{expected.profile!r} != {manifest['profile']!r}"
            )
        motion = manifest["motion"]
        if motion["enabled"]:
            missing = REQUIRED_MOTION_CONFIRMATIONS - confirmations
            if missing:
                raise SafetyConfirmationError(
                    "missing explicit motion confirmations: "
                    + ", ".join(sorted(missing))
                )
            if expected.git_dirty:
                raise SafetyConfirmationError("motion runs require a clean expected build")
            if not re.fullmatch(r"[0-9a-fA-F]{40}", expected.git_sha):
                raise SafetyConfirmationError(
                    "motion runs require an exact 40-hex firmware Git SHA"
                )
        motion_variable = motion["variable"]
        motion_value = variables.get(motion_variable)
        motion_definition = manifest["variables"][motion_variable]
        if (
            isinstance(motion_value, bool)
            or not isinstance(motion_value, int)
            or motion_value == 0
            or motion_value < motion_definition["minimum"]
            or motion_value > motion_definition["maximum"]
        ):
            raise GateError("resolved motion variable is missing, zero or outside manifest bounds")

        started = self.clock()
        try:
            manifest_bytes = manifest_path.read_bytes()
            on_disk_manifest = json.loads(manifest_bytes)
        except (OSError, json.JSONDecodeError) as exc:
            raise ManifestError(f"cannot re-read manifest {manifest_path}: {exc}") from exc
        if on_disk_manifest != manifest:
            raise ManifestError("manifest changed after validation")
        evidence: dict[str, Any] = {
            "_monotonic_origin": started,
            "evidence_schema_version": 1,
            "manifest": {
                "id": manifest["id"],
                "level": manifest["level"],
                "profile": manifest["profile"],
                "evidence_class": manifest["evidence_class"],
                "verification_limits": list(manifest["verification_limits"]),
                "path": str(manifest_path.resolve()),
                "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            },
            "started_at": datetime.now(timezone.utc).isoformat(),
            "expected": {
                "git_sha": expected.git_sha,
                "git_dirty": expected.git_dirty,
                "profile": expected.profile,
                "board": expected.board,
            },
            "hardware": {
                "id": hardware_id,
                "pcb_revision": pcb_revision,
            },
            "firmware_artifact": {
                "sha256": firmware_artifact_sha256,
                "uri": firmware_artifact_uri,
                "runtime_binding_limit": (
                    "Artifact provenance does not independently prove the running binary"
                ),
            },
            "connection": {
                "port": port,
                "baudrate": baudrate,
            },
            "safety": {
                "confirmations": sorted(confirmations),
                "maximum_test_duration_seconds": manifest["safety"][
                    "maximum_test_duration_seconds"
                ],
                "software_cleanup_is_best_effort": True,
            },
            "commands": [],
            "steps": [],
            "assertions": [],
            "cleanup_errors": [],
        }
        reservation = EvidenceReservation.reserve(
            output_path,
            metadata={
                "manifest_id": evidence["manifest"]["id"],
                "manifest_sha256": evidence["manifest"]["sha256"],
                "started_at": evidence["started_at"],
                "hardware_id": hardware_id,
                "pcb_revision": pcb_revision,
                "port": port,
                "expected_profile": expected.profile,
                "expected_git_sha": expected.git_sha,
                "firmware_artifact_sha256": firmware_artifact_sha256,
            },
        )
        primary_error: BaseException | None = None
        cleanup_errors: list[BaseException] = []
        opened = False
        motion_attempted = False
        selected_endpoint_id: str | None = None
        active_safety_gate = False

        with TerminationSignalGuard():
            try:
                self.client.open()
                opened = True
                try:
                    identity = identify_gateway(
                        self.client,
                        execute=lambda command: self._record_command(
                            evidence,
                            f"identity:{command.lower()}",
                            command,
                        ),
                    )
                except (HilInterrupted, KeyboardInterrupt):
                    raise
                except BaseException as exc:
                    raise GateError(
                        f"identity discovery gate failed: {type(exc).__name__}: {exc}"
                    ) from exc
                evidence["observed"] = identity.to_dict()
                verify_gates(identity, expected)

                context = dict(variables)
                selector = manifest.get("endpoint_selector")
                if selector is not None:
                    if len(identity.endpoints) != selector["exact_inventory_count"]:
                        raise GateError(
                            "endpoint inventory mismatch: "
                            f"observed={len(identity.endpoints)} "
                            f"expected={selector['exact_inventory_count']}"
                        )
                    wanted = {item.upper() for item in selector["capabilities"]}
                    selected = [
                        endpoint
                        for endpoint in identity.endpoints
                        if endpoint.available and wanted <= endpoint.capabilities
                    ]
                    if len(selected) != selector["exact_count"]:
                        raise GateError(
                            "endpoint selector expected "
                            f"{selector['exact_count']}, got {len(selected)}"
                        )
                    selected_endpoint = selected[0]
                    target = manifest["target"]
                    if selected_endpoint.endpoint_id != target["endpoint_id"]:
                        raise GateError(
                            "selected endpoint ID does not match manifest target: "
                            f"{selected_endpoint.endpoint_id!r} != {target['endpoint_id']!r}"
                        )
                    if selected_endpoint.name != target["endpoint_name"]:
                        raise GateError(
                            "selected endpoint name does not match manifest target: "
                            f"{selected_endpoint.name!r} != {target['endpoint_name']!r}"
                        )
                    if selected_endpoint.criticality != selector["target_criticality"]:
                        raise GateError(
                            "selected endpoint criticality does not match manifest: "
                            f"{selected_endpoint.criticality!r} != "
                            f"{selector['target_criticality']!r}"
                        )
                    if (
                        motion_definition["minimum"] < selected_endpoint.min_rpm
                        or motion_definition["maximum"] > selected_endpoint.max_rpm
                    ):
                        raise GateError(
                            "manifest RPM range is outside selected endpoint limits: "
                            f"manifest=[{motion_definition['minimum']},"
                            f"{motion_definition['maximum']}] endpoint=["
                            f"{selected_endpoint.min_rpm},{selected_endpoint.max_rpm}]"
                        )
                    if not (
                        selected_endpoint.min_rpm
                        <= motion_value
                        <= selected_endpoint.max_rpm
                    ):
                        raise GateError("resolved motion value is outside endpoint limits")
                    context["endpoint_id"] = selected_endpoint.endpoint_id
                    selected_endpoint_id = selected_endpoint.endpoint_id
                    evidence["selected_endpoint"] = {
                        **selected_endpoint.to_dict(),
                        "manifest_min_rpm": motion_definition["minimum"],
                        "manifest_max_rpm": motion_definition["maximum"],
                        "requested_rpm": motion_value,
                    }

                deadline = started + float(
                    manifest["safety"]["maximum_test_duration_seconds"]
                )
                remaining = deadline - self.clock()
                if remaining <= 0:
                    raise TimeoutError("maximum test duration elapsed before pre-stop")
                pre_stop_errors = self._stop_all(
                    evidence,
                    "pre_stop",
                    timeout=min(remaining, MAX_COMMAND_TIMEOUT_SECONDS),
                )
                if pre_stop_errors:
                    raise CleanupError("pre-run STOP ALL failed")

                for step in manifest["steps"]:
                    active_safety_gate = bool(step.get("safety_gate", False))
                    step_started = self.clock()
                    remaining = deadline - step_started
                    if remaining <= 0:
                        raise TimeoutError("maximum test duration exceeded")
                    if "sleep_seconds" in step:
                        if float(step["sleep_seconds"]) > remaining:
                            raise TimeoutError(
                                "sleep would exceed maximum test duration"
                            )
                        self.sleep(float(step["sleep_seconds"]))
                        if self.clock() > deadline:
                            raise TimeoutError("maximum test duration exceeded")
                        evidence["steps"].append(
                            {
                                "id": step["id"],
                                "kind": "sleep",
                                "elapsed_seconds": round(
                                    self.clock() - step_started, 6
                                ),
                            }
                        )
                        active_safety_gate = False
                        continue
                    try:
                        command = step["command"].format_map(context)
                        expected_response = step["expect_prefix"].format_map(context)
                    except (KeyError, ValueError) as exc:
                        raise ManifestError(
                            f"cannot render step {step['id']}: {exc}"
                        ) from exc
                    if step.get("motion", False):
                        motion_attempted = True
                    response = self._record_command(
                        evidence,
                        f"step:{step['id']}",
                        command,
                        timeout=min(
                            remaining,
                            MAX_COMMAND_TIMEOUT_SECONDS,
                            float(step.get("timeout_seconds", remaining)),
                        ),
                    )
                    verb = command.split(maxsplit=1)[0].upper()
                    if verb in {"SET_ENDPOINT_SPEED", "STOP_ENDPOINT"}:
                        response_matched = bool(response.lines) and (
                            response.lines[-1] == expected_response
                        )
                    else:
                        response_matched = any(
                            line.startswith(expected_response)
                            for line in response.lines
                        )
                    if not response_matched:
                        raise StepError(
                            f"step {step['id']} expected response {expected_response!r}"
                        )
                    assertion = step.get("assert")
                    if assertion is not None:
                        try:
                            observed = _numeric_observation(
                                response, assertion["field"]
                            )
                            required_observed = _assert_required_fields(
                                response,
                                assertion["record"],
                                assertion.get("required_fields", {}),
                                expected_endpoint_id=(
                                    selected_endpoint_id
                                    if assertion["record"].upper()
                                    == "ENDPOINT_OBSERVATION"
                                    else None
                                ),
                            )
                        except AssertionFailure as exc:
                            if step.get("safety_gate", False):
                                raise GateError(
                                    f"step {step['id']} safety gate failed: {exc}"
                                ) from exc
                            raise
                        if "expected_variable" in assertion:
                            expected_value = float(context[assertion["expected_variable"]])
                        else:
                            expected_value = float(assertion["expected_value"])
                        tolerance = float(assertion["tolerance"])
                        passed = abs(observed - expected_value) <= tolerance
                        assertion_evidence = {
                            "step_id": step["id"],
                            "field": assertion["field"],
                            "observed": observed,
                            "expected": expected_value,
                            "tolerance": tolerance,
                            "required_fields": required_observed,
                            "passed": passed,
                        }
                        evidence["assertions"].append(assertion_evidence)
                        if not passed:
                            if step.get("safety_gate", False):
                                raise GateError(
                                    f"step {step['id']} safety gate observed "
                                    f"{observed} outside {expected_value} +/- {tolerance}"
                                )
                            raise AssertionFailure(
                                f"step {step['id']} observed {observed} outside "
                                f"{expected_value} +/- {tolerance}"
                            )
                    evidence["steps"].append(
                        {
                            "id": step["id"],
                            "kind": "command",
                            "command": command,
                            "elapsed_seconds": round(
                                self.clock() - step_started, 6
                            ),
                        }
                    )
                    active_safety_gate = False
            except BaseException as exc:
                if active_safety_gate and not isinstance(
                    exc,
                    (GateError, HilInterrupted, KeyboardInterrupt),
                ):
                    primary_error = GateError(
                        f"pre-motion safety gate failed: {type(exc).__name__}: {exc}"
                    )
                else:
                    primary_error = exc
            finally:
                if opened:
                    final_stop_errors = self._stop_all(evidence, "final_stop")
                    cleanup_errors.extend(final_stop_errors)
                    if (
                        motion_attempted
                        and not final_stop_errors
                        and selected_endpoint_id is not None
                    ):
                        cleanup_errors.extend(
                            self._verify_final_stop(
                                evidence,
                                manifest["cleanup"],
                                selected_endpoint_id,
                            )
                        )
                    try:
                        self.client.close()
                    except BaseException as exc:
                        cleanup_errors.append(exc)
                        evidence["cleanup_errors"].append(_error_payload(exc))

        if cleanup_errors:
            if primary_error is not None:
                evidence["test_error"] = _error_payload(primary_error)
            primary_error = CleanupError("final STOP ALL cleanup was not verified")
            evidence["status"] = "ABORTED_FOR_SAFETY"
            evidence["error"] = _error_payload(primary_error)
        elif primary_error is None:
            evidence["status"] = (
                "PASS" if evidence["assertions"] else "INCONCLUSIVE"
            )
        elif isinstance(
            primary_error,
            (
                HilInterrupted,
                KeyboardInterrupt,
                SafetyConfirmationError,
                GateError,
                CleanupError,
            ),
        ):
            evidence["status"] = "ABORTED_FOR_SAFETY"
            evidence["error"] = _error_payload(primary_error)
        elif isinstance(primary_error, AssertionFailure):
            evidence["status"] = "FAIL"
            evidence["error"] = _error_payload(primary_error)
        else:
            evidence["status"] = "INCONCLUSIVE"
            evidence["error"] = _error_payload(primary_error)
        evidence["elapsed_seconds"] = round(max(0.0, self.clock() - started), 6)
        evidence["completed_at"] = datetime.now(timezone.utc).isoformat()
        evidence.pop("_monotonic_origin", None)
        reservation.finalize(evidence)
        if primary_error is not None:
            raise primary_error
        return evidence


def _parse_assignments(items: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ManifestError(f"--set requires NAME=VALUE, got {item!r}")
        name, value = item.split("=", 1)
        if not IDENTIFIER_RE.fullmatch(name) or name in values:
            raise ManifestError(f"invalid or duplicate variable {name!r}")
        values[name] = value
    return values


def _client_from_args(args: argparse.Namespace) -> SerialGatewayClient:
    try:
        return SerialGatewayClient(
            args.port,
            baudrate=args.baud,
            command_timeout=args.timeout,
        )
    except ValueError as exc:
        raise GateError(str(exc)) from exc


def _expectations(args: argparse.Namespace) -> GateExpectations:
    return GateExpectations(
        git_sha=args.expect_git_sha,
        git_dirty=args.expect_git_dirty == "1",
        profile=args.expect_profile,
        board=args.expect_board,
    )


def cmd_validate(args: argparse.Namespace) -> int:
    for raw_path in args.manifest:
        path = Path(raw_path)
        manifest = load_manifest(path)
        print(f"OK MANIFEST {manifest['id']} {path}")
    return 0


def cmd_identify(args: argparse.Namespace) -> int:
    client = _client_from_args(args)
    with TerminationSignalGuard():
        client.open()
        try:
            identity = identify_gateway(client)
        finally:
            client.close()
    print(json.dumps(identity.to_dict(), indent=2, sort_keys=True))
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path)
    variables = resolve_variables(manifest, _parse_assignments(args.set_values))
    confirmations = {
        name
        for enabled, name in (
            (args.authorize_motion, "motion_authorized"),
            (args.confirm_unloaded, "wheels_unloaded"),
            (args.confirm_cutoff, "power_cutoff_ready"),
        )
        if enabled
    }
    runner = HilRunner(_client_from_args(args))
    result = runner.run(
        manifest,
        expected=_expectations(args),
        variables=variables,
        confirmations=confirmations,
        output_path=Path(args.evidence),
        manifest_path=manifest_path,
        hardware_id=args.hardware_id,
        pcb_revision=args.pcb_revision,
        firmware_artifact_sha256=args.firmware_artifact_sha256,
        firmware_artifact_uri=args.firmware_artifact_uri,
        port=args.port,
        baudrate=args.baud,
    )
    if result["status"] != "PASS":
        print(
            f"ERR HIL_RESULT STATUS:{result['status']} "
            f"EVIDENCE:{Path(args.evidence).expanduser()}",
            file=sys.stderr,
        )
        return 3
    print(f"OK HIL {manifest['id']} EVIDENCE:{Path(args.evidence).expanduser()}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate manifests without I/O")
    validate.add_argument("manifest", nargs="+")
    validate.set_defaults(func=cmd_validate)

    def add_connection_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--port", required=True)
        command.add_argument("--baud", type=int, default=DEFAULT_BAUDRATE)
        command.add_argument("--timeout", type=float, default=1.0)

    identify = subparsers.add_parser("identify", help="read identity/profile/endpoints")
    add_connection_options(identify)
    identify.set_defaults(func=cmd_identify)

    run = subparsers.add_parser("run", help="run one validated HIL manifest")
    add_connection_options(run)
    run.add_argument("manifest")
    run.add_argument("--evidence", required=True)
    run.add_argument("--expect-git-sha", required=True)
    run.add_argument("--expect-git-dirty", required=True, choices=("0", "1"))
    run.add_argument("--expect-profile", required=True)
    run.add_argument("--expect-board", required=True)
    run.add_argument("--hardware-id", required=True)
    run.add_argument("--pcb-revision", required=True)
    run.add_argument("--firmware-artifact-sha256", required=True)
    run.add_argument("--firmware-artifact-uri")
    run.add_argument("--set", dest="set_values", action="append", default=[])
    run.add_argument("--authorize-motion", action="store_true")
    run.add_argument("--confirm-unloaded", action="store_true")
    run.add_argument("--confirm-cutoff", action="store_true")
    run.set_defaults(func=cmd_run)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except HilInterrupted as exc:
        print(f"ERR HIL_INTERRUPTED SIGNAL:{exc.signum}", file=sys.stderr)
        return 128 + exc.signum
    except KeyboardInterrupt:
        print("ERR HIL_INTERRUPTED", file=sys.stderr)
        return 130
    except (HilError, GatewayClientError) as exc:
        print(f"ERR HIL {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
