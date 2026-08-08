#!/usr/bin/env python3
"""Analyze the seven-turn AS5600 constant-speed linearity experiment.

Input CSV columns (as emitted by the diagnostic ESP32 firmware):

    time_ms,raw_angle,angle_deg,servo_us,phase,i2c_valid,valid,glitch,
    status,unwrapped_deg,phase_displacement_deg

The analysis deliberately uses no third-party packages.  For each direction it:

* rejects failed I2C reads, rejected samples and glitches;
* finds complete revolutions with an isotonic (monotonic) crossing estimate;
* defines constant speed independently inside every complete revolution;
* estimates a robust 128-node periodic error curve across repeated turns;
* keeps only Fourier components coherent in both directions;
* exports a safe, monotonic correction LUT in centidegrees;
* writes machine-readable JSON and an offline HTML/SVG report.

The time reference measures the complete steering system under a constant servo
command.  With no independent angular standard, a coherent cyclic component is
consistent with encoder/magnet non-linearity but can also include repeatable
mechanical or motor speed ripple.  The two-direction coherence and hysteresis
metrics make that limitation visible rather than hiding it.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import html
import json
import math
import statistics
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


PHASES = ("LEFT_2000_US", "RIGHT_1040_US")
EXPECTED_TURNS = 7
LUT_NODES = 128
RAW_COUNTS = 4096
RAW_STEP_DEG = 360.0 / RAW_COUNTS
NODE_STEP_DEG = 360.0 / LUT_NODES
CENTIDEGREE = 0.01


class AnalysisError(RuntimeError):
    """Raised when a recording cannot support a defensible calibration."""


@dataclass
class Sample:
    time_ms: float
    raw_angle: int
    angle_deg: float
    servo_us: int
    phase: str
    status: int
    unwrapped_deg: float
    phase_displacement_deg: float


@dataclass
class Turn:
    number: int
    t0_ms: float
    t1_ms: float
    speed_deg_s: float
    phase_deg: List[float]
    measured_phase_deg: List[float]
    error_deg: List[float]


@dataclass
class DirectionAnalysis:
    phase: str
    direction_sign: int
    input_samples: int
    turns: List[Turn]
    node_turn_values: List[List[float]]
    curve: List[float]
    node_repeatability: List[float]
    sample_phase: List[float]
    sample_measured_phase: List[float]
    sample_error: List[float]


def finite(value: float, name: str) -> float:
    if not math.isfinite(value):
        raise AnalysisError(f"{name} is not finite")
    return value


def median(values: Sequence[float]) -> float:
    if not values:
        raise AnalysisError("median requested for an empty sequence")
    return float(statistics.median(values))


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def rms(values: Sequence[float]) -> float:
    return math.sqrt(mean([v * v for v in values])) if values else 0.0


def percentile(values: Sequence[float], percentage: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentage / 100.0
    lo = int(math.floor(position))
    hi = int(math.ceil(position))
    if lo == hi:
        return ordered[lo]
    fraction = position - lo
    return ordered[lo] * (1.0 - fraction) + ordered[hi] * fraction


def robust_sigma(values: Sequence[float]) -> float:
    """Median absolute deviation expressed as an equivalent Gaussian sigma."""
    if len(values) < 2:
        return 0.0
    center = median(values)
    return 1.4826 * median([abs(v - center) for v in values])


def centered(values: Sequence[float]) -> List[float]:
    offset = mean(values)
    return [v - offset for v in values]


def parse_bool(value: object) -> bool:
    return str(value).strip().lower() in {"1", "true", "yes", "y", "on"}


def parse_float(row: Dict[str, str], key: str) -> float:
    try:
        return finite(float(row[key]), key)
    except (KeyError, TypeError, ValueError) as exc:
        raise AnalysisError(f"invalid {key!r} value: {row.get(key)!r}") from exc


def parse_int(row: Dict[str, str], key: str) -> int:
    try:
        return int(row[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise AnalysisError(f"invalid {key!r} value: {row.get(key)!r}") from exc


def load_csv(path: Path) -> Tuple[Dict[str, List[Sample]], Dict[str, object]]:
    required = {
        "time_ms",
        "raw_angle",
        "angle_deg",
        "servo_us",
        "phase",
        "i2c_valid",
        "valid",
        "glitch",
        "status",
        "unwrapped_deg",
        "phase_displacement_deg",
    }
    phases: Dict[str, List[Sample]] = {phase: [] for phase in PHASES}
    stats: Dict[str, object] = {
        "rows_total": 0,
        "rows_target_phases": 0,
        "rows_accepted": 0,
        "rejected_i2c": 0,
        "rejected_not_accepted": 0,
        "rejected_glitch": 0,
        "rejected_raw_range": 0,
        "ignored_other_phase": 0,
    }

    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(line for line in handle if not line.startswith("#"))
        if reader.fieldnames is None:
            raise AnalysisError("CSV has no header")
        missing = sorted(required.difference(reader.fieldnames))
        if missing:
            raise AnalysisError("CSV is missing columns: " + ", ".join(missing))

        for row_number, row in enumerate(reader, start=2):
            stats["rows_total"] = int(stats["rows_total"]) + 1
            phase = (row.get("phase") or "").strip()
            if phase not in phases:
                stats["ignored_other_phase"] = int(stats["ignored_other_phase"]) + 1
                continue
            stats["rows_target_phases"] = int(stats["rows_target_phases"]) + 1

            i2c_valid = parse_bool(row.get("i2c_valid", "0"))
            accepted = parse_bool(row.get("valid", "0"))
            # Future firmware may add an explicit accepted column.  If present,
            # both validity flags must agree before a point is used.
            if "accepted" in row and row.get("accepted", "") != "":
                accepted = accepted and parse_bool(row["accepted"])
            glitch = parse_bool(row.get("glitch", "0"))
            reject = False
            if not i2c_valid:
                stats["rejected_i2c"] = int(stats["rejected_i2c"]) + 1
                reject = True
            if not accepted:
                stats["rejected_not_accepted"] = int(stats["rejected_not_accepted"]) + 1
                reject = True
            if glitch:
                stats["rejected_glitch"] = int(stats["rejected_glitch"]) + 1
                reject = True
            if reject:
                continue

            try:
                raw_angle = parse_int(row, "raw_angle")
                if not 0 <= raw_angle < RAW_COUNTS:
                    stats["rejected_raw_range"] = int(stats["rejected_raw_range"]) + 1
                    continue
                sample = Sample(
                    time_ms=parse_float(row, "time_ms"),
                    raw_angle=raw_angle,
                    angle_deg=parse_float(row, "angle_deg"),
                    servo_us=parse_int(row, "servo_us"),
                    phase=phase,
                    status=parse_int(row, "status"),
                    unwrapped_deg=parse_float(row, "unwrapped_deg"),
                    phase_displacement_deg=parse_float(
                        row, "phase_displacement_deg"
                    ),
                )
            except AnalysisError as exc:
                raise AnalysisError(f"CSV row {row_number}: {exc}") from exc
            phases[phase].append(sample)
            stats["rows_accepted"] = int(stats["rows_accepted"]) + 1

    for phase in PHASES:
        phases[phase].sort(key=lambda point: point.time_ms)
        if len(phases[phase]) < 20:
            raise AnalysisError(
                f"phase {phase} has only {len(phases[phase])} accepted samples"
            )
    return phases, stats


def isotonic_non_decreasing(values: Sequence[float]) -> List[float]:
    """Unweighted pool-adjacent-violators fit, expanded to input length."""
    if not values:
        return []
    blocks: List[List[float]] = []  # [start, end, weight, mean]
    for index, value in enumerate(values):
        blocks.append([float(index), float(index), 1.0, float(value)])
        while len(blocks) >= 2 and blocks[-2][3] > blocks[-1][3]:
            right = blocks.pop()
            left = blocks.pop()
            weight = left[2] + right[2]
            combined = (left[3] * left[2] + right[3] * right[2]) / weight
            blocks.append([left[0], right[1], weight, combined])
    fitted = [0.0] * len(values)
    for start, end, _weight, value in blocks:
        for index in range(int(start), int(end) + 1):
            fitted[index] = value
    return fitted


def crossing_time(
    times: Sequence[float], monotonic_progress: Sequence[float], target: float
) -> float:
    index = bisect.bisect_left(monotonic_progress, target)
    if index <= 0:
        return times[0]
    if index >= len(times):
        raise AnalysisError(f"recording does not reach crossing {target:.1f} deg")
    lo = index - 1
    hi = index
    # Isotonic plateaus are possible.  Expand to find a meaningful bracket.
    while lo > 0 and monotonic_progress[lo] >= target:
        lo -= 1
    while hi + 1 < len(times) and monotonic_progress[hi] <= monotonic_progress[lo]:
        hi += 1
    p0, p1 = monotonic_progress[lo], monotonic_progress[hi]
    if p1 <= p0:
        return (times[lo] + times[hi]) * 0.5
    fraction = min(1.0, max(0.0, (target - p0) / (p1 - p0)))
    return times[lo] + fraction * (times[hi] - times[lo])


def coalesce_xy(x_values: Sequence[float], y_values: Sequence[float]) -> Tuple[List[float], List[float]]:
    pairs = sorted(zip(x_values, y_values))
    x_out: List[float] = []
    y_out: List[float] = []
    bucket: List[float] = []
    current: Optional[float] = None
    for x_value, y_value in pairs:
        if current is None or abs(x_value - current) <= 1.0e-9:
            current = x_value if current is None else current
            bucket.append(y_value)
        else:
            x_out.append(current)
            y_out.append(median(bucket))
            current = x_value
            bucket = [y_value]
    if current is not None:
        x_out.append(current)
        y_out.append(median(bucket))
    return x_out, y_out


def periodic_interp(
    x_values: Sequence[float], y_values: Sequence[float], target: float
) -> float:
    """Linear interpolation on a 360-degree periodic domain."""
    xs, ys = coalesce_xy([x % 360.0 for x in x_values], y_values)
    if len(xs) < 2:
        raise AnalysisError("not enough distinct phase points to interpolate a turn")
    x = target % 360.0
    extended_x = [xs[-1] - 360.0] + xs + [xs[0] + 360.0]
    extended_y = [ys[-1]] + ys + [ys[0]]
    hi = bisect.bisect_right(extended_x, x)
    hi = min(max(1, hi), len(extended_x) - 1)
    lo = hi - 1
    width = extended_x[hi] - extended_x[lo]
    if width <= 0:
        return extended_y[lo]
    fraction = (x - extended_x[lo]) / width
    return extended_y[lo] + fraction * (extended_y[hi] - extended_y[lo])


def analyze_direction(phase: str, samples: Sequence[Sample]) -> DirectionAnalysis:
    times = [sample.time_ms for sample in samples]
    displacement = [sample.phase_displacement_deg for sample in samples]
    endpoint_delta = median(displacement[-min(10, len(displacement)):]) - median(
        displacement[: min(10, len(displacement))]
    )
    if abs(endpoint_delta) < 360.0:
        raise AnalysisError(f"phase {phase} travels less than one revolution")
    sign = 1 if endpoint_delta > 0 else -1
    expected_sign = -1 if phase == "LEFT_2000_US" else 1
    if sign != expected_sign:
        raise AnalysisError(
            f"phase {phase} moves in sign {sign:+d}; expected {expected_sign:+d}"
        )

    origin_displacement = displacement[0]
    progress = [sign * (value - origin_displacement) for value in displacement]
    fitted_progress = isotonic_non_decreasing(progress)
    fitted_origin = fitted_progress[0]
    fitted_progress = [value - fitted_origin for value in fitted_progress]
    available_turns = int(math.floor((fitted_progress[-1] + 1.0e-6) / 360.0))
    complete_turns = min(EXPECTED_TURNS, available_turns)
    if complete_turns < 2:
        raise AnalysisError(
            f"phase {phase} contains only {complete_turns} complete revolutions"
        )

    crossings = [times[0]]
    for turn_index in range(1, complete_turns + 1):
        crossings.append(crossing_time(times, fitted_progress, turn_index * 360.0))

    first_angle = samples[0].angle_deg
    first_unwrapped = samples[0].unwrapped_deg
    turns: List[Turn] = []
    all_phase: List[float] = []
    all_error: List[float] = []

    for turn_index in range(complete_turns):
        t0, t1 = crossings[turn_index], crossings[turn_index + 1]
        duration_ms = t1 - t0
        if duration_ms <= 0:
            raise AnalysisError(f"phase {phase}, turn {turn_index + 1}: bad duration")
        turn_phase: List[float] = []
        turn_measured_phase: List[float] = []
        turn_error: List[float] = []
        for sample in samples:
            # Include both endpoints; periodic coalescing handles their shared phase.
            if sample.time_ms < t0 - 1.0e-6 or sample.time_ms > t1 + 1.0e-6:
                continue
            fraction = min(1.0, max(0.0, (sample.time_ms - t0) / duration_ms))
            ideal_progress = (turn_index + fraction) * 360.0
            ideal_abs = first_angle + sign * ideal_progress
            measured_abs = first_angle + (sample.unwrapped_deg - first_unwrapped)
            error = measured_abs - ideal_abs
            phase_deg = ideal_abs % 360.0
            turn_phase.append(phase_deg)
            turn_measured_phase.append(measured_abs % 360.0)
            turn_error.append(error)
            all_phase.append(phase_deg)
            all_error.append(error)
        if len(turn_phase) < 8:
            raise AnalysisError(
                f"phase {phase}, turn {turn_index + 1}: only {len(turn_phase)} points"
            )
        # A per-turn constant is not observable and can be biased by the first
        # sample.  Remove it before combining turns.
        turn_error = centered(turn_error)
        turns.append(
            Turn(
                number=turn_index + 1,
                t0_ms=t0,
                t1_ms=t1,
                speed_deg_s=sign * 360000.0 / duration_ms,
                phase_deg=turn_phase,
                measured_phase_deg=turn_measured_phase,
                error_deg=turn_error,
            )
        )

    nodes = [index * NODE_STEP_DEG for index in range(LUT_NODES)]
    node_turn_values: List[List[float]] = [[] for _ in nodes]
    for turn in turns:
        for index, node in enumerate(nodes):
            node_turn_values[index].append(
                periodic_interp(turn.phase_deg, turn.error_deg, node)
            )
    curve = [median(values) for values in node_turn_values]
    curve = centered(periodic_smooth(curve, passes=2))
    repeatability = [robust_sigma(values) for values in node_turn_values]

    # Rebuild sample arrays from the centered turns so before/after metrics use
    # the same identifiable (zero-mean per revolution) error definition.
    all_phase = [value for turn in turns for value in turn.phase_deg]
    all_measured_phase = [
        value for turn in turns for value in turn.measured_phase_deg
    ]
    all_error = [value for turn in turns for value in turn.error_deg]
    return DirectionAnalysis(
        phase=phase,
        direction_sign=sign,
        input_samples=len(samples),
        turns=turns,
        node_turn_values=node_turn_values,
        curve=curve,
        node_repeatability=repeatability,
        sample_phase=all_phase,
        sample_measured_phase=all_measured_phase,
        sample_error=all_error,
    )


def periodic_smooth(values: Sequence[float], passes: int = 2) -> List[float]:
    """Short symmetric binomial low-pass, with periodic boundaries."""
    current = list(values)
    weights = (1.0, 4.0, 6.0, 4.0, 1.0)
    divisor = sum(weights)
    for _ in range(passes):
        size = len(current)
        current = [
            sum(weights[k] * current[(index + k - 2) % size] for k in range(5))
            / divisor
            for index in range(size)
        ]
    return current


def correlation(left: Sequence[float], right: Sequence[float]) -> float:
    l = centered(left)
    r = centered(right)
    denominator = math.sqrt(sum(v * v for v in l) * sum(v * v for v in r))
    return sum(a * b for a, b in zip(l, r)) / denominator if denominator else 0.0


def harmonic_coefficients(values: Sequence[float], harmonic: int) -> Tuple[float, float]:
    size = len(values)
    a = 2.0 / size * sum(
        value * math.cos(2.0 * math.pi * harmonic * index / size)
        for index, value in enumerate(values)
    )
    b = 2.0 / size * sum(
        value * math.sin(2.0 * math.pi * harmonic * index / size)
        for index, value in enumerate(values)
    )
    return a, b


def coherent_common_curve(
    left: Sequence[float], right: Sequence[float], max_harmonic: int = 16
) -> Tuple[List[float], List[Dict[str, object]]]:
    """Keep spectral components with consistent phase and useful amplitude.

    Two recordings cannot estimate statistical coherence in the formal DSP
    sense.  This conservative test retains a harmonic only when the coefficient
    vectors point within 60 degrees and neither direction is below 25% of the
    other's amplitude.  Opposed/direction-only components are excluded.
    """
    size = len(left)
    retained: List[Tuple[int, float, float]] = []
    details: List[Dict[str, object]] = []
    for harmonic in range(1, min(max_harmonic, size // 2 - 1) + 1):
        la, lb = harmonic_coefficients(left, harmonic)
        ra, rb = harmonic_coefficients(right, harmonic)
        amp_left = math.hypot(la, lb)
        amp_right = math.hypot(ra, rb)
        if amp_left > 1.0e-12 and amp_right > 1.0e-12:
            phase_cosine = (la * ra + lb * rb) / (amp_left * amp_right)
            amplitude_ratio = min(amp_left, amp_right) / max(amp_left, amp_right)
        else:
            phase_cosine = 0.0
            amplitude_ratio = 0.0
        keep = phase_cosine >= 0.5 and amplitude_ratio >= 0.25
        common_a = (la + ra) * 0.5 if keep else 0.0
        common_b = (lb + rb) * 0.5 if keep else 0.0
        if keep:
            retained.append((harmonic, common_a, common_b))
        details.append(
            {
                "harmonic": harmonic,
                "left_amplitude_deg": amp_left,
                "right_amplitude_deg": amp_right,
                "phase_cosine": phase_cosine,
                "amplitude_ratio": amplitude_ratio,
                "retained": keep,
                "common_amplitude_deg": math.hypot(common_a, common_b),
            }
        )
    common = []
    for index in range(size):
        value = 0.0
        for harmonic, a, b in retained:
            phase = 2.0 * math.pi * harmonic * index / size
            value += a * math.cos(phase) + b * math.sin(phase)
        common.append(value)
    return centered(periodic_smooth(common, passes=1)), details


def interpolate_nodes(nodes: Sequence[float], angle_deg: float) -> float:
    position = (angle_deg % 360.0) / NODE_STEP_DEG
    index = int(math.floor(position)) % LUT_NODES
    fraction = position - math.floor(position)
    following = (index + 1) % LUT_NODES
    return nodes[index] + fraction * (nodes[following] - nodes[index])


def inverse_correction_from_error(error_vs_ideal: Sequence[float]) -> List[float]:
    """Convert measured-minus-ideal error into a correction indexed by raw angle.

    The measured error curve is naturally parameterized by ideal angle ``theta``:
    ``measured = theta + error(theta)``.  Firmware, however, only knows the
    measured angle.  This resampling builds the inverse mapping
    ``correction(measured) = -error(theta)`` on the uniform LUT grid.
    """
    ideal_nodes = [index * NODE_STEP_DEG for index in range(LUT_NODES)]
    measured_nodes = [
        (angle + error) % 360.0
        for angle, error in zip(ideal_nodes, error_vs_ideal)
    ]
    desired = [-error for error in error_vs_ideal]
    correction = [
        periodic_interp(measured_nodes, desired, node) for node in ideal_nodes
    ]
    return centered(periodic_smooth(correction, passes=1))


def quantize_correction(values: Sequence[float], scale: float) -> List[int]:
    lut = [int(round(value * scale / CENTIDEGREE)) for value in values]
    # Remove any quantized DC offset; it cannot improve linearity.
    dc = int(round(mean(lut)))
    return [value - dc for value in lut]


def validate_mapping(lut_centideg: Sequence[int]) -> Dict[str, object]:
    correction_deg = [value * CENTIDEGREE for value in lut_centideg]
    mapped = [
        raw * RAW_STEP_DEG + interpolate_nodes(correction_deg, raw * RAW_STEP_DEG)
        for raw in range(RAW_COUNTS)
    ]
    mapped.append(360.0 + correction_deg[0])
    increments = [mapped[index + 1] - mapped[index] for index in range(RAW_COUNTS)]
    invalid = sum(1 for increment in increments if increment <= 0.0)
    return {
        "strictly_monotonic": invalid == 0,
        "invalid_increment_count": invalid,
        "min_increment_deg_per_count": min(increments),
        "max_increment_deg_per_count": max(increments),
        "nominal_increment_deg_per_count": RAW_STEP_DEG,
    }


def make_safe_lut(correction: Sequence[float]) -> Tuple[List[int], float, Dict[str, object]]:
    # Quantization is included in the monotonicity test.  Scale down only when
    # necessary; normally well-behaved encoder corrections remain at 100%.
    for permille in range(1000, -1, -1):
        scale = permille / 1000.0
        lut = quantize_correction(correction, scale)
        validation = validate_mapping(lut)
        if bool(validation["strictly_monotonic"]):
            return lut, scale, validation
    raise AnalysisError("could not produce a monotonic correction LUT")


def error_metrics(values: Sequence[float]) -> Dict[str, float]:
    if not values:
        return {"rmse_deg": 0.0, "mae_deg": 0.0, "peak_to_peak_deg": 0.0,
                "max_abs_deg": 0.0, "p95_abs_deg": 0.0}
    return {
        "rmse_deg": rms(values),
        "mae_deg": mean([abs(value) for value in values]),
        "peak_to_peak_deg": max(values) - min(values),
        "max_abs_deg": max(abs(value) for value in values),
        "p95_abs_deg": percentile([abs(value) for value in values], 95.0),
    }


def speed_metrics(direction: DirectionAnalysis) -> Dict[str, object]:
    signed = [turn.speed_deg_s for turn in direction.turns]
    magnitudes = [abs(value) for value in signed]
    avg = mean(magnitudes)
    standard_deviation = statistics.pstdev(magnitudes) if len(magnitudes) > 1 else 0.0
    return {
        "signed_mean_deg_s": mean(signed),
        "magnitude_mean_deg_s": avg,
        "magnitude_median_deg_s": median(magnitudes),
        "magnitude_min_deg_s": min(magnitudes),
        "magnitude_max_deg_s": max(magnitudes),
        "magnitude_stddev_deg_s": standard_deviation,
        "coefficient_of_variation_percent": 100.0 * standard_deviation / avg if avg else 0.0,
        "per_turn_deg_s": signed,
        "per_turn_duration_ms": [turn.t1_ms - turn.t0_ms for turn in direction.turns],
    }


def direction_report(
    direction: DirectionAnalysis, correction_deg: Sequence[float]
) -> Dict[str, object]:
    residual = [
        error + interpolate_nodes(correction_deg, measured_phase)
        for measured_phase, error in zip(
            direction.sample_measured_phase, direction.sample_error
        )
    ]
    before = error_metrics(direction.sample_error)
    after = error_metrics(residual)
    coverage = sum(1 for values in direction.node_turn_values if values) / LUT_NODES
    return {
        "phase": direction.phase,
        "direction_sign": direction.direction_sign,
        "accepted_input_samples": direction.input_samples,
        "used_samples": len(direction.sample_error),
        "complete_turns": len(direction.turns),
        "requested_turns": EXPECTED_TURNS,
        "coverage_fraction": coverage,
        "speed": speed_metrics(direction),
        "repeatability": {
            "node_sigma_rms_deg": rms(direction.node_repeatability),
            "node_sigma_median_deg": median(direction.node_repeatability),
            "node_sigma_p95_deg": percentile(direction.node_repeatability, 95.0),
            "node_sigma_max_deg": max(direction.node_repeatability),
        },
        "before": before,
        "after": after,
        "rmse_improvement_percent": (
            100.0 * (1.0 - after["rmse_deg"] / before["rmse_deg"])
            if before["rmse_deg"]
            else 0.0
        ),
    }


def svg_chart(
    title: str,
    series: Sequence[Tuple[str, str, Sequence[float]]],
    width: int = 1000,
    height: int = 360,
) -> str:
    margin_left, margin_right, margin_top, margin_bottom = 62, 24, 44, 52
    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom
    all_values = [value for _name, _color, values in series for value in values]
    magnitude = max([abs(value) for value in all_values] + [0.25])
    y_limit = math.ceil(magnitude * 4.0) / 4.0

    def x_coord(index: int) -> float:
        return margin_left + plot_w * index / (LUT_NODES - 1)

    def y_coord(value: float) -> float:
        return margin_top + plot_h * (y_limit - value) / (2.0 * y_limit)

    elements = [
        f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title)}">',
        f'<text class="chart-title" x="{margin_left}" y="25">{html.escape(title)}</text>',
        f'<rect class="plot" x="{margin_left}" y="{margin_top}" width="{plot_w}" height="{plot_h}"/>',
    ]
    for tick in range(5):
        value = y_limit - tick * y_limit / 2.0
        y = y_coord(value)
        elements.append(f'<line class="grid" x1="{margin_left}" y1="{y:.2f}" x2="{width-margin_right}" y2="{y:.2f}"/>')
        elements.append(f'<text class="axis" x="{margin_left-8}" y="{y+4:.2f}" text-anchor="end">{value:.2f}</text>')
    for degree in (0, 90, 180, 270, 360):
        x = margin_left + plot_w * degree / 360.0
        elements.append(f'<line class="grid" x1="{x:.2f}" y1="{margin_top}" x2="{x:.2f}" y2="{height-margin_bottom}"/>')
        elements.append(f'<text class="axis" x="{x:.2f}" y="{height-margin_bottom+23}" text-anchor="middle">{degree}°</text>')
    elements.append(f'<text class="axis-label" x="18" y="{height/2}" transform="rotate(-90 18 {height/2})" text-anchor="middle">Error (grados)</text>')
    elements.append(f'<text class="axis-label" x="{margin_left+plot_w/2}" y="{height-10}" text-anchor="middle">Ángulo ideal</text>')
    for name, color, values in series:
        points = " ".join(
            f"{x_coord(index):.2f},{y_coord(value):.2f}"
            for index, value in enumerate(values)
        )
        elements.append(f'<polyline fill="none" stroke="{color}" stroke-width="2.4" points="{points}"/>')
    legend_x = width - margin_right - 185
    for index, (name, color, _values) in enumerate(series):
        y = 20 + index * 18
        elements.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x+24}" y2="{y}" stroke="{color}" stroke-width="3"/>')
        elements.append(f'<text class="legend" x="{legend_x+31}" y="{y+4}">{html.escape(name)}</text>')
    elements.append("</svg>")
    return "".join(elements)


def metric_card(label: str, value: str, note: str = "") -> str:
    return (
        '<div class="card"><span class="label">'
        + html.escape(label)
        + '</span><strong>'
        + html.escape(value)
        + '</strong><small>'
        + html.escape(note)
        + "</small></div>"
    )


def write_html(report: Dict[str, object], path: Path) -> None:
    curves = report["curves"]
    left = curves["left_before_deg"]
    right = curves["right_before_deg"]
    common = curves["coherent_common_vs_ideal_deg"]
    correction = curves["correction_deg"]
    left_after = curves["left_after_deg"]
    right_after = curves["right_after_deg"]
    before_svg = svg_chart(
        "No linealidad medida antes de corregir",
        (("Izquierda", "#ff9f43", left), ("Derecha", "#38bdf8", right),
         ("Componente coherente", "#e879f9", common)),
    )
    after_svg = svg_chart(
        "Residual después de aplicar la LUT",
        (("Izquierda corregida", "#ff9f43", left_after),
         ("Derecha corregida", "#38bdf8", right_after),
         ("Corrección aplicada", "#34d399", correction)),
    )
    combined_before = report["metrics"]["combined_before"]
    combined_after = report["metrics"]["combined_after"]
    hysteresis = report["metrics"]["hysteresis"]
    mapping = report["mapping_validation"]
    cards = "".join(
        [
            metric_card("RMSE antes", f'{combined_before["rmse_deg"]:.3f}°'),
            metric_card("RMSE después", f'{combined_after["rmse_deg"]:.3f}°'),
            metric_card(
                "Mejora RMSE",
                f'{report["metrics"]["combined_rmse_improvement_percent"]:.1f}%'
            ),
            metric_card("Histéresis RMS", f'{hysteresis["rms_deg"]:.3f}°'),
            metric_card(
                "Mapa 4096 cuentas",
                "Monótono" if mapping["strictly_monotonic"] else "NO MONÓTONO",
                f'paso mínimo {mapping["min_increment_deg_per_count"]:.5f}°',
            ),
            metric_card(
                "Escala de corrección",
                f'{100.0 * report["correction_scale"]:.1f}%'
            ),
        ]
    )
    direction_rows = []
    for phase in PHASES:
        item = report["directions"][phase]
        direction_rows.append(
            "<tr>"
            f'<td>{html.escape(phase)}</td><td>{item["complete_turns"]}</td>'
            f'<td>{item["speed"]["signed_mean_deg_s"]:.3f}</td>'
            f'<td>{item["speed"]["coefficient_of_variation_percent"]:.2f}%</td>'
            f'<td>{item["repeatability"]["node_sigma_rms_deg"]:.3f}°</td>'
            f'<td>{item["before"]["rmse_deg"]:.3f}°</td>'
            f'<td>{item["after"]["rmse_deg"]:.3f}°</td>'
            "</tr>"
        )
    warning = ""
    if report["limitations"]:
        warning = '<div class="warning"><strong>Advertencias</strong><ul>' + "".join(
            f"<li>{html.escape(item)}</li>" for item in report["limitations"]
        ) + "</ul></div>"

    document = f"""<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Linealidad AS5600 — 7 vueltas</title>
