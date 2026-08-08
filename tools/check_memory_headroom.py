#!/usr/bin/env python3
"""Enforce the ESP32-S3 link-time internal-memory headroom floor.

On ESP32-S3, ``iram0_0_seg`` extends into memory that is aliased by
``dram0_0_seg``.  The one-byte remainder shown for the dedicated IRAM bank by
``idf.py size`` is therefore not the application's effective linker margin.
This gate reads the linker map and uses the smaller of the remaining IRAM
linker region and the remaining shared D/IRAM region.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Sequence


MIN_EFFECTIVE_HEADROOM_BYTES = 192 * 1024
SCHEMA_VERSION = 1

_REGION_PATTERN = re.compile(
    r"^\s*(?P<name>iram0_0_seg|dram0_0_seg)\s+"
    r"(?P<origin>0[xX][0-9a-fA-F]+)\s+"
    r"(?P<length>0[xX][0-9a-fA-F]+)(?:\s+.*)?$",
    re.MULTILINE,
)
_SYMBOL_PATTERN = re.compile(
    r"^\s*(?P<value>0[xX][0-9a-fA-F]+)\s+"
    r"(?P<name>_diram_i_start|_iram_end|_heap_low_start)\s*=\s*.+$",
    re.MULTILINE,
)


class MemoryMapError(ValueError):
    """The linker map does not contain one unambiguous memory layout."""


@dataclass(frozen=True)
class MemoryRegion:
    name: str
    origin: int
    length: int

    @property
    def end(self) -> int:
        return self.origin + self.length


@dataclass(frozen=True)
class HeadroomReport:
    iram_region: MemoryRegion
    dram_region: MemoryRegion
    diram_i_start: int
    iram_end: int
    heap_low_start: int
    minimum_effective_headroom_bytes: int = MIN_EFFECTIVE_HEADROOM_BYTES

    @property
    def iram_headroom_bytes(self) -> int:
        return self.iram_region.end - self.iram_end

    @property
    def dram_headroom_bytes(self) -> int:
        return self.dram_region.end - self.heap_low_start

    @property
    def aliased_iram_used_bytes(self) -> int:
        return max(self.iram_end - self.diram_i_start, 0)

    @property
    def aliased_iram_used_end_in_dram(self) -> int:
        return self.dram_region.origin + self.aliased_iram_used_bytes

    @property
    def effective_headroom_bytes(self) -> int:
        return min(self.iram_headroom_bytes, self.dram_headroom_bytes)

    @property
    def passed(self) -> bool:
        return self.effective_headroom_bytes >= self.minimum_effective_headroom_bytes

    def to_dict(
        self, *, profile: str, map_file: str, map_sha256: str
    ) -> dict[str, object]:
        return {
            "schema_version": SCHEMA_VERSION,
            "profile": profile,
            "map_file": map_file,
            "map_sha256": map_sha256,
            "status": "PASS" if self.passed else "FAIL",
            "minimum_effective_headroom_bytes": self.minimum_effective_headroom_bytes,
            "effective_headroom_bytes": self.effective_headroom_bytes,
            "iram": {
                "region_origin": self.iram_region.origin,
                "region_length": self.iram_region.length,
                "region_end": self.iram_region.end,
                "used_end": self.iram_end,
                "headroom_bytes": self.iram_headroom_bytes,
            },
            "dram": {
                "region_origin": self.dram_region.origin,
                "region_length": self.dram_region.length,
                "region_end": self.dram_region.end,
                "used_end": self.heap_low_start,
                "headroom_bytes": self.dram_headroom_bytes,
            },
            "diram_alias": {
                "iram_start": self.diram_i_start,
                "length": self.iram_region.end - self.diram_i_start,
                "iram_used_bytes": self.aliased_iram_used_bytes,
                "iram_used_end_in_dram": self.aliased_iram_used_end_in_dram,
            },
        }


def _require_single(
    values: dict[str, list[int]], name: str, *, source_kind: str
) -> int:
    matches = values.get(name, [])
    if not matches:
        raise MemoryMapError(f"missing {source_kind} {name}")
    if len(matches) != 1:
        raise MemoryMapError(f"duplicate {source_kind} {name}")
    return matches[0]


def _parse_regions(text: str) -> dict[str, MemoryRegion]:
    matches: dict[str, list[MemoryRegion]] = {}
    for match in _REGION_PATTERN.finditer(text):
        name = match.group("name")
        region = MemoryRegion(
            name=name,
            origin=int(match.group("origin"), 16),
            length=int(match.group("length"), 16),
        )
        matches.setdefault(name, []).append(region)

    regions: dict[str, MemoryRegion] = {}
    for name in ("iram0_0_seg", "dram0_0_seg"):
        candidates = matches.get(name, [])
        if not candidates:
            raise MemoryMapError(f"missing memory region {name}")
        if len(candidates) != 1:
            raise MemoryMapError(f"duplicate memory region {name}")
        region = candidates[0]
        if region.length <= 0:
            raise MemoryMapError(f"memory region {name} has non-positive length")
        regions[name] = region
    return regions


def _parse_symbols(text: str) -> dict[str, int]:
    matches: dict[str, list[int]] = {}
    for match in _SYMBOL_PATTERN.finditer(text):
        matches.setdefault(match.group("name"), []).append(
            int(match.group("value"), 16)
        )
    return {
        name: _require_single(matches, name, source_kind="linker symbol")
        for name in ("_diram_i_start", "_iram_end", "_heap_low_start")
    }


def _validate_used_end(region: MemoryRegion, used_end: int, symbol: str) -> None:
    if used_end < region.origin or used_end > region.end:
        raise MemoryMapError(
            f"{symbol} 0x{used_end:x} is outside {region.name} "
            f"[0x{region.origin:x}, 0x{region.end:x}]"
        )


def analyze_memory_map(text: str) -> HeadroomReport:
    """Parse and evaluate one ESP-IDF linker map."""

    regions = _parse_regions(text)
    symbols = _parse_symbols(text)
    iram_region = regions["iram0_0_seg"]
    dram_region = regions["dram0_0_seg"]
    diram_i_start = symbols["_diram_i_start"]
    iram_end = symbols["_iram_end"]
    heap_low_start = symbols["_heap_low_start"]

    _validate_used_end(iram_region, iram_end, "_iram_end")
    _validate_used_end(dram_region, heap_low_start, "_heap_low_start")

    if not iram_region.origin <= diram_i_start < iram_region.end:
        raise MemoryMapError(
            f"_diram_i_start 0x{diram_i_start:x} is outside {iram_region.name} "
            f"[0x{iram_region.origin:x}, 0x{iram_region.end:x})"
        )

    aliased_iram_length = iram_region.end - diram_i_start
    if aliased_iram_length != dram_region.length:
        raise MemoryMapError(
            "D/IRAM alias tail mismatch: "
            f"iram0_0_seg tail is {aliased_iram_length} bytes but "
            f"dram0_0_seg is {dram_region.length} bytes"
        )

    aliased_iram_used_bytes = max(iram_end - diram_i_start, 0)
    aliased_iram_used_end_in_dram = dram_region.origin + aliased_iram_used_bytes
    if heap_low_start < aliased_iram_used_end_in_dram:
        raise MemoryMapError(
            f"_heap_low_start 0x{heap_low_start:x} precedes aliased IRAM "
            f"used end 0x{aliased_iram_used_end_in_dram:x} in dram0_0_seg"
        )

    return HeadroomReport(
        iram_region=iram_region,
        dram_region=dram_region,
        diram_i_start=diram_i_start,
        iram_end=iram_end,
        heap_low_start=heap_low_start,
    )


def _human_summary(
    report: HeadroomReport, *, profile: str, map_sha256: str
) -> str:
    status = "PASS" if report.passed else "FAIL"
    return "\n".join(
        (
            f"Memory headroom gate: {status}",
            f"  profile: {profile}",
            f"  map SHA-256: {map_sha256}",
            "  aliased IRAM use in D/IRAM: "
            f"{report.aliased_iram_used_bytes} bytes "
            f"(ends at 0x{report.aliased_iram_used_end_in_dram:x})",
            f"  IRAM linker-region headroom: {report.iram_headroom_bytes} bytes",
            f"  D/IRAM linker-region headroom: {report.dram_headroom_bytes} bytes",
            f"  effective headroom: {report.effective_headroom_bytes} bytes",
            "  required effective headroom: "
            f"{report.minimum_effective_headroom_bytes} bytes "
            f"({report.minimum_effective_headroom_bytes // 1024} KiB)",
        )
    )


def _write_json(path: Path, document: dict[str, object]) -> None:
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Check effective ESP32-S3 link-time internal-memory headroom."
    )
    parser.add_argument("map_file", type=Path, help="ESP-IDF application linker map")
    parser.add_argument(
        "--profile",
        default="unspecified",
        help="build profile label recorded in the evidence",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        help="optional path for machine-readable evidence",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        if args.json_output is not None:
            args.json_output.unlink(missing_ok=True)
        map_bytes = args.map_file.read_bytes()
        map_sha256 = hashlib.sha256(map_bytes).hexdigest()
        text = map_bytes.decode(encoding="utf-8", errors="replace")
        report = analyze_memory_map(text)
        document = report.to_dict(
            profile=args.profile,
            map_file=args.map_file.name,
            map_sha256=map_sha256,
        )
        if args.json_output is not None:
            _write_json(args.json_output, document)
    except (MemoryMapError, OSError) as exc:
        print(f"Memory headroom gate: ERROR: {exc}", file=sys.stderr)
        return 2

    print(_human_summary(report, profile=args.profile, map_sha256=map_sha256))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
