#!/usr/bin/env python3
"""LAN maintenance client for Botfarms ESP32-S3 firmware."""

from __future__ import annotations

import argparse
import csv
import json
import os
import socket
import sys
import time
import uuid
from pathlib import Path
from typing import Any


DEFAULT_PORT = 32321
REQUEST_TYPE = "botfarms_maintenance_request"
RESPONSE_TYPE = "botfarms_maintenance_response"
REPO_ROOT = Path(__file__).resolve().parents[1]
CSV_FIELDS = [
    "time_iso",
    "motor",
    "RPM",
    "CURRENT_DA",
    "STEER_DEG",
    "STATUS",
    "BUS_DV",
    "MOTOR_TEMP_DC",
    "MOS_TEMP_DC",
    "POS",
    "ERROR",
    "ONLINE",
    "STALE",
    "raw_line",
]


class LanCtlError(RuntimeError):
    pass


def env_file_value(name: str) -> str:
    env_path = REPO_ROOT / ".env"
    try:
        lines = env_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return ""

    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() != name:
            continue
        value = value.strip().strip('"').strip("'")
        return value
    return ""


def resolve_token(args: argparse.Namespace) -> str:
    return args.token or os.environ.get("BOTFARMS_MAINT_TOKEN", "") or env_file_value("BOTFARMS_MAINT_TOKEN")


def make_request(token: str, action: str, command: str | None = None, request_id: str | None = None) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "type": REQUEST_TYPE,
        "request_id": request_id or uuid.uuid4().hex,
        "token": token,
        "action": action,
    }
    if command is not None:
        payload["command"] = command
    return payload


def decode_response(data: bytes) -> dict[str, Any]:
    try:
        response = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LanCtlError(f"bad JSON response: {exc}") from exc
    if response.get("type") != RESPONSE_TYPE:
        raise LanCtlError(f"bad response type: {response.get('type')}")
    return response


def transact(args: argparse.Namespace, action: str, command: str | None = None, host: str | None = None) -> tuple[dict[str, Any], tuple[str, int]]:
    target_host = host or args.host
    if not target_host:
        raise LanCtlError("--host is required")

    token = resolve_token(args)
    request = make_request(token, action, command)
    request_bytes = json.dumps(request, separators=(",", ":")).encode("utf-8")
    timeout_s = max(0.05, args.timeout_ms / 1000.0)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        if getattr(args, "broadcast", None):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

        last_error: Exception | None = None
        for _ in range(max(1, args.retries + 1)):
            sock.sendto(request_bytes, (target_host, args.port))
            try:
                while True:
                    data, addr = sock.recvfrom(8192)
                    response = decode_response(data)
                    if response.get("request_id") == request["request_id"]:
                        return response, addr
            except (socket.timeout, LanCtlError) as exc:
                last_error = exc

    raise LanCtlError(f"no response from {target_host}:{args.port}: {last_error}")


def discover(args: argparse.Namespace) -> list[tuple[dict[str, Any], tuple[str, int]]]:
    token = resolve_token(args)
    broadcast_host = args.broadcast or args.host or "255.255.255.255"
    request = make_request(token, "hello")
    request_bytes = json.dumps(request, separators=(",", ":")).encode("utf-8")
    timeout_s = max(0.05, args.timeout_ms / 1000.0)
    responses: list[tuple[dict[str, Any], tuple[str, int]]] = []
    seen: set[tuple[str, int]] = set()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(timeout_s)
        for _ in range(max(1, args.retries + 1)):
            sock.sendto(request_bytes, (broadcast_host, args.port))
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                try:
                    data, addr = sock.recvfrom(8192)
                except socket.timeout:
                    break
                try:
                    response = decode_response(data)
                except LanCtlError:
                    continue
                if response.get("request_id") != request["request_id"] or addr in seen:
                    continue
                seen.add(addr)
                responses.append((response, addr))
    return responses


def response_lines(response: dict[str, Any]) -> list[str]:
    lines = response.get("lines")
    if isinstance(lines, list):
        return [str(line) for line in lines]
    return []


def print_response(response: dict[str, Any], addr: tuple[str, int] | None, as_json: bool) -> int:
    if as_json:
        envelope = {"from": f"{addr[0]}:{addr[1]}" if addr else None, "response": response}
        print(json.dumps(envelope, indent=2, sort_keys=True))
    else:
        if addr:
            print(f"FROM {addr[0]}:{addr[1]}")
        lines = response_lines(response)
        if lines:
            for line in lines:
                print(line)
        else:
            print(f"{response.get('status', 'err').upper()} {response.get('detail', 'UNKNOWN')}")

    if response.get("status") != "ok":
        return 1
    if any(line.startswith("ERR ") for line in response_lines(response)):
        return 2
    return 0


def command_text(parts: list[str]) -> str:
    clean = " ".join(parts).replace("\r", " ").replace("\n", " ").strip()
    if not clean:
        raise LanCtlError("empty command")
    return clean


