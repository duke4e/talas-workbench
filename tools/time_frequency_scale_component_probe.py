#!/usr/bin/env python3
"""Offline sinusoidal identifiability probe, not production DSP or an oracle.

Requires NumPy. Receives original-rate samples only; reference frequencies and
amplitudes appear exclusively in fixture generation and independent scoring.
The final quarter of each window is excluded from all parameter estimation.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path

import numpy as np


FS = 48000
WINDOW = 8192
MAX_PARTIALS = 8  # Diagnostic work cap, unrelated to the node's Detectors field.
MAX_CONDITION = 1e12
UNIT_ROOT_TOLERANCE = 1e-4
RELATIVE_RESIDUAL_LIMIT = 1e-4
MAX_COMPONENT_RMS_RATIO = 4.0
MAX_ALIAS_ASSIGNMENTS = 256
FREQUENCY_ERROR_LIMIT = 0.02
RELATIVE_AMPLITUDE_ERROR_LIMIT = 0.002
DC_ERROR_LIMIT = 1e-6


def sinusoid_columns(frames: np.ndarray, frequencies: np.ndarray) -> np.ndarray:
    angle = 2 * np.pi / FS * frames[:, None] * frequencies[None, :]
    return np.column_stack((np.ones(len(frames)), np.cos(angle), np.sin(angle)))


def estimate(samples: np.ndarray, method: str = "subspace-refined") -> dict:
    """Select the first stationary model that predicts unseen full-rate data."""
    training_count = 3 * len(samples) // 4
    training = samples[:training_count]
    validation = samples[training_count:]
    if not np.all(np.isfinite(samples)):
        return {"accepted": False, "reason": "nonfinite input"}
    frames = np.arange(len(samples))
    signal_rms = max(float(np.std(training)), 1e-12)
    attempts = []

    def fit(frequencies: np.ndarray, frequency_condition: float, root_error: float,
            refine: bool = False):
        columns = sinusoid_columns(frames, frequencies)
        coefficients, _, rank, singular = np.linalg.lstsq(
            columns[:training_count], training, rcond=None)
        fit_condition = float(singular[0] / singular[-1]) if singular[-1] > 0 else math.inf
        if rank != columns.shape[1] or fit_condition > MAX_CONDITION:
            return None
        refinement_steps = 0
        initial_frequencies = frequencies.copy()
        if refine:
            for _ in range(3):
                train_residual = training - columns[:training_count] @ coefficients
                if np.sqrt(np.mean(train_residual ** 2)) / signal_rms < 1e-10:
                    break
                count = len(frequencies)
                time_factor = 2 * np.pi * frames[:training_count, None] / FS
                derivative = time_factor * (
                    -columns[:training_count, 1 + count:] * coefficients[1:1 + count]
                    + columns[:training_count, 1:1 + count] * coefficients[1 + count:])
                jacobian = np.column_stack((columns[:training_count], derivative))
                delta, _, jacobian_rank, jacobian_singular = np.linalg.lstsq(
                    jacobian, train_residual, rcond=None)
                if (jacobian_rank != jacobian.shape[1]
                        or jacobian_singular[0] / jacobian_singular[-1] > MAX_CONDITION):
                    break
                frequency_step = delta[-count:]
                # Refinement is local to the selected alias, with bounded work.
                max_step = FS / training_count / 2
                frequency_step *= min(1.0, max_step / max(float(np.max(np.abs(frequency_step))), 1e-20))
                trial = frequencies + frequency_step
                if (np.any(trial <= 0) or np.any(trial >= FS / 2)
                        or np.any(np.diff(trial) < 1e-6)
                        or np.max(np.abs(trial - initial_frequencies)) > 2 * max_step):
                    break
                trial_columns = sinusoid_columns(frames, trial)
                trial_coefficients, _, trial_rank, trial_singular = np.linalg.lstsq(
                    trial_columns[:training_count], training, rcond=None)
                if (trial_rank != trial_columns.shape[1]
                        or trial_singular[0] / trial_singular[-1] > MAX_CONDITION):
                    break
                trial_residual = training - trial_columns[:training_count] @ trial_coefficients
                if np.sum(trial_residual ** 2) >= np.sum(train_residual ** 2):
                    break
                frequencies, columns, coefficients = trial, trial_columns, trial_coefficients
                fit_condition = float(trial_singular[0] / trial_singular[-1])
                refinement_steps += 1
        residual = samples - columns @ coefficients
        train_error = float(np.sqrt(np.mean(residual[:training_count] ** 2)) / signal_rms)
        held_out_error = float(np.sqrt(np.mean(residual[training_count:] ** 2)) / signal_rms)
        count = len(frequencies)
        amplitudes = np.hypot(coefficients[1:1 + count], coefficients[1 + count:])
        phases = np.arctan2(coefficients[1:1 + count], coefficients[1 + count:])
        component_rms_ratio = float(np.linalg.norm(amplitudes) / np.sqrt(2) / signal_rms)
        return {"accepted": True, "frequencies": frequencies.tolist(),
                "amplitudes": amplitudes.tolist(), "dc": float(coefficients[0]),
                "phases_at_start": phases.tolist(),
                "partials": count, "training_relative_rms": train_error,
                "held_out_relative_rms": held_out_error,
                "frequency_condition": frequency_condition, "fit_condition": fit_condition,
                "root_error": root_error, "component_rms_ratio": component_rms_ratio,
                "refinement_steps": refinement_steps}

    # An explicit constant model also avoids relative residuals of a zero signal.
    if np.max(np.abs(training - np.mean(training))) < 1e-12:
        constant = float(np.mean(training))
        if np.max(np.abs(validation - constant)) < 1e-12:
            return {"accepted": True, "frequencies": [], "amplitudes": [],
                    "phases_at_start": [],
                    "dc": constant, "partials": 0, "training_relative_rms": 0.0,
                    "held_out_relative_rms": 0.0, "frequency_condition": 1.0,
                    "fit_condition": 1.0, "root_error": 0.0}
        return {"accepted": False, "reason": "constant training with changed validation"}

    spectrum = np.abs(np.fft.rfft(training * np.hanning(training_count + 1)[:-1]))
    peak_indices = np.flatnonzero((spectrum[1:-1] > spectrum[:-2])
                                 & (spectrum[1:-1] >= spectrum[2:])) + 1
    if len(peak_indices):
        peak_indices = peak_indices[spectrum[peak_indices] >= .01 * np.max(spectrum[peak_indices])]
    peak_indices = peak_indices[np.argsort(spectrum[peak_indices])[-16:]]
    guides = peak_indices * FS / training_count
    if len(guides) == 0:
        return {"accepted": False, "reason": "no interior frequency guides"}

    stride = max(1, len(samples) // 256)
    decimated_rate = FS / stride
    differences = np.diff(training[::stride])
    if method.startswith("subspace"):
        # A lag matrix uses the observed sequence span to separate modes before
        # solving their one-step shift. Adjacent rows share the same modes.
        lagged = np.lib.stride_tricks.sliding_window_view(
            differences, len(differences) // 2).T
        subspace, mode_strengths, _ = np.linalg.svd(lagged, full_matrices=False)
    for order in range(2, 2 * MAX_PARTIALS + 1, 2):
        if method.startswith("subspace"):
            basis = subspace[:, :order]
            shift, _, rank, singular = np.linalg.lstsq(basis[:-1], basis[1:], rcond=None)
            condition = (float(mode_strengths[0] / mode_strengths[order - 1])
                         if mode_strengths[order - 1] > 0 else math.inf)
            condition = max(condition, float(singular[0] / singular[-1])
                            if singular[-1] > 0 else math.inf)
            roots = np.linalg.eigvals(shift)
        else:
            histories = np.lib.stride_tricks.sliding_window_view(differences, order + 1)
            # y[n] + a1*y[n-1] + ... + ap*y[n-p] = 0.
            coefficients, _, rank, singular = np.linalg.lstsq(
                histories[:, :-1][:, ::-1], -histories[:, -1], rcond=None)
            condition = float(singular[0] / singular[-1]) if singular[-1] > 0 else math.inf
            roots = np.roots(np.r_[1.0, coefficients])
        if rank != order or condition > MAX_CONDITION:
            attempts.append({"partials": order // 2, "reason": "frequency-solve conditioning",
                             "frequency_condition": condition if math.isfinite(condition) else None,
                             "rank": int(rank)})
            continue
        root_error = float(np.max(np.abs(np.abs(roots) - 1)))
        positive = roots[roots.imag > 1e-8]
        if len(positive) != order // 2 or root_error > UNIT_ROOT_TOLERANCE:
            attempts.append({"partials": order // 2, "reason": "nonstationary roots",
                             "root_error": root_error, "frequency_condition": condition})
            continue
        aliases = np.angle(positive) * decimated_rate / (2 * np.pi)
        candidate_lists = []
        for alias in aliases:
            candidates = np.concatenate([
                np.arange(stride // 2 + 1) * decimated_rate + sign * alias
                for sign in (1, -1)])
            candidates = candidates[(candidates > 0) & (candidates < FS / 2)]
            distances = np.min(np.abs(candidates[:, None] - guides[None, :]), axis=1)
            eligible = distances <= 2 * FS / training_count
            # A hidden partial need not have a nearby local maximum. Always
            # retain its nearest guide assignment, then validate the raw data.
            eligible[np.argmin(distances)] = True
            candidate_lists.append(candidates[eligible])
        assignments = math.prod(len(candidates) for candidates in candidate_lists)
        if assignments == 0 or assignments > MAX_ALIAS_ASSIGNMENTS:
            attempts.append({"partials": order // 2, "reason": "alias search bounds",
                             "assignments": assignments})
            continue
        best = None
        for candidates in itertools.product(*candidate_lists):
            frequencies = np.sort(candidates)
            if np.any(np.diff(frequencies) < 1e-6):
                continue
            fitted = fit(frequencies, condition, root_error)
            if fitted is not None and (best is None or fitted["training_relative_rms"]
                                       < best["training_relative_rms"]):
                best = fitted
        if best is None:
            attempts.append({"partials": order // 2, "reason": "no independent alias assignment"})
            continue
        if method == "subspace-refined" and best["component_rms_ratio"] <= MAX_COMPONENT_RMS_RATIO:
            refined = fit(np.array(best["frequencies"]), condition, root_error, refine=True)
            if refined is not None:
                best = refined
        if max(best["training_relative_rms"], best["held_out_relative_rms"]) > RELATIVE_RESIDUAL_LIMIT:
            attempts.append({"partials": order // 2, "reason": "residual",
                             "training_relative_rms": best["training_relative_rms"],
                             "held_out_relative_rms": best["held_out_relative_rms"]})
            continue
        if best["component_rms_ratio"] > MAX_COMPONENT_RMS_RATIO:
            attempts.append({"partials": order // 2, "reason": "component cancellation",
                             "component_rms_ratio": best["component_rms_ratio"]})
            continue
        return best
    return {"accepted": False, "reason": "no validated stationary model", "attempts": attempts}


def waveform(frequencies, amplitudes, phases, dc, center_seconds):
    time = center_seconds + (np.arange(WINDOW) - WINDOW / 2) / FS
    samples = np.full(WINDOW, dc, dtype=float)
    for frequency, amplitude, phase in zip(frequencies, amplitudes, phases, strict=True):
        samples += amplitude * np.sin(2 * np.pi * frequency * time + phase)
    return samples


def score(result, frequencies, amplitudes, dc):
    if not result["accepted"] or len(result["frequencies"]) != len(frequencies):
        return {"pass": False}
    indices = np.argsort(frequencies)
    frequency_error = float(np.max(np.abs(np.array(result["frequencies"])
                                          - np.array(frequencies)[indices]), initial=0))
    amplitude_error = float(np.max(np.abs(np.array(result["amplitudes"])
                                          / np.array(amplitudes)[indices] - 1), initial=0))
    dc_error = abs(result["dc"] - dc)
    return {"pass": frequency_error <= FREQUENCY_ERROR_LIMIT
            and amplitude_error <= RELATIVE_AMPLITUDE_ERROR_LIMIT and dc_error <= DC_ERROR_LIMIT,
            "max_frequency_error_hz": frequency_error,
            "max_relative_amplitude_error": amplitude_error, "dc_error": dc_error}


def run_probe(method):
    rows = []
    for upper, scale, rate, dc, swapped, shifted, seconds in itertools.product(
            (40, 28), (.0625, .125), (.25, .75), (0, .1), (False, True),
            (False, True), (3, 5, 9)):
        # Exactly the synthesis centers N/2 + k*H used by the native source path.
        logical_center = WINDOW // 2 + round((seconds * FS - WINDOW // 2) / (WINDOW // 4)) * (WINDOW // 4)
        amplitudes = (.08, .25) if swapped else (.25, .08)
        phases = (.73, -1.1) if shifted else (0, 0)
        samples = waveform((20, upper), amplitudes, phases, dc, rate * logical_center / FS)
        result = estimate(samples, method)
        rows.append({"group": "low_partials", "upper": upper, "scale": scale,
                     "rate": rate, "dc": dc, "swapped": swapped, "shifted": shifted,
                     "logical_center": logical_center, "result": result,
                     **score(result, (20, upper), amplitudes, dc)})

    bands = [(17.3, 25.7), (173.2, 181.1), (999.3, 1007.7),
             (4999.3, 5007.7), (13001, 13009), (200, 414.9, 901.3),
             (27.1, 41.3, 76.4), (400, 409, 430)]
    dispersed = (91.1, 171.3, 251.9, 332.3, 411.7, 493.1, 573.7, 653.9)
    bands += [dispersed[:count] for count in range(1, 9)]
    # The decimated data cannot distinguish these collisions. They are diagnostic
    # acceptance coverage, not a requirement that this first estimator handle them.
    bands += [(64, 1564), (89, 1589)]
    for frequencies, center in itertools.product(bands, (.731, 3.141)):
        amplitudes = tuple(.25 / (1 + index) for index in range(len(frequencies)))
        phases = tuple(.37 - .63 * index for index in range(len(frequencies)))
        samples = waveform(frequencies, amplitudes, phases, .1, center)
        result = estimate(samples, method)
        rows.append({"group": "generalization", "source_frequencies": frequencies,
                     "center": center, "result": result,
                     **score(result, frequencies, amplitudes, .1)})

    time = np.arange(WINDOW) / FS
    rng = np.random.default_rng(271828)
    controls = {
        "chirp_16hz_per_second": .1 + .25 * np.sin(2 * np.pi * (80 * time + 8 * time ** 2)),
        "chirp_160hz_per_second": .1 + .25 * np.sin(2 * np.pi * (80 * time + 80 * time ** 2)),
        "amplitude_ramp": .1 + (.05 + .2 * time / time[-1]) * np.sin(2 * np.pi * 173 * time),
        "tone_step": .1 + .25 * np.sin(2 * np.pi * np.where(time < time[-1] / 2, 80, 173) * time),
        "impulse": np.eye(1, WINDOW, WINDOW // 2).ravel(),
        "noise": rng.normal(0, .1, WINDOW),
        "tone_plus_noise": waveform((20, 28), (.25, .08), (.73, -1.1), .1, 3)
            + rng.normal(0, .001, WINDOW),
        "held_out_change": np.r_[waveform((20, 28), (.25, .08), (.73, -1.1), .1, 3)[:6144],
                                  rng.normal(0, .1, 2048)],
    }
    for name, samples in controls.items():
        result = estimate(samples, method)
        rows.append({"group": "rejection_control", "name": name, "result": result,
                     "pass": not result["accepted"]})

    constant = estimate(np.full(WINDOW, .1), method)
    rows.append({"group": "endpoint", "name": "dc", "result": constant,
                 **score(constant, (), (), .1)})
    nyquist = estimate(.1 + .25 * (-1.0) ** np.arange(WINDOW), method)
    rows.append({"group": "endpoint", "name": "nyquist", "result": nyquist,
                 "pass": not nyquist["accepted"]})
    return rows


def run_stress_probe(method):
    """Check contiguous native capture centers and realistic input perturbations.

    This is still a diagnostic, not a production output synthesis test. Scale
    never enters estimation; duplicate source windows across Scale are omitted.
    """
    rows = []
    hop = WINDOW // 4
    centers = [WINDOW // 2 + index * hop for index in range(260)
               if 3 * FS <= WINDOW // 2 + index * hop <= 11 * FS]
    for upper, rate, dc, swapped, shifted in itertools.product(
            (40, 28), (.25, .75), (0, .1), (False, True), (False, True)):
        frequencies = np.array((20, upper))
        amplitudes = (.08, .25) if swapped else (.25, .08)
        phases = np.array((.73, -1.1) if shifted else (0, 0))
        for center in centers:
            samples = waveform(frequencies, amplitudes, phases, dc, rate * center / FS)
            result = estimate(samples, method)
            scored = score(result, frequencies, amplitudes, dc)
            if result["accepted"] and len(result["frequencies"]) == 2:
                model_center_phase = (np.array(result["phases_at_start"])
                                      + np.array(result["frequencies"]) * np.pi * WINDOW / FS)
                source_center_phase = phases + frequencies * 2 * np.pi * rate * center / FS
                phase_error = float(np.max(np.abs(np.angle(np.exp(
                    1j * (model_center_phase - source_center_phase))))))
                scored["max_center_phase_error_radians"] = phase_error
                scored["pass"] = scored["pass"] and phase_error <= 1e-4
            rows.append({"group": "consecutive_windows", "upper": upper, "rate": rate,
                         "dc": dc, "swapped": swapped, "shifted": shifted,
                         "logical_center": center, "result": result, **scored})

    rng = np.random.default_rng(314159)
    for frequencies, amplitude_ratio, center in itertools.product(
            ((20, 28), (173.2, 181.1), (4999.3, 5007.7)), (.32, .1, .01), (.731, 3.141)):
        amplitudes = (.25, .25 * amplitude_ratio)
        for label, bits, noise in (("double", None, 0), ("float32", 32, 0),
                                   ("pcm24", 24, 0), ("pcm16", 16, 0),
                                   ("noise_1e-6", None, 1e-6), ("noise_1e-5", None, 1e-5)):
            samples = waveform(frequencies, amplitudes, (.73, -1.1), .1, center)
            if bits == 32:
                samples = samples.astype(np.float32).astype(float)
            elif bits:
                quantum = 2 ** (bits - 1)
                samples = np.round(samples * quantum) / quantum
            if noise:
                samples += rng.normal(0, noise, WINDOW)
            result = estimate(samples, method)
            rows.append({"group": "input_sensitivity", "format": label,
                         "source_frequencies": frequencies, "amplitude_ratio": amplitude_ratio,
                         "center": center, "result": result,
                         **score(result, frequencies, amplitudes, .1)})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stress", action="store_true",
                        help="add contiguous-window and input-sensitivity diagnostics")
    parser.add_argument("--method", choices=("ar", "subspace", "subspace-refined"),
                        default="subspace-refined")
    arguments = parser.parse_args()
    rows = run_probe(arguments.method)
    if arguments.stress:
        rows.extend(run_stress_probe(arguments.method))
    summary = {}
    for group in sorted({row["group"] for row in rows}):
        grouped = [row for row in rows if row["group"] == group]
        passed = [row for row in grouped if row["pass"]]
        summary[group] = {"passed": len(passed), "total": len(grouped)}
        errors = [row["max_frequency_error_hz"] for row in passed if "max_frequency_error_hz" in row]
        if errors:
            summary[group]["max_frequency_error_hz"] = max(errors)
        print(f"{group}: {len(passed)}/{len(grouped)}")
    diagnostic_groups = ("generalization", "input_sensitivity")
    inaccurate_acceptances = sum(row["result"]["accepted"] and not row["pass"]
                                for row in rows if row["group"] in diagnostic_groups)
    arguments.output.write_text(json.dumps({
        "method": arguments.method, "numpy_version": np.__version__,
        "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "summary": summary, "inaccurate_acceptances": inaccurate_acceptances,
        "rows": rows}, indent=2, allow_nan=False) + "\n")
    # Unsupported diagnostic profiles may reject. An inaccurate accepted model
    # fails the command regardless of which coverage group exposed it.
    passed = all(row["pass"] for row in rows
                 if row["group"] not in diagnostic_groups) and inaccurate_acceptances == 0
    print("Inaccurate diagnostic acceptances:", inaccurate_acceptances)
    print("Offline component diagnostic:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
