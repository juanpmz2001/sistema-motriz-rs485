#!/usr/bin/env python3
"""Read every documented SVD48 register group through maintenance LAN."""

from __future__ import annotations

import argparse
import json
import math
import re
import socket
import struct
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REQUEST_TYPE = "botfarms_maintenance_request"
RESPONSE_TYPE = "botfarms_maintenance_response"
DEFAULT_PORT = 32321
REPO_ROOT = Path(__file__).resolve().parents[1]
VALUE_RE = re.compile(r"R(?P<offset>\d+):0x(?P<hex>[0-9A-Fa-f]{4})/(?P<unsigned>\d+)")


@dataclass(frozen=True)
class ReadTarget:
    group: str
    name: str
    address: int
    count: int = 1
    value_type: str = "u16"
    confidence: str = "manual"


def target(group: str, name: str, address: int, count: int = 1,
           value_type: str = "u16", confidence: str = "manual") -> ReadTarget:
    return ReadTarget(group, name, address, count, value_type, confidence)


def catalog() -> list[ReadTarget]:
    items: list[ReadTarget] = []

    def scalars(group: str, entries: list[tuple[int, str]], confidence: str = "manual") -> None:
        items.extend(target(group, name, address, confidence=confidence) for address, name in entries)

    scalars("throttle", [
        (0x2000, "throttle_dead_zone"), (0x2001, "throttle_curve_mode"),
        (0x2002, "throttle_curve_points"),
        (0x2010, "normal_forward_force"), (0x2011, "normal_reverse_force"),
        (0x2012, "normal_acceleration"), (0x2013, "normal_max_speed"),
        (0x2020, "sport_forward_force"), (0x2021, "sport_reverse_force"),
        (0x2022, "sport_acceleration"), (0x2023, "sport_max_speed"),
        (0x2030, "turbo_forward_force"), (0x2031, "turbo_reverse_force"),
        (0x2032, "turbo_acceleration"), (0x2033, "turbo_max_speed"),
    ])
    items.extend([
        target("throttle", "throttle_curve_00_15", 0x2060, 16, "u16[16]"),
        target("throttle", "throttle_curve_16_31", 0x2070, 16, "u16[16]"),
    ])
    scalars("brake", [
        (0x2080, "brake_dead_zone"), (0x2081, "brake_curve_mode"),
        (0x2082, "brake_curve_points"),
        (0x2090, "normal_brake_force"), (0x2091, "normal_brake_ramp"),
        (0x2092, "normal_brake_speed"),
        (0x20A0, "sport_brake_force"), (0x20A1, "sport_brake_ramp"),
        (0x20A2, "sport_brake_speed"),
        (0x20B0, "turbo_brake_force"), (0x20B1, "turbo_brake_ramp"),
        (0x20B2, "turbo_brake_speed"),
    ])
    items.extend([
        target("brake", "brake_curve_00_15", 0x20E0, 16, "u16[16]"),
        target("brake", "brake_curve_16_31", 0x20F0, 16, "u16[16]"),
    ])
    scalars("remote_vehicle", [
        (0x2100, "remote_mode"), (0x2101, "remote_direction"),
        (0x2102, "throttle_stroke"), (0x2103, "brake_stroke"),
        (0x2130, "vehicle_speed"), (0x2131, "battery_level"),
        (0x2132, "vehicle_power"), (0x2133, "m1_temperature"),
        (0x2134, "m2_temperature"), (0x2135, "drive_temperature"),
    ], "suspect")
    scalars("vehicle", [
        (0x2200, "maximum_acceleration"), (0x2201, "wheel_diameter_mm"),
        (0x2202, "motor_teeth"), (0x2203, "wheel_teeth"),
        (0x2280, "throttle_input_type"), (0x2281, "ppm_minimum"),
        (0x2282, "ppm_center"), (0x2283, "ppm_maximum"),
    ], "observed_or_manual")

    scalars("board", [
        (0x3001, "slave_id"), (0x3002, "software_version"),
        (0x3003, "hardware_version"), (0x3004, "bootloader_version"),
        (0x3005, "product_id"), (0x3006, "rs485_baud_enum"),
        (0x3007, "can_baud_enum"), (0x3008, "control_input_source"),
        (0x3009, "maximum_bus_voltage_dv"), (0x300A, "overload_timeout_ms"),
        (0x300B, "power_on_encoder_calibration"),
        (0x3100, "save_parameters_command"), (0x3180, "heartbeat_or_in_position"),
    ], "manual_or_suspect")
    for index, address in enumerate(range(0x3200, 0x3218, 2)):
        items.append(target("can_upload", f"can_active_packet_{index}", address, 2, "u32"))
    for address in range(0x3300, 0x330F):
        items.append(target("rs232_upload", f"rs232_upload_0x{address:04x}", address,
                            confidence="suspect"))

    for address, name in [
        (0x5000, "m1_lq"), (0x5002, "m2_lq"),
        (0x5008, "m1_ld"), (0x500A, "m2_ld"),
        (0x5010, "m1_rs"), (0x5012, "m2_rs"),
    ]:
        items.append(target("motor_electrical", name, address, 2, "float32_words"))
    scalars("motor_general", [
        (0x5018, "m1_pole_pairs"), (0x5019, "m2_pole_pairs"),
        (0x501C, "m1_max_speed_rpm"), (0x501D, "m2_max_speed_rpm"),
        (0x5020, "m1_max_current_a"), (0x5021, "m2_max_current_a"),
        (0x5024, "m1_kv_tenth_rpm_per_v"), (0x5025, "m2_kv_tenth_rpm_per_v"),
        (0x5028, "m1_rotation_direction"), (0x5029, "m2_rotation_direction"),
        (0x502C, "m1_sensor_type"), (0x502D, "m2_sensor_type"),
    ])
    scalars("motion_config", [
        (0x5100, "m1_control_mode"), (0x5101, "m2_control_mode"),
        (0x5104, "m1_position_mode"), (0x5105, "m2_position_mode"),
        (0x5108, "m1_max_acceleration_rpm_s"), (0x5109, "m2_max_acceleration_rpm_s"),
        (0x510C, "m1_max_deceleration_rpm_s"), (0x510D, "m2_max_deceleration_rpm_s"),
        (0x5110, "m1_s_curve_ms"), (0x5111, "m2_s_curve_ms"),
    ])

    for address, name in [
        (0x5200, "m1_speed_kp"), (0x5202, "m2_speed_kp"),
        (0x5208, "m1_speed_ki"), (0x520A, "m2_speed_ki"),
        (0x5210, "m1_speed_kd"), (0x5212, "m2_speed_kd"),
        (0x5218, "m1_position_kp"), (0x521A, "m2_position_kp"),
        (0x5220, "m1_position_ki"), (0x5222, "m2_position_ki"),
        (0x5228, "m1_position_kd"), (0x522A, "m2_position_kd"),
        (0x5230, "m1_current_loop_gain"), (0x5232, "m2_current_loop_gain"),
        (0x5238, "m1_speed_feed_forward"), (0x523A, "m2_speed_feed_forward"),
    ]:
        items.append(target("pid", name, address, 2, "float32_words"))
    scalars("pid", [(0x5240, "m1_speed_dead_zone"), (0x5241, "m2_speed_dead_zone")])

    scalars("command_state", [
        (0x5300, "m1_control_command"), (0x5301, "m2_control_command"),
        (0x5304, "m1_given_speed_rpm"), (0x5305, "m2_given_speed_rpm"),
        (0x5308, "m1_given_current_da"), (0x5309, "m2_given_current_da"),
    ])
    items.extend([
        target("command_state", "m1_given_position", 0x530C, 2, "i32"),
        target("command_state", "m2_given_position", 0x530E, 2, "i32"),
    ])
    scalars("telemetry", [
        (0x5400, "m1_status"), (0x5401, "m2_status"),
        (0x5404, "m1_motor_temperature_dc"), (0x5405, "m2_motor_temperature_dc"),
        (0x5408, "m1_bus_voltage_dv"), (0x5409, "m2_bus_voltage_dv"),
        (0x540C, "m1_mos_temperature_dc"), (0x540D, "m2_mos_temperature_dc"),
        (0x5410, "m1_actual_speed_rpm"), (0x5411, "m2_actual_speed_rpm"),
        (0x5414, "m1_actual_current_da"), (0x5415, "m2_actual_current_da"),
    ])
    items.extend([
        target("telemetry", "m1_position", 0x5418, 2, "i32"),
        target("telemetry", "m2_position", 0x541A, 2, "i32"),
        target("telemetry", "m1_error", 0x5420, 2, "u32"),
        target("telemetry", "m2_error", 0x5422, 2, "u32"),
    ])

    scalars("encoder", [
        (0x5500, "m1_calibration_command"), (0x5501, "m2_calibration_command"),
        (0x5504, "encoder_lines_or_bits"),
        (0x5508, "m1_installation_direction"), (0x5509, "m2_installation_direction"),
        (0x550C, "m1_encoder_bias_deg"), (0x550D, "m2_encoder_bias_deg"),
        (0x5580, "m1_encoder_temperature_dc"), (0x5581, "m2_encoder_temperature_dc"),
        (0x5584, "m1_encoder_calibration_status"), (0x5585, "m2_encoder_calibration_status"),
    ], "manual_or_suspect")
    scalars("hall", [
        (0x5600, "m1_calibration_command"), (0x5601, "m2_calibration_command"),
        (0x5605, "m2_calibration_current_candidate_a"),
        (0x5609, "m2_calibration_current_candidate_b"),
        (0x5620, "m1_installation_120_or_60"), (0x5621, "m2_installation_120_or_60"),
        (0x5624, "m1_calibration_current"),
    ], "manual_or_suspect")
    items.extend([
        target("hall", "m1_angle_table", 0x5640, 8, "i16[8]"),
        target("hall", "m2_angle_table", 0x5650, 8, "i16[8]"),
    ])
    scalars("hall", [
        (0x5680, "m1_sensor_temperature_dc"), (0x5681, "m2_sensor_temperature_dc"),
        (0x5684, "m1_calibration_status"), (0x5685, "m2_calibration_status"),
        (0x5688, "m1_hall_status"), (0x5689, "m2_hall_status"),
        (0x568C, "m1_current_angle_deg"), (0x568D, "m2_current_angle_deg"),
    ], "observed_or_manual")
    return items