def parse_motor_line(line: str) -> dict[str, str] | None:
    parts = line.split()
    if len(parts) < 3 or parts[0] != "DATA" or not parts[1].startswith("MOTOR_"):
        return None
    values: dict[str, str] = {
        "motor": parts[1].removeprefix("MOTOR_"),
        "raw_line": line,
    }
    for token in parts[2:]:
        if ":" not in token:
            continue
        key, value = token.split(":", 1)
        values[key] = value
    return values


def cmd_discover(args: argparse.Namespace) -> int:
    responses = discover(args)
    if not responses:
        raise LanCtlError("no devices discovered")
    exit_code = 0
    for response, addr in responses:
        exit_code = max(exit_code, print_response(response, addr, args.json))
    return exit_code


def cmd_status(args: argparse.Namespace) -> int:
    response, addr = transact(args, "status")
    return print_response(response, addr, args.json)


def cmd_command(args: argparse.Namespace) -> int:
    response, addr = transact(args, "command", command_text(args.command))
    return print_response(response, addr, args.json)


def cmd_convenience(command: str):
    def _run(args: argparse.Namespace) -> int:
        response, addr = transact(args, "command", command)
        return print_response(response, addr, args.json)

    return _run


def cmd_motor(args: argparse.Namespace) -> int:
    response, addr = transact(args, "command", f"GET_MOTOR {args.motor}")
    return print_response(response, addr, args.json)


def cmd_watch(args: argparse.Namespace) -> int:
    csv_file = None
    writer = None
    if args.csv:
        csv_file = open(args.csv, "a", newline="", encoding="utf-8")
        writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
        if csv_file.tell() == 0:
            writer.writeheader()

    samples = 0
    try:
        while args.count == 0 or samples < args.count:
            response, addr = transact(args, "command", f"GET_MOTOR {args.motor}")
            code = print_response(response, addr if samples == 0 else None, args.json)
            if writer:
                for line in response_lines(response):
                    parsed = parse_motor_line(line)
                    if not parsed:
                        continue
                    row = {field: parsed.get(field, "") for field in CSV_FIELDS}
                    row["time_iso"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
                    writer.writerow(row)
                csv_file.flush()
            if code != 0:
                return code
            samples += 1
            time.sleep(max(0.05, args.period_ms / 1000.0))
    except KeyboardInterrupt:
        return 130
    finally:
        if csv_file:
            csv_file.close()
    return 0


def add_common(parser: argparse.ArgumentParser, host_required: bool = True) -> None:
    parser.add_argument("--host", required=host_required, help="ESP IPv4 address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port, default {DEFAULT_PORT}")
    parser.add_argument("--timeout-ms", type=int, default=1000, help="response timeout per try")
    parser.add_argument("--retries", type=int, default=2, help="number of retries after the first send")
    parser.add_argument("--token", default="", help="maintenance LAN token; defaults to BOTFARMS_MAINT_TOKEN")
    parser.add_argument("--json", action="store_true", help="print raw JSON response")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Botfarms ESP32-S3 LAN maintenance client")
    sub = parser.add_subparsers(dest="cmd", required=True)

    discover_parser = sub.add_parser("discover", help="discover authenticated ESP devices by UDP broadcast")
    add_common(discover_parser, host_required=False)
    discover_parser.add_argument("--broadcast", default="", help="broadcast address, e.g. 192.168.1.255")
    discover_parser.set_defaults(func=cmd_discover)

    status_parser = sub.add_parser("status", help="read maintenance LAN status")
    add_common(status_parser)
    status_parser.set_defaults(func=cmd_status)

    command_parser = sub.add_parser("command", help="send one LAN-safe ASCII command")
    add_common(command_parser)
    command_parser.add_argument("command", nargs=argparse.REMAINDER)
    command_parser.set_defaults(func=cmd_command)

    for name, command in [
        ("version", "VERSION"),
        ("platform-status", "PLATFORM_STATUS"),
        ("safety-status", "SAFETY_STATUS"),
        ("wifi-status", "WIFI_STATUS"),
        ("ota-status", "OTA_CONFIG"),
        ("stop-all", "STOP ALL"),
        ("poll-once", "POLL_ONCE"),
    ]:
        p = sub.add_parser(name, help=f"send {command}")
        add_common(p)
        p.set_defaults(func=cmd_convenience(command))

    motor_parser = sub.add_parser("motor", help="read GET_MOTOR n")
    add_common(motor_parser)
    motor_parser.add_argument("motor", type=int)
    motor_parser.set_defaults(func=cmd_motor)

    watch_parser = sub.add_parser("watch", help="poll GET_MOTOR n periodically")
    add_common(watch_parser)
    watch_parser.add_argument("--motor", type=int, default=0)
    watch_parser.add_argument("--period-ms", type=int, default=100)
    watch_parser.add_argument("--count", type=int, default=0, help="0 means run until Ctrl-C")
    watch_parser.add_argument("--csv", default="", help="optional CSV output path")
    watch_parser.set_defaults(func=cmd_watch)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except LanCtlError as exc:
        print(f"ERR {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
