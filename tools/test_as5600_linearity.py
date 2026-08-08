#!/usr/bin/env python3
"""Regression tests for the offline bidirectional AS5600 LUT analyzer."""

from __future__ import annotations

import csv
from contextlib import redirect_stdout
import hashlib
import io
import json
import math
from pathlib import Path
import re
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze_as5600_linearity as analyzer


FIELDS = (
    "time_ms",
    "raw_angle",
    "unwrapped_deg",
    "phase",
    "i2c_valid",
    "valid",
    "glitch",
)
CAPTURE_COORDINATE_OFFSET = 188.26171875
CALIBRATION_ID = "synthetic-as5600-linearity-v1"
HARDWARE_IDENTITY = "synthetic_sensor_magnet_shaft_fixture_v1"


def coherent_error(angle_degrees: float) -> float:
    radians = math.radians(angle_degrees)
    return (
        1.70 * math.sin(radians + 0.23)
        + 0.55 * math.sin(3.0 * radians - 0.48)
        + 0.20 * math.cos(5.0 * radians + 0.19)
    )


def quantized_unwrapped(observed_continuous: float) -> tuple[int, float]:
    raw = int(round((observed_continuous % 360.0) / 360.0 * analyzer.RAW_COUNTS))
    raw %= analyzer.RAW_COUNTS
    raw_degrees = raw * analyzer.RAW_STEP_DEGREES
    turns = round((observed_continuous - raw_degrees) / 360.0)
    return raw, raw_degrees + turns * 360.0


def synthetic_rows(
    *,
    turns: int = analyzer.REQUIRED_COMPLETE_TURNS,
    negative_error=coherent_error,
    positive_error=coherent_error,
) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    elapsed_ms = 0.0
    points_per_turn = 96
    # The historical capture's unwrapped value is relative to a capture origin,
    # not to AS5600 raw count zero.  Keep that distinction in this fixture.
    for phase, sign, error in (
        ("LEFT_2000_US", -1, negative_error),
        ("RIGHT_1040_US", 1, positive_error),
    ):
        start_degrees = 37.0
        for index in range(turns * points_per_turn + 1):
            true_degrees = start_degrees + sign * index * 360.0 / points_per_turn
            raw, physical_unwrapped = quantized_unwrapped(
                true_degrees + error(true_degrees)
            )
            unwrapped = physical_unwrapped - CAPTURE_COORDINATE_OFFSET
            rows.append(
                {
                    "time_ms": f"{elapsed_ms:.4f}",
                    "raw_angle": str(raw),
                    "unwrapped_deg": f"{unwrapped:.8f}",
                    "phase": phase,
                    "i2c_valid": "1",
                    "valid": "1",
                    "glitch": "0",
                }
            )
            elapsed_ms += 40.0 + 0.15 * math.sin(index * 0.37)
        elapsed_ms += 1000.0
    return rows


