#!/usr/bin/env python3
"""Regression tests for the linker-map memory-headroom gate."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest

from check_memory_headroom import (
    MIN_EFFECTIVE_HEADROOM_BYTES,
    MemoryMapError,
    analyze_memory_map,
    main,
)


IRAM_ORIGIN = 0x40374000
DIRAM_I_START = 0x40378000
DRAM_ORIGIN = 0x3FC88000
DRAM_REGION_LENGTH = 0x53700
IRAM_REGION_LENGTH = DIRAM_I_START - IRAM_ORIGIN + DRAM_REGION_LENGTH


def map_text(
    *,
    iram_headroom: int = MIN_EFFECTIVE_HEADROOM_BYTES + 8192,
    dram_headroom: int = MIN_EFFECTIVE_HEADROOM_BYTES + 4096,
) -> str:
    iram_end = IRAM_ORIGIN + IRAM_REGION_LENGTH - iram_headroom
    heap_low_start = DRAM_ORIGIN + DRAM_REGION_LENGTH - dram_headroom
    return (
        "Memory Configuration\n"
        "\n"
        "Name             Origin             Length             Attributes\n"
        f"iram0_0_seg      0x{IRAM_ORIGIN:08x}         "
        f"0x{IRAM_REGION_LENGTH:08x}         xr\n"
        f"dram0_0_seg      0x{DRAM_ORIGIN:08x}         "
        f"0x{DRAM_REGION_LENGTH:08x}         rw\n"
        "\n"
        "Linker script and memory map\n"
        f"                0x{DIRAM_I_START:08x}                        "
        "_diram_i_start = 0x40378000\n"
        f"                0x{iram_end:08x}                        "
        "_iram_end = ABSOLUTE (.)\n"
        f"                0x{heap_low_start:08x}                        "
        "_heap_low_start = ABSOLUTE (.)\n"
    )


def reformat_address(value: int) -> str:
    return f"0x{value:08x}"


class MemoryHeadroomParserTests(unittest.TestCase):
    def test_fixed_floor_is_192_kib(self) -> None:
        self.assertEqual(MIN_EFFECTIVE_HEADROOM_BYTES, 196_608)

    def test_passes_above_floor(self) -> None:
        report = analyze_memory_map(map_text())
        self.assertTrue(report.passed)
        self.assertEqual(
            report.effective_headroom_bytes,
            MIN_EFFECTIVE_HEADROOM_BYTES + 4096,
        )

    def test_passes_at_exact_floor(self) -> None:
        report = analyze_memory_map(
            map_text(
                iram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES,
                dram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES,
            )
        )
        self.assertTrue(report.passed)
        self.assertEqual(
            report.effective_headroom_bytes,
            MIN_EFFECTIVE_HEADROOM_BYTES,
        )

    def test_fails_below_floor_for_valid_alias_layouts(self) -> None:
        cases = (
            {
                "iram_headroom": MIN_EFFECTIVE_HEADROOM_BYTES - 1,
                "dram_headroom": MIN_EFFECTIVE_HEADROOM_BYTES - 1,
            },
            {
                "iram_headroom": MIN_EFFECTIVE_HEADROOM_BYTES + 4096,
                "dram_headroom": MIN_EFFECTIVE_HEADROOM_BYTES - 1,
            },
        )
        for values in cases:
            with self.subTest(values=values):
                report = analyze_memory_map(map_text(**values))
                self.assertFalse(report.passed)
                self.assertEqual(
                    report.effective_headroom_bytes,
                    MIN_EFFECTIVE_HEADROOM_BYTES - 1,
                )

    def test_rejects_missing_required_entry(self) -> None:
        entries = (
            "iram0_0_seg",
            "dram0_0_seg",
            "_diram_i_start",
            "_iram_end",
            "_heap_low_start",
        )
        source = map_text()
        for entry in entries:
            with self.subTest(entry=entry):
                filtered = "\n".join(
                    line for line in source.splitlines() if entry not in line
                )
                with self.assertRaisesRegex(MemoryMapError, "missing"):
                    analyze_memory_map(filtered)

    def test_rejects_duplicate_required_entry(self) -> None:
        duplicate_lines = (
            f"iram0_0_seg 0x{IRAM_ORIGIN:08x} "
            f"0x{IRAM_REGION_LENGTH:08x} xr",
            f"dram0_0_seg 0x{DRAM_ORIGIN:08x} "
            f"0x{DRAM_REGION_LENGTH:08x} rw",
            "0x40378000 _diram_i_start = 0x40378000",
            "0x40380000 _iram_end = ABSOLUTE (.)",
            "0x3fc90000 _heap_low_start = ABSOLUTE (.)",
        )
        for line in duplicate_lines:
            with self.subTest(line=line):
                with self.assertRaisesRegex(MemoryMapError, "duplicate"):
                    analyze_memory_map(map_text() + line + "\n")

    def test_rejects_malformed_required_entry(self) -> None:
        replacements = (
            (f"0x{IRAM_ORIGIN:08x}", "not-a-hex-address"),
            (
                "_diram_i_start = 0x40378000",
                "_diram_i_start without assignment",
            ),
            ("_iram_end = ABSOLUTE (.)", "_iram_end without assignment"),
        )
        for old, new in replacements:
            with self.subTest(new=new):
                with self.assertRaisesRegex(MemoryMapError, "missing"):
                    analyze_memory_map(map_text().replace(old, new, 1))

    def test_rejects_symbol_outside_its_region(self) -> None:
        source = map_text()
        replacements = (
            (
                reformat_address(
                    IRAM_ORIGIN
                    + IRAM_REGION_LENGTH
                    - (MIN_EFFECTIVE_HEADROOM_BYTES + 8192)
                ),
                reformat_address(IRAM_ORIGIN - 1),
            ),
            (
                reformat_address(
                    DRAM_ORIGIN
                    + DRAM_REGION_LENGTH
                    - (MIN_EFFECTIVE_HEADROOM_BYTES + 4096)
                ),
                reformat_address(DRAM_ORIGIN + DRAM_REGION_LENGTH + 1),
            ),
        )
        for old, new in replacements:
            with self.subTest(new=new):
                with self.assertRaisesRegex(MemoryMapError, "outside"):
                    analyze_memory_map(source.replace(old, new, 1))

    def test_rejects_diram_alias_start_outside_iram(self) -> None:
        source = map_text()
        old = (
            f"0x{DIRAM_I_START:08x}                        "
            "_diram_i_start"
        )
        for value in (IRAM_ORIGIN - 1, IRAM_ORIGIN + IRAM_REGION_LENGTH):
            with self.subTest(value=value):
                new = f"0x{value:08x}                        _diram_i_start"
                with self.assertRaisesRegex(MemoryMapError, "outside"):
                    analyze_memory_map(source.replace(old, new, 1))

    def test_rejects_diram_alias_tail_length_mismatch(self) -> None:
        old = f"0x{DRAM_REGION_LENGTH:08x}         rw"
        new = f"0x{DRAM_REGION_LENGTH - 1:08x}         rw"
        with self.assertRaisesRegex(MemoryMapError, "alias tail mismatch"):
            analyze_memory_map(map_text().replace(old, new, 1))

    def test_rejects_heap_before_aliased_iram_used_end(self) -> None:
        with self.assertRaisesRegex(MemoryMapError, "precedes aliased IRAM"):
            analyze_memory_map(
                map_text(
                    iram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES + 4096,
                    dram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES + 8192,
                )
            )


class MemoryHeadroomCliTests(unittest.TestCase):
    def test_cli_writes_human_and_json_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="memory-headroom-") as raw:
            root = Path(raw)
            map_path = root / "firmware.map"
            json_path = root / "memory-headroom.json"
            map_path.write_text(map_text(), encoding="utf-8")
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                result = main(
                    (
                        "--profile",
                        "current_robot",
                        "--json-output",
                        str(json_path),
                        str(map_path),
                    )
                )

            self.assertEqual(result, 0)
            self.assertIn("Memory headroom gate: PASS", stdout.getvalue())
            document = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(document["schema_version"], 1)
            self.assertEqual(document["profile"], "current_robot")
            self.assertEqual(document["map_file"], "firmware.map")
            self.assertRegex(document["map_sha256"], r"^[0-9a-f]{64}$")
            self.assertEqual(document["status"], "PASS")
            self.assertEqual(
                document["minimum_effective_headroom_bytes"],
                MIN_EFFECTIVE_HEADROOM_BYTES,
            )
            self.assertEqual(
                document["diram_alias"]["iram_start"], DIRAM_I_START
            )
            self.assertEqual(
                document["diram_alias"]["length"], DRAM_REGION_LENGTH
            )

            first_hash = document["map_sha256"]
            map_path.write_text(
                map_text() + "ignored map annotation\n", encoding="utf-8"
            )
            changed_json_path = root / "changed-memory-headroom.json"
            with redirect_stdout(io.StringIO()):
                changed_result = main(
                    (
                        "--json-output",
                        str(changed_json_path),
                        str(map_path),
                    )
                )
            self.assertEqual(changed_result, 0)
            changed_document = json.loads(
                changed_json_path.read_text(encoding="utf-8")
            )
            self.assertRegex(changed_document["map_sha256"], r"^[0-9a-f]{64}$")
            self.assertNotEqual(changed_document["map_sha256"], first_hash)

    def test_cli_returns_failure_after_writing_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="memory-headroom-") as raw:
            root = Path(raw)
            map_path = root / "firmware.map"
            json_path = root / "memory-headroom.json"
            map_path.write_text(
                map_text(
                    iram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES - 1,
                    dram_headroom=MIN_EFFECTIVE_HEADROOM_BYTES - 1,
                ),
                encoding="utf-8",
            )
            with redirect_stdout(io.StringIO()):
                result = main(("--json-output", str(json_path), str(map_path)))

            self.assertEqual(result, 1)
            document = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "FAIL")

    def test_cli_reports_parse_errors_without_false_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="memory-headroom-") as raw:
            root = Path(raw)
            map_path = root / "firmware.map"
            json_path = root / "memory-headroom.json"
            map_path.write_text("not a linker map\n", encoding="utf-8")
            json_path.write_text(
                '{"status": "PASS", "stale": true}\n', encoding="utf-8"
            )
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                result = main(("--json-output", str(json_path), str(map_path)))

            self.assertEqual(result, 2)
            self.assertIn("Memory headroom gate: ERROR", stderr.getvalue())
            self.assertFalse(json_path.exists())


if __name__ == "__main__":
    unittest.main()
