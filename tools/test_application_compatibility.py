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
        cls.handle_command = extract_c_function(cls.gateway, "handle_command")
        cls.get_motor_branch = extract_dispatch_branch(
            cls.handle_command, "GET_MOTOR"
        )
        cls.set_speed_branch = extract_dispatch_branch(
            cls.handle_command, "SET_SPEED"
        )
        cls.handle_stop = extract_c_function(cls.gateway, "handle_stop")
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

    def test_diagnostic_startup_advertises_only_the_restricted_allowlist(self) -> None:
        diagnostic_help = compact(self.print_diagnostic_help)
        self.assertIn("MODE:DIAGNOSTIC_ONLY", diagnostic_help)
        self.assertIn("STOP ALL", diagnostic_help)
        for blocked in ("SET_SPEED", "ENABLE", "WRITE_REG", "SVD48_IDENTIFY"):
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
