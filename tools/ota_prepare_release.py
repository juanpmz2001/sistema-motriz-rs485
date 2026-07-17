#!/usr/bin/env python3
"""Prepare an OTA release directory and manifest for the ESP32 firmware."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_VERSION_H = REPO_ROOT / "main" / "app_version.h"
DEFAULT_BINARY = REPO_ROOT / "build" / "sistema-motriz-rs485.bin"
DEFAULT_OUTPUT = REPO_ROOT / "ota_release"
DEFAULT_MANIFEST_PATH = "/api/firmware/latest"
SOURCE_PATTERNS = (
    "CMakeLists.txt",
    "main/CMakeLists.txt",
    "main/**/*.c",
    "main/**/*.h",
    "components/**/CMakeLists.txt",
    "components/**/*.c",
    "components/**/*.h",
    "sdkconfig",
    "sdkconfig.defaults",
    "partitions*.csv",
)


def parse_app_version(path: Path) -> dict[str, str | int]:
    text = path.read_text(encoding="utf-8")
    values: dict[str, str | int] = {}
    string_keys = ("FW_PROJECT", "FW_TARGET", "FW_VERSION")
    for key in string_keys:
        match = re.search(rf'^\s*#define\s+{key}\s+"([^"]+)"\s*$', text, re.MULTILINE)
        if not match:
            raise SystemExit(f"Missing {key} in {path}")
        values[key] = match.group(1)

    match = re.search(r"^\s*#define\s+FW_BUILD_NUMBER\s+(\d+)\s*$", text, re.MULTILINE)
    if not match:
        raise SystemExit(f"Missing FW_BUILD_NUMBER in {path}")
    values["FW_BUILD_NUMBER"] = int(match.group(1))
    return values


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def latest_source_file() -> Path | None:
    latest: Path | None = None
    latest_mtime = -1.0
    for pattern in SOURCE_PATTERNS:
        for path in REPO_ROOT.glob(pattern):
            if not path.is_file():
                continue
            mtime = path.stat().st_mtime
            if mtime > latest_mtime:
                latest = path
                latest_mtime = mtime
    return latest


def relative_to_repo(path: Path) -> Path:
    try:
        return path.relative_to(REPO_ROOT)
    except ValueError:
        return path


def validate_binary_is_fresh(binary: Path) -> None:
    latest = latest_source_file()
    if latest is None:
        return
    if binary.stat().st_mtime + 1.0 < latest.stat().st_mtime:
        raise SystemExit(
            f"Firmware binary is older than source file {relative_to_repo(latest)}.\n"
            "Run: idf.py build\n"
            f"Then rerun this script. Stale binary: {relative_to_repo(binary)}"
        )


def validate_host(host: str) -> str:
    host = host.strip()
    forbidden = {"localhost", "127.0.0.1", "0.0.0.0", "::1", "[::1]"}
    if not host or host.lower() in forbidden or "://" in host or "/" in host:
        raise SystemExit(
            "Use a LAN IP or DNS name reachable by the ESP32, not localhost/127.0.0.1/0.0.0.0"
        )
    return host


def validate_manifest_path(path: str) -> str:
    if not path.startswith("/"):
        raise SystemExit("Manifest path must start with '/', for example /api/firmware/latest")
    if path.endswith("/"):
        raise SystemExit("Manifest path must point to a file, not end with '/'")
    return path


def validate_args(args: argparse.Namespace) -> None:
    if args.port <= 0 or args.port > 65535:
        raise SystemExit("Port must be in range 1..65535")
    if args.min_supported_build < 0:
        raise SystemExit("min-supported-build must be >= 0")


def build_manifest(args: argparse.Namespace) -> tuple[dict[str, str | int], Path, str]:
    binary = args.binary.resolve()
    if not binary.exists():
        raise SystemExit(f"Firmware binary not found: {binary}\nRun: idf.py build")
    if binary.stat().st_size == 0:
        raise SystemExit(f"Firmware binary is empty: {binary}")
    validate_binary_is_fresh(binary)

    version = parse_app_version(APP_VERSION_H)
    project = str(version["FW_PROJECT"])
    target = str(version["FW_TARGET"])
    fw_version = str(version["FW_VERSION"])
    build_number = int(version["FW_BUILD_NUMBER"])
    if target != "esp32s3":
        raise SystemExit(f"Refusing to prepare OTA for target {target!r}; expected 'esp32s3'")
    filename = f"{project}-v{fw_version}-b{build_number}.bin"
    host = validate_host(args.host)
    manifest_path = validate_manifest_path(args.manifest_path)
    sha256 = sha256_file(binary)
    size = binary.stat().st_size
    url = f"http://{host}:{args.port}/firmware/{filename}"

    manifest: dict[str, str | int] = {
        "project": project,
        "target": target,
        "version": fw_version,
        "build_number": build_number,
        "min_supported_build": args.min_supported_build,
        "url": url,
        "filename": filename,
        "size": size,
        "sha256": sha256,
    }
    return manifest, binary, manifest_path


def write_release(output: Path, binary: Path, manifest_path: str, manifest: dict[str, str | int]) -> None:
    filename = str(manifest["filename"])
    firmware_dir = output / "firmware"
    manifest_file = output / manifest_path.lstrip("/")
    output_binary = firmware_dir / filename
    firmware_dir.mkdir(parents=True, exist_ok=True)
    manifest_file.parent.mkdir(parents=True, exist_ok=True)
    if output_binary.exists() and sha256_file(output_binary) != str(manifest["sha256"]):
        raise SystemExit(
            f"Refusing to overwrite {output_binary} with different content for the same build number.\n"
            "Increment FW_BUILD_NUMBER in main/app_version.h, run idf.py build, then rerun this script."
        )
    shutil.copy2(binary, output_binary)
    manifest_file.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def print_next_steps(args: argparse.Namespace, manifest_path: str, manifest: dict[str, str | int]) -> None:
    print("OTA release prepared")
    print(f"  project: {manifest['project']}")
    print(f"  target: {manifest['target']}")
    print(f"  version: {manifest['version']}")
    print(f"  build_number: {manifest['build_number']}")
    print(f"  size: {manifest['size']}")
    print(f"  sha256: {manifest['sha256']}")
    print(f"  manifest: {args.output / manifest_path.lstrip('/')}")
    print(f"  binary: {args.output / 'firmware' / str(manifest['filename'])}")
    print()
    print("Serve it from another terminal:")
    print(f"  python3 -m http.server {args.port} --bind 0.0.0.0 --directory {args.output}")
    print()
    print("Configure the ESP32 serial gateway:")
    print(f"  OTA_SET_SERVER {args.host} {args.port}")
    print(f"  OTA_SET_MANIFEST {manifest_path}")
    print("  OTA_ANNOUNCE_TOKEN_SET <token>")
    print("  WIFI_CONNECT")
    print("  OTA_CHECK")
    print("  OTA_DOWNLOAD_TEST")
    print("  STOP ALL")
    print("  PLATFORM_STATUS")
    print("  OTA_UPDATE")
    print()
    print("Or announce it over LAN after the ESP has Wi-Fi and an announce token:")
    print(f"  BOTFARMS_OTA_TOKEN=<token> python3 tools/ota_announce.py --server-port {args.port} --manifest {manifest_path} --action check")
    print(f"  BOTFARMS_OTA_TOKEN=<token> python3 tools/ota_announce.py --server-port {args.port} --manifest {manifest_path} --action update")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Generate OTA manifest and release files.")
    parser.add_argument("--host", required=True, help="LAN IP/DNS reachable by the ESP32; never localhost")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--manifest-path", default=DEFAULT_MANIFEST_PATH)
    parser.add_argument("--min-supported-build", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true", help="Print manifest without writing release files")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    validate_args(args)
    args.output = args.output.resolve()
    manifest, binary, manifest_path = build_manifest(args)

    if args.dry_run:
        print(json.dumps(manifest, indent=2, sort_keys=True))
        return 0

    write_release(args.output, binary, manifest_path, manifest)
    print_next_steps(args, manifest_path, manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
