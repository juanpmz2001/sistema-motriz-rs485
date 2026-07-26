"""Shared SVD48 register helpers for authenticated maintenance LAN tools."""

from __future__ import annotations

import argparse
import re
from typing import Any

from esp_lanctl import LanCtlError, response_lines, transact


WORD_RE = re.compile(r"R(?P<index>\d+):0x(?P<hex>[0-9A-Fa-f]{4})/(?P<unsigned>\d+)")


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def signed32(high_word: int, low_word: int) -> int:
    value = (high_word << 16) | low_word
    return value - 0x100000000 if value & 0x80000000 else value


def command(args: argparse.Namespace, text: str) -> dict[str, Any]:
    response, _ = transact(args, "command", text)
    return response


def command_succeeded(response: dict[str, Any]) -> bool:
    return response.get("status") == "ok" and not any(
        line.startswith("ERR ") for line in response_lines(response)
    )


def response_summary(response: dict[str, Any]) -> str:
    lines = response_lines(response)
    return lines[0] if lines else str(response.get("detail", "UNKNOWN"))


def read_words(
    args: argparse.Namespace,
    drive: int,
    address: int,
    count: int,
) -> tuple[list[int] | None, str]:
    label = f"0x{address:04X}x{count}"
    try:
        response = command(args, f"READ_REG {drive} 0x{address:04X} {count}")
    except LanCtlError as exc:
        return None, f"{label}:LAN:{exc}"
    if not command_succeeded(response):
        return None, f"{label}:{response.get('detail', 'ERR')}"
    values = [
        int(match.group("unsigned"))
        for line in response_lines(response)
        for match in WORD_RE.finditer(line)
    ]
    if len(values) != count:
        return None, f"{label}:WORDS:{len(values)}"
    return values, ""
