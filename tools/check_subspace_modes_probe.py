#!/usr/bin/env python3
"""Check native bounded-matrix eigenvalues and row-shift fits against NumPy."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def complex_array(values: list) -> np.ndarray:
    pairs = np.asarray(values, dtype=float)
    if pairs.ndim != 2 or pairs.shape[1] != 2 or not np.all(np.isfinite(pairs)):
        raise ValueError("malformed complex array")
    return pairs[:, 0] + 1j * pairs[:, 1]


def root_error(actual: np.ndarray, expected: np.ndarray) -> float:
    # The spectra here are separated, or have identical repeated roots.
    # Pair each reference root with the nearest still-unmatched native root.
    unmatched = list(actual)
    maximum = 0.0
    for root in expected:
        index = min(range(len(unmatched)), key=lambda i: abs(unmatched[i] - root))
        maximum = max(maximum, float(abs(unmatched.pop(index) - root)))
    return maximum


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--input", type=Path, required=True)
    arguments = parser.parse_args()
    counts = {"eigen": 0, "modes": 0}
    maximum_root_error = maximum_shift_error = maximum_normal_residual = 0.0
    maximum_source_residual = maximum_unit_error = 0.0
    clean = 0
    for number, line in enumerate(arguments.input.read_text().splitlines(), 1):
        row = json.loads(line)
        size = row["size"]
        roots = complex_array(row["roots"])
        if not 1 <= size <= 32 or roots.shape != (size,):
            raise ValueError(f"row {number}: malformed roots")
        if row["kind"] == "eigen":
            matrix = complex_array(row["matrix"]).reshape(size, size)
        elif row["kind"] == "modes":
            basis = np.asarray(row["basis"], dtype=float)
            matrix = np.asarray(row["shift"], dtype=float)
            if (basis.shape != (row["rows"], size) or matrix.shape != (size, size)
                    or not np.all(np.isfinite(basis)) or not np.all(np.isfinite(matrix))):
                raise ValueError(f"row {number}: malformed basis or shift")
            u0, u1 = basis[:-1], basis[1:]
            reference, _, rank, _ = np.linalg.lstsq(u0, u1, rcond=None)
            shift_error = float(np.linalg.norm(matrix - reference))
            normal_residual = float(np.linalg.norm(u0.T @ (u0 @ matrix - u1)))
            if rank != size or shift_error > 1e-10 or normal_residual > 1e-10:
                raise ValueError(f"row {number}: shift error {shift_error}, normal residual {normal_residual}")
            maximum_shift_error = max(maximum_shift_error, shift_error)
            maximum_normal_residual = max(maximum_normal_residual, normal_residual)
            maximum_source_residual = max(maximum_source_residual, float(np.linalg.norm(u0 @ matrix - u1)))
            maximum_unit_error = max(maximum_unit_error, float(np.max(np.abs(np.abs(roots) - 1))))
            clean += not row["quantized"]
            # Compare roots against the independently solved shift, not only
            # NumPy's eigensolver applied to the native shift matrix.
            matrix = reference
        else:
            raise ValueError(f"row {number}: unknown fixture kind")
        scale = float(np.max(np.abs(matrix)))
        if scale == 0:
            expected = np.zeros(size)
            actual = roots
        else:
            expected = np.linalg.eigvals(matrix / scale)
            actual = roots / scale
        # Apply the declared tolerance in physical eigenvalue units. Uniform
        # scale fixtures additionally require accuracy after scaling back up.
        floor = 1.0 if scale == 0 or row["name"] == "scaled_similarity" else 1.0 / scale
        error = root_error(actual, expected) / max(floor, float(np.max(np.abs(expected))))
        if error > 1e-9:
            raise ValueError(f"row {number} {row['name']}: eigenvalue error {error}")
        maximum_root_error = max(maximum_root_error, error)
        counts[row["kind"]] += 1
    if counts != {"eigen": 129, "modes": 37} or clean != 23:
        raise ValueError(f"expected all declared 129+37 profiles with 23 clean sources, got {counts}, clean={clean}")
    print(f"Native/NumPy eigenvalues: {counts['eigen']}/{counts['eigen']} PASS")
    print(f"Native/NumPy subspace shifts and roots: {counts['modes']}/{counts['modes']} PASS")
    print(f"Maximum normalized eigenvalue error: {maximum_root_error:.12g}")
    print(f"Maximum shift Frobenius error: {maximum_shift_error:.12g}")
    print(f"Maximum normal-equation residual: {maximum_normal_residual:.12g}")
    print(f"Maximum source shift residual (diagnostic): {maximum_source_residual:.12g}")
    print(f"Maximum source unit-radius error (diagnostic): {maximum_unit_error:.12g}")
    print(f"NumPy version: {np.__version__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
