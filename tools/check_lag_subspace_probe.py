#!/usr/bin/env python3
"""Compare exported native lag-subspace results to independent NumPy SVD."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--input", type=Path, required=True)
    arguments = parser.parse_args()
    cases = 0
    maximum_singular_error = 0.0
    maximum_projector_error = 0.0
    for line in arguments.input.read_text().splitlines():
        row = json.loads(line)
        sequence = np.asarray(row["sequence"], dtype=float)
        native_basis = np.asarray(row["basis"], dtype=float).T
        native_singular = np.asarray(row["singular_values"], dtype=float)
        modes, columns = row["modes"], row["columns"]
        matrix = np.lib.stride_tricks.sliding_window_view(sequence, columns)
        if (matrix.shape[0] != row["rows"] or native_basis.shape != (row["rows"], modes)
                or native_singular.shape != (modes,)
                or not all(np.all(np.isfinite(a)) for a in (matrix, native_basis, native_singular))):
            raise ValueError(f"case {cases}: malformed native export")
        expected_basis, expected_singular, _ = np.linalg.svd(matrix, full_matrices=False)
        expected_rank = int(np.count_nonzero(expected_singular > np.linalg.norm(matrix) * 1e-12))
        if row["rank"] != expected_rank:
            raise ValueError(f"case {cases}: rank {row['rank']} differs from reference {expected_rank}")
        expected_basis = expected_basis[:, :modes]
        singular_error = float(np.max(np.abs(native_singular / expected_singular[:modes] - 1)))
        projector_error = float(np.linalg.norm(native_basis @ native_basis.T
                                              - expected_basis @ expected_basis.T))
        if singular_error > 1e-10 or projector_error > 1e-8:
            raise ValueError(f"case {cases} {row['name']}: singular error {singular_error}, "
                             f"projector error {projector_error}")
        maximum_singular_error = max(maximum_singular_error, singular_error)
        maximum_projector_error = max(maximum_projector_error, projector_error)
        cases += 1
    if cases != 36:
        raise ValueError(f"expected all 36 declared native profiles, received {cases}")
    print(f"Native/NumPy subspaces: {cases}/{cases} PASS")
    print(f"Maximum relative singular-value error: {maximum_singular_error:.12g}")
    print(f"Maximum projector Frobenius error: {maximum_projector_error:.12g}")
    print(f"NumPy version: {np.__version__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
