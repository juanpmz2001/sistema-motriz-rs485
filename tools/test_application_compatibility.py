#!/usr/bin/env python3
"""Static characterization of the transitional serial application API.

These checks intentionally inspect the existing C handlers instead of reimplementing
the command parser.  They catch source-level compatibility and routing drift, but do
not prove runtime parsing, transport output, scheduling, or hardware behavior.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
GATEWAY_SOURCE = ROOT / "components/serial_gateway/serial_gateway.c"
GATEWAY_HEADER = ROOT / "components/serial_gateway/include/serial_gateway.h"
COMPOSITION_SOURCE = ROOT / "components/robot_composition/robot_composition.c"
ROBOT_CONTROL_SOURCE = ROOT / "components/robot_control/robot_control.c"
PROFILE_SOURCE = ROOT / "components/robot_profile/robot_profile.c"
MAIN_SOURCE = ROOT / "main/main.c"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
MAIN_CMAKE = ROOT / "main/CMakeLists.txt"
VERSION_HEADER = ROOT / "main/app_version.h"
IDENTITY_SCRIPT = ROOT / "cmake/generate_firmware_identity.cmake"
CAPABILITIES_HEADER = (
    ROOT / "components/robot_capabilities/include/robot_capabilities.h"
)
APPLICATION_PORT_HEADER = (
    ROOT / "components/robot_capabilities/include/actuation_application_port.h"
)


def read_source(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def compact(source: str) -> str:
    """Collapse formatting whitespace while preserving C tokens and strings."""

    return re.sub(r"\s+", " ", source).strip()


def closing_brace(source: str, opening_brace: int) -> int:
    """Locate the close of one C block without counting quoted/comment braces."""

    depth = 0
    index = opening_brace
    state = "code"
    escaped = False
    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "line_comment":
            if character == "\n":
                state = "code"
        elif state == "block_comment":
            if character == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif state == "string" and character == '"':
                state = "code"
            elif state == "character" and character == "'":
                state = "code"
        elif character == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif character == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif character == '"':
            state = "string"
        elif character == "'":
            state = "character"
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1

    raise AssertionError(f"unterminated C block at offset {opening_brace}")


def extract_c_function(source: str, function_name: str) -> str:
    """Return one C function definition using a comment/string-aware brace scan."""

    signature = re.compile(
        rf"(?m)^[A-Za-z_][^;{{}}]*\b{re.escape(function_name)}\s*"
        rf"\([^;{{}}]*\)\s*\{{"
    )
    matches = list(signature.finditer(source))
    if len(matches) != 1:
        raise AssertionError(
            f"expected one definition of {function_name}, found {len(matches)}"
        )

    opening_brace = matches[0].end() - 1
    return source[matches[0].start() : closing_brace(source, opening_brace) + 1]


def extract_c_initializer(source: str, symbol_name: str) -> str:
    """Return one aggregate initializer using the same brace-aware scanner."""

    signature = re.compile(
        rf"(?m)^[^;\n]*\b{re.escape(symbol_name)}\b[^;=]*=\s*\{{"
    )
    matches = list(signature.finditer(source))
    if len(matches) != 1:
        raise AssertionError(
            f"expected one initializer for {symbol_name}, found {len(matches)}"
        )

    opening_brace = matches[0].end() - 1
    return source[matches[0].start() : closing_brace(source, opening_brace) + 1]


def extract_dispatch_branch(source: str, command: str) -> str:
    """Return the body of one argv[0] command branch from handle_command."""

    condition = re.compile(
        rf"(?:else\s+)?if\s*\(\s*strcasecmp\s*\(\s*argv\[0\]\s*,\s*"
        rf'"{re.escape(command)}"\s*\)\s*==\s*0\s*\)\s*\{{'
    )
    matches = list(condition.finditer(source))
    if len(matches) != 1:
        raise AssertionError(
            f"expected one dispatch branch for {command}, found {len(matches)}"
        )
    opening_brace = matches[0].end() - 1
    return source[matches[0].start() : closing_brace(source, opening_brace) + 1]


class ApplicationCompatibilityCharacterization(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gateway = read_source(GATEWAY_SOURCE)
        cls.header = read_source(GATEWAY_HEADER)
        cls.composition = read_source(COMPOSITION_SOURCE)
        cls.robot_control = read_source(ROBOT_CONTROL_SOURCE)
        cls.profile = read_source(PROFILE_SOURCE)
        cls.main = read_source(MAIN_SOURCE)
        cls.root_cmake = read_source(ROOT_CMAKE)
        cls.main_cmake = read_source(MAIN_CMAKE)
        cls.version_header = read_source(VERSION_HEADER)
        cls.identity_script = read_source(IDENTITY_SCRIPT)
        cls.capabilities_header = read_source(CAPABILITIES_HEADER)
        cls.application_port_header = read_source(APPLICATION_PORT_HEADER)
        cls.handle_command = extract_c_function(cls.gateway, "handle_command")
        cls.get_motor_branch = extract_dispatch_branch(
            cls.handle_command, "GET_MOTOR"
        )
        cls.set_speed_branch = extract_dispatch_branch(
            cls.handle_command, "SET_SPEED"
        )
        cls.move_vel_branch = extract_dispatch_branch(
            cls.handle_command, "MOVE_VEL"
        )
        cls.handle_stop = extract_c_function(cls.gateway, "handle_stop")
        cls.handle_endpoints = extract_c_function(cls.gateway, "handle_endpoints")
        cls.handle_set_endpoint_speed = extract_c_function(
            cls.gateway, "handle_set_endpoint_speed"
        )
        cls.handle_stop_endpoint = extract_c_function(
            cls.gateway, "handle_stop_endpoint"
        )
        cls.handle_get_endpoint_observation = extract_c_function(
            cls.gateway, "handle_get_endpoint_observation"
        )
        cls.handle_version = extract_c_function(cls.gateway, "handle_version")
        cls.handle_profile_status = extract_c_function(
            cls.gateway, "handle_profile_status"
        )
        cls.application_result_name = extract_c_function(
            cls.gateway, "application_result_name"
        )
        cls.handle_read_reg = extract_c_function(cls.gateway, "handle_read_reg")
        cls.handle_write_reg = extract_c_function(cls.gateway, "handle_write_reg")
        cls.handle_write_regs = extract_c_function(cls.gateway, "handle_write_regs")
        cls.print_motor_full = extract_c_function(cls.gateway, "print_motor_full")
        cls.print_help = extract_c_function(cls.gateway, "print_help")
        cls.print_diagnostic_help = extract_c_function(
            cls.gateway, "print_diagnostic_help"
        )
        cls.handle_diagnostic_command = extract_c_function(
            cls.gateway, "handle_diagnostic_command"
        )
        cls.gateway_rx_task = extract_c_function(cls.gateway, "gateway_rx_task")
        cls.configured_motor_count = extract_c_function(
            cls.gateway, "configured_motor_count"
        )
        cls.parse_motor_arg = extract_c_function(cls.gateway, "parse_motor_arg")
        cls.bench_profile = extract_c_initializer(cls.profile, "SINGLE_MOTOR")
        cls.application_ops = extract_c_initializer(
            cls.composition, "APPLICATION_OPS"
        )
        cls.endpoint_for_legacy_motor = extract_c_function(
            cls.composition, "endpoint_for_legacy_motor"
        )
        cls.application_set_speed = extract_c_function(
            cls.composition, "application_set_speed"
        )
        cls.application_stop_motor = extract_c_function(
            cls.composition, "application_stop_motor"
        )
        cls.application_stop_all = extract_c_function(
            cls.composition, "application_stop_all"
        )
        cls.application_motor_count = extract_c_function(
            cls.composition, "application_motor_count"
        )
        cls.application_endpoint_count = extract_c_function(
            cls.composition, "application_endpoint_count"
        )
        cls.application_endpoint_at = extract_c_function(
            cls.composition, "application_endpoint_at"
        )
        cls.application_set_endpoint_speed = extract_c_function(
            cls.composition, "application_set_endpoint_speed"
        )
        cls.application_stop_endpoint = extract_c_function(
            cls.composition, "application_stop_endpoint"
        )
        cls.application_get_endpoint_velocity_observation = extract_c_function(
            cls.composition, "application_get_endpoint_velocity_observation"
        )
        cls.create_svd48_endpoint = extract_c_function(
            cls.composition, "svd48_factory_create_endpoint"
        )
        cls.robot_control_move_vel = extract_c_function(
            cls.robot_control, "robot_control_move_vel"
        )

    def assert_has_calls(self, source: str, *function_names: str) -> None:
        for function_name in function_names:
            with self.subTest(function=function_name):
                self.assertRegex(source, rf"\b{re.escape(function_name)}\s*\(")

    def assert_has_strings(self, source: str, *strings: str) -> None:
        for value in strings:
            with self.subTest(response=value):
                self.assertIn(f'"{value}\\n"', source)

    def assert_no_calls(self, source: str, *function_names: str) -> None:
        for function_name in function_names:
            with self.subTest(forbidden_function=function_name):
                self.assertNotRegex(source, rf"\b{re.escape(function_name)}\s*\(")

    def test_public_help_and_dispatch_keep_existing_command_spellings(self) -> None:
        help_text = compact(self.print_help)
        for syntax in (
            "GET_MOTOR n",
            "SET_SPEED n rpm",
            "STOP n|ALL",
            "READ_REG drive reg [count]",
            "WRITE_REG drive reg value CONFIRM",
            "WRITE_REGS drive start value [value...] CONFIRM",
        ):
            with self.subTest(syntax=syntax):
                self.assertIn(syntax, help_text)

        dispatch = compact(self.handle_command)
        for command in (
            "GET_MOTOR",
            "SET_SPEED",
            "STOP",
            "READ_REG",
            "WRITE_REG",
            "WRITE_REGS",
        ):
            with self.subTest(command=command):
                self.assertIn(f'strcasecmp(argv[0], "{command}") == 0', dispatch)
        self.assertIn("handle_read_reg(handle, argc, argv)", dispatch)
        self.assertIn("handle_write_reg(handle, argc, argv)", dispatch)
        self.assertIn("handle_write_regs(handle, argc, argv)", dispatch)
        self.assertIn("handle_stop(handle, argc, argv)", dispatch)

    def test_endpoint_commands_are_public_serial_api_and_use_generic_port(self) -> None:
        help_text = compact(self.print_help)
        dispatch = compact(self.handle_command)
        for syntax in (
            "ENDPOINTS",
            "SET_ENDPOINT_SPEED id rpm",
            "STOP_ENDPOINT id",
            "GET_ENDPOINT_OBSERVATION id",
        ):
            with self.subTest(syntax=syntax):
                self.assertIn(syntax, help_text)

        handlers = {
            "ENDPOINTS": "handle_endpoints(handle, argc, argv)",
            "SET_ENDPOINT_SPEED": "handle_set_endpoint_speed(handle, argc, argv)",
            "STOP_ENDPOINT": "handle_stop_endpoint(handle, argc, argv)",
            "GET_ENDPOINT_OBSERVATION": (
                "handle_get_endpoint_observation(handle, argc, argv)"
            ),
        }
        for command, call in handlers.items():
            with self.subTest(command=command):
                self.assertIn(
                    f'strcasecmp(argv[0], "{command}") == 0', dispatch
                )
                self.assertIn(call, dispatch)

        endpoint_list = self.handle_endpoints
        self.assert_has_calls(
            endpoint_list,
            "actuation_application_endpoint_count",
            "actuation_application_endpoint_at",
        )
        self.assert_has_strings(
            endpoint_list,
            "ERR USAGE ENDPOINTS",
            "DATA ENDPOINTS COUNT:%u",
            "ERR ENDPOINT_ENUMERATION_FAILED INDEX:%u",
            "DATA ENDPOINT ID:%u NAME:%s CRITICALITY:%s AVAILABLE:%u CAPABILITIES:0x%08lx VELOCITY_RPM:%u VELOCITY_OBSERVATION:%u STOPPABLE:%u MIN_RPM:%d MAX_RPM:%d",
        )
        self.assertIn("endpoint_criticality_name", endpoint_list)

        set_speed = self.handle_set_endpoint_speed
        self.assert_has_calls(
            set_speed,
            "actuation_application_find_endpoint",
            "actuation_application_set_endpoint_speed_rpm",
        )
        self.assert_has_strings(
            set_speed,
            "ERR USAGE SET_ENDPOINT_SPEED id rpm",
            "ERR BAD_ENDPOINT ID:%u",
            "ERR ENDPOINT_UNAVAILABLE ID:%u",
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:VELOCITY_RPM",
            "ERR ENDPOINT_SPEED_OUT_OF_RANGE ID:%u REQUESTED:%d MIN_RPM:%d MAX_RPM:%d",
            "OK SET_ENDPOINT_SPEED ID:%u RPM_TARGET:%d",
            "ERR SET_ENDPOINT_SPEED_FAILED ID:%u RESULT:%s",
        )
        self.assertIn(
            "rpm < endpoint.min_rpm || rpm > endpoint.max_rpm",
            compact(set_speed),
        )

        stop = self.handle_stop_endpoint
        self.assert_has_calls(
            stop,
            "actuation_application_find_endpoint",
            "actuation_application_stop_endpoint",
        )
        self.assert_has_strings(
            stop,
            "ERR USAGE STOP_ENDPOINT id",
            "ERR ENDPOINT_CAPABILITY_UNSUPPORTED ID:%u CAPABILITY:STOPPABLE",
            "OK STOP_ENDPOINT ID:%u",
            "ERR STOP_ENDPOINT_FAILED ID:%u RESULT:%s",
        )
        stop_compact = compact(stop)
        self.assertIn(
            "(endpoint.capabilities & ROBOT_CAPABILITY_STOPPABLE) == 0U",
            stop_compact,
        )
        self.assertNotIn("endpoint.available", stop_compact)
        self.assertNotIn("ERR ENDPOINT_UNAVAILABLE", stop_compact)

        observation = self.handle_get_endpoint_observation
        self.assert_has_calls(
            observation,
            "actuation_application_find_endpoint",
            "actuation_application_get_endpoint_velocity_observation",
            "endpoint_health_name",
            "velocity_observation_source_name",
        )
        self.assert_has_strings(
            observation,
            "ERR USAGE GET_ENDPOINT_OBSERVATION id",
            "ERR ENDPOINT_OBSERVATION_UNAVAILABLE ID:%u",
            "DATA ENDPOINT_OBSERVATION ID:%u TYPE:VELOCITY_RPM VALID:%u RPM:%d TIMESTAMP_MS:%lu SOURCE:%s ONLINE:%u STALE:%u HEALTH:%s HEALTH_AVAILABLE:%u",
        )

        for source in (endpoint_list, set_speed, stop, observation):
            with self.subTest(handler=source.split("(", 1)[0].strip()):
                self.assertNotIn("svd48", source.lower())
                self.assert_no_calls(
                    source,
                    "robot_control_set_motor_speed",
                    "robot_control_stop_motor",
                    "robot_control_get_motor",
                )

    def test_unknown_application_results_are_not_reported_as_success(self) -> None:
        result_names = compact(self.application_result_name)
        self.assertIn("case ACTUATION_APPLICATION_OK: return \"OK\";", result_names)
        self.assertIn("default: return \"UNKNOWN\";", result_names)

    def test_version_and_profile_report_reproducible_build_identity(self) -> None:
        self.assertIn("GIT_SHA:%s GIT_DIRTY:%u", self.handle_version)
        self.assertIn("BOARD:%s", self.handle_profile_status)

        header = compact(self.header)
        self.assertIn("const char *fw_git_sha", header)
        self.assertIn("bool fw_git_dirty", header)
        self.assertIn("const char *board_name", header)

        main = compact(self.main)
        self.assertGreaterEqual(main.count(".fw_git_sha = FW_GIT_SHA"), 2)
        self.assertGreaterEqual(
            main.count(".fw_git_dirty = FW_GIT_DIRTY != 0"), 2
        )
        self.assertIn("profile->board->id", main)

        self.assertIn("BOTFARMS_FW_GIT_SHA_OVERRIDE", self.root_cmake)
        self.assertIn("BOTFARMS_FW_GIT_DIRTY_OVERRIDE", self.root_cmake)
        identity_script = compact(self.identity_script)
        self.assertIn("rev-parse --verify HEAD", identity_script)
        self.assertIn(
            "status --porcelain --untracked-files=normal",
            identity_script,
        )
        self.assertIn("copy_if_different", identity_script)
        self.assertIn("add_custom_target(botfarms_firmware_identity ALL", self.main_cmake)
        self.assertIn("add_dependencies(${COMPONENT_LIB} botfarms_firmware_identity)", self.main_cmake)
        self.assertIn("botfarms_firmware_identity.h", self.version_header)

    def test_observation_boundary_is_explicitly_velocity_typed(self) -> None:
        capability = compact(self.capabilities_header)
        application = compact(self.application_port_header)
        self.assertIn("robot_velocity_observation_t", capability)
        self.assertIn("robot_velocity_observation_port_t", capability)
        self.assertIn("robot_velocity_observation_source_t", capability)
        self.assertIn("robot_endpoint_read_velocity_observation", capability)
        self.assertIn("get_endpoint_velocity_observation", application)
        self.assertIn(
            "actuation_application_get_endpoint_velocity_observation",
            application,
        )
        self.assertNotIn("robot_endpoint_observation", capability)
        self.assertNotIn("get_endpoint_observation", application)

    def test_motor_indices_are_zero_based_and_bounded_by_configured_count(self) -> None:
        count_source = compact(self.configured_motor_count)
        self.assertIn(
            "actuation_application_legacy_motor_count( handle->config.actuation)",
            count_source,
        )
        self.assertIn("count > 0U ? count", count_source)
        self.assertIn(
            "robot_control_get_motor_count(handle->config.robot)", count_source
        )

        parse_source = compact(self.parse_motor_arg)
        self.assertIn("size_t motor_count = configured_motor_count(handle)", parse_source)
        self.assertIn("parsed < 0", parse_source)
        self.assertIn("(size_t)parsed >= motor_count", parse_source)
        self.assertIn("*value = (uint8_t)parsed", parse_source)

        get_motor_source = compact(self.get_motor_branch)
        set_speed_source = compact(self.set_speed_branch)
        self.assertRegex(
            get_motor_source,
            r'GET_MOTOR"\) == 0.*?parse_motor_arg\(handle, argv\[1\], &motor\)',
        )
        self.assertRegex(
            set_speed_source,
            r'SET_SPEED"\) == 0.*?parse_motor_arg\(handle, argv\[1\], &motor\)',
        )
        self.assertIn("parse_motor_arg(handle, argv[1], &motor)", self.handle_stop)

    def test_set_speed_syntax_results_and_migrated_route(self) -> None:
        source = self.set_speed_branch
        self.assert_has_strings(
            source,
            "ERR USAGE SET_SPEED n rpm",
            "ERR BAD_MOTOR",
            "ERR SET_SPEED_OUT_OF_RANGE REQUESTED:%d MAX_RPM:%.1f",
            "OK MOTOR_%u RPM_TARGET:%d",
            "ERR SET_SPEED_FAILED 0x%x",
        )
        self.assert_has_calls(
            source,
            "actuation_application_legacy_motor_limits_rpm",
            "actuation_application_set_legacy_motor_speed_rpm",
        )
        normalized = compact(source)
        self.assertIn("rpm < min_rpm || rpm > max_rpm", normalized)
        self.assertIn(
            "== ACTUATION_APPLICATION_OK ? ESP_OK : ESP_ERR_INVALID_ARG",
            normalized,
        )
        self.assert_no_calls(source, "robot_control_set_motor_speed")

    def test_stop_n_and_all_keep_results_and_migrated_route(self) -> None:
        source = self.handle_stop
        self.assert_has_strings(
            source,
            "ERR USAGE STOP n|ALL",
            "OK STOP ALL",
            "ERR BAD_MOTOR",
            "OK STOP %u",
            "ERR STOP_FAILED 0x%x",
        )
        self.assertIn('strcasecmp(argv[1], "ALL") == 0', source)
        self.assert_has_calls(
            source,
            "actuation_application_stop_all",
            "actuation_application_stop_legacy_motor",
            "parse_motor_arg",
        )
        self.assertIn(
            "== ACTUATION_APPLICATION_OK ? ESP_OK : ESP_FAIL", compact(source)
        )
        self.assert_no_calls(
            source,
            "robot_control_stop_motor",
            "robot_control_stop_all",
            "svd48_stop_motor",
        )

    def test_get_motor_keeps_legacy_read_facade_and_response_shape(self) -> None:
        dispatch = self.get_motor_branch
        self.assert_has_strings(dispatch, "ERR USAGE GET_MOTOR n")
        self.assert_has_calls(dispatch, "print_motor_full")

        output = self.print_motor_full
        self.assert_has_calls(output, "robot_control_get_motor")
        self.assert_has_strings(output, "ERR BAD_MOTOR")
        for field in (
            "DATA MOTOR_%u",
            "RPM:%d",
            "CURRENT_DA:%d",
            "STEER_DEG:%.1f",
            "ERROR:0x%08lx",
            "ONLINE:%u",
            "STALE:%u",
            "COMM_ERR:%u",
            "EXC_FUNC:0x%02X",
            "EXC_CODE:0x%02X",
        ):
            with self.subTest(field=field):
                self.assertIn(field, output)
        self.assert_no_calls(output, "actuation_application_set_legacy_motor_speed_rpm")

    def test_read_reg_syntax_limits_results_and_legacy_route(self) -> None:
        source = self.handle_read_reg
        self.assert_has_strings(
            source,
            "ERR USAGE READ_REG drive_id reg [count]",
            "ERR READ_REG_FAILED DRIVE:%u REG:0x%04x COUNT:%u ERR:0x%x",
        )
        self.assertIn('"DATA REG DRIVE:%u START:0x%04x COUNT:%u"', source)
        self.assertIn('" R%u:0x%04x/%u"', source)
        normalized = compact(source)
        self.assertIn("uint16_t quantity = 1", normalized)
        self.assertIn("quantity == 0 || quantity > 16", normalized)
        self.assert_has_calls(source, "robot_control_read_svd48_registers")
        self.assert_no_calls(source, "actuation_application_set_legacy_motor_speed_rpm")

    def test_write_reg_requires_confirm_and_preserves_verified_outcomes(self) -> None:
        source = self.handle_write_reg
        normalized = compact(source)
        self.assertIn('strcasecmp(argv[4], "CONFIRM") != 0', normalized)
        self.assert_has_strings(
            source,
            "ERR USAGE WRITE_REG drive_id reg value CONFIRM",
            "ERR WRITE_REG_ACTUATION_BLOCKED REG:0x%04x",
            "ERR WRITE_REG_ROBOT_NOT_STOPPED REASON:%s",
            "ERR WRITE_REG_PRE_READ_FAILED DRIVE:%u REG:0x%04x ERR:0x%x",
            "ERR WRITE_REG_FAILED DRIVE:%u REG:0x%04x VALUE:0x%04x OUTCOME:UNKNOWN ERR:0x%x",
            "ERR WRITE_REG_READBACK_FAILED DRIVE:%u REG:0x%04x OLD:0x%04x VALUE:0x%04x OUTCOME:ACKED_UNVERIFIED ERR:0x%x",
            "ERR WRITE_REG_READBACK_MISMATCH DRIVE:%u REG:0x%04x OLD:0x%04x VALUE:0x%04x READBACK:0x%04x",
            "OK WRITE_REG DRIVE:%u REG:0x%04x OLD:0x%04x/%u VALUE:0x%04x/%u READBACK:0x%04x/%u VERIFIED:1",
        )
        self.assert_has_calls(
            source,
            "svd48_register_is_runtime_actuation",
            "robot_control_is_safe_for_ota",
            "robot_control_read_svd48_registers",
            "robot_control_write_svd48_register",
        )
        self.assertGreaterEqual(
            len(re.findall(r"\brobot_control_read_svd48_registers\s*\(", source)),
            2,
        )
        self.assert_no_calls(source, "actuation_application_set_legacy_motor_speed_rpm")

    def test_write_regs_requires_confirm_and_preserves_verified_outcomes(self) -> None:
        source = self.handle_write_regs
        normalized = compact(source)
        self.assertIn('strcasecmp(argv[argc - 1], "CONFIRM") != 0', normalized)
        self.assertIn(
            "value_count > SVD48_MAINTENANCE_WRITE_MAX_REGISTERS", normalized
        )
        self.assert_has_strings(
            source,
            "ERR USAGE WRITE_REGS drive_id start_reg value [value...] CONFIRM",
            "ERR WRITE_REGS_BAD_VALUE INDEX:%d",
            "ERR WRITE_REGS_BAD_RANGE START:0x%04x COUNT:%u",
            "ERR WRITE_REGS_ACTUATION_BLOCKED START:0x%04x COUNT:%u",
            "ERR WRITE_REGS_ROBOT_NOT_STOPPED REASON:%s",
            "ERR WRITE_REGS_PRE_READ_FAILED DRIVE:%u START:0x%04x COUNT:%u ERR:0x%x",
            "ERR WRITE_REGS_FAILED DRIVE:%u START:0x%04x COUNT:%u OUTCOME:UNKNOWN ERR:0x%x",
            "ERR WRITE_REGS_READBACK_FAILED DRIVE:%u START:0x%04x COUNT:%u OUTCOME:ACKED_UNVERIFIED ERR:0x%x",
            "ERR WRITE_REGS_READBACK_MISMATCH DRIVE:%u START:0x%04x INDEX:%u OLD:0x%04x VALUE:0x%04x READBACK:0x%04x",
        )
        self.assertIn(
            '"OK WRITE_REGS DRIVE:%u START:0x%04x COUNT:%u VERIFIED:1"', source
        )
        self.assert_has_calls(
            source,
            "svd48_write_multiple_range_is_valid",
            "svd48_register_range_has_runtime_actuation",
            "robot_control_is_safe_for_ota",
            "robot_control_read_svd48_registers",
            "robot_control_write_svd48_registers",
        )
        self.assertGreaterEqual(
            len(re.findall(r"\brobot_control_read_svd48_registers\s*\(", source)),
            2,
        )
        self.assert_no_calls(source, "actuation_application_set_legacy_motor_speed_rpm")

    def test_gateway_configuration_exposes_migrated_and_legacy_ports(self) -> None:
        header = compact(self.header)
        self.assertIn("robot_control_handle_t robot", header)
        self.assertIn("actuation_application_port_t *actuation", header)

    def test_bench_profile_exposes_only_legacy_index_zero(self) -> None:
        bench = compact(self.bench_profile)
        self.assertIn('.name = "bench_single_svd48_motor"', bench)
        self.assertIn(".endpoint_count = 1", bench)
        self.assertIn('.endpoints = {{1, "bench_motor", 1, 0,', bench)
        self.assertIn("ROBOT_PROFILE_NO_GEOMETRY", bench)

        endpoint_lookup = compact(self.endpoint_for_legacy_motor)
        self.assertIn("motor < composition->legacy_binding_count", endpoint_lookup)
        self.assertIn("composition->legacy_endpoint_ids[motor]", endpoint_lookup)

        endpoint_creation = compact(self.create_svd48_endpoint)
        self.assertIn(
            "size_t legacy_index = composition->legacy_binding_count++",
            endpoint_creation,
        )
        self.assertIn(
            "composition->legacy_endpoint_ids[legacy_index] = endpoint->id",
            endpoint_creation,
        )
        self.assertIn(
            "return composition ? composition->legacy_binding_count : 0U",
            compact(self.application_motor_count),
        )

    def test_bench_move_vel_is_explicitly_unsupported(self) -> None:
        main = compact(self.main)
        self.assertIn(
            ".motion_kinematics_enabled = profile->application.kind == "
            "ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY",
            main,
        )

        move = compact(self.robot_control_move_vel)
        self.assertIn("!handle->config.motion_kinematics_enabled", move)
        self.assertIn(
            "svd48_get_motor_count(handle->config.svd48) != SVD48_MOTOR_COUNT",
            move,
        )
        self.assertIn("return ESP_ERR_NOT_SUPPORTED", move)
        self.assert_has_calls(self.move_vel_branch, "robot_control_move_vel")
        self.assert_has_strings(
            self.move_vel_branch,
            "ERR USAGE MOVE_VEL vx vy wz",
            "ERR MOVE_VEL_FAILED 0x%x",
        )

    def test_composition_application_ops_route_speed_and_stop(self) -> None:
        self.assert_has_calls(
            self.application_set_speed,
            "endpoint_for_legacy_motor",
            "actuation_coordinator_set_velocity_rpm",
        )
        self.assert_has_calls(
            self.application_stop_motor,
            "endpoint_for_legacy_motor",
            "actuation_coordinator_stop_endpoint",
        )
        self.assert_has_calls(
            self.application_stop_all,
            "actuation_coordinator_stop_all",
        )

        ops = compact(self.application_ops)
        self.assertIn(".set_legacy_motor_speed_rpm = application_set_speed", ops)
        self.assertIn(".stop_legacy_motor = application_stop_motor", ops)
        self.assertIn(".stop_all = application_stop_all", ops)
        self.assertIn(".legacy_motor_count = application_motor_count", ops)

        self.assert_has_calls(
            self.application_endpoint_at,
            "robot_endpoint_registry_at",
            "robot_endpoint_capabilities",
        )
        self.assertIn(
            "info->criticality = endpoint->criticality",
            compact(self.application_endpoint_at),
        )
        self.assert_has_calls(
            self.application_set_endpoint_speed,
            "robot_endpoint_registry_find",
            "actuation_coordinator_set_velocity_rpm",
        )
        self.assert_has_calls(
            self.application_stop_endpoint,
            "robot_endpoint_registry_find",
            "actuation_coordinator_stop_endpoint",
        )
        self.assert_has_calls(
            self.application_get_endpoint_velocity_observation,
            "robot_endpoint_registry_find",
            "robot_endpoint_read_velocity_observation",
        )
        self.assertIn(".endpoint_count = application_endpoint_count", ops)
        self.assertIn(".endpoint_at = application_endpoint_at", ops)
        self.assertIn(
            ".set_endpoint_speed_rpm = application_set_endpoint_speed", ops
        )
        self.assertIn(".stop_endpoint = application_stop_endpoint", ops)
        self.assertIn(
            ".get_endpoint_velocity_observation = "
            "application_get_endpoint_velocity_observation",
            ops,
        )

    def test_diagnostic_startup_advertises_only_the_restricted_allowlist(self) -> None:
        diagnostic_help = compact(self.print_diagnostic_help)
        self.assertIn("MODE:DIAGNOSTIC_ONLY", diagnostic_help)
        self.assertIn("STOP ALL", diagnostic_help)
        for blocked in (
            "SET_SPEED",
            "ENABLE",
            "WRITE_REG",
            "SVD48_IDENTIFY",
            "ENDPOINTS",
            "SET_ENDPOINT_SPEED",
            "STOP_ENDPOINT",
            "GET_ENDPOINT_OBSERVATION",
        ):
            with self.subTest(blocked=blocked):
                self.assertNotIn(blocked, diagnostic_help)

        self.assertIn(
            "print_diagnostic_help(handle)",
            compact(self.handle_diagnostic_command),
        )
        startup = compact(self.gateway_rx_task)
        self.assertIn("if (handle->config.diagnostic_only)", startup)
        self.assertIn("print_diagnostic_help(handle)", startup)
        self.assertIn("else { print_help(handle); }", startup)


if __name__ == "__main__":
    unittest.main()
