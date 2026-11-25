# Native row-shift and eigenvalue primitives

Follow-up: [native coefficient fitting/refinement](time-frequency-scale-native-fit.md)
now passes its separate numerical/state gates. Alias selection and audio
integration remain open.

Date: 2026-09-27. The native lag-subspace prototype now has a portable C++
row-shift regression and complex eigenvalue solver. Both are linked into talas_dsp;
neither is called by PreparedTimeFrequencyScale. The original nearby-partial
output gate remains **34/64 (53.125%)**, including its unresolved20/28Hz cases.

## Implementation and scope

`IncrementalSubspaceModes` immediately owns the selected real basis columns. It
forms G=U0^T U0 and C=U0^T U1, solves G*A=C using staged pivoted Gauss--Jordan,
and finds the eigenvalues of A. U0/U1 omit the final/initial basis row. Thus a
clean exponential mode has root exp(i*2*pi*f/1500) for the current stride32
input. The roots are unordered and aliased; original-rate frequency selection,
automatic model-order selection and source-model validation remain separate work.
The36 source fixtures explicitly supply their known even model dimension2K.

`IncrementalComplexEigenvalues` owns a matrix of at most16x16 complex values.
Staged Householder reduction produces Hessenberg form, followed by shifted
complex QR. A stable quadratic offset chooses the Wilkinson shift; a fixed
exceptional shift occurs every16 iterations without deflation. Total QR work is
capped at1024 iterations. Deflation uses machine epsilon and the two-stage
Ahues--Tisseur sensitivity criterion, with abs(real)+abs(imag) as the complex
magnitude convention. See the primary
[LAPACK ZLAHQR source](https://raw.githubusercontent.com/Reference-LAPACK/lapack/master/SRC/zlahqr.f).

No partial result is visible before success. Singular prefix regression,
nonfinite input, scaling that loses a nonzero component, numeric overflow and
nonconvergence fail closed. This is not a guarantee of forward eigenvalue
accuracy for every ill-conditioned matrix. Unit-circle proximity alone never
qualifies a source model. Diagnostic capacity8sinusoids is not the Detectors control.

Both helpers use fixed storage, own all input and resume from scalar cursors.
Every advance call consumes supplied work and returns its unused remainder.
The shift helper's final eigenvalue setup includes a bounded256-entry copy in
one step; initial basis capture is also immediate and bounded. Work units are
not CPU cycles. Reset invalidates scalar state. Native sizes are **13,000 bytes**
for the eigenvalue solver and **45,744 bytes** for the row-shift helper, including
its embedded eigenvalue state. The lag-subspace state remains76,920bytes.

## Correctness results

The predeclared gate was written before the first native build/run. Tests pass in ordinary Release and
Debug, with no special optimization added to these source files.

| Check | Release | Debug |
|---|---:|---:|
| Analytic matrix spectra |36/36|36/36|
| Additional deterministic real/complex matrices |32/32|32/32|
| Native stationary-source mode profiles |36/36|36/36|
| Independent NumPy matrix eigenvalues |68/68|68/68|
| Independent NumPy source shift and roots |36/36|36/36|
| Pending eigenvalue checkpoint copies |12/12|12/12|
| Pending row-shift checkpoint copies |14/14|14/14|
| Focused CTest suites, including existing lag subspace |2/2|2/2|

Matrix fixtures include dense nonnormal similarities, complex spectra,
rotations, a16-cycle, repeated triangular roots, clustered companions, and
the sensitive2x2 matrices identified during review. The additional32 matrices
cover dimensions1..16, both real and complex. Uniform scales1e-200 and1e200
also pass after explicitly restoring their root units.

Maximum analytic root error is **3.46643e-11**, below1e-9. The22 clean source
profiles have maximum analytic root error **1.79659e-12**. PCM16 cases use the
same captured noisy basis as the independent least-squares reference; they do
not assert noise-free source frequencies. All68 matrix roots and36 source
shifts/roots pass independent NumPy2.3.5 comparison in both builds:

- Maximum root error under the declared physical-unit normalization:
  **3.23104437947e-11** (limit1e-9).
- Maximum shift-matrix Frobenius error: **1.86853294324e-14** (limit1e-10).
- Maximum normal-equation residual: **1.68538634877e-15** (limit1e-10).
- Diagnostic source shift residual: **.000660966743404**.
- Diagnostic source unit-radius error: **2.07357283455e-5**.

Different work partitions and copied pending states produce exact roots and
shift matrices. Copies overwrite previously completed destination state; the
original job can then reset independently. Mutating the caller's matrix or
resetting its basis after capture cannot affect the job. Capture, advance,
state copy and reset allocate nothing. Invalid dimensions, zero work, zero
matrices, hidden/out-of-range results, singular U0 and overflow also pass.
The shared source fixture helper preserves the original36 lag-subspace profiles;
the existing suite still passes after that extraction.
The1024-iteration nonconvergence branch is not directly forced by a fixture;
the iteration/work caps remain explicit. The NumPy comparison is a separate
required command and is not automatically invoked by CTest.

## Primitive work and timing

Across68 matrix profiles, maximum work is **388,684 units**, maximum QR
iterations **61**. Across36 source profiles, row-shift plus eigenvalue extraction
takes at most **228,406 units** and **46** iterations. Every observed job remains
below its conservative declared work bound. The lag-subspace solve is additional.

Whole-job maxima observed across the direct/CTest diagnostics were2.47696ms
Release /16.3403ms Debug for the matrix solver and.684666ms /7.72933ms for source
mode extraction. These are local primitive measurements, not audio deadlines.

Separate serial `--budget-probe` runs measure16-mode eigenvalue and row-shift
jobs. The table reports the larger p99 and maximum of those two jobs per row:

| Build | Work per call | Calls, eigen / modes | p99 µs | Maximum µs |
|---|---:|---:|---:|---:|
|Release|512|461 /447|1.917|2.833|
|Release|2048|116 /112|6.916|7.208|
|Release|8192|29 /28|26.458|26.500|
|Debug|512|461 /447|25.625|37.291|
|Debug|2048|116 /112|93.834|101.750|
|Debug|8192|29 /28|324.958|343.750|

No production work budget has been chosen from these samples. Debug uses its
ordinary-g flags; Release uses-O3-DNDEBUG. Both benchmark options remainOFF.
The formal benchmark prerequisites were not bypassed. Combined rendering,
snapshot costs and platform scheduling still require their own measurements.

## Review history and reproduction

Before the first run, review rejected the draft global1e-12 last-row deflation
test: [[1,1],[1e-13,1]] would falsely return repeated roots. Hessenberg reduction
and the sensitivity criterion replaced it;1e-16 real/complex coupling tests
also remain mandatory. A second review found normalization could erase the
lower coupling of [[1,1e300],[1e-300,1]]; capture now explicitly rejects that loss.
Neither counterexample was removed or used to loosen the1e-9 accuracy gate.
The independent checker was tightened to apply its gate in physical root units
rather than normalized matrix units; the reported final comparisons include it.

Build/run targets: `talas_incremental_subspace_modes_tests` and
`talas_incremental_lag_subspace_tests`. Export the former with `--dump`, then run
`tools/check_subspace_modes_probe.py --input <export.jsonl>` using the bundled
NumPy runtime with one BLAS thread and warnings as errors. `--budget-probe`
performs only the separate primitive work-partition diagnostic.

Logs live under the registered build/workbench-debug root, prefix
`time-frequency-scale-native-modes-`: Release/Debug configure/build/CTest,
initial/final Release direct tests, saved Debug LastTest, both104-row exports
with empty stderr, original/final NumPy reports, and both budget probes.

Next work: bounded original-rate alias selection, coefficient fitting and
frequency refinement, followed by owned source snapshots, validation of a
completed model against the current source, phase/identity continuity,
confidence transitions, fallback and complete callback/checkpoint integration.
The original64 production profiles and existing latency remain unchanged.
