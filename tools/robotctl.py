#!/usr/bin/env python3
"""Small PC CLI for the ESP32 SVD48 serial gateway."""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - depends on user environment
    serial = None


DEFAULT_BAUD = 115200


def open_serial(args: argparse.Namespace):
    if serial is None:
        raise SystemExit("pyserial is required: python -m pip install pyserial")
    return serial.Serial(args.port, args.baud, timeout=args.timeout)


def send_command(args: argparse.Namespace, command: str, read_lines: int = 1) -> list[str]:
    command_name = command.strip().split(maxsplit=1)[0].upper()
    with open_serial(args) as ser:
        ser.reset_input_buffer()
        ser.write((command.strip() + "\n").encode("ascii"))
        ser.flush()
        lines: list[str] = []
        deadline = time.time() + args.timeout
        while len(lines) < read_lines and time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").strip()
            if text.startswith(">"):
                text = text[1:].strip()
            if not text or text.startswith(">") or text.startswith("OK READY"):
                continue
            if text.startswith("DATA HELP") and command_name != "HELP":
                continue
            lines.append(text)
        return lines


def print_response(lines: list[str]) -> int:
    if not lines:
        print("ERR TIMEOUT")
        return 2
    for line in lines:
        print(line)
    return 1 if any(line.startswith("ERR") for line in lines) else 0


def cmd_raw(args: argparse.Namespace) -> int:
    return print_response(send_command(args, " ".join(args.command)))


def cmd_ping(args: argparse.Namespace) -> int:
    return print_response(send_command(args, "PING"))


def cmd_get_speed(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"GET_SPEED {args.motor}"))


def cmd_get_motor(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"GET_MOTOR {args.motor}"))


def cmd_set_speed(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"SET_SPEED {args.motor} {args.rpm}"))


def cmd_enable(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"ENABLE {args.target}"))


def cmd_stop(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"STOP {args.target}"))


def cmd_clear_fault(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"CLEAR_FAULT {args.target}"))


def cmd_move_vel(args: argparse.Namespace) -> int:
    return print_response(send_command(args, f"MOVE_VEL {args.vx} {args.vy} {args.wz}"))


def cmd_watch(args: argparse.Namespace) -> int:
    if serial is None:
        raise SystemExit("pyserial is required: python -m pip install pyserial")
    with open_serial(args) as ser:
        ser.reset_input_buffer()
        try:
            while True:
                for motor in range(4):
                    ser.write(f"GET_MOTOR {motor}\n".encode("ascii"))
                    ser.flush()
                    deadline = time.time() + args.timeout
                    while time.time() < deadline:
                        raw = ser.readline()
                        if not raw:
                            continue
                        text = raw.decode("utf-8", errors="replace").strip()
                        if text.startswith(">"):
                            text = text[1:].strip()
                        if text.startswith("DATA MOTOR_") or text.startswith("ERR"):
                            print(text)
                            break
                time.sleep(args.period)
        except KeyboardInterrupt:
            return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="CLI for the ESP32 SVD48 robot gateway")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM6 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=1.0)

    sub = parser.add_subparsers(dest="cmd", required=True)

    raw = sub.add_parser("raw")
    raw.add_argument("command", nargs=argparse.REMAINDER)
    raw.set_defaults(func=cmd_raw)

    sub.add_parser("ping").set_defaults(func=cmd_ping)

    get_speed = sub.add_parser("get-speed")
    get_speed.add_argument("motor", type=int)
    get_speed.set_defaults(func=cmd_get_speed)

    get_motor = sub.add_parser("get-motor")
    get_motor.add_argument("motor", type=int)
    get_motor.set_defaults(func=cmd_get_motor)

    set_speed = sub.add_parser("set-speed")
    set_speed.add_argument("motor", type=int)
    set_speed.add_argument("rpm", type=int)
    set_speed.set_defaults(func=cmd_set_speed)

    enable = sub.add_parser("enable")
    enable.add_argument("target", nargs="?", default="ALL")
    enable.set_defaults(func=cmd_enable)

    stop = sub.add_parser("stop")
    stop.add_argument("target", nargs="?", default="ALL")
    stop.set_defaults(func=cmd_stop)

    clear_fault = sub.add_parser("clear-fault")
    clear_fault.add_argument("target", nargs="?", default="ALL")
    clear_fault.set_defaults(func=cmd_clear_fault)

    move_vel = sub.add_parser("move-vel")
    move_vel.add_argument("vx", type=float)
    move_vel.add_argument("vy", type=float)
    move_vel.add_argument("wz", type=float)
    move_vel.set_defaults(func=cmd_move_vel)

    watch = sub.add_parser("watch")
    watch.add_argument("--period", type=float, default=0.2)
    watch.set_defaults(func=cmd_watch)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cmd == "raw" and not args.command:
        raise SystemExit("raw requires a command")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
