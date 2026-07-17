#!/usr/bin/env python3
"""Send an authenticated LAN OTA announce packet to the ESP32."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time


DEFAULT_TARGET = "255.255.255.255"
DEFAULT_ANNOUNCE_PORT = 32320
DEFAULT_SERVER_PORT = 8080
DEFAULT_MANIFEST_PATH = "/api/firmware/latest"
ANNOUNCE_TYPE = "botfarms_ota_offer"


def build_payload(args: argparse.Namespace) -> bytes:
    token = args.token or os.environ.get("BOTFARMS_OTA_TOKEN")
    if not token:
        raise SystemExit("Provide --token or set BOTFARMS_OTA_TOKEN")
    if args.server_port <= 0 or args.server_port > 65535:
        raise SystemExit("--server-port must be in range 1..65535")
    if args.announce_port <= 0 or args.announce_port > 65535:
        raise SystemExit("--announce-port must be in range 1..65535")
    if not args.manifest.startswith("/"):
        raise SystemExit("--manifest must start with '/'")

    payload = {
        "type": ANNOUNCE_TYPE,
        "token": token,
        "port": args.server_port,
        "manifest": args.manifest,
        "action": args.action,
    }
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def send_announce(args: argparse.Namespace, payload: bytes) -> int:
    deadline = time.time() + args.timeout
    responses: list[tuple[str, int, str]] = []

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(min(args.timeout, 1.0))

        for attempt in range(args.count):
            sock.sendto(payload, (args.target, args.announce_port))
            print(f"sent attempt={attempt + 1} target={args.target}:{args.announce_port}")

            wait_until = min(deadline, time.time() + args.interval)
            while time.time() < wait_until:
                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                text = data.decode("utf-8", errors="replace")
                responses.append((addr[0], addr[1], text))
                print(f"response from {addr[0]}:{addr[1]} {text}")

            if responses and args.stop_after_first_response:
                break
            if attempt + 1 < args.count:
                time.sleep(args.interval)

    if not responses:
        print("no responses")
        return 2

    for _, _, text in responses:
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            continue
        if parsed.get("status") == "ok":
            return 0
    return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Announce a local OTA release to ESP32 devices on the LAN.")
    parser.add_argument("--target", default=DEFAULT_TARGET, help="ESP IP or broadcast address")
    parser.add_argument("--announce-port", type=int, default=DEFAULT_ANNOUNCE_PORT)
    parser.add_argument("--server-port", type=int, default=DEFAULT_SERVER_PORT)
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST_PATH)
    parser.add_argument("--token", help="OTA announce token; otherwise BOTFARMS_OTA_TOKEN")
    parser.add_argument(
        "--action",
        choices=["config", "check", "download_test", "update"],
        default="check",
        help="update reboots only if the robot is safe for OTA",
    )
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--stop-after-first-response", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.count <= 0:
        raise SystemExit("--count must be > 0")
    if args.timeout <= 0:
        raise SystemExit("--timeout must be > 0")
    if args.interval <= 0:
        raise SystemExit("--interval must be > 0")
    payload = build_payload(args)
    return send_announce(args, payload)


if __name__ == "__main__":
    sys.exit(main())
