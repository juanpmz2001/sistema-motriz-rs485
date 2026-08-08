#!/usr/bin/env python3
"""Build an offline AS5600 cyclic-linearity calibration candidate.

This tool reimplements the analysis used by the isolated ``ensayo-nueva-pata``
experiment without connecting to hardware or retaining capture samples.  It
expects one CSV containing a negative and a positive constant-speed pass.  The
input is summarized only by hashes, counts and derived quality metrics in the
resulting JSON report.

The result is an *offline calibration candidate*.  It does not establish a
mechanical zero, a steering reference, or an absolute physical angle.  Those
remain separate hardware qualification activities.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import io
import json
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence


RAW_COUNTS = 4096
LUT_NODES = 128
RAW_STEP_DEGREES = 360.0 / RAW_COUNTS
NODE_STEP_DEGREES = 360.0 / LUT_NODES
CENTIDEGREE = 0.01
REQUIRED_COMPLETE_TURNS = 7
MIN_SAMPLES_PER_TURN = 16
MAX_BACKTRACK_DEGREES = 3.0
MAX_RAW_UNWRAP_RESIDUAL_DEGREES = RAW_STEP_DEGREES * 2.0 + 1.0e-6
MIN_DIRECTION_CORRELATION = 0.80
FLAT_CURVE_RMS_DEGREES = 0.03
MAX_REPEATABILITY_RMS_DEGREES = 2.0
MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT = 5.0
MIN_SAFE_CORRECTION_SCALE = 0.50
CALIBRATION_REPORT_SCHEMA_VERSION = 2
CALIBRATION_REPORT_KIND = "as5600_cyclic_linearity_calibration_candidate"
CROSS_VALIDATION_REPORT_KIND = "as5600_cyclic_linearity_cross_validation"


class CalibrationError(RuntimeError):
    """The supplied capture cannot justify a calibration candidate."""


@dataclass(frozen=True)
class Sample:
    """One accepted capture row; never emitted to the report."""

    time_ms: float
    raw_angle: int
    unwrapped_degrees: float


@dataclass(frozen=True)
class PassAnalysis:
    direction: str
    sign: int
    accepted_samples: int
    complete_turns: int
    used_turns: int
    curve_degrees: list[float]
    node_repeatability_degrees: list[float]
    speeds_degrees_per_second: list[float]
    sample_errors_degrees: list[float]
    sample_measured_phases_degrees: list[float]


def finite(value: object, field: str, row_number: int) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise CalibrationError(
            f"row {row_number}: {field} is not a number"
        ) from exc
    if not math.isfinite(result):
        raise CalibrationError(f"row {row_number}: {field} is not finite")
    return result


def integer(value: object, field: str, row_number: int) -> int:
    try:
        result = int(str(value).strip(), 10)
    except (TypeError, ValueError) as exc:
        raise CalibrationError(
            f"row {row_number}: {field} is not an integer"
        ) from exc
    return result


def boolean(value: object, field: str, row_number: int) -> bool:
    normalized = str(value).strip().lower()
    if normalized in {"1", "true", "yes", "y", "on"}:
        return True
    if normalized in {"0", "false", "no", "n", "off"}:
        return False
    raise CalibrationError(f"row {row_number}: {field} is not a boolean")


def direction_from_label(value: object, row_number: int) -> str | None:
    """Normalize the historical phase names and simple generic aliases."""

    label = str(value).strip().upper()
    if label in {
        "POSITIVE",
        "+",
        "FORWARD",
        "RIGHT_1040_US",
        "RIGHT",
    }:
        return "positive"
    if label in {
        "NEGATIVE",
        "-",
        "REVERSE",
        "LEFT_2000_US",
        "LEFT",
    }:
        return "negative"
    if not label or label in {"IDLE", "STOP", "NEUTRAL"}:
        return None
    # The historical capture also contains neutral/setup phases.  They are not
    # calibration passes and are deliberately omitted; missing target passes
    # still fail the completeness gate below.
    return None


def wrapped_distance_degrees(left: float, right: float) -> float:
    """Signed shortest distance from right to left on a cyclic 360-degree axis."""

    return (left - right + 180.0) % 360.0 - 180.0


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def median(values: Sequence[float]) -> float:
    if not values:
        raise CalibrationError("median requested for an empty collection")
    return float(statistics.median(values))


def centered(values: Sequence[float]) -> list[float]:
    offset = mean(values)
    return [value - offset for value in values]


def rms(values: Sequence[float]) -> float:
    return math.sqrt(mean([value * value for value in values])) if values else 0.0


def coefficient_of_variation_percent(values: Sequence[float]) -> float:
    """Return population CV for positive speed magnitudes."""

    if not values:
        return 0.0
    average = mean(values)
    if average <= 0.0:
        return 0.0
    return 100.0 * statistics.pstdev(values) / average


def robust_sigma(values: Sequence[float]) -> float:
    if len(values) < 2:
        return 0.0
    center = median(values)
    return 1.4826 * median([abs(value - center) for value in values])


def percentile(values: Sequence[float], percent: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def isotonic_non_decreasing(values: Sequence[float]) -> list[float]:
    """Return a pool-adjacent-violators fit while retaining every input row."""

    if not values:
        return []
    blocks: list[list[float]] = []  # start, end, weight, mean
    for index, value in enumerate(values):
        blocks.append([float(index), float(index), 1.0, float(value)])
        while len(blocks) > 1 and blocks[-2][3] > blocks[-1][3]:
            right = blocks.pop()
            left = blocks.pop()
            weight = left[2] + right[2]
            blocks.append(
                [
                    left[0],
                    right[1],
                    weight,
                    (left[3] * left[2] + right[3] * right[2]) / weight,
                ]
            )
    fitted = [0.0] * len(values)
    for start, end, _weight, value in blocks:
        for index in range(int(start), int(end) + 1):
            fitted[index] = value
    return fitted


def crossing_time(
    times: Sequence[float], progress: Sequence[float], target: float
) -> float:
    index = bisect.bisect_left(progress, target)
    if index == 0:
        return times[0]
    if index >= len(progress):
        raise CalibrationError(f"capture does not reach {target:.1f} degrees")
    lower = index - 1
    upper = index
    while lower > 0 and progress[lower] >= target:
        lower -= 1
    while upper + 1 < len(progress) and progress[upper] <= progress[lower]:
        upper += 1
    span = progress[upper] - progress[lower]
    if span <= 0.0:
        return (times[lower] + times[upper]) * 0.5
    fraction = min(1.0, max(0.0, (target - progress[lower]) / span))
    return times[lower] + fraction * (times[upper] - times[lower])


def coalesce_xy(
    x_values: Sequence[float], y_values: Sequence[float]
) -> tuple[list[float], list[float]]:
    pairs = sorted(zip((value % 360.0 for value in x_values), y_values))
    xs: list[float] = []
    ys: list[float] = []
    bucket: list[float] = []
    current: float | None = None
    for x_value, y_value in pairs:
        if current is None or abs(x_value - current) <= 1.0e-9:
            if current is None:
                current = x_value
            bucket.append(y_value)
            continue
        xs.append(current)
        ys.append(median(bucket))
        current = x_value
        bucket = [y_value]
    if current is not None:
        xs.append(current)
        ys.append(median(bucket))
    return xs, ys


def periodic_interpolate(
    x_values: Sequence[float], y_values: Sequence[float], target: float
) -> float:
    xs, ys = coalesce_xy(x_values, y_values)
    if len(xs) < 2:
        raise CalibrationError("a turn has fewer than two distinct phase samples")
    value = target % 360.0
    extended_x = [xs[-1] - 360.0, *xs, xs[0] + 360.0]
    extended_y = [ys[-1], *ys, ys[0]]
    upper = bisect.bisect_right(extended_x, value)
    upper = min(max(1, upper), len(extended_x) - 1)
    lower = upper - 1
    width = extended_x[upper] - extended_x[lower]
    if width <= 0.0:
        return extended_y[lower]
    fraction = (value - extended_x[lower]) / width
    return extended_y[lower] + fraction * (
        extended_y[upper] - extended_y[lower]
    )


def periodic_smooth(values: Sequence[float], passes: int = 2) -> list[float]:
    current = list(values)
    if not current:
        return []
    weights = (1.0, 4.0, 6.0, 4.0, 1.0)
    divisor = sum(weights)
    for _ in range(passes):
        size = len(current)
        current = [
            sum(weights[offset] * current[(index + offset - 2) % size]
                for offset in range(len(weights)))
            / divisor
            for index in range(size)
        ]
    return current


def validate_pass_samples(direction: str, samples: Sequence[Sample]) -> float:
    if len(samples) < REQUIRED_COMPLETE_TURNS * MIN_SAMPLES_PER_TURN:
        raise CalibrationError(
            f"{direction} pass has only {len(samples)} accepted samples; "
            "capture is incomplete"
        )
    for index in range(1, len(samples)):
        if samples[index].time_ms <= samples[index - 1].time_ms:
            raise CalibrationError(
                f"{direction} pass timestamps are not strictly increasing at "
                f"accepted sample {index + 1}"
            )
    # Historical captures store unwrapped_deg relative to the capture's start,
    # while raw_angle remains in the AS5600's physical cyclic coordinate.  Find
    # that fixed offset rather than incorrectly assuming both start at zero.
    offsets = [
        wrapped_distance_degrees(
            sample.raw_angle * RAW_STEP_DEGREES,
            sample.unwrapped_degrees % 360.0,
        )
        for sample in samples
    ]
    offset_radians = math.atan2(
        sum(math.sin(math.radians(value)) for value in offsets),
        sum(math.cos(math.radians(value)) for value in offsets),
    )
    raw_coordinate_offset = math.degrees(offset_radians) % 360.0
    for index, sample in enumerate(samples, start=1):
        expected = sample.raw_angle * RAW_STEP_DEGREES
        observed = (sample.unwrapped_degrees + raw_coordinate_offset) % 360.0
        residual = abs(wrapped_distance_degrees(expected, observed))
        if residual > MAX_RAW_UNWRAP_RESIDUAL_DEGREES:
            raise CalibrationError(
                f"{direction} pass accepted sample {index} has raw/unwrapped "
                f"mismatch {residual:.3f} deg"
            )
    return raw_coordinate_offset


def analyze_pass(direction: str, samples: Sequence[Sample]) -> PassAnalysis:
    validate_pass_samples(direction, samples)
    sign = 1 if direction == "positive" else -1
    times = [sample.time_ms for sample in samples]
    start = samples[0].unwrapped_degrees
    start_raw_phase = samples[0].raw_angle * RAW_STEP_DEGREES
    progress = [sign * (sample.unwrapped_degrees - start) for sample in samples]
    backward_steps = [
        previous - following
        for previous, following in zip(progress, progress[1:])
        if following < previous
    ]
    largest_backtrack = max(backward_steps, default=0.0)
    if largest_backtrack > MAX_BACKTRACK_DEGREES:
        raise CalibrationError(
            f"{direction} pass is not monotonic: largest backtrack "
            f"{largest_backtrack:.3f} deg exceeds {MAX_BACKTRACK_DEGREES:.3f} deg"
        )
    fitted_progress = isotonic_non_decreasing(progress)
    fitted_progress = [value - fitted_progress[0] for value in fitted_progress]
    complete_turns = int(math.floor((fitted_progress[-1] + 1.0e-6) / 360.0))
    if complete_turns < REQUIRED_COMPLETE_TURNS:
        raise CalibrationError(
            f"{direction} pass has {complete_turns} complete turns; "
            f"{REQUIRED_COMPLETE_TURNS} are required"
        )
    crossings = [times[0]]
    for turn in range(1, REQUIRED_COMPLETE_TURNS + 1):
        crossings.append(crossing_time(times, fitted_progress, turn * 360.0))

    nodes = [index * NODE_STEP_DEGREES for index in range(LUT_NODES)]
    node_values: list[list[float]] = [[] for _ in nodes]
    all_errors: list[float] = []
    all_measured_phases: list[float] = []
    speeds: list[float] = []
    for turn in range(REQUIRED_COMPLETE_TURNS):
        start_time = crossings[turn]
        end_time = crossings[turn + 1]
        duration = end_time - start_time
        if duration <= 0.0:
            raise CalibrationError(f"{direction} pass turn {turn + 1} has no duration")
        phase: list[float] = []
        errors: list[float] = []
        measured_phases: list[float] = []
        for sample in samples:
            if sample.time_ms < start_time - 1.0e-6 or sample.time_ms > end_time + 1.0e-6:
                continue
            fraction = min(1.0, max(0.0, (sample.time_ms - start_time) / duration))
            ideal_unwrapped = start + sign * (turn + fraction) * 360.0
            # The LUT must be indexed by the actual raw AS5600 phase, not by
            # the arbitrary unwrapped capture origin.
            phase.append((start_raw_phase + sign * (turn + fraction) * 360.0) % 360.0)
            errors.append(sample.unwrapped_degrees - ideal_unwrapped)
            measured_phases.append(sample.raw_angle * RAW_STEP_DEGREES)
        if len(phase) < MIN_SAMPLES_PER_TURN:
            raise CalibrationError(
                f"{direction} pass turn {turn + 1} has only {len(phase)} "
                "samples; capture is incomplete"
            )
        errors = centered(errors)
        for node_index, node in enumerate(nodes):
            node_values[node_index].append(periodic_interpolate(phase, errors, node))
        all_errors.extend(errors)
        all_measured_phases.extend(measured_phases)
        speeds.append(sign * 360000.0 / duration)

    speed_cv_percent = coefficient_of_variation_percent(
        [abs(value) for value in speeds]
    )
    if speed_cv_percent > MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT:
        raise CalibrationError(
            f"{direction} pass speed is not stable enough: turn-speed CV "
            f"{speed_cv_percent:.3f}% exceeds "
            f"{MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT:.3f}%"
        )

    curve = centered(periodic_smooth([median(values) for values in node_values]))
    repeatability = [robust_sigma(values) for values in node_values]
    repeatability_rms = rms(repeatability)
    if repeatability_rms > MAX_REPEATABILITY_RMS_DEGREES:
        raise CalibrationError(
            f"{direction} pass is not repeatable enough: node sigma RMS "
            f"{repeatability_rms:.3f} deg"
        )
    return PassAnalysis(
        direction=direction,
        sign=sign,
        accepted_samples=len(samples),
        complete_turns=complete_turns,
        used_turns=REQUIRED_COMPLETE_TURNS,
        curve_degrees=curve,
        node_repeatability_degrees=repeatability,
        speeds_degrees_per_second=speeds,
        sample_errors_degrees=all_errors,
        sample_measured_phases_degrees=all_measured_phases,
    )


def correlation(left: Sequence[float], right: Sequence[float]) -> float:
    left_centered = centered(left)
    right_centered = centered(right)
    denominator = math.sqrt(
        sum(value * value for value in left_centered)
        * sum(value * value for value in right_centered)
    )
    if denominator == 0.0:
        return 0.0
    return sum(
        first * second for first, second in zip(left_centered, right_centered)
    ) / denominator


def harmonic_coefficients(values: Sequence[float], harmonic: int) -> tuple[float, float]:
    size = len(values)
    cosine = 2.0 / size * sum(
        value * math.cos(2.0 * math.pi * harmonic * index / size)
        for index, value in enumerate(values)
    )
    sine = 2.0 / size * sum(
        value * math.sin(2.0 * math.pi * harmonic * index / size)
        for index, value in enumerate(values)
    )
    return cosine, sine


def coherent_common_curve(
    negative: Sequence[float], positive: Sequence[float]
) -> tuple[list[float], list[dict[str, Any]]]:
    """Retain only low-order components that agree in phase and amplitude."""

    retained: list[tuple[int, float, float]] = []
    details: list[dict[str, Any]] = []
    for harmonic in range(1, 17):
        neg_cos, neg_sin = harmonic_coefficients(negative, harmonic)
        pos_cos, pos_sin = harmonic_coefficients(positive, harmonic)
        negative_amplitude = math.hypot(neg_cos, neg_sin)
        positive_amplitude = math.hypot(pos_cos, pos_sin)
        if negative_amplitude and positive_amplitude:
            phase_cosine = (
                neg_cos * pos_cos + neg_sin * pos_sin
            ) / (negative_amplitude * positive_amplitude)
            amplitude_ratio = min(negative_amplitude, positive_amplitude) / max(
                negative_amplitude, positive_amplitude
            )
        else:
            phase_cosine = 0.0
            amplitude_ratio = 0.0
        retained_here = phase_cosine >= 0.5 and amplitude_ratio >= 0.25
        common_cos = (neg_cos + pos_cos) * 0.5 if retained_here else 0.0
        common_sin = (neg_sin + pos_sin) * 0.5 if retained_here else 0.0
        if retained_here:
            retained.append((harmonic, common_cos, common_sin))
        details.append(
            {
                "harmonic": harmonic,
                "negative_amplitude_degrees": negative_amplitude,
                "positive_amplitude_degrees": positive_amplitude,
                "phase_cosine": phase_cosine,
                "amplitude_ratio": amplitude_ratio,
                "retained": retained_here,
            }
        )
    curve: list[float] = []
    for index in range(LUT_NODES):
        value = 0.0
        for harmonic, cosine, sine in retained:
            phase = 2.0 * math.pi * harmonic * index / LUT_NODES
            value += cosine * math.cos(phase) + sine * math.sin(phase)
        curve.append(value)
    return centered(periodic_smooth(curve, passes=1)), details


def assess_coherence(
    negative: PassAnalysis, positive: PassAnalysis
) -> tuple[float, float, float]:
    negative_energy = rms(centered(negative.curve_degrees))
    positive_energy = rms(centered(positive.curve_degrees))
    if max(negative_energy, positive_energy) <= FLAT_CURVE_RMS_DEGREES:
        return 1.0, negative_energy, positive_energy
    if min(negative_energy, positive_energy) <= FLAT_CURVE_RMS_DEGREES:
        raise CalibrationError(
            "bidirectional curves are incoherent: one pass has a material "
            "linearity signal and the other does not"
        )
    value = correlation(negative.curve_degrees, positive.curve_degrees)
    if value < MIN_DIRECTION_CORRELATION:
        raise CalibrationError(
            "bidirectional curves are incoherent: correlation "
            f"{value:.3f} is below {MIN_DIRECTION_CORRELATION:.2f}"
        )
    return value, negative_energy, positive_energy


def inverse_correction_from_error(error_vs_ideal: Sequence[float]) -> list[float]:
    """Invert measured-minus-ideal error into a correction indexed by raw phase."""

    ideal_nodes = [index * NODE_STEP_DEGREES for index in range(LUT_NODES)]
    measured_nodes = [
        (angle + error) % 360.0 for angle, error in zip(ideal_nodes, error_vs_ideal)
    ]
    correction = [
        periodic_interpolate(measured_nodes, [-error for error in error_vs_ideal], node)
        for node in ideal_nodes
    ]
    return centered(periodic_smooth(correction, passes=1))


def interpolate_lut_degrees(lut_degrees: Sequence[float], raw_angle: float) -> float:
    position = (raw_angle % 360.0) / NODE_STEP_DEGREES
    lower = int(math.floor(position)) % LUT_NODES
    fraction = position - math.floor(position)
    upper = (lower + 1) % LUT_NODES
    return lut_degrees[lower] + fraction * (lut_degrees[upper] - lut_degrees[lower])


def validate_lut(lut_centideg: Sequence[int]) -> dict[str, Any]:
    if len(lut_centideg) != LUT_NODES:
        raise CalibrationError(f"LUT must contain exactly {LUT_NODES} nodes")
    values: list[int] = []
    for index, value in enumerate(lut_centideg):
        if isinstance(value, bool) or not isinstance(value, int):
            raise CalibrationError(f"LUT node {index} is not an integer centidegree")
        if not -32768 <= value <= 32767:
            raise CalibrationError(f"LUT node {index} exceeds int16 range")
        values.append(value)
    corrections = [value * CENTIDEGREE for value in values]
    mapped = [
        raw * RAW_STEP_DEGREES
        + interpolate_lut_degrees(corrections, raw * RAW_STEP_DEGREES)
        for raw in range(RAW_COUNTS)
    ]
    mapped.append(360.0 + corrections[0])
    increments = [
        mapped[index + 1] - mapped[index] for index in range(RAW_COUNTS)
    ]
    invalid = sum(1 for increment in increments if increment <= 0.0)
    return {
        "strictly_monotonic": invalid == 0,
        "invalid_increment_count": invalid,
        "min_increment_degrees_per_count": min(increments),
        "max_increment_degrees_per_count": max(increments),
        "nominal_increment_degrees_per_count": RAW_STEP_DEGREES,
    }


def quantize_correction(values: Sequence[float], scale: float) -> list[int]:
    quantized = [int(round(value * scale / CENTIDEGREE)) for value in values]
    dc_offset = int(round(mean(quantized)))
    return [value - dc_offset for value in quantized]


def make_safe_lut(correction_degrees: Sequence[float]) -> tuple[list[int], float, dict[str, Any]]:
    for permille in range(1000, -1, -1):
        scale = permille / 1000.0
        candidate = quantize_correction(correction_degrees, scale)
        validation = validate_lut(candidate)
        if validation["strictly_monotonic"]:
            if scale < MIN_SAFE_CORRECTION_SCALE:
                raise CalibrationError(
                    "candidate correction needs unsafe attenuation to remain "
                    f"monotonic ({scale:.3f})"
                )
            return candidate, scale, validation
    raise CalibrationError("could not construct a monotonic cyclic LUT")


def error_metrics(values: Sequence[float]) -> dict[str, float]:
    return {
        "rms_degrees": rms(values),
        "mean_absolute_degrees": mean([abs(value) for value in values]),
        "p95_absolute_degrees": percentile([abs(value) for value in values], 95.0),
        "max_absolute_degrees": max([abs(value) for value in values], default=0.0),
    }


def corrected_residuals(
    analysis: PassAnalysis, correction_degrees: Sequence[float]
) -> list[float]:
    return [
        error + interpolate_lut_degrees(correction_degrees, phase)
        for error, phase in zip(
            analysis.sample_errors_degrees,
            analysis.sample_measured_phases_degrees,
        )
    ]


def pass_metadata(analysis: PassAnalysis) -> dict[str, Any]:
    magnitudes = [abs(value) for value in analysis.speeds_degrees_per_second]
    speed_mean = mean(magnitudes)
    return {
        "accepted_samples": analysis.accepted_samples,
        "complete_turns": analysis.complete_turns,
        "used_turns": analysis.used_turns,
        "speed_degrees_per_second": {
            "mean_signed": mean(analysis.speeds_degrees_per_second),
            "mean_magnitude": speed_mean,
            "coefficient_of_variation_percent": coefficient_of_variation_percent(
                magnitudes
            ),
        },
        "node_repeatability": {
            "rms_degrees": rms(analysis.node_repeatability_degrees),
            "p95_degrees": percentile(analysis.node_repeatability_degrees, 95.0),
            "max_degrees": max(analysis.node_repeatability_degrees),
        },
    }


def canonical_sha256(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def load_capture(path: Path) -> tuple[dict[str, list[Sample]], dict[str, Any]]:
    try:
        raw_bytes = path.read_bytes()
        text = raw_bytes.decode("utf-8-sig")
    except (OSError, UnicodeDecodeError) as exc:
        raise CalibrationError(f"cannot read input CSV: {exc}") from exc
    reader = csv.DictReader(
        line for line in io.StringIO(text) if line.strip() and not line.startswith("#")
    )
    if reader.fieldnames is None:
        raise CalibrationError("input CSV has no header")
    required = {"time_ms", "raw_angle", "unwrapped_deg", "i2c_valid", "valid", "glitch"}
    missing = sorted(required.difference(reader.fieldnames))
    if missing:
        raise CalibrationError("input CSV is missing columns: " + ", ".join(missing))
    label_field = "direction" if "direction" in reader.fieldnames else "phase"
    if label_field not in reader.fieldnames:
        raise CalibrationError("input CSV needs either a direction or phase column")

    passes: dict[str, list[Sample]] = {"negative": [], "positive": []}
    filtering: dict[str, dict[str, int]] = {
        direction: {
            "rows": 0,
            "accepted": 0,
            "rejected_i2c": 0,
            "rejected_invalid": 0,
            "rejected_glitch": 0,
        }
        for direction in passes
    }
    ignored_rows = 0
    for row_number, row in enumerate(reader, start=2):
        direction = direction_from_label(row.get(label_field, ""), row_number)
        if direction is None:
            ignored_rows += 1
            continue
        statistics_for_direction = filtering[direction]
        statistics_for_direction["rows"] += 1
        i2c_valid = boolean(row.get("i2c_valid"), "i2c_valid", row_number)
        accepted = boolean(row.get("valid"), "valid", row_number)
        glitch = boolean(row.get("glitch"), "glitch", row_number)
        if not i2c_valid:
            statistics_for_direction["rejected_i2c"] += 1
        if not accepted:
            statistics_for_direction["rejected_invalid"] += 1
        if glitch:
            statistics_for_direction["rejected_glitch"] += 1
        if not i2c_valid or not accepted or glitch:
            continue
        raw_angle = integer(row.get("raw_angle"), "raw_angle", row_number)
        if not 0 <= raw_angle < RAW_COUNTS:
            raise CalibrationError(
                f"row {row_number}: raw_angle {raw_angle} is outside 0..{RAW_COUNTS - 1}"
            )
        passes[direction].append(
            Sample(
                time_ms=finite(row.get("time_ms"), "time_ms", row_number),
                raw_angle=raw_angle,
                unwrapped_degrees=finite(
                    row.get("unwrapped_deg"), "unwrapped_deg", row_number
                ),
            )
        )
        statistics_for_direction["accepted"] += 1
    return passes, {
        "input_file_name": path.name,
        "input_sha256": hashlib.sha256(raw_bytes).hexdigest(),
        "input_bytes": len(raw_bytes),
        "filtering": filtering,
        "ignored_rows": ignored_rows,
    }


def analyze_bidirectional_capture(
    path: Path,
) -> tuple[
    PassAnalysis,
    PassAnalysis,
    dict[str, Any],
    float,
    float,
    float,
]:
    passes, input_metadata = load_capture(path)
    negative = analyze_pass("negative", passes["negative"])
    positive = analyze_pass("positive", passes["positive"])
    direction_correlation, negative_energy, positive_energy = assess_coherence(
        negative, positive
    )
    return (
        negative,
        positive,
        input_metadata,
        direction_correlation,
        negative_energy,
        positive_energy,
    )


def analyze_capture(
    path: Path,
    calibration_id: str | None = None,
    hardware_identity: str | None = None,
) -> dict[str, Any]:
    (
        negative,
        positive,
        input_metadata,
        direction_correlation,
        negative_energy,
        positive_energy,
    ) = analyze_bidirectional_capture(path)
    common_error, harmonic_details = coherent_common_curve(
        negative.curve_degrees, positive.curve_degrees
    )
    if max(negative_energy, positive_energy) > FLAT_CURVE_RMS_DEGREES and not any(
        detail["retained"] for detail in harmonic_details
    ):
        raise CalibrationError(
            "bidirectional curves have no coherent retained harmonic component"
        )
    correction_request = inverse_correction_from_error(common_error)
    lut_centideg, correction_scale, mapping_validation = make_safe_lut(
        correction_request
    )
    correction_degrees = [value * CENTIDEGREE for value in lut_centideg]
    negative_residual = corrected_residuals(negative, correction_degrees)
    positive_residual = corrected_residuals(positive, correction_degrees)
    return {
        "schema_version": CALIBRATION_REPORT_SCHEMA_VERSION,
        "kind": CALIBRATION_REPORT_KIND,
        "calibration": {
            "id": calibration_id,
            "hardware_identity": hardware_identity,
        },
        "provenance": {
            "analyzer": "tools/analyze_as5600_linearity.py",
            "algorithm_version": 2,
            "source_experiment": "origin/ensayo-nueva-pata",
            "source_experiment_commit": "d5297b7974ad748ee4bc6733ce3be9bbc97b9cab",
            **input_metadata,
        },
        "scope": {
            "offline_candidate": True,
            "not_proven": [
                "mechanical_zero",
                "absolute_physical_angle",
                "closed_loop_or_hardware_qualification",
            ],
        },
        "method": {
            "required_complete_turns_per_direction": REQUIRED_COMPLETE_TURNS,
            "lut_nodes": LUT_NODES,
            "raw_counts": RAW_COUNTS,
            "units": "signed centidegrees",
            "interpolation": "cyclic linear interpolation between 128 nodes",
            "reference": "constant speed independently fitted between each complete turn",
            "maximum_turn_speed_coefficient_of_variation_percent": (
                MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT
            ),
            "constant_speed_limitation": (
                "turn-speed CV rejects unstable passes but cannot prove absence of "
                "intra-turn speed ripple without an independent reference"
            ),
            "coherence_rule": (
                "bidirectional correlation >= "
                f"{MIN_DIRECTION_CORRELATION:.2f}; matching low-order harmonics only"
            ),
        },
        "passes": {
            "negative": pass_metadata(negative),
            "positive": pass_metadata(positive),
        },
        "quality": {
            "direction_curve_correlation": direction_correlation,
            "negative_curve_rms_degrees": negative_energy,
            "positive_curve_rms_degrees": positive_energy,
            "negative_before": error_metrics(negative.sample_errors_degrees),
            "positive_before": error_metrics(positive.sample_errors_degrees),
            "negative_after": error_metrics(negative_residual),
            "positive_after": error_metrics(positive_residual),
            "harmonics": harmonic_details,
            "curves_degrees": {
                "negative_error_vs_ideal": negative.curve_degrees,
                "positive_error_vs_ideal": positive.curve_degrees,
                "coherent_error_vs_ideal": common_error,
            },
        },
        "lut_centideg": lut_centideg,
        "lut_sha256": canonical_sha256(lut_centideg),
        "correction_scale": correction_scale,
        "mapping_validation": mapping_validation,
    }


def write_report(report: Mapping[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, sort_keys=True, ensure_ascii=False, indent=2, allow_nan=False)
        + "\n",
        encoding="utf-8",
    )


def require_nonempty_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise CalibrationError(f"{field} must be a non-empty string")
    return value.strip()


def require_sha256_text(value: object, field: str) -> str:
    text = require_nonempty_text(value, field)
    if len(text) != 64 or any(character not in "0123456789abcdef" for character in text):
        raise CalibrationError(f"{field} must be a lowercase SHA-256 hex digest")
    return text


def require_mapping(value: object, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise CalibrationError(f"{field} must be a JSON object")
    return value


def require_nonnegative_finite_number(value: object, field: str) -> float:
    if isinstance(value, bool):
        raise CalibrationError(f"{field} must be a finite number")
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise CalibrationError(f"{field} must be a finite number") from exc
    if not math.isfinite(number) or number < 0.0:
        raise CalibrationError(f"{field} must be a non-negative finite number")
    return number


def require_positive_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise CalibrationError(f"{field} must be a positive integer")
    return value


def read_json_document(path: Path, label: str) -> tuple[dict[str, Any], bytes]:
    try:
        raw_bytes = path.read_bytes()
        document = json.loads(raw_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CalibrationError(f"cannot read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise CalibrationError(f"{label} must be a JSON object")
    return document, raw_bytes


def validate_lut_report_document(document: Mapping[str, Any]) -> dict[str, Any]:
    """Validate only LUT structure/integrity, never a physical calibration claim."""

    if document.get("kind") != CALIBRATION_REPORT_KIND:
        raise CalibrationError("calibration report kind is not recognized")
    values = document.get("lut_centideg")
    if not isinstance(values, list):
        raise CalibrationError("calibration report has no lut_centideg array")
    validation = validate_lut(values)
    if not validation["strictly_monotonic"]:
        raise CalibrationError(
            "calibration report LUT is not strictly monotonic across all 4096 raw counts"
        )
    expected_hash = document.get("lut_sha256")
    if expected_hash != canonical_sha256(values):
        raise CalibrationError("calibration report LUT hash does not match")
    return validation


def validate_lut_report_file(path: Path) -> dict[str, Any]:
    document, _raw_bytes = read_json_document(path, "calibration LUT report")
    return validate_lut_report_document(document)


def validate_candidate_report_document(document: Mapping[str, Any]) -> dict[str, Any]:
    """Validate candidate provenance/schema; this still is not physical evidence."""

    validation = validate_lut_report_document(document)
    if document.get("schema_version") != CALIBRATION_REPORT_SCHEMA_VERSION:
        raise CalibrationError("calibration candidate schema_version is not supported")

    calibration = require_mapping(document.get("calibration"), "calibration")
    require_nonempty_text(calibration.get("id"), "calibration.id")
    require_nonempty_text(
        calibration.get("hardware_identity"), "calibration.hardware_identity"
    )

    provenance = require_mapping(document.get("provenance"), "provenance")
    require_nonempty_text(provenance.get("analyzer"), "provenance.analyzer")
    require_positive_int(provenance.get("algorithm_version"), "provenance.algorithm_version")
    require_nonempty_text(
        provenance.get("source_experiment"), "provenance.source_experiment"
    )
    source_commit = require_nonempty_text(
        provenance.get("source_experiment_commit"),
        "provenance.source_experiment_commit",
    )
    if len(source_commit) != 40 or any(
        character not in "0123456789abcdef" for character in source_commit
    ):
        raise CalibrationError(
            "provenance.source_experiment_commit must be a lowercase 40-hex commit"
        )
    require_nonempty_text(
        provenance.get("input_file_name"), "provenance.input_file_name"
    )
    require_sha256_text(provenance.get("input_sha256"), "provenance.input_sha256")
    require_positive_int(provenance.get("input_bytes"), "provenance.input_bytes")
    filtering = require_mapping(provenance.get("filtering"), "provenance.filtering")
    for direction in ("negative", "positive"):
        counts = require_mapping(filtering.get(direction), f"provenance.filtering.{direction}")
        rows = require_positive_int(counts.get("rows"), f"provenance.filtering.{direction}.rows")
        accepted = require_positive_int(
            counts.get("accepted"), f"provenance.filtering.{direction}.accepted"
        )
        if accepted > rows:
            raise CalibrationError(
                f"provenance.filtering.{direction}.accepted exceeds rows"
            )
        for name in ("rejected_i2c", "rejected_invalid", "rejected_glitch"):
            value = counts.get(name)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise CalibrationError(
                    f"provenance.filtering.{direction}.{name} must be a non-negative integer"
                )
    ignored_rows = provenance.get("ignored_rows")
    if isinstance(ignored_rows, bool) or not isinstance(ignored_rows, int) or ignored_rows < 0:
        raise CalibrationError("provenance.ignored_rows must be a non-negative integer")

    scope = require_mapping(document.get("scope"), "scope")
    if scope.get("offline_candidate") is not True:
        raise CalibrationError("scope.offline_candidate must be true")
    not_proven = scope.get("not_proven")
    if not isinstance(not_proven, list) or not {
        "mechanical_zero",
        "absolute_physical_angle",
        "closed_loop_or_hardware_qualification",
    }.issubset(not_proven):
        raise CalibrationError("scope.not_proven lacks required physical limitations")

    method = require_mapping(document.get("method"), "method")
    if method.get("required_complete_turns_per_direction") != REQUIRED_COMPLETE_TURNS:
        raise CalibrationError("method required complete turns do not match this analyzer")
    if method.get("lut_nodes") != LUT_NODES or method.get("raw_counts") != RAW_COUNTS:
        raise CalibrationError("method LUT/raw dimensions do not match this analyzer")
    maximum_cv = require_nonnegative_finite_number(
        method.get("maximum_turn_speed_coefficient_of_variation_percent"),
        "method.maximum_turn_speed_coefficient_of_variation_percent",
    )
    if abs(maximum_cv - MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT) > 1.0e-9:
        raise CalibrationError("method maximum turn-speed CV does not match this analyzer")
    require_nonempty_text(
        method.get("constant_speed_limitation"), "method.constant_speed_limitation"
    )

    passes = require_mapping(document.get("passes"), "passes")
    for direction in ("negative", "positive"):
        pass_info = require_mapping(passes.get(direction), f"passes.{direction}")
        require_positive_int(pass_info.get("accepted_samples"), f"passes.{direction}.accepted_samples")
        if pass_info.get("complete_turns", 0) < REQUIRED_COMPLETE_TURNS:
            raise CalibrationError(f"passes.{direction}.complete_turns is incomplete")
        if pass_info.get("used_turns") != REQUIRED_COMPLETE_TURNS:
            raise CalibrationError(f"passes.{direction}.used_turns is not deterministic")
        speed = require_mapping(
            pass_info.get("speed_degrees_per_second"),
            f"passes.{direction}.speed_degrees_per_second",
        )
        cv = require_nonnegative_finite_number(
            speed.get("coefficient_of_variation_percent"),
            f"passes.{direction}.speed_degrees_per_second.coefficient_of_variation_percent",
        )
        if cv > MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT:
            raise CalibrationError(f"passes.{direction} exceeds the maximum turn-speed CV")
        repeatability = require_mapping(
            pass_info.get("node_repeatability"),
            f"passes.{direction}.node_repeatability",
        )
        if require_nonnegative_finite_number(
            repeatability.get("rms_degrees"),
            f"passes.{direction}.node_repeatability.rms_degrees",
        ) > MAX_REPEATABILITY_RMS_DEGREES:
            raise CalibrationError(f"passes.{direction} exceeds repeatability limit")

    quality = require_mapping(document.get("quality"), "quality")
    correlation_value = require_nonnegative_finite_number(
        quality.get("direction_curve_correlation"), "quality.direction_curve_correlation"
    )
    if correlation_value > 1.0:
        raise CalibrationError("quality.direction_curve_correlation exceeds one")
    for name in ("negative_before", "positive_before", "negative_after", "positive_after"):
        metrics = require_mapping(quality.get(name), f"quality.{name}")
        for metric in (
            "rms_degrees",
            "mean_absolute_degrees",
            "p95_absolute_degrees",
            "max_absolute_degrees",
        ):
            require_nonnegative_finite_number(metrics.get(metric), f"quality.{name}.{metric}")

    scale = require_nonnegative_finite_number(
        document.get("correction_scale"), "correction_scale"
    )
    if scale < MIN_SAFE_CORRECTION_SCALE or scale > 1.0:
        raise CalibrationError("correction_scale is outside the safe analyzer range")
    return validation


def validate_candidate_report_file(path: Path) -> dict[str, Any]:
    document, _raw_bytes = read_json_document(path, "calibration candidate report")
    return validate_candidate_report_document(document)


def load_candidate_report(path: Path) -> tuple[dict[str, Any], bytes]:
    document, raw_bytes = read_json_document(path, "calibration candidate report")
    validate_candidate_report_document(document)
    return document, raw_bytes


def cross_validate_candidate(
    candidate_path: Path, validation_capture_path: Path
) -> dict[str, Any]:
    """Apply a fixed candidate LUT to a second capture without refitting it."""

    candidate, candidate_bytes = load_candidate_report(candidate_path)
    (
        negative,
        positive,
        input_metadata,
        direction_correlation,
        negative_energy,
        positive_energy,
    ) = analyze_bidirectional_capture(validation_capture_path)
    candidate_provenance = require_mapping(candidate["provenance"], "provenance")
    if input_metadata["input_sha256"] == candidate_provenance["input_sha256"]:
        raise CalibrationError(
            "cross-validation capture is identical to the candidate input capture"
        )

    candidate_lut = candidate["lut_centideg"]
    correction_degrees = [value * CENTIDEGREE for value in candidate_lut]
    negative_residual = corrected_residuals(negative, correction_degrees)
    positive_residual = corrected_residuals(positive, correction_degrees)
    negative_before = error_metrics(negative.sample_errors_degrees)
    positive_before = error_metrics(positive.sample_errors_degrees)
    negative_after = error_metrics(negative_residual)
    positive_after = error_metrics(positive_residual)
    combined_before = error_metrics(
        [*negative.sample_errors_degrees, *positive.sample_errors_degrees]
    )
    combined_after = error_metrics([*negative_residual, *positive_residual])
    calibration = require_mapping(candidate["calibration"], "calibration")

    return {
        "schema_version": 1,
        "kind": CROSS_VALIDATION_REPORT_KIND,
        "candidate": {
            "report_sha256": hashlib.sha256(candidate_bytes).hexdigest(),
            "lut_sha256": candidate["lut_sha256"],
            "candidate_input_sha256": candidate_provenance["input_sha256"],
            "calibration_id": calibration["id"],
            "hardware_identity": calibration["hardware_identity"],
        },
        "validation_capture": input_metadata,
        "scope": {
            "offline_cross_validation": True,
            "candidate_lut_refit": False,
            "candidate_lut_replaced": False,
            "not_proven": [
                "mechanical_zero",
                "absolute_physical_angle",
                "closed_loop_or_hardware_qualification",
            ],
        },
        "method": {
            "required_complete_turns_per_direction": REQUIRED_COMPLETE_TURNS,
            "candidate_application": "fixed candidate LUT applied without refit or rescale",
            "maximum_turn_speed_coefficient_of_variation_percent": (
                MAX_TURN_SPEED_COEFFICIENT_OF_VARIATION_PERCENT
            ),
            "constant_speed_limitation": (
                "turn-speed CV rejects unstable passes but cannot prove absence of "
                "intra-turn speed ripple without an independent reference"
            ),
        },
        "passes": {
            "negative": pass_metadata(negative),
            "positive": pass_metadata(positive),
        },
        "quality": {
            "direction_curve_correlation": direction_correlation,
            "negative_curve_rms_degrees": negative_energy,
            "positive_curve_rms_degrees": positive_energy,
            "negative_before": negative_before,
            "positive_before": positive_before,
            "negative_with_candidate": negative_after,
            "positive_with_candidate": positive_after,
            "combined_before": combined_before,
            "combined_with_candidate": combined_after,
            "candidate_reduced_rms": {
                "negative": negative_after["rms_degrees"] < negative_before["rms_degrees"],
                "positive": positive_after["rms_degrees"] < positive_before["rms_degrees"],
                "combined": combined_after["rms_degrees"] < combined_before["rms_degrees"],
            },
        },
    }


def analyze_file(
    input_path: Path,
    output_path: Path,
    calibration_id: str,
    hardware_identity: str,
) -> dict[str, Any]:
    report = analyze_capture(
        input_path,
        require_nonempty_text(calibration_id, "calibration_id"),
        require_nonempty_text(hardware_identity, "hardware_identity"),
    )
    write_report(report, output_path)
    return report


def cross_validate_file(
    candidate_path: Path, validation_capture_path: Path, output_path: Path
) -> dict[str, Any]:
    report = cross_validate_candidate(candidate_path, validation_capture_path)
    write_report(report, output_path)
    return report


# Kept as a Python compatibility alias. Its intentionally narrow semantics are
# exposed by the CLI name ``--validate-lut-report`` below.
def validate_report_file(path: Path) -> dict[str, Any]:
    return validate_lut_report_file(path)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Construye una LUT cíclica AS5600 de 128 correcciones centigrado "
            "a partir de pasadas positiva/negativa. Es un candidato offline: "
            "no prueba cero mecánico ni ángulo físico absoluto."
        )
    )
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        help=(
            "CSV con time_ms, raw_angle, unwrapped_deg, i2c_valid, valid, "
            "glitch y direction o phase"
        ),
    )
    parser.add_argument(
        "--out",
        type=Path,
        help="ruta explícita para el informe JSON (no guarda muestras crudas)",
    )
    parser.add_argument(
        "--calibration-id",
        help="identificador versionado y no vacío de la calibración candidata",
    )
    parser.add_argument(
        "--hardware-identity",
        help="identidad no vacía de sensor/imán/eje/geometría del fixture",
    )
    validation_group = parser.add_mutually_exclusive_group()
    validation_group.add_argument(
        "--validate-lut-report",
        type=Path,
        help=(
            "valida solamente monotonía e integridad de LUT; no valida evidencia "
            "de calibración ni hardware"
        ),
    )
    validation_group.add_argument(
        "--validate-candidate-report",
        type=Path,
        help=(
            "valida esquema/procedencia declarada de un candidato; no prueba "
            "la captura ni el hardware"
        ),
    )
    validation_group.add_argument(
        "--validate-report",
        dest="legacy_validate_report",
        type=Path,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--cross-validate-candidate",
        type=Path,
        help=(
            "aplica una LUT candidata fija al CSV positional de entrada; requiere "
            "--out y nunca reajusta la LUT"
        ),
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        lut_report_path = args.validate_lut_report or args.legacy_validate_report
        if lut_report_path is not None:
            if (
                args.input is not None
                or args.out is not None
                or args.cross_validate_candidate is not None
                or args.calibration_id is not None
                or args.hardware_identity is not None
            ):
                raise CalibrationError(
                    "--validate-lut-report cannot be combined with candidate or capture arguments"
                )
            if args.legacy_validate_report is not None:
                print(
                    "WARNING: --validate-report is retained as a compatibility alias; "
                    "use --validate-lut-report for its LUT-only semantics.",
                    file=sys.stderr,
                )
            validation = validate_lut_report_file(lut_report_path)
            print(
                "LUT structural validation OK (not calibration evidence): "
                f"minimum increment {validation['min_increment_degrees_per_count']:.8f} deg/count"
            )
            return 0
        if args.validate_candidate_report is not None:
            if (
                args.input is not None
                or args.out is not None
                or args.cross_validate_candidate is not None
                or args.calibration_id is not None
                or args.hardware_identity is not None
            ):
                raise CalibrationError(
                    "--validate-candidate-report cannot be combined with capture arguments"
                )
            validation = validate_candidate_report_file(args.validate_candidate_report)
            print(
                "Candidate report structure/provenance OK (not physical evidence): "
                f"minimum increment {validation['min_increment_degrees_per_count']:.8f} deg/count"
            )
            return 0
        if args.input is None or args.out is None:
            raise CalibrationError("input CSV and --out are required")
        if args.cross_validate_candidate is not None:
            if args.calibration_id is not None or args.hardware_identity is not None:
                raise CalibrationError(
                    "--cross-validate-candidate takes fixture identity from the candidate report"
                )
            report = cross_validate_file(
                args.cross_validate_candidate, args.input, args.out
            )
            print(
                "Offline cross-validation report written: "
                f"{args.out} (candidate LUT unchanged, combined RMS "
                f"{report['quality']['combined_before']['rms_degrees']:.3f} -> "
                f"{report['quality']['combined_with_candidate']['rms_degrees']:.3f} deg)"
            )
            return 0
        if args.calibration_id is None or args.hardware_identity is None:
            raise CalibrationError(
                "--calibration-id and --hardware-identity are required for a candidate report"
            )
        report = analyze_file(
            args.input, args.out, args.calibration_id, args.hardware_identity
        )
        print(
            "Offline calibration candidate written: "
            f"{args.out} (LUT {len(report['lut_centideg'])} nodes, "
            f"correlation {report['quality']['direction_curve_correlation']:.3f})"
        )
        return 0
    except (CalibrationError, csv.Error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
