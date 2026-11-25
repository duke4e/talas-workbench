# Native lag-subspace kernel

Follow-up: [native row-shift/eigenvalue primitives](time-frequency-scale-native-modes.md)
now pass their separate numerical/state gates; production integration remains open.

2026-09-27. The offline component-estimator experiment now has a portable C++
subspace primitive with fixed storage and resumable work. It is linked into
`talas_dsp` and exercised by its own test target. The audio processor does not yet
call it; TimeFrequencyScale's native nearby-partial output gate remains
**34/64 (53.125%)**.

## What is implemented

`IncrementalLagSubspace` captures at most 191 finite sequence values, forms a lag
matrix in fixed storage, and orthogonalizes its columns with cyclic one-sided
Jacobi rotations. For the main fixture the matrix has 97 rows and 95 columns.
Its leading normalized columns span the temporal modes needed by the later
frequency solve. Rotating the transposed lag matrix avoids maintaining a second
95-by-95 rotation matrix.

Matrix formation, pair dot products, rotations, norms, sorting and rank selection
advance under an explicit work budget. Zero work leaves state unchanged. The
sequence is owned, and copying the object preserves pending work independently.
Begin, advance, copy and reset allocate no heap memory. Reset invalidates visible
state without clearing the matrix. Object size on this arm64 build is **76,920
bytes**.

Inputs are normalized before products, preserving physical singular-value units
at output. The relative singular floor and pair-correlation tolerance are both
1e-12; sweeps are capped at 24. Invalid input, overflow and nonconvergence expose
no usable partial result. This primitive reports numerical rank; it does not
classify stationarity or choose a sinusoid count. Component pairing, roots,
aliases and original-rate reconstruction validation belong to the next stage.

The predeclared criteria fix dimensions, numerical limits, accuracy gates and
the conservative work bound. The maximum bound for the main shape is **20,923,861 units**. A unit is
one documented row or bookkeeping operation, not a measured number of CPU cycles.

## Numerical and state verification

Both ordinary Release and Debug pass the new
`talas_incremental_lag_subspace_tests` target. Debug CTest reports **1/1 passed in
2.12 seconds**. The primitive translation unit retains ordinary configuration
flags; it was not added to the existing optimized-Debug source exceptions.

The 36 native matrix profiles include low nearby pairs, swapped amplitudes,
multiple source centers, clean and simulated PCM16 input, higher pairs through
13kHz with quieter components, and one to eight dispersed sinusoids. These have
their own matrix-span criteria, separate from the offline frequency estimator's
broader 32/36 generalization result.

| Check | Result |
| --- | --- |
| Native stationary matrix profiles | 36/36 in Release and Debug |
| Clean numerical rank | Exactly twice the source tone count in all 22 clean profiles |
| Largest analytical span residual | .000765528; PCM16 limit .005 |
| Pending state copied into dirty destinations | Nine positions; exact continuation |
| Work partition independence | Exact singular values and basis entries |
| Input ownership | Source mutation after capture has no effect |
| Allocation checks | Zero in begin, advance, copy and reset |
| Boundary/invalid data | Zero, nonfinite, oversized, invalid dimensions and overflow covered |
| Full-rank noise | Ranks 1/2/8/95 preserved at the corresponding dimensions |
| Extreme finite scaling | Singular values retain units at 1e-200 and 1e200 |

Independent sine/cosine vectors verify the recovered span; orthonormality and
matrix-energy checks use 1e-10 tolerances. Read-only review identified a missing
null-mode assertion. Before adding it, independent NumPy analysis confirmed that
all clean input sequences have the expected numerical rank at the declared
floor; the largest first tail ratio was 7.754e-13. The native tests now enforce
exact clean rank. Quantization is allowed to create additional modes.

Exported native sequences and bases are compared with NumPy SVD using
`tools/check_lag_subspace_probe.py`. Full numerical rank, leading singular values
and subspace projectors must agree. Projector comparison permits mathematically
equivalent vector signs and rotations. Release passes **36/36**, with maximum
relative singular-value error **1.621e-14** and projector Frobenius error
**3.016e-12**. Debug also passes **36/36** against the independent comparison.

## Scheduling evidence

The stationary fixtures require up to **9,769,008 work units and 13 sweeps**.
Their observed maximum whole-job duration is **29.6311ms in Release** and
**73.147ms in ordinary Debug**. These whole jobs cannot simply be placed in a
small audio block. The implementation supports splitting them without changing
the numerical result or losing captured state.

A separate Release diagnostic runs a PCM16 nearby pair and deterministic
full-rank noise. It measures only individual `advance` calls, with prepared
timing storage and no concurrent native build or test job:

| Work per advance | Pair calls | Noise calls | Largest p99 across the two | Largest observed call |
| --- | --- | --- | --- | --- |
| 4096 | 2168 | 1965 | 22.750µs | 115.292µs |
| 8192 | 1084 | 983 | 45.292µs | 118.583µs |
| 16384 | 542 | 492 | 68.584µs | 207.750µs |
| 32768 | 271 | 246 | 119.875µs | 129.708µs |

The two jobs use 8,879,193 and 8,047,892 units respectively. Observed maxima
include scheduling noise, so they need not increase monotonically with budget.
This is a primitive diagnostic, not an audio-device deadline guarantee, a full
renderer measurement, or a formal benchmark. No public callback budget or native
latency has changed. The existing formal benchmark prerequisites remain in force.

## Reproduction

```sh
cmake --build build/core-release --target talas_incremental_lag_subspace_tests -j2
build/core-release/talas_incremental_lag_subspace_tests
build/core-release/talas_incremental_lag_subspace_tests --dump > build/workbench-debug/native-subspace.jsonl
PYTHONDONTWRITEBYTECODE=1 OPENBLAS_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1 \
  python3 -W error tools/check_lag_subspace_probe.py --input build/workbench-debug/native-subspace.jsonl
build/core-release/talas_incremental_lag_subspace_tests --budget-probe
```

Use the existing Debug root for the corresponding Debug build and test. Logs
remain under registered `build/workbench-debug`, prefixed
`time-frequency-scale-native-subspace-`: configure/build/test logs, exported
`*-vectors.jsonl`, independent `*-numpy.log`, and `budget-probe.log`. Earlier
Release measurements are retained alongside final expanded tests. No cleanup
or commit was performed.

## Remaining integration

The next native work is the row-shift solve and bounded extraction of conjugate
frequency modes, followed by alias selection and full-rate coefficient/frequency
refinement. A numerical job may span multiple audio blocks while processing
state retains its progress and owned samples. Validate its completed model
against the current captured source before use.

Then verify synthesis phase through Rate/Scale changes, model entry/exit and
fallback continuity, exact checkpoint/reset/retry, and combined callback cost.
Keep N8192, existing source authority and latency, and the original 64 output
thresholds. The passing subspace tests are prerequisite evidence; the native
output failure, four offline alias-collision cases and agreement with an
external reference remain open.