def write_capture(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_candidate(input_path: Path, output_path: Path) -> dict[str, object]:
    return analyzer.analyze_file(
        input_path,
        output_path,
        CALIBRATION_ID,
        HARDWARE_IDENTITY,
    )


def shifted_capture_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Make a separately hashed but physically equivalent synthetic capture."""

    shifted = [dict(row) for row in rows]
    for row in shifted:
        row["time_ms"] = f"{float(row['time_ms']) + 500.0:.4f}"
    return shifted


def inject_large_turn_speed_variation(rows: list[dict[str, str]]) -> None:
    """Keep raw samples valid while making one directional pass non-constant."""

    positive_indexes = [
        index for index, row in enumerate(rows) if row["phase"] == "RIGHT_1040_US"
    ]
    points_per_turn = 96
    turn_durations_ms = (4000.0, 4000.0, 4000.0, 8000.0, 8000.0, 8000.0, 8000.0)
    for relative_index, row_index in enumerate(positive_indexes):
        turn = min(relative_index // points_per_turn, analyzer.REQUIRED_COMPLETE_TURNS - 1)
        fraction = (relative_index - turn * points_per_turn) / points_per_turn
        elapsed = sum(turn_durations_ms[:turn]) + fraction * turn_durations_ms[turn]
        rows[row_index]["time_ms"] = f"{100000.0 + elapsed:.4f}"


class As5600LinearityTests(unittest.TestCase):
    def test_fits_deterministic_bidirectional_capture_with_provenance(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            root = Path(directory)
            capture = root / "synthetic.csv"
            output = root / "candidate.json"
            repeat_output = root / "candidate-repeat.json"
            rows = synthetic_rows()
            # The historical source CSV has setup/neutral rows before the two
            # actual calibration passes; they must not become a third pass.
            rows.insert(
                0,
                {
                    "time_ms": "-40.0",
                    "raw_angle": "0",
                    "unwrapped_deg": "0.0",
                    "phase": "NEUTRAL_START",
                    "i2c_valid": "1",
                    "valid": "1",
                    "glitch": "0",
                },
            )
            write_capture(capture, rows)

            with redirect_stdout(io.StringIO()):
                self.assertEqual(
                    analyzer.main(
                        (
                            str(capture),
                            "--out",
                            str(output),
                            "--calibration-id",
                            CALIBRATION_ID,
                            "--hardware-identity",
                            HARDWARE_IDENTITY,
                        )
                    ),
                    0,
                )
            write_candidate(capture, repeat_output)
            stored = json.loads(output.read_text(encoding="utf-8"))

            self.assertEqual(output.read_text(encoding="utf-8"), repeat_output.read_text(encoding="utf-8"))
            self.assertEqual(stored["kind"], "as5600_cyclic_linearity_calibration_candidate")
            self.assertEqual(stored["schema_version"], analyzer.CALIBRATION_REPORT_SCHEMA_VERSION)
            self.assertEqual(stored["calibration"]["id"], CALIBRATION_ID)
            self.assertEqual(
                stored["calibration"]["hardware_identity"], HARDWARE_IDENTITY
            )
            self.assertEqual(len(stored["lut_centideg"]), analyzer.LUT_NODES)
            self.assertTrue(all(isinstance(value, int) for value in stored["lut_centideg"]))
            self.assertTrue(stored["mapping_validation"]["strictly_monotonic"])
            self.assertEqual(stored["mapping_validation"]["invalid_increment_count"], 0)
            self.assertEqual(stored["correction_scale"], 1.0)
            self.assertGreater(stored["quality"]["direction_curve_correlation"], 0.95)
            self.assertEqual(
                stored["provenance"]["input_sha256"],
                hashlib.sha256(capture.read_bytes()).hexdigest(),
            )
            self.assertEqual(stored["provenance"]["input_file_name"], capture.name)
            self.assertEqual(stored["provenance"]["ignored_rows"], 1)
            self.assertRegex(
                stored["provenance"]["source_experiment_commit"], r"^[0-9a-f]{40}$"
            )
            self.assertEqual(stored["passes"]["negative"]["used_turns"], 7)
            self.assertEqual(stored["passes"]["positive"]["used_turns"], 7)
            self.assertLess(
                stored["quality"]["negative_after"]["rms_degrees"],
                stored["quality"]["negative_before"]["rms_degrees"],
            )
            self.assertLess(
                stored["quality"]["positive_after"]["rms_degrees"],
                stored["quality"]["positive_before"]["rms_degrees"],
            )
            serialized = output.read_text(encoding="utf-8")
            self.assertNotIn("raw_angle", serialized)
            self.assertNotIn("unwrapped_deg", serialized)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(
                    analyzer.main(("--validate-candidate-report", str(output))), 0
                )

    def test_rejects_capture_with_fewer_than_seven_complete_turns(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            capture = Path(directory) / "incomplete.csv"
            write_capture(capture, synthetic_rows(turns=6))
            with self.assertRaisesRegex(analyzer.CalibrationError, "complete turns"):
                analyzer.analyze_capture(capture)

    def test_rejects_unflagged_non_monotonic_pass(self) -> None:
        rows = synthetic_rows()
        positive_start = next(
            index
            for index, row in enumerate(rows)
            if row["phase"] == "RIGHT_1040_US"
        )
        index = positive_start + 20
        previous = float(rows[index - 1]["unwrapped_deg"])
        corrupted = previous - analyzer.MAX_BACKTRACK_DEGREES - 10.0
        raw, physical_unwrapped = quantized_unwrapped(
            corrupted + CAPTURE_COORDINATE_OFFSET
        )
        unwrapped = physical_unwrapped - CAPTURE_COORDINATE_OFFSET
        rows[index]["raw_angle"] = str(raw)
        rows[index]["unwrapped_deg"] = f"{unwrapped:.8f}"
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            capture = Path(directory) / "non-monotonic.csv"
            write_capture(capture, rows)
            with self.assertRaisesRegex(analyzer.CalibrationError, "not monotonic"):
                analyzer.analyze_capture(capture)

    def test_rejects_incoherent_bidirectional_curves(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            capture = Path(directory) / "incoherent.csv"
            write_capture(
                capture,
                synthetic_rows(positive_error=lambda angle: -coherent_error(angle)),
            )
            with self.assertRaisesRegex(analyzer.CalibrationError, "incoherent"):
                analyzer.analyze_capture(capture)

    def test_validation_rejects_a_non_monotonic_lut(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            root = Path(directory)
            capture = root / "synthetic.csv"
            candidate = root / "candidate.json"
            write_capture(capture, synthetic_rows())
            write_candidate(capture, candidate)
            document = json.loads(candidate.read_text(encoding="utf-8"))
            document["lut_centideg"] = [
                3000 if index % 2 else -3000 for index in range(analyzer.LUT_NODES)
            ]
            document["lut_sha256"] = analyzer.canonical_sha256(document["lut_centideg"])
            candidate.write_text(
                json.dumps(document, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(analyzer.CalibrationError, "not strictly monotonic"):
                analyzer.validate_report_file(candidate)

    def test_rejects_excessive_turn_speed_variation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            capture = Path(directory) / "unstable-speed.csv"
            rows = synthetic_rows()
            inject_large_turn_speed_variation(rows)
            write_capture(capture, rows)
            with self.assertRaisesRegex(analyzer.CalibrationError, "speed is not stable"):
                analyzer.analyze_capture(capture)

    def test_cross_validation_applies_a_fixed_candidate_without_refit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            root = Path(directory)
            calibration_capture = root / "calibration.csv"
            validation_capture = root / "validation.csv"
            candidate = root / "candidate.json"
            cross_report = root / "cross-validation.json"
            rows = synthetic_rows()
            write_capture(calibration_capture, rows)
            write_capture(validation_capture, shifted_capture_rows(rows))
            write_candidate(calibration_capture, candidate)
            candidate_before = candidate.read_text(encoding="utf-8")

            with redirect_stdout(io.StringIO()):
                self.assertEqual(
                    analyzer.main(
                        (
                            str(validation_capture),
                            "--cross-validate-candidate",
                            str(candidate),
                            "--out",
                            str(cross_report),
                        )
                    ),
                    0,
                )

            document = json.loads(cross_report.read_text(encoding="utf-8"))
            candidate_document = json.loads(candidate_before)
            self.assertEqual(candidate.read_text(encoding="utf-8"), candidate_before)
            self.assertEqual(
                document["kind"], "as5600_cyclic_linearity_cross_validation"
            )
            self.assertTrue(document["scope"]["offline_cross_validation"])
            self.assertFalse(document["scope"]["candidate_lut_refit"])
            self.assertFalse(document["scope"]["candidate_lut_replaced"])
            self.assertEqual(
                document["candidate"]["lut_sha256"], candidate_document["lut_sha256"]
            )
            self.assertNotIn("lut_centideg", document)
            self.assertLess(
                document["quality"]["combined_with_candidate"]["rms_degrees"],
                document["quality"]["combined_before"]["rms_degrees"],
            )

    def test_cross_validation_rejects_the_candidate_capture_itself(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            root = Path(directory)
            capture = root / "capture.csv"
            candidate = root / "candidate.json"
            write_capture(capture, synthetic_rows())
            write_candidate(capture, candidate)
            with self.assertRaisesRegex(analyzer.CalibrationError, "identical"):
                analyzer.cross_validate_candidate(candidate, capture)

    def test_candidate_validation_requires_identity_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory(prefix="as5600-linearity-") as directory:
            root = Path(directory)
            capture = root / "capture.csv"
            candidate = root / "candidate.json"
            minimal = root / "minimal-lut.json"
            write_capture(capture, synthetic_rows())
            write_candidate(capture, candidate)
            document = json.loads(candidate.read_text(encoding="utf-8"))
            del document["calibration"]
            candidate.write_text(
                json.dumps(document, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(analyzer.CalibrationError, "calibration"):
                analyzer.validate_candidate_report_file(candidate)

            lut = [0] * analyzer.LUT_NODES
            minimal.write_text(
                json.dumps(
                    {
                        "kind": "as5600_cyclic_linearity_calibration_candidate",
                        "lut_centideg": lut,
                        "lut_sha256": analyzer.canonical_sha256(lut),
                    }
                ),
                encoding="utf-8",
            )
            self.assertTrue(analyzer.validate_lut_report_file(minimal)["strictly_monotonic"])
            with self.assertRaisesRegex(analyzer.CalibrationError, "schema_version"):
                analyzer.validate_candidate_report_file(minimal)

    def test_help_declares_the_offline_and_non_absolute_scope(self) -> None:
        help_text = re.sub(r"\s+", " ", analyzer.build_parser().format_help().lower())
        self.assertIn("offline", help_text)
        self.assertIn("cero mecánico", help_text)
        self.assertIn("ángulo físico absoluto", help_text)
        self.assertIn("no guarda muestras crudas", help_text)
        self.assertIn("cross-validate-candidate", help_text)
        self.assertIn("validate-lut-report", help_text)


if __name__ == "__main__":
    unittest.main()
