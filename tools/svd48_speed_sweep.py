#!/usr/bin/env python3
"""Run bounded SVD48 speed steps and capture direct-register telemetry to CSV."""

from __future__ import annotations

import argparse
import csv
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from esp_lanctl import LanCtlError
from svd48_lan import (
    command,
    command_succeeded,
    read_words,
    response_summary,
    signed16,
    signed32,
)


CSV_FIELDS = [
    "time_utc",
    "elapsed_ms",
    "phase",
    "drive_id",
    "channel",
    "logical_motor",
    "requested_rpm",
    "given_rpm",
    "actual_speed_raw_d10",
    "actual_rpm",
    "current_a",
    "position_counts",
    "control_command",
    "run_status",
    "hall_state",
    "hall_angle_deg",
    "error_code",
    "sample_duration_ms",
    "read_errors",
]


def parse_targets(value: str) -> list[int]:
    targets: list[int] = []
    for part in value.split(","):
        try:
            target = int(part.strip(), 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"bad RPM target: {part}") from exc
        if target == 0 or abs(target) > 15:
            raise argparse.ArgumentTypeError("targets must be non-zero and within +/-15 RPM")
        targets.append(target)
    if not targets:
        raise argparse.ArgumentTypeError("at least one target is required")
    return targets


def selected(words: list[int] | None, channel_index: int, signed: bool = False) -> Any:
    if words is None:
        return ""
    value = words[channel_index]
    return signed16(value) if signed else value


def take_sample(
    args: argparse.Namespace,
    phase: str,
    requested_rpm: int,
    started: float,
) -> dict[str, Any]:
    sample_started = time.monotonic()
    errors: list[str] = []
    blocks: dict[str, list[int] | None] = {}
    for name, address, count in (
        ("control", 0x5300, 2),
        ("given_rpm", 0x5304, 2),
        ("status", 0x5400, 2),
        ("actual_rpm", 0x5410, 2),
        ("current", 0x5414, 2),
        ("position", 0x5418, 4),
        ("error", 0x5420, 4),
        ("hall_state", 0x5688, 2),
        ("hall_angle", 0x568C, 2),
    ):
        words, error = read_words(args, args.drive, address, count)
        blocks[name] = words
        if error:
            errors.append(error)

    index = args.motor - 1
    speed_raw = selected(blocks["actual_rpm"], index, signed=True)
    current_raw = selected(blocks["current"], index, signed=True)
    position_words = blocks["position"]
    error_words = blocks["error"]
    position = (
        ""
        if position_words is None
        else signed32(position_words[index * 2], position_words[index * 2 + 1])
    )
    error_code = (
        ""
        if error_words is None
        else f"0x{((error_words[index * 2] << 16) | error_words[index * 2 + 1]):08X}"
    )
    return {
        "time_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_ms": round((time.monotonic() - started) * 1000),
        "phase": phase,
        "drive_id": args.drive,
        "channel": f"M{args.motor}",
        "logical_motor": args.logical_motor,
        "requested_rpm": requested_rpm,
        "given_rpm": selected(blocks["given_rpm"], index, signed=True),
        "actual_speed_raw_d10": speed_raw,
        "actual_rpm": "" if speed_raw == "" else f"{speed_raw / 10.0:.1f}",
        "current_a": "" if current_raw == "" else f"{current_raw / 10.0:.1f}",
        "position_counts": position,
        "control_command": selected(blocks["control"], index),
        "run_status": selected(blocks["status"], index),
        "hall_state": selected(blocks["hall_state"], index),
        "hall_angle_deg": selected(blocks["hall_angle"], index, signed=True),
        "error_code": error_code,
        "sample_duration_ms": round((time.monotonic() - sample_started) * 1000),
        "read_errors": "|".join(errors),
    }


def capture_for(
    writer: csv.DictWriter,
    stream: Any,
    args: argparse.Namespace,
    phase: str,
    requested_rpm: int,
    duration_s: float,
    started: float,
) -> None:
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        writer.writerow(take_sample(args, phase, requested_rpm, started))
        stream.flush()
        time.sleep(max(0.02, args.period_ms / 1000.0))


def require_command(args: argparse.Namespace, text: str) -> None:
    response = command(args, text)
    print(f"{text}: {response_summary(response)}")
    if not command_succeeded(response):
        raise RuntimeError(response_summary(response))


def run(args: argparse.Namespace) -> int:
    expected_logical = (args.drive - 1) * 2 + (args.motor - 1)
    if args.logical_motor != expected_logical:
        raise ValueError(
            f"drive {args.drive} M{args.motor} maps to logical motor {expected_logical} "
            "in the current firmware configuration"
        )

    output = Path(args.csv)
    output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS, lineterminator="\n")
        writer.writeheader()
        require_command(args, f"STOP {args.logical_motor}")
        writer.writerow(take_sample(args, "baseline", 0, started))
        stream.flush()
        try:
            for target in args.targets:
                require_command(args, f"SET_SPEED {args.logical_motor} {target}")
                hold_s = args.hold_s or abs(target) / args.expected_accel_rpm_s + 2.0
                capture_for(writer, stream, args, "accelerate_hold", target, hold_s, started)

                require_command(args, f"SET_SPEED {args.logical_motor} 0")
                decel_s = abs(target) / args.expected_accel_rpm_s + 2.0
                capture_for(writer, stream, args, "decelerate_zero", 0, decel_s, started)
                require_command(args, f"STOP {args.logical_motor}")
        finally:
            try:
                command(args, f"SET_SPEED {args.logical_motor} 0")
            finally:
                stop = command(args, f"STOP {args.logical_motor}")
                print(f"FINAL_STOP: {response_summary(stop)}")
            writer.writerow(take_sample(args, "final", 0, started))
            stream.flush()
    print(f"CSV: {output}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=32321)
    parser.add_argument("--timeout-ms", type=int, default=1200)
    parser.add_argument("--retries", type=int, default=2)
    parser.add_argument("--token", default="")
    parser.add_argument("--drive", type=int, default=2)
    parser.add_argument("--motor", type=int, choices=(1, 2), required=True)
    parser.add_argument("--logical-motor", type=int, choices=range(4), required=True)
    parser.add_argument("--targets", type=parse_targets, default=parse_targets("1,3,5,10,15"))
    parser.add_argument("--period-ms", type=int, default=80)
    parser.add_argument("--hold-s", type=float, default=0.0, help="0 derives hold time from acceleration")
    parser.add_argument("--expected-accel-rpm-s", type=float, default=3.0)
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
        raise SystemExit("Refusing speed movement without --confirm-elevated")
    if args.expected_accel_rpm_s <= 0 or args.hold_s < 0:
        raise SystemExit("acceleration must be positive and hold must be non-negative")
    try:
        return run(args)
    except (LanCtlError, OSError, RuntimeError, ValueError) as exc:
        print(f"ERR {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