def env_file_value(name: str) -> str:
    try:
        lines = (REPO_ROOT / ".env").read_text(encoding="utf-8").splitlines()
    except OSError:
        return ""
    for raw in lines:
        if "=" not in raw or raw.lstrip().startswith("#"):
            continue
        key, value = raw.split("=", 1)
        if key.strip() == name:
            return value.strip().strip('"').strip("'")
    return ""


def request(host: str, port: int, token_value: str, command: str,
            timeout_s: float, retries: int) -> dict[str, Any]:
    request_id = uuid.uuid4().hex
    payload = json.dumps({
        "type": REQUEST_TYPE,
        "request_id": request_id,
        "token": token_value,
        "action": "command",
        "command": command,
    }, separators=(",", ":")).encode("utf-8")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        for _ in range(retries + 1):
            sock.sendto(payload, (host, port))
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                try:
                    data, source = sock.recvfrom(8192)
                except socket.timeout:
                    break
                try:
                    response = json.loads(data.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if response.get("type") != RESPONSE_TYPE or response.get("request_id") != request_id:
                    continue
                response["source"] = f"{source[0]}:{source[1]}"
                return response
    return {"status": "err", "detail": "LAN_TIMEOUT", "lines": []}


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def words_to_float(words: list[int], swapped: bool) -> float | None:
    if len(words) != 2:
        return None
    ordered = list(reversed(words)) if swapped else words
    value = struct.unpack(">f", struct.pack(">HH", ordered[0], ordered[1]))[0]
    return value if math.isfinite(value) else None


def parse_result(read_target: ReadTarget, response: dict[str, Any], elapsed_ms: int) -> dict[str, Any]:
    lines = [str(line) for line in response.get("lines", [])]
    words = [int(match.group("unsigned")) for line in lines for match in VALUE_RE.finditer(line)]
    result: dict[str, Any] = {
        "group": read_target.group,
        "name": read_target.name,
        "address": read_target.address,
        "address_hex": f"0x{read_target.address:04X}",
        "count": read_target.count,
        "value_type": read_target.value_type,
        "confidence": read_target.confidence,
        "ok": response.get("status") == "ok" and len(words) == read_target.count,
        "status": response.get("status", "err"),
        "detail": response.get("detail", ""),
        "elapsed_ms": elapsed_ms,
        "lines": lines,
        "words_unsigned": words,
        "words_signed": [signed16(value) for value in words],
        "words_hex": [f"0x{value:04X}" for value in words],
    }
    if len(words) == 2:
        result["u32_high_first"] = (words[0] << 16) | words[1]
        result["u32_low_first"] = (words[1] << 16) | words[0]
        result["float32_high_first"] = words_to_float(words, False)
        result["float32_low_first"] = words_to_float(words, True)
    return result


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# SVD48 Read-Only Inventory",
        "",
        f"- Captured: `{report['captured_at']}`",
        f"- ESP: `{report['host']}:{report['port']}`",
        f"- Drive ID: `{report['drive_id']}`",
        f"- Requests: `{report['summary']['requests']}`",
        f"- Successful: `{report['summary']['successful']}`",
        f"- Failed/unsupported: `{report['summary']['failed']}`",
        "- Operation: read-only `READ_REG`; no writes or movement commands.",
        "",
        "For two-word values, both word orders are preserved in the JSON because",
        "the controller's float order has not been physically verified.",
        "",
        "| Group | Name | Address | Type | Result | Values |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for item in report["reads"]:
        if item["ok"]:
            values = " ".join(item["words_hex"])
            result = "OK"
        else:
            values = "<br>".join(item["lines"]) or item["detail"] or "NO_RESPONSE"
            result = "ERR"
        values = values.replace("|", "\\|")
        lines.append(
            f"| {item['group']} | {item['name']} | {item['address_hex']} "
            f"x{item['count']} | {item['value_type']} | {result} | {values} |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--drive", type=int, default=2)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--token", default="")
    parser.add_argument("--timeout-ms", type=int, default=1200)
    parser.add_argument("--retries", type=int, default=1)
    parser.add_argument("--delay-ms", type=int, default=30)
    parser.add_argument("--output-prefix", type=Path, required=True)
    args = parser.parse_args()
    token_value = args.token or env_file_value("BOTFARMS_MAINT_TOKEN")
    if not token_value:
        raise SystemExit("BOTFARMS_MAINT_TOKEN missing")
    if not 1 <= args.drive <= 247:
        raise SystemExit("drive must be 1..247")

    reads: list[dict[str, Any]] = []
    targets = catalog()
    for index, read_target in enumerate(targets, 1):
        command = f"READ_REG {args.drive} 0x{read_target.address:04X} {read_target.count}"
        started = time.monotonic()
        response = request(args.host, args.port, token_value, command,
                           max(0.1, args.timeout_ms / 1000), max(0, args.retries))
        elapsed_ms = round((time.monotonic() - started) * 1000)
        result = parse_result(read_target, response, elapsed_ms)
        reads.append(result)
        status = "OK" if result["ok"] else "ERR"
        print(f"[{index:03d}/{len(targets):03d}] {status} {read_target.name} "
              f"0x{read_target.address:04X} x{read_target.count}")
        time.sleep(max(0, args.delay_ms) / 1000)

    successful = sum(1 for item in reads if item["ok"])
    report = {
        "schema_version": 1,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "host": args.host,
        "port": args.port,
        "drive_id": args.drive,
        "summary": {
            "requests": len(reads),
            "successful": successful,
            "failed": len(reads) - successful,
        },
        "reads": reads,
    }
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    json_path = args.output_prefix.with_suffix(".json")
    md_path = args.output_prefix.with_suffix(".md")
    json_path.write_text(json.dumps(report, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    md_path.write_text(markdown(report), encoding="utf-8")
    print(f"JSON {json_path}")
    print(f"MARKDOWN {md_path}")
    print(f"SUMMARY requests={len(reads)} successful={successful} failed={len(reads) - successful}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
