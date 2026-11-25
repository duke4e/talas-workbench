#!/usr/bin/env python3
"""Independently verify native fixed-frequency and refined fits with NumPy."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def columns(count: int, frequencies: np.ndarray, rate: float) -> np.ndarray:
    angle = np.arange(count)[:, None] * (2 * np.pi * (frequencies / rate))[None, :]
    return np.column_stack((np.ones(count), np.cos(angle), np.sin(angle)))


def frobenius_condition(singular: np.ndarray) -> float:
    return float(np.linalg.norm(singular) * np.linalg.norm(1 / singular)) if singular[-1] > 0 else float("inf")


def reference_refinement(samples: np.ndarray, initial: np.ndarray, rate: float, noise_floor: float):
    train = 3 * len(samples) // 4
    normalization = max(float(np.std(samples[:train])), noise_floor)
    frequencies = initial.copy()
    design = columns(len(samples), frequencies, rate)
    coefficients, _, rank, singular = np.linalg.lstsq(design[:train], samples[:train], rcond=None)
    if rank != design.shape[1] or frobenius_condition(singular) > 1e12:
        raise ValueError("reference initial fit is ill-conditioned")
    count = len(frequencies)
    if count == 0:
        return frequencies, coefficients
    for _ in range(3):
        residual = samples[:train] - design[:train] @ coefficients
        ratio = np.linalg.norm(coefficients[1:]) / np.sqrt(2) / normalization
        if np.linalg.norm(residual) / np.sqrt(train) / normalization < 1e-10 or ratio > 4:
            break
        derivative = (2 * np.pi * np.arange(train) / train)[:, None] * (
            -design[:train, 1 + count:] * coefficients[1:1 + count]
            + design[:train, 1:1 + count] * coefficients[1 + count:])
        jacobian = np.column_stack((design[:train], derivative))
        delta, _, rank, singular = np.linalg.lstsq(jacobian, residual, rcond=None)
        if rank != jacobian.shape[1] or frobenius_condition(singular) > 1e12:
            break
        step = delta[-count:]
        step *= min(1.0, .5 / max(float(np.max(np.abs(step))), 1e-20))
        trial = frequencies + step * rate / train
        if (np.any(trial <= 0) or np.any(trial >= rate / 2) or np.any(np.diff(trial) < 1e-6)
                or np.max(np.abs(trial - initial)) > rate / train):
            break
        trial_design = columns(len(samples), trial, rate)
        trial_coefficients, _, rank, singular = np.linalg.lstsq(trial_design[:train], samples[:train], rcond=None)
        if rank != trial_design.shape[1] or frobenius_condition(singular) > 1e12:
            break
        trial_residual = samples[:train] - trial_design[:train] @ trial_coefficients
        if np.dot(trial_residual, trial_residual) >= np.dot(residual, residual):
            break
        frequencies, design, coefficients = trial, trial_design, trial_coefficients
    return frequencies, coefficients


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--input", type=Path, required=True)
    arguments = parser.parse_args()
    maxima = {name: 0.0 for name in ("coefficient", "diagnostic", "normal_residual", "condition_relative", "refined_frequency_hz", "refined_coefficient")}
    cases = 0
    for line in arguments.input.read_text().splitlines():
        row = json.loads(line)
        samples = np.asarray(row["samples"], dtype=float)
        frequencies = np.asarray(row["frequencies"], dtype=float)
        initial = np.asarray(row["initial"], dtype=float)
        actual = np.asarray(row["coefficients"], dtype=float)
        rate = row["rate"]
        if (not all(np.all(np.isfinite(value)) for value in (samples, frequencies, initial, actual))
                or actual.shape != (1 + 2 * len(frequencies),) or frequencies.shape != initial.shape):
            raise ValueError(f"case {cases}: malformed data")
        train = 3 * len(samples) // 4
        scale = float(np.max(np.abs(samples[:train]))) or 1.0
        normalized = samples / scale
        design = columns(len(samples), frequencies, rate)
        reference, _, rank, singular = np.linalg.lstsq(design[:train], normalized[:train], rcond=None)
        coefficient_error = float(np.max(np.abs(actual / scale - reference)))
        residual = normalized - design @ (actual / scale)
        normal_residual = float(np.linalg.norm(design[:train].T @ residual[:train])
                                / (np.linalg.norm(design[:train]) * max(np.linalg.norm(normalized[:train]), 1.0)))
        normalization = max(float(np.std(normalized[:train])), 1e-12 / scale)
        # Constant fits are checked directly; tiny numerical SVD error must not
        # be amplified by the zero-signal relative diagnostic's1e-12 floor.
        training_error = float(np.linalg.norm(residual[:train]) / np.sqrt(train) / normalization)
        validation_error = float(np.linalg.norm(residual[train:]) / np.sqrt(len(samples) - train) / normalization)
        component_ratio = float(np.linalg.norm(actual[1:] / scale) / np.sqrt(2) / normalization)
        diagnostic_error = max(abs(training_error - row["training_rms"]), abs(validation_error - row["validation_rms"]),
                               abs(component_ratio - row["component_ratio"]))
        condition = float(singular[0] / singular[-1])
        condition_error = abs(row["condition_bound"] / frobenius_condition(singular) - 1)
        if rank != design.shape[1] or condition > row["condition_bound"] * (1 + 1e-9):
            raise ValueError(f"case {cases}: invalid claimed condition upper bound")
        refined_frequencies, refined_coefficients = reference_refinement(normalized, initial, rate, 1e-12 / scale)
        refined_frequency_error = float(np.max(np.abs(frequencies - refined_frequencies), initial=0))
        refined_coefficient_error = float(np.max(np.abs(actual / scale - refined_coefficients)))
        errors = dict(coefficient=coefficient_error, diagnostic=diagnostic_error, normal_residual=normal_residual, condition_relative=condition_error,
                      refined_frequency_hz=refined_frequency_error, refined_coefficient=refined_coefficient_error)
        limits = dict(coefficient=1e-9, diagnostic=1e-9, normal_residual=1e-9, condition_relative=1e-9,
                      refined_frequency_hz=1e-5, refined_coefficient=1e-6)
        for key, value in errors.items():
            if not np.isfinite(value) or value > limits[key]:
                raise ValueError(f"case {cases} {row['name']}: {key}={value}, limit={limits[key]}")
            maxima[key] = max(maxima[key], value)
        cases += 1
    if cases != 68:
        raise ValueError(f"expected all68 declared fits, got {cases}")
    print(f"Native/NumPy fixed and refined sinusoid fits: {cases}/{cases} PASS")
    for key, value in maxima.items():
        print(f"Maximum {key}: {value:.12g}")
    print(f"NumPy version: {np.__version__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
