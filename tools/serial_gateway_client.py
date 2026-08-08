#!/usr/bin/env python3
"""Persistent ASCII gateway client used by the host HIL runner.

The module deliberately imports pyserial only when a real connection is opened so
manifest validation and fake-backed tests have no third-party dependency.
"""

from __future__ import annotations

from dataclasses import dataclass
import importlib
import math
import time
from typing import Any, Callable, Protocol


DEFAULT_BAUDRATE = 115200
MAX_COMMAND_TIMEOUT_SECONDS = 10.0


class GatewayClientError(RuntimeError):
    """Base error for gateway transport and protocol failures."""


class GatewayTimeout(GatewayClientError):
    """The gateway did not complete a response before the monotonic deadline."""

    def __init__(self, command: str, partial_lines: tuple[str, ...] = ()) -> None:
        super().__init__(f"timeout waiting for {command!r}")
        self.command = command
        self.partial_lines = partial_lines


class GatewayProtocolError(GatewayClientError):
    """The serial stream or command violates the ASCII gateway contract."""


class GatewayCommandError(GatewayClientError):
    """The gateway returned an ERR response."""

    def __init__(self, response: "GatewayResponse") -> None:
        detail = next(
            (line for line in response.lines if line.startswith("ERR")),
            "ERR UNKNOWN",
        )
        super().__init__(f"command {response.command!r} failed: {detail}")
        self.response = response


@dataclass(frozen=True)
class GatewayResponse:
    """One synchronous command response on a persistent connection."""

    command: str
    lines: tuple[str, ...]
    elapsed_seconds: float

    @property
    def failed(self) -> bool:
        return any(line.startswith("ERR") for line in self.lines)

    def to_dict(self) -> dict[str, Any]:
        return {
            "command": self.command,
            "lines": list(self.lines),
            "elapsed_seconds": round(self.elapsed_seconds, 6),
        }


class SerialLike(Protocol):
    timeout: float

    def reset_input_buffer(self) -> None: ...

    def write(self, data: bytes) -> int: ...

    def flush(self) -> None: ...

    def readline(self) -> bytes: ...

    def close(self) -> None: ...


SerialFactory = Callable[..., SerialLike]
Clock = Callable[[], float]


def _pyserial_factory(**kwargs: Any) -> SerialLike:
    try:
        serial_module = importlib.import_module("serial")
    except ImportError as exc:  # pragma: no cover - depends on operator machine
        raise GatewayClientError(
            "pyserial is required for identify/run: python -m pip install pyserial"
        ) from exc
    return serial_module.Serial(**kwargs)


def normalize_command(command: str) -> str:
    clean = " ".join(command.strip().split())
    if not clean:
        raise GatewayProtocolError("empty command")
    if "\r" in command or "\n" in command:
        raise GatewayProtocolError("commands must contain exactly one ASCII line")
    try:
        clean.encode("ascii")
    except UnicodeEncodeError as exc:
        raise GatewayProtocolError("commands must be ASCII") from exc
    return clean


