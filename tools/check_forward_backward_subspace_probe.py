#!/usr/bin/env python3
"""Check the native forward/backward subspace probe against a direct NumPy SVD.

The oracle forms the full 97-by-190 matrix. It does not use the native even/odd
factorization. Projector comparisons account for the retained singular gap;
orthogonality and reconstruction are checked separately, including weak modes.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


def reject_nonfinite(value: str) -> None:
    raise ValueError(f"nonfinite JSON token: {value}")


def check(row: dict, index: int) -> dict[str, float]:
    name = row.get("name", str(index))
    sequence = np.asarray(row["sequence"], dtype=np.float64)
    if sequence.shape != (191,) or not np.all(np.isfinite(sequence)):
        raise ValueError(f"{name}: expected 191 finite samples")
    if row["status"] != "Ready":
        raise ValueError(f"{name}: valid fixture did not finish Ready")
    rank = row["rank"]
    if isinstance(rank, bool) or not isinstance(rank, int) or not 0 <= rank <= 97:
        raise ValueError(f"{name}: invalid rank")
    for key in ("consumed_work", "maximum_work"):
        if isinstance(row[key], bool) or not isinstance(row[key], int) or row[key] < 0:
            raise ValueError(f"{name}: invalid {key}")
    if row["consumed_work"] > row["maximum_work"]:
        raise ValueError(f"{name}: declared work bound exceeded")

    scale = float(np.max(np.abs(sequence))) or 1.0
    hankel = np.lib.stride_tricks.sliding_window_view(sequence / scale, 95).copy()
    augmented = np.column_stack((hankel, hankel[::-1]))
    expected_basis, expected_singular, _ = np.linalg.svd(augmented, full_matrices=False)
    frobenius = float(np.linalg.norm(augmented, "fro"))
    expected_rank = int(np.count_nonzero(expected_singular > 1e-12 * frobenius))
    if rank != expected_rank:
        raise ValueError(f"{name}: rank {rank} differs from direct SVD {expected_rank}")

    singular = np.asarray(row["singular_values"], dtype=np.float64)
    basis = np.asarray(row["basis"], dtype=np.float64)
    if singular.shape != (rank,) or basis.size != 97 * rank:
        raise ValueError(f"{name}: invalid retained output dimensions")
    basis = basis.reshape(97, rank)
    if not np.all(np.isfinite(singular)) or not np.all(np.isfinite(basis)):
        raise ValueError(f"{name}: nonfinite output")
    if np.any(singular <= 0) or np.any(np.diff(singular) > 0):
        raise ValueError(f"{name}: retained singular values must decrease and be positive")
    singular = singular / scale
    epsilon = np.finfo(np.float64).eps
    absolute_tolerance = 256 * epsilon * frobenius
    singular_error = float(np.max(np.abs(singular - expected_singular[:rank]), initial=0.0))
    if singular_error > absolute_tolerance:
        raise ValueError(f"{name}: singular-value absolute error {singular_error:g}")
    strong = expected_singular[:rank] >= math.sqrt(epsilon) * frobenius
    relative_error = float(np.max(np.abs(singular[strong] / expected_singular[:rank][strong] - 1), initial=0.0))
    if relative_error > 1e-9:
        raise ValueError(f"{name}: strong singular-value relative error {relative_error:g}")

    orthogonality = float(np.linalg.norm(basis.T @ basis - np.eye(rank), "fro"))
    if orthogonality > 1e-10:
        raise ValueError(f"{name}: basis orthogonality error {orthogonality:g}")
    residual = float(np.linalg.norm(augmented - basis @ (basis.T @ augmented), "fro"))
    optimal_residual = float(np.linalg.norm(expected_singular[rank:]))
    if residual > optimal_residual + absolute_tolerance:
        raise ValueError(f"{name}: reconstruction residual exceeds discarded singular tail")

    reference = expected_basis[:, :rank]
    projector_error = float(np.linalg.norm(basis @ basis.T - reference @ reference.T, "fro"))
    gap = (expected_singular[rank - 1] - (expected_singular[rank] if rank < 97 else 0)) if rank else 1.0
    projector_tolerance = max(1e-9, 64 * epsilon * frobenius / gap)
    if projector_error > projector_tolerance:
        raise ValueError(f"{name}: projector error {projector_error:g} exceeds gap-scaled {projector_tolerance:g}")
    return {
        "strong_singular_relative_error": relative_error,
        "singular_absolute_error_over_frobenius": singular_error / frobenius if frobenius else 0.0,
        "orthogonality_error": orthogonality,
        "projector_error": projector_error,
        "projector_tolerance": projector_tolerance,
        "reconstruction_excess_over_frobenius": max(0.0, residual - optimal_residual) / frobenius if frobenius else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    maxima: dict[str, float] = {}
    count = 0
    with args.probe.open() as source:
        for line in source:
            if not line.strip():
                continue
            row = json.loads(line, parse_constant=reject_nonfinite)
            for key, value in check(row, count).items():
                maxima[key] = max(maxima.get(key, 0.0), value)
            count += 1
    if count == 0:
        raise ValueError("empty native probe")
    print(json.dumps({"verified": count, "maxima": maxima}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
