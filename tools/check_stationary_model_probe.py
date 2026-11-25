#!/usr/bin/env python3
"""Independently verify the native stationary-model JSONL probe with NumPy."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


PROFILE_COUNTS = {
    "selected": (96, 96),
    "broader": (36, 32),
    "sensitivity": (108, 108),
    "control": (8, 0),
    "constant": (3, 3),
    "nyquist": (1, 0),
    "size_rate": (21, 21),
    "uniform_scale": (1, 1),
    "decimation_boundary": (8, 0),
    "alias_search": (7, 6),
}
DENSE_PROFILE_COUNTS = {
    "dense_nearby": (12, 12),
    "dense_off_bin": (12, 12),
    "dense_heldout_change": (3, 0),
}


def reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"nonstandard/nonfinite JSON value: {value}")


def vector(row: dict, key: str, case: int) -> np.ndarray:
    try:
        result = np.asarray(row[key], dtype=np.float64)
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"case {case}: invalid {key}: {error}") from error
    if result.ndim != 1 or not np.all(np.isfinite(result)):
        raise ValueError(f"case {case}: {key} must be a finite one-dimensional array")
    return result


def finite_scalar(row: dict, key: str, case: int) -> float:
    value = row.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"case {case}: {key} must be finite")
    return float(value)


def expected_guides(samples: np.ndarray, rate: float) -> tuple[np.ndarray, np.ndarray]:
    count = len(samples)
    training = 3 * count // 4
    source = samples[:training]
    scale = float(np.max(np.abs(source))) or 1.0
    normalized = source / scale

    mean = float(np.mean(normalized))
    if max(mean - float(np.min(normalized)), float(np.max(normalized)) - mean) < 1e-12 / scale:
        return np.empty(0), np.empty(0)

    indices = np.arange(training, dtype=np.float64)
    window = 0.5 - 0.5 * np.cos(2 * np.pi * indices / training)
    magnitudes = np.abs(np.fft.rfft(normalized * window))
    bins = np.arange(1, training // 2, dtype=np.int64)
    tolerance = 64 * np.finfo(float).eps * training
    peaks = bins[(magnitudes[1:-1] > magnitudes[:-2] + tolerance)
                 & (magnitudes[1:-1] >= magnitudes[2:] - tolerance)]
    if peaks.size == 0:
        return np.empty(0), np.empty(0)

    strongest = float(np.max(magnitudes[peaks]))
    ranked = sorted((int(index) for index in peaks), key=lambda index: (-magnitudes[index], index))[:16]
    selected = sorted(index for index in ranked if magnitudes[index] >= 0.01 * strongest)
    return np.asarray(selected, dtype=np.float64), magnitudes[selected]


def design_matrix(count: int, frequencies: np.ndarray, rate: float) -> np.ndarray:
    if frequencies.size == 0:
        return np.ones((count, 1), dtype=np.float64)
    angle = (2 * np.pi * np.arange(count, dtype=np.float64)[:, None]
             * (frequencies / rate)[None, :])
    return np.column_stack((np.ones(count), np.cos(angle), np.sin(angle)))


def verify_row(row: dict, case: int, maxima: dict[str, float], groups: dict[str, list[int]]) -> None:
    name = row.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError(f"case {case}: missing profile name")
    group = groups.setdefault(name, [0, 0])
    group[0] += 1

    if row.get("case") != case:
        raise ValueError(f"case {case}: unexpected case index {row.get('case')!r}")
    for key in ("expected", "accepted", "phase_gate"):
        if not isinstance(row.get(key), bool):
            raise ValueError(f"case {case} {name}: {key} must be boolean")
    expected = row["expected"]
    accepted = row["accepted"]
    group[1] += int(accepted)
    if accepted != expected:
        raise ValueError(f"case {case} {name}: accepted={accepted}, expected={expected}")

    samples = vector(row, "samples", case)
    count = len(samples)
    if count < 1024 or count > 65536 or count & (count - 1):
        raise ValueError(f"case {case} {name}: invalid sample count {count}")
    rate = finite_scalar(row, "rate", case)
    center = finite_scalar(row, "center", case)
    units = finite_scalar(row, "units", case)
    dc_truth = finite_scalar(row, "dc_truth", case)
    if rate <= 0 or units <= 0:
        raise ValueError(f"case {case} {name}: rate and units must be positive")

    truth = vector(row, "truth", case)
    amplitudes = vector(row, "amplitudes", case)
    phases = vector(row, "phases", case)
    frequencies = vector(row, "frequencies", case)
    coefficients = vector(row, "coefficients", case)
    native_guides = vector(row, "guides", case)
    native_magnitudes = vector(row, "guide_magnitudes", case)
    for key in ("training_rms", "validation_rms"):
        finite_scalar(row, key, case)

    if truth.size != amplitudes.size or truth.size != phases.size:
        raise ValueError(f"case {case} {name}: truth/amplitude/phase lengths differ")
    if native_guides.size != native_magnitudes.size or native_guides.size > 16:
        raise ValueError(f"case {case} {name}: malformed guide arrays")
    skipped_orders = row.get("skipped_alias_orders")
    assignments = row.get("assignments")
    if (isinstance(skipped_orders, bool) or not isinstance(skipped_orders, int) or skipped_orders < 0
            or isinstance(assignments, bool) or not isinstance(assignments, int) or assignments < 0):
        raise ValueError(f"case {case} {name}: malformed alias-search diagnostics")
    if name == "alias_search":
        if truth.size == 2 and assignments < 4:
            raise ValueError(f"case {case} {name}: two-partial alias search tried fewer than4 assignments")
        if truth.size == 3 and assignments < 27:
            raise ValueError(f"case {case} {name}: three-partial alias search tried fewer than27 assignments")
        if truth.size == 4 and assignments < 256:
            raise ValueError(f"case {case} {name}: four-partial alias search tried fewer than256 assignments")
        if truth.size == 5 and (accepted or skipped_orders == 0):
            raise ValueError(f"case {case} {name}: over-cap order lacks its explicit rejection diagnostic")

    guide_bins, guide_mags = expected_guides(samples, rate)
    training = 3 * count // 4
    scale = float(np.max(np.abs(samples[:training]))) or 1.0
    native_bins = native_guides * (training / rate)
    rounded_native_bins = np.rint(native_bins)
    if np.any(np.abs(native_bins - rounded_native_bins) > 1e-7):
        raise ValueError(f"case {case} {name}: native guide is not on a training FFT bin")
    native_order = np.argsort(rounded_native_bins, kind="stable")
    expected_order = np.argsort(guide_bins, kind="stable")
    if not np.array_equal(rounded_native_bins[native_order].astype(np.int64), guide_bins[expected_order].astype(np.int64)):
        raise ValueError(f"case {case} {name}: FFT guide bins differ from direct NumPy spectrum")
    guide_error = float(np.max(np.abs(native_magnitudes[native_order] - guide_mags[expected_order]), initial=0.0))
    if guide_error > 1e-9:
        raise ValueError(f"case {case} {name}: normalized FFT guide magnitude error {guide_error:.12g} > 1e-9")
    maxima["guide_magnitude"] = max(maxima["guide_magnitude"], guide_error)

    if not expected:
        if frequencies.size != 0 or coefficients.shape != (1,) or coefficients[0] != 0:
            raise ValueError(f"case {case} {name}: rejected model exposed fitted output")
        if row["training_rms"] != 0 or row["validation_rms"] != 0:
            raise ValueError(f"case {case} {name}: rejected model exposed diagnostics")
        return

    if frequencies.size != truth.size or coefficients.size != 1 + 2 * frequencies.size:
        raise ValueError(f"case {case} {name}: accepted output dimensions differ from truth")
    if frequencies.size and (np.any(frequencies <= 0) or np.any(frequencies >= rate / 2)
                             or np.any(np.diff(frequencies) < 1e-6)):
        raise ValueError(f"case {case} {name}: fitted frequencies are not ordered inside Nyquist")
    if not np.all(np.isfinite(frequencies)) or not np.all(np.isfinite(coefficients)):
        raise ValueError(f"case {case} {name}: fitted output is nonfinite")

    frequency_error = float(np.max(np.abs(frequencies - truth), initial=0.0))
    if frequency_error > 0.02:
        raise ValueError(f"case {case} {name}: frequency error {frequency_error:.12g} > 0.02 Hz")
    maxima["frequency_hz"] = max(maxima["frequency_hz"], frequency_error)

    amplitude_error = 0.0
    for index, amplitude in enumerate(amplitudes):
        cosine = coefficients[1 + index]
        sine = coefficients[1 + frequencies.size + index]
        measured = math.hypot(cosine, sine) / units
        error = abs(measured / amplitude - 1.0)
        amplitude_error = max(amplitude_error, error)
    if amplitude_error > 0.002:
        raise ValueError(f"case {case} {name}: relative amplitude error {amplitude_error:.12g} > 0.002")
    maxima["relative_amplitude"] = max(maxima["relative_amplitude"], amplitude_error)

    dc_error = abs(coefficients[0] / units - dc_truth)
    if dc_error > 1e-6:
        raise ValueError(f"case {case} {name}: DC error {dc_error:.12g} > 1e-6")
    maxima["dc"] = max(maxima["dc"], dc_error)

    if row["phase_gate"]:
        phase_error = 0.0
        for index, (source_frequency, source_phase) in enumerate(zip(truth, phases)):
            center_phase = math.atan2(coefficients[1 + index], coefficients[1 + frequencies.size + index])
            center_phase += 2 * math.pi * frequencies[index] * count / (2 * rate)
            target_phase = 2 * math.pi * source_frequency * center + source_phase
            difference = math.remainder(center_phase - target_phase, 2 * math.pi)
            phase_error = max(phase_error, abs(difference))
        if phase_error > 1e-4:
            raise ValueError(f"case {case} {name}: clean center phase error {phase_error:.12g} > 1e-4 rad")
        maxima["phase_rad"] = max(maxima["phase_rad"], phase_error)

    normalized = samples / scale
    design = design_matrix(count, frequencies, rate)
    reference, _, rank, _ = np.linalg.lstsq(design[:training], normalized[:training], rcond=None)
    if rank != design.shape[1]:
        raise ValueError(f"case {case} {name}: NumPy reference design is rank deficient")
    normalized_coefficients = coefficients / scale
    coefficient_error = float(np.max(np.abs(normalized_coefficients - reference), initial=0.0))
    if coefficient_error > 1e-8:
        raise ValueError(f"case {case} {name}: normalized coefficient error {coefficient_error:.12g} > 1e-8")
    maxima["coefficient"] = max(maxima["coefficient"], coefficient_error)

    residual = normalized - design @ normalized_coefficients
    normalizer = max(float(np.std(normalized[:training])), 1e-12 / scale)
    training_rms = float(np.linalg.norm(residual[:training]) / math.sqrt(training) / normalizer)
    validation_rms = float(np.linalg.norm(residual[training:]) / math.sqrt(count - training) / normalizer)
    diagnostic_error = max(abs(training_rms - row["training_rms"]),
                           abs(validation_rms - row["validation_rms"]))
    if diagnostic_error > 1e-8:
        raise ValueError(f"case {case} {name}: residual diagnostic error {diagnostic_error:.12g} > 1e-8")
    maxima["diagnostic"] = max(maxima["diagnostic"], diagnostic_error)

    matrix = design[:training]
    normal_residual = float(np.linalg.norm(matrix.T @ residual[:training])
                            / (np.linalg.norm(matrix) * max(float(np.linalg.norm(normalized[:training])), 1.0)))
    if normal_residual > 1e-8:
        raise ValueError(f"case {case} {name}: relative normal-equation residual {normal_residual:.12g} > 1e-8")
    maxima["normal_residual"] = max(maxima["normal_residual"], normal_residual)

    if frequencies.size == 0:
        physical_peak_error = float(np.max(np.abs(residual), initial=0.0) * scale)
        if physical_peak_error >= 1e-12:
            raise ValueError(f"case {case} {name}: constant-model residual {physical_peak_error:.12g} is not below 1e-12")
    else:
        component_ratio = float(np.linalg.norm(normalized_coefficients[1:]) / math.sqrt(2) / normalizer)
        if training_rms > 1e-4 or validation_rms > 1e-4 or component_ratio > 4:
            raise ValueError(f"case {case} {name}: accepted fit violates native residual/cancellation gates")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--input", type=Path, required=True, help="JSONL emitted by the native --dump probe")
    parser.add_argument("--profiles", choices=("baseline", "dense"), default="baseline")
    arguments = parser.parse_args()
    profile_counts = DENSE_PROFILE_COUNTS if arguments.profiles == "dense" else PROFILE_COUNTS

    maxima = {name: 0.0 for name in (
        "frequency_hz", "relative_amplitude", "dc", "phase_rad", "guide_magnitude",
        "coefficient", "diagnostic", "normal_residual",
    )}
    groups: dict[str, list[int]] = {}
    cases = accepted_count = 0
    with arguments.input.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                raise ValueError(f"line {line_number}: blank JSONL record")
            try:
                row = json.loads(line, parse_constant=reject_nonfinite_json)
            except json.JSONDecodeError as error:
                raise ValueError(f"line {line_number}: invalid JSON: {error}") from error
            if not isinstance(row, dict):
                raise ValueError(f"line {line_number}: each record must be an object")
            verify_row(row, cases, maxima, groups)
            accepted_count += int(row["accepted"])
            cases += 1

    expected_cases = sum(counts[0] for counts in profile_counts.values())
    expected_acceptances = sum(counts[1] for counts in profile_counts.values())
    if cases != expected_cases or accepted_count != expected_acceptances:
        raise ValueError(f"expected {expected_cases} profiles and{expected_acceptances} acceptances, got {cases} and{accepted_count}")
    if set(groups) != set(profile_counts):
        raise ValueError(f"profile groups differ: got {sorted(groups)}")
    for name, (expected_total, expected_accepted) in profile_counts.items():
        actual_total, actual_accepted = groups[name]
        if (actual_total, actual_accepted) != (expected_total, expected_accepted):
            raise ValueError(f"group {name}: got{actual_total}/{actual_accepted}, expected{expected_total}/{expected_accepted}")

    print(f"Native/NumPy stationary-model probe: {cases}/{cases} PASS ({accepted_count} accepted, {cases - accepted_count} rejected)")
    for name in sorted(profile_counts):
        total, accepted = groups[name]
        print(f"{name}: {accepted}/{total} accepted")
    for name, value in maxima.items():
        print(f"Maximum {name}: {value:.12g}")
    print(f"NumPy version: {np.__version__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