<style>
:root{{--bg:#08111f;--panel:#101d30;--text:#e8f0fb;--muted:#9fb0c8;--line:#2b3b52}}
*{{box-sizing:border-box}} body{{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,sans-serif}}
main{{max-width:1120px;margin:auto;padding:28px}} h1{{margin:0 0 5px;font-size:29px}} h2{{margin:28px 0 10px}}
.subtitle,.label,small{{color:var(--muted)}} .cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:10px;margin:20px 0}}
.card,.panel,.warning{{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px}}
.card span,.card strong,.card small{{display:block}} .card strong{{font-size:22px;margin:4px 0}}
.panel{{overflow:auto;margin:14px 0}} svg{{display:block;min-width:760px;width:100%;background:#0b1728;border-radius:8px}}
.plot{{fill:#081321;stroke:#34455e}} .grid{{stroke:#263750;stroke-width:1}} .axis,.legend{{fill:#aebed2;font-size:12px}}
.axis-label,.chart-title{{fill:#e8f0fb;font-size:14px}} .chart-title{{font-weight:700;font-size:16px}}
table{{border-collapse:collapse;width:100%;min-width:760px}} th,td{{padding:9px;border-bottom:1px solid var(--line);text-align:right}}
th:first-child,td:first-child{{text-align:left}} .warning{{border-color:#9a6b24;background:#2a2113}} code{{color:#a7f3d0}}
footer{{color:var(--muted);margin-top:24px}} @media print{{body{{background:#fff;color:#111}}.panel,.card{{break-inside:avoid}}}}
</style></head><body><main>
<h1>Linealidad del AS5600</h1>
<div class="subtitle">Ensayo a comando constante · siete vueltas por sentido · LUT periódica de 128 nodos</div>
<div class="cards">{cards}</div>{warning}
<section class="panel">{before_svg}</section>
<section class="panel">{after_svg}</section>
<h2>Calidad de la medición</h2><div class="panel"><table><thead><tr><th>Fase</th><th>Vueltas</th><th>Velocidad °/s</th><th>CV velocidad</th><th>Repetibilidad RMS</th><th>RMSE antes</th><th>RMSE después</th></tr></thead>
<tbody>{''.join(direction_rows)}</tbody></table></div>
<h2>Cómo se obtuvo</h2>
<p>Cada vuelta se delimitó interpolando cruces equivalentes sobre el desplazamiento ajustado de forma monótona. Dentro de cada vuelta, el tiempo se convirtió en un ángulo ideal a velocidad constante. Las siete curvas se combinaron con una mediana robusta. La corrección usa únicamente armónicos con fase y amplitud coherentes en ambos sentidos; lo demás se reporta como histéresis o falta de repetibilidad.</p>
<p>La LUT exportada está en <code>as5600_linearity_lut.h</code>, en centésimas de grado, y fue validada cuenta por cuenta para que el mapa corregido de 4096 posiciones sea estrictamente creciente.</p>
<footer>Entrada SHA-256: {html.escape(report['input']['sha256'])}<br>Informe autocontenido; no necesita Internet.</footer>
</main></body></html>"""
    path.write_text(document, encoding="utf-8")


def write_header(report: Dict[str, object], path: Path) -> None:
    values = report["lut_centideg"]
    correction_source = report.get("correction_source", {})
    calibration_sha = correction_source.get("calibration_input_sha256") or report["input"]["sha256"]
    validation_note = ""
    if correction_source.get("mode") == "external_validation":
        validation_note = (
            f"// Independently validated with CSV SHA-256: {report['input']['sha256']}\n"
        )
    lines = []
    for offset in range(0, len(values), 8):
        lines.append("  " + ", ".join(f"{value:6d}" for value in values[offset:offset + 8]) + ",")
    contents = f"""// Generated by tools/analyze_as5600_linearity.py
// Calibration CSV SHA-256: {calibration_sha}
{validation_note}//
//
// Each entry is a signed correction in centidegrees (0.01 degree), sampled
// every 32 raw AS5600 counts.  Interpolate cyclically between adjacent nodes.
// corrected_deg = raw * 360 / 4096 + interpolated_correction_centideg / 100.
// The complete 4096-count mapping was verified strictly monotonic.
#ifndef AS5600_LINEARITY_LUT_H
#define AS5600_LINEARITY_LUT_H

#include <stdint.h>

static constexpr uint16_t AS5600_LINEARITY_LUT_SIZE = {LUT_NODES};
static constexpr uint8_t AS5600_LINEARITY_COUNTS_PER_NODE = {RAW_COUNTS // LUT_NODES};
static constexpr float AS5600_LINEARITY_CENTIDEG_TO_DEG = 0.01f;

static constexpr int16_t AS5600_LINEARITY_LUT_CENTIDEG[AS5600_LINEARITY_LUT_SIZE] = {{
{chr(10).join(lines)}
}};

static inline float as5600LinearityCorrectionDeg(uint16_t rawAngle) {{
  rawAngle &= 0x0FFFu;
  const uint8_t index = rawAngle >> 5;
  const uint8_t next = (index + 1u) & 0x7Fu;
  const float fraction = float(rawAngle & 0x1Fu) / 32.0f;
  const float a = float(AS5600_LINEARITY_LUT_CENTIDEG[index]);
  const float b = float(AS5600_LINEARITY_LUT_CENTIDEG[next]);
  return (a + (b - a) * fraction) * AS5600_LINEARITY_CENTIDEG_TO_DEG;
}}

static inline float as5600CorrectedAngleDeg(uint16_t rawAngle) {{
  float value = float(rawAngle & 0x0FFFu) * (360.0f / 4096.0f) +
                as5600LinearityCorrectionDeg(rawAngle);
  if (value < 0.0f) value += 360.0f;
  if (value >= 360.0f) value -= 360.0f;
  return value;
}}

#endif  // AS5600_LINEARITY_LUT_H
"""
    path.write_text(contents, encoding="utf-8")


def load_validation_lut(report_path: Path) -> Tuple[List[int], Dict[str, object]]:
    """Load and validate a LUT from a previous analyzer JSON report."""
    try:
        raw_bytes = report_path.read_bytes()
        previous = json.loads(raw_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AnalysisError(f"cannot read validation report {report_path}: {exc}") from exc
    values = previous.get("lut_centideg")
    if not isinstance(values, list) or len(values) != LUT_NODES:
        raise AnalysisError(
            f"validation report LUT must contain exactly {LUT_NODES} nodes"
        )
    lut: List[int] = []
    for index, value in enumerate(values):
        if isinstance(value, bool) or not isinstance(value, int):
            raise AnalysisError(f"validation LUT node {index} is not an integer")
        if not -32768 <= value <= 32767:
            raise AnalysisError(f"validation LUT node {index} exceeds int16")
        lut.append(value)
    validation = validate_mapping(lut)
    if not validation["strictly_monotonic"]:
        raise AnalysisError("external validation LUT is not strictly monotonic")
    return lut, {
        "mode": "external_validation",
        "path": str(report_path.resolve()),
        "sha256": hashlib.sha256(raw_bytes).hexdigest(),
        "calibration_input_sha256": previous.get("input", {}).get("sha256"),
        "original_correction_scale": previous.get("correction_scale", 1.0),
    }


def analyze_file(
    input_path: Path,
    out_dir: Path,
    external_lut: Optional[Sequence[int]] = None,
    external_lut_source: Optional[Dict[str, object]] = None,
) -> Dict[str, object]:
    phases, filter_stats = load_csv(input_path)
    left = analyze_direction("LEFT_2000_US", phases["LEFT_2000_US"])
    right = analyze_direction("RIGHT_1040_US", phases["RIGHT_1040_US"])
    common, harmonics = coherent_common_curve(left.curve, right.curve)
    requested_correction = inverse_correction_from_error(common)
    # In the LUT (measured-angle) coordinate, the correction is exactly the
    # negative of this coherent error component before safety scaling/rounding.
    common_vs_measured = [-value for value in requested_correction]
    candidate_lut, candidate_scale, candidate_mapping = make_safe_lut(
        requested_correction
    )
    if external_lut is None:
        lut_centideg = candidate_lut
        correction_scale = candidate_scale
        mapping = candidate_mapping
        correction_source: Dict[str, object] = {"mode": "fitted_from_input"}
    else:
        lut_centideg = list(external_lut)
        mapping = validate_mapping(lut_centideg)
        if not mapping["strictly_monotonic"]:
            raise AnalysisError("external validation LUT is not strictly monotonic")
        correction_source = dict(external_lut_source or {})
        correction_source.setdefault("mode", "external_validation")
        correction_scale = float(
            correction_source.get("original_correction_scale", 1.0)
        )
    correction_deg = [value * CENTIDEGREE for value in lut_centideg]
    left_after_curve = [
        error + interpolate_nodes(correction_deg, angle + error)
        for angle, error in zip(
            [index * NODE_STEP_DEG for index in range(LUT_NODES)], left.curve
        )
    ]
    right_after_curve = [
        error + interpolate_nodes(correction_deg, angle + error)
        for angle, error in zip(
            [index * NODE_STEP_DEG for index in range(LUT_NODES)], right.curve
        )
    ]

    direction_items = {
        left.phase: direction_report(left, correction_deg),
        right.phase: direction_report(right, correction_deg),
    }
    combined_before_values = left.sample_error + right.sample_error
    combined_after_values = [
        error + interpolate_nodes(correction_deg, measured_phase)
        for direction in (left, right)
        for measured_phase, error in zip(
            direction.sample_measured_phase, direction.sample_error
        )
    ]
    combined_before = error_metrics(combined_before_values)
    combined_after = error_metrics(combined_after_values)
    hysteresis_values = [a - b for a, b in zip(left.curve, right.curve)]
    limitations: List[str] = []
    if external_lut is not None:
        limitations.append(
            "Modo validación: las métricas posteriores usan una LUT externa; no se reajustó con este CSV."
        )
    for direction in (left, right):
        if len(direction.turns) < EXPECTED_TURNS:
            limitations.append(
                f"{direction.phase}: solo hay {len(direction.turns)} de {EXPECTED_TURNS} vueltas completas."
            )
        if direction_items[direction.phase]["speed"]["coefficient_of_variation_percent"] > 3.0:
            limitations.append(
                f"{direction.phase}: la velocidad por vuelta varía más de 3 %."
            )
    if correlation(left.curve, right.curve) < 0.5:
        limitations.append(
            "La correlación entre sentidos es baja; la LUT conserva únicamente los armónicos coincidentes."
        )
    limitations.append(
        "Sin una referencia angular externa, la curva coherente puede incluir ondulación mecánica o de velocidad, además del encoder/imán."
    )

    raw_bytes = input_path.read_bytes()
    report: Dict[str, object] = {
        "format_version": 1,
        "input": {
            "path": str(input_path.resolve()),
            "sha256": hashlib.sha256(raw_bytes).hexdigest(),
            "bytes": len(raw_bytes),
            "filtering": filter_stats,
        },
        "method": {
            "expected_turns_per_direction": EXPECTED_TURNS,
            "lut_nodes": LUT_NODES,
            "node_step_deg": NODE_STEP_DEG,
            "reference": "constant speed independently between interpolated same-phase crossings of every revolution",
            "turn_aggregation": "node-wise median after per-turn DC removal",
            "periodic_smoothing": "two passes of [1,4,6,4,1]/16",
            "coherence_rule": "harmonics 1..16: phase cosine >= 0.5 and amplitude ratio >= 0.25",
            "correction_units": "signed centidegrees; cyclic linear interpolation every 32 raw counts",
            "curve_coordinates": "error curves use ideal angle; correction LUT uses measured/raw angle",
        },
        "directions": direction_items,
        "metrics": {
            "combined_before": combined_before,
            "combined_after": combined_after,
            "combined_rmse_improvement_percent": (
                100.0 * (1.0 - combined_after["rmse_deg"] / combined_before["rmse_deg"])
                if combined_before["rmse_deg"]
                else 0.0
            ),
            "direction_curve_correlation": correlation(left.curve, right.curve),
            "hysteresis": {
                "rms_deg": rms(hysteresis_values),
                "peak_to_peak_deg": max(hysteresis_values) - min(hysteresis_values),
                "max_abs_deg": max(abs(value) for value in hysteresis_values),
            },
        },
        "harmonics": harmonics,
        "correction_source": correction_source,
        "correction_scale": correction_scale,
        "mapping_validation": mapping,
        "lut_centideg": lut_centideg,
        "candidate_refit": {
            "lut_centideg": candidate_lut,
            "correction_scale": candidate_scale,
            "mapping_validation": candidate_mapping,
            "rms_difference_from_applied_deg": rms(
                [
                    (candidate - applied) * CENTIDEGREE
                    for candidate, applied in zip(candidate_lut, lut_centideg)
                ]
            ),
        },
        "curves": {
            "angle_deg": [index * NODE_STEP_DEG for index in range(LUT_NODES)],
            "left_before_deg": left.curve,
            "right_before_deg": right.curve,
            # Backward/simple name uses the LUT coordinate, so by definition
            # requested_correction_deg[i] == -coherent_common_deg[i].
            "coherent_common_deg": common_vs_measured,
            "coherent_common_vs_ideal_deg": common,
            "coherent_common_vs_measured_deg": common_vs_measured,
            "requested_correction_deg": requested_correction,
            "correction_deg": correction_deg,
            "left_after_deg": left_after_curve,
            "right_after_deg": right_after_curve,
            "left_repeatability_sigma_deg": left.node_repeatability,
            "right_repeatability_sigma_deg": right.node_repeatability,
        },
        "limitations": limitations,
        "outputs": {
            "json": "as5600_linearity_report.json",
            "header": "as5600_linearity_lut.h",
            "html": "as5600_linearity_report.html",
        },
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / report["outputs"]["json"]
    header_path = out_dir / report["outputs"]["header"]
    html_path = out_dir / report["outputs"]["html"]
    json_path.write_text(
        json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    write_header(report, header_path)
    write_html(report, html_path)
    return report


def synthetic_error(angle_deg: float) -> float:
    radians = math.radians(angle_deg)
    return (
        1.65 * math.sin(radians + 0.24)
        + 0.58 * math.sin(3.0 * radians - 0.47)
        + 0.22 * math.cos(5.0 * radians + 0.19)
    )


def write_synthetic_csv(path: Path) -> None:
    fields = [
        "time_ms", "raw_angle", "angle_deg", "servo_us", "phase",
        "i2c_valid", "valid", "glitch", "status", "unwrapped_deg",
        "phase_displacement_deg",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        elapsed = 0.0
        global_index = 0
        for phase, sign, servo, start_angle, points_per_turn in (
            ("LEFT_2000_US", -1, 2000, 37.0, 143),
            ("RIGHT_1040_US", 1, 1040, 37.0, 157),
        ):
            start_measured = start_angle + synthetic_error(start_angle)
            previous_observed = start_measured
            unwrapped_observed = start_measured
            total_points = EXPECTED_TURNS * points_per_turn + 5
            for point in range(total_points):
                true_travel = point * 360.0 / points_per_turn
                true_angle = start_angle + sign * true_travel
                direction_ripple = sign * 0.12 * math.sin(
                    2.0 * math.radians(true_angle) + 0.31
                )
                deterministic_noise = 0.025 * math.sin(point * 1.731 + (0.3 if sign > 0 else 0.0))
                observed_continuous = (
                    true_angle + synthetic_error(true_angle) + direction_ripple + deterministic_noise
                )
                if point == 0:
                    unwrapped_observed = observed_continuous
                else:
                    # The synthetic continuous model is already unwrapped.
                    unwrapped_observed += observed_continuous - previous_observed
                previous_observed = observed_continuous
                observed_mod = observed_continuous % 360.0
                raw = int(round(observed_mod / 360.0 * RAW_COUNTS)) % RAW_COUNTS
                is_invalid = global_index > 0 and global_index % 401 == 0
                is_glitch = global_index > 0 and global_index % 577 == 0
                writer.writerow(
                    {
                        "time_ms": f"{elapsed:.3f}",
                        "raw_angle": -1 if is_invalid else ((raw + 1300) % RAW_COUNTS if is_glitch else raw),
                        "angle_deg": -1 if is_invalid else f"{raw * RAW_STEP_DEG:.5f}",
                        "servo_us": servo,
                        "phase": phase,
                        "i2c_valid": 0 if is_invalid else 1,
                        "valid": 0 if (is_invalid or is_glitch) else 1,
                        "glitch": 1 if is_glitch else 0,
                        "status": 32,
                        "unwrapped_deg": f"{unwrapped_observed:.6f}",
                        "phase_displacement_deg": f"{unwrapped_observed - start_measured:.6f}",
                    }
                )
                # Constant phase speed, with acquisition timing jitter that does
                # not affect the physical angle/time law appreciably.
                elapsed += 40.0 + 0.15 * math.sin(global_index * 0.37)
                global_index += 1
            elapsed += 1000.0


def run_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="as5600-linearity-selftest-") as temp:
        root = Path(temp)
        csv_path = root / "synthetic.csv"
        out_dir = root / "report"
        write_synthetic_csv(csv_path)
        report = analyze_file(csv_path, out_dir)
        before = report["metrics"]["combined_before"]["rmse_deg"]
        after = report["metrics"]["combined_after"]["rmse_deg"]
        if not before > 0.8:
            raise AssertionError(f"synthetic before RMSE unexpectedly low: {before}")
        if not after < before * 0.45:
            raise AssertionError(
                f"correction did not sufficiently improve synthetic data: {before} -> {after}"
            )
        if not report["mapping_validation"]["strictly_monotonic"]:
            raise AssertionError("synthetic LUT is not monotonic")
        for phase in PHASES:
            if report["directions"][phase]["complete_turns"] != EXPECTED_TURNS:
                raise AssertionError(f"{phase} did not retain seven turns")
        for filename in report["outputs"].values():
            if not (out_dir / filename).is_file():
                raise AssertionError(f"missing self-test output {filename}")
        filtering = report["input"]["filtering"]
        if filtering["rejected_i2c"] < 1 or filtering["rejected_glitch"] < 1:
            raise AssertionError("synthetic invalid/glitch filters were not exercised")
        common = report["curves"]["coherent_common_deg"]
        requested = report["curves"]["requested_correction_deg"]
        if max(abs(a + b) for a, b in zip(common, requested)) > 1.0e-12:
            raise AssertionError("requested correction is not exactly -common")
        parsed_json = json.loads((out_dir / report["outputs"]["json"]).read_text(encoding="utf-8"))
        if parsed_json["mapping_validation"]["invalid_increment_count"] != 0:
            raise AssertionError("serialized mapping validation changed")
        rendered_html = (out_dir / report["outputs"]["html"]).read_text(encoding="utf-8")
        if rendered_html.count("<svg ") != 2 or "Residual después" not in rendered_html:
            raise AssertionError("HTML/SVG report is incomplete")
        rendered_header = (out_dir / report["outputs"]["header"]).read_text(encoding="utf-8")
        if "int16_t AS5600_LINEARITY_LUT_CENTIDEG" not in rendered_header:
            raise AssertionError("C++ LUT header is incomplete")
        print(
            "SELF-TEST OK: "
            f"combined RMSE {before:.4f} deg -> {after:.4f} deg, "
            f"correction scale {report['correction_scale']:.3f}, "
            f"min map step {report['mapping_validation']['min_increment_deg_per_count']:.6f} deg"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Analyze the AS5600 seven-turn bidirectional linearity CSV."
    )
    parser.add_argument("input", nargs="?", type=Path, help="input CSV file")
    parser.add_argument(
        "--out-dir", type=Path, default=Path("as5600-linearity-analysis"),
        help="output directory (default: %(default)s)",
    )
    parser.add_argument(
        "--validation-report", type=Path,
        help=(
            "apply the lut_centideg from a previous JSON report instead of "
            "refitting the correction; intended for an independent second CSV"
        ),
    )
    parser.add_argument(
        "--self-test", action="store_true",
        help="run a deterministic synthetic end-to-end test and exit",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            run_self_test()
            return 0
        if args.input is None:
            raise AnalysisError("an input CSV is required (or use --self-test)")
        external_lut = None
        external_source = None
        if args.validation_report is not None:
            external_lut, external_source = load_validation_lut(
                args.validation_report
            )
        report = analyze_file(
            args.input, args.out_dir, external_lut, external_source
        )
        before = report["metrics"]["combined_before"]["rmse_deg"]
        after = report["metrics"]["combined_after"]["rmse_deg"]
        print(f"Analysis complete: {args.out_dir.resolve()}")
        print(f"Combined RMSE: {before:.4f} deg -> {after:.4f} deg")
        print(f"Correction source: {report['correction_source']['mode']}")
        print(
            "LUT mapping: "
            + ("strictly monotonic" if report["mapping_validation"]["strictly_monotonic"] else "NOT monotonic")
        )
        return 0
    except (AnalysisError, OSError, csv.Error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
