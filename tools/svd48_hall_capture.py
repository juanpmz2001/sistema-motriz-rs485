#!/usr/bin/env python3
"""Run one SVD48 Hall calibration and capture timestamped LAN telemetry."""

from __future__ import annotations

import argparse
import csv
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from esp_lanctl import LanCtlError
from svd48_lan import command, command_succeeded, read_words, response_summary, signed16


CSV_FIELDS = [
    "time_utc",
    "elapsed_ms",
    "phase",
    "calibration_motor",
    "m1_actual_speed_raw_d10",
    "m2_actual_speed_raw_d10",
    "m1_actual_rpm",
    "m2_actual_rpm",
    "m1_current_a",
    "m2_current_a",
    "m1_status",
    "m2_status",
    "m1_error",
    "m2_error",
    "m1_online",
    "m2_online",
    "m1_stale",
    "m2_stale",
    "m1_control_command",
    "m2_control_command",
    "m1_given_rpm",
    "m2_given_rpm",
    "m1_hall_calibration_status",
    "m2_hall_calibration_status",
    "m1_hall_state",
    "m2_hall_state",
    "m1_hall_angle_deg",
    "m2_hall_angle_deg",
    "read_errors",
]


def take_sample(
    args: argparse.Namespace,
    drive: int,
    calibration_motor: int,
    phase: str,
    started: float,
) -> dict[str, Any]:
    errors: list[str] = []
    blocks: dict[str, list[int] | None] = {}
    for name, address, count in (
        ("control", 0x5300, 2),
        ("given_rpm", 0x5304, 2),
        ("status", 0x5400, 2),
        ("actual_rpm", 0x5410, 2),
        ("actual_current", 0x5414, 2),
        ("motor_error", 0x5420, 4),
        ("hall_calibration", 0x5684, 2),
        ("hall_state", 0x5688, 2),
        ("hall_angle", 0x568C, 2),
    ):
        values, error = read_words(args, drive, address, count)
        blocks[name] = values
        if error:
            errors.append(error)

    elapsed_ms = round((time.monotonic() - started) * 1000)
    row: dict[str, Any] = {
        "time_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_ms": elapsed_ms,
        "phase": phase,
        "calibration_motor": calibration_motor,
        "read_errors": "|".join(errors),
    }
    for index in range(2):
        prefix = f"m{index + 1}_"
        speed_raw = (
            "" if blocks["actual_rpm"] is None else signed16(blocks["actual_rpm"][index])
        )
        row[prefix + "actual_speed_raw_d10"] = speed_raw
        row[prefix + "actual_rpm"] = "" if speed_raw == "" else f"{speed_raw / 10.0:.1f}"
        row[prefix + "current_a"] = (
            ""
            if blocks["actual_current"] is None
            else f"{signed16(blocks['actual_current'][index]) / 10.0:.1f}"
        )
        row[prefix + "status"] = (
            "" if blocks["status"] is None else signed16(blocks["status"][index])
        )
        error_words = blocks["motor_error"]
        row[prefix + "error"] = (
            ""
            if error_words is None
            else f"0x{((error_words[index * 2] << 16) | error_words[index * 2 + 1]):08X}"
        )
        telemetry_ok = all(
            blocks[name] is not None
            for name in ("status", "actual_rpm", "actual_current", "motor_error")
        )
        row[prefix + "online"] = 1 if telemetry_ok else 0
        row[prefix + "stale"] = 0 if telemetry_ok else 1

    for name, field in (
        ("control", "control_command"),
        ("given_rpm", "given_rpm"),
        ("hall_calibration", "hall_calibration_status"),
        ("hall_state", "hall_state"),
        ("hall_angle", "hall_angle_deg"),
    ):
        values = blocks[name]
        for index in range(2):
            value: Any = "" if values is None else values[index]
            if values is not None and name in ("given_rpm", "hall_angle"):
                value = signed16(value)
            row[f"m{index + 1}_{field}"] = value
    return row


def print_response_summary(label: str, response: dict[str, Any]) -> None:
    print(f"{label}: {response_summary(response)}")


def hall_calibrate(args: argparse.Namespace) -> int:
    output = Path(args.csv)
    output.parent.mkdir(parents=True, exist_ok=True)
    calibration_reg = 0x5600 if args.motor == 1 else 0x5601
    status_field = f"m{args.motor}_hall_calibration_status"
    started = time.monotonic()
    saw_calibrating = False
    terminal_status: int | None = None

    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS, lineterminator="\n")
        writer.writeheader()
        baseline = take_sample(args, args.drive, args.motor, "baseline", started)
        writer.writerow(baseline)
        stream.flush()

        start_response = command(
            args,
            f"WRITE_REG {args.drive} 0x{calibration_reg:04X} 1 CONFIRM",
        )
        print_response_summary("START", start_response)
        if not command_succeeded(start_response):
            return 2

        try:
            deadline = time.monotonic() + args.max_duration_s
            while time.monotonic() < deadline:
                row = take_sample(args, args.drive, args.motor, "calibrating", started)
                writer.writerow(row)
                stream.flush()
                status = row.get(status_field, "")
                if status != "":
                    status = int(status)
                    saw_calibrating = saw_calibrating or status == 1
                    if (saw_calibrating and status != 1) or (
                        not saw_calibrating
                        and row["elapsed_ms"] >= args.minimum_observation_ms
                        and status == 2
                    ):
                        terminal_status = status
                        break
                time.sleep(max(0.05, args.period_ms / 1000.0))
        finally:
            try:
                cancel = command(
                    args,
                    f"WRITE_REG {args.drive} 0x{calibration_reg:04X} 0 CONFIRM",
                )
                print_response_summary("CALIBRATION_STOP", cancel)
            except LanCtlError as exc:
                print(f"CALIBRATION_STOP: LAN error: {exc}")
            try:
                stop = command(args, "STOP ALL")
                print_response_summary("STOP_ALL", stop)
            except LanCtlError as exc:
                print(f"STOP_ALL: LAN error: {exc}")

        final = take_sample(args, args.drive, args.motor, "final", started)
        writer.writerow(final)
        stream.flush()
        if final.get(status_field, "") != "":
            terminal_status = int(final[status_field])

    table_address = 0x5640 if args.motor == 1 else 0x5650
    table, table_error = read_words(args, args.drive, table_address, 8)
    print(f"CSV: {output}")
    print(f"HALL_TABLE_M{args.motor}: {table if table is not None else table_error}")
    print(
        f"RESULT: motor=M{args.motor} saw_calibrating={int(saw_calibrating)} "
        f"terminal_status={terminal_status}"
    )
    return 0 if terminal_status == 0 else 3


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=32321)
    parser.add_argument("--timeout-ms", type=int, default=1200)
    parser.add_argument("--retries", type=int, default=2)
    parser.add_argument("--token", default="")
    parser.add_argument("--drive", type=int, default=2)
    parser.add_argument("--motor", type=int, choices=(1, 2), required=True)
    parser.add_argument("--period-ms", type=int, default=150)
    parser.add_argument("--max-duration-s", type=float, default=20.0)
    parser.add_argument("--minimum-observation-ms", type=int, default=1500)
    parser.add_argument("--csv", required=True)
    parser.add_argument(
        "--confirm-elevated",
        action="store_true",
        help="required acknowledgement that wheels are clear and a physical cutoff operator is present",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if not args.confirm_elevated:
        raise SystemExit("Refusing Hall movement without --confirm-elevated")
    try:
        return hall_calibrate(args)
    except (LanCtlError, OSError, ValueError) as exc:
        print(f"ERR {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