class SerialGatewayClient:
    """A single persistent serial connection with monotonic command deadlines."""

    def __init__(
        self,
        port: str,
        *,
        baudrate: int = DEFAULT_BAUDRATE,
        command_timeout: float = 1.0,
        response_idle: float = 0.05,
        poll_interval: float = 0.02,
        max_line_bytes: int = 4096,
        serial_factory: SerialFactory | None = None,
        clock: Clock = time.monotonic,
    ) -> None:
        if not port or "\r" in port or "\n" in port:
            raise ValueError("a non-empty single-line serial port is required")
        if baudrate <= 0:
            raise ValueError("baudrate must be positive")
        if (
            any(
                isinstance(value, bool)
                for value in (command_timeout, response_idle, poll_interval)
            )
            or not all(
                math.isfinite(value)
                for value in (command_timeout, response_idle, poll_interval)
            )
            or command_timeout <= 0
            or command_timeout > MAX_COMMAND_TIMEOUT_SECONDS
            or response_idle <= 0
            or poll_interval <= 0
        ):
            raise ValueError(
                f"timeouts must be finite and command_timeout no greater than "
                f"{MAX_COMMAND_TIMEOUT_SECONDS:g} seconds"
            )
        if max_line_bytes < 64:
            raise ValueError("max_line_bytes is too small")

        self.port = port
        self.baudrate = baudrate
        self.command_timeout = command_timeout
        self.response_idle = response_idle
        self.poll_interval = poll_interval
        self.max_line_bytes = max_line_bytes
        self._serial_factory = serial_factory or _pyserial_factory
        self._clock = clock
        self._serial: SerialLike | None = None

    @property
    def is_open(self) -> bool:
        return self._serial is not None

    def open(self) -> "SerialGatewayClient":
        if self._serial is not None:
            return self
        handle = self._serial_factory(
            port=self.port,
            baudrate=self.baudrate,
            timeout=self.poll_interval,
            write_timeout=self.command_timeout,
        )
        try:
            handle.reset_input_buffer()
        except BaseException:
            handle.close()
            raise
        self._serial = handle
        return self

    def close(self) -> None:
        handle = self._serial
        self._serial = None
        if handle is not None:
            handle.close()

    def __enter__(self) -> "SerialGatewayClient":
        return self.open()

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def execute(
        self,
        command: str,
        *,
        timeout: float | None = None,
    ) -> GatewayResponse:
        handle = self._serial
        if handle is None:
            raise GatewayClientError("serial connection is not open")

        clean_command = normalize_command(command)
        timeout_seconds = self.command_timeout if timeout is None else timeout
        if (
            isinstance(timeout_seconds, bool)
            or not math.isfinite(timeout_seconds)
            or timeout_seconds <= 0
            or timeout_seconds > MAX_COMMAND_TIMEOUT_SECONDS
        ):
            raise ValueError(
                f"command timeout must be between 0 and {MAX_COMMAND_TIMEOUT_SECONDS:g} seconds"
            )

        payload = (clean_command + "\n").encode("ascii")
        started = self._clock()
        written = handle.write(payload)
        if written != len(payload):
            raise GatewayProtocolError(
                f"short serial write for {clean_command!r}: {written}/{len(payload)}"
            )
        handle.flush()

        deadline = started + timeout_seconds
        last_line_at: float | None = None
        lines: list[str] = []
        command_name = clean_command.split(maxsplit=1)[0].upper()

        while True:
            now = self._clock()
            if now >= deadline:
                raise GatewayTimeout(clean_command, tuple(lines))
            try:
                handle.timeout = min(self.poll_interval, max(0.001, deadline - now))
            except (AttributeError, TypeError):
                pass

            raw = handle.readline()
            now = self._clock()
            if raw:
                if len(raw) > self.max_line_bytes:
                    raise GatewayProtocolError("gateway response line exceeds limit")
                try:
                    line = raw.decode("ascii").strip()
                except UnicodeDecodeError as exc:
                    raise GatewayProtocolError("gateway response is not ASCII") from exc
                while line.startswith(">"):
                    line = line[1:].strip()
                if not line or line == clean_command or line.startswith("OK READY"):
                    continue
                if line.startswith("DATA HELP") and command_name != "HELP":
                    continue

                lines.append(line)
                last_line_at = now
                if line.startswith("OK") or line.startswith("ERR"):
                    return GatewayResponse(
                        clean_command,
                        tuple(lines),
                        max(0.0, now - started),
                    )
                continue

            if lines and last_line_at is not None and now - last_line_at >= self.response_idle:
                return GatewayResponse(
                    clean_command,
                    tuple(lines),
                    max(0.0, now - started),
                )

    def execute_checked(
        self,
        command: str,
        *,
        timeout: float | None = None,
    ) -> GatewayResponse:
        response = self.execute(command, timeout=timeout)
        if response.failed:
            raise GatewayCommandError(response)
        return response


__all__ = [
    "DEFAULT_BAUDRATE",
    "GatewayClientError",
    "GatewayCommandError",
    "GatewayProtocolError",
    "GatewayResponse",
    "GatewayTimeout",
    "MAX_COMMAND_TIMEOUT_SECONDS",
    "SerialGatewayClient",
    "normalize_command",
]
