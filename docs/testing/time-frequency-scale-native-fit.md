# Native original-rate sinusoid fitting

Date:2026-09-27. `IncrementalSinusoidFit` now implements bounded coefficient
fitting and local frequency refinement for supplied candidate frequencies.
It is linked into talas_dsp but is **not called by the audio processor**. This
standalone helper receives candidate frequencies; automatic discovery now uses
it as described below. Audio integration remains open. The nearby-partial
production gate remains **34/64 (53.125%)**.

Subsequent work now combines this helper with automatic discovery/alias choice;
see [native stationary model evidence](time-frequency-scale-native-model.md).
The optional `validate=false` mode ranks candidates using only the training
prefix, never exposes them as accepted, and reports `validationPerformed=false`.
Both build variants retain the68-profile numerical gate and verify exact
training-only invariance to NaN or huge values in the heldout quarter.

## Numerical contract

The helper owns32..65536 samples (N divisible4) and0..8 sorted candidates inside
DC/Nyquist. The first3N/4 samples train; the finalN/4 validates. Normalization
uses only training samples. Capturing input does not imply a usable model:
begin checks shape/controls and copies raw samples; subsequent budgeted stages
validate sample values, normalize and collect statistics.

Streaming Givens QR fits DC/cos/sin coefficients. Up to3 training-only
Gauss--Newton attempts add frequency derivatives expressed in training-bin
units, limit each step to.5bin and total motion to1bin, and fully re-fit the
linear coefficients. Only strictly lower training RSS replaces the previous
fit. Refinement stops below1e-10 relative training RMS or above4component-RMS
ratio. Each linear/Jacobian solve checks the conservative κ₂ upper bound
||R||F*||invR||F<=1e12 using staged triangular inverse solves. The initial
ill-conditioned fit fails; a failed refinement preserves the previous fit.

Ready means a completed numerical result. `accepted()` separately requires
training and validation relative RMS<=1e-4 and componentRMS/observedRMS<=4,
using a physical1e-12 signal floor. A zero-partial constant model requires every
residual<1e-12. Pending/failed states expose no parameters. The8-partial work
capacity has no claimed relationship to the Detectors control.

State is trivially copyable with **530,848 bytes** of fixed storage, including
capacity for65536 double samples. No caller pointer or heap allocation remains
in the job. Reset invalidates scalar state; sample checks, QR, conditioning,
refinement and validation all resume from explicit work cursors. Initial raw
capture is one bounded memcpy; work units are not elapsed-time guarantees.

## Correctness evidence

The predeclared criteria retain the original production gate. Ordinary Release and Debug both pass:

| Check | Result in each build |
|---|---:|
| Stationary positive profiles |68/68 (100%)|
| Profiles that exercise frequency refinement |54/54|
| Nonstationary/wrong/cancellation controls plus changed constant |10/10|
| Independent NumPy fit/refinement comparisons |68/68|
| Pending checkpoint copies into dirty state |14/14|
| Focused CTest suite |1/1|

The68 positive profiles consist of36 existing source profiles (14PCM16),
18float32/noise sensitivity profiles, nine N/Fs combinations
(N1024/8192/65536;44100/48000/96000Hz), two uniform scales1e-200/1e200 and
three constants. The first54 explicitly receive candidate frequencies offset
alternately+.025/-.025Hz from fixture truth. This tests fitting/refinement,
**not automatic frequency discovery**. Independent analytic scoring yields:

| Measurement | Maximum error | Unchanged limit |
|---|---:|---:|
| Frequency |.000536490Hz|.02Hz|
| Relative amplitude |.0000963807 (.00963807%)|.002 (.2%)|
| DC |2.39622e-7|1e-6|

All9 negative source fixtures reject: chirp, amplitude ramp/step, impulse,
noise, changed validation, noisy tone, wrong alias and an almost-coincident
cancellation model. The cancellation fixture is numerically fitted but has
component ratio>4 and receives no refinement. A changed constant also rejects.

The NumPy checker independently solves the final fixed-frequency fit and
performs the same bounded training-only refinement using SVD-based least
squares. It derives the Frobenius condition bound from singular values rather
than repeating native QR/inverse code. All68 exports in each build pass:

- Normalized coefficient error: **7.77156117238e-15** (limit1e-9).
- Residual/ratio diagnostic difference: **4.4408920985e-15** (limit1e-9).
- Relative normal-equation residual: **7.10027120049e-15** (limit1e-9).
- Relative condition-bound difference: **2.88657986403e-15** (limit1e-9).
- Native/NumPy refined frequency difference: **1.13686837722e-13Hz** (limit1e-5).
- Normalized refined coefficient difference: **7.77156117238e-15** (limit1e-6).

The maximum final linear condition bound in positive profiles is41.2808.
These comparisons use NumPy2.3.5 and warnings as errors; they are separate
commands and are not automatically run by CTest.

State/admission checks cover exact work partitions,14 pending copies,
caller sample/candidate mutation, hidden results, zero budget, finite extreme
scales, minimum32-sample input, capacity limits, invalid rates/frequencies,
nonfinite samples, normalization loss, near-DC ill-conditioning and physical
coefficient overflow. All capture/advance/copy/reset allocation counters stay0.
Changing only validation samples to1e6 preserves training parameters and
refinement counts exactly, while changing acceptance. A forced24-row/25-column
Jacobian failure exactly preserves the17-column `refine=false` fit and its
validation decision. Final CTest times: Release.89s, Debug2.57s.

## Scheduling finding and repair

The initial implementation passed numerical gates, but capture included finite
checks and normalization. For65536 samples that took **131.041µs Release** and
**903.916µs Debug**. The Debug call alone exceeded the666.667µs duration of a
48kHz/32-sample block. Those original timing logs remain available.

Capture now only copies raw data after bounded shape/control checks. Validation
and normalization/statistics have separate resumable stages, covered by new
checkpoint positions. The same65536-sample capture measured **10.917µs Release**
and **14.417µs Debug** afterward;8192-sample captures measured1.167..1.584µs and
1.666..2.708µs respectively. Numerical gates still pass unchanged. Begin success
now means owned capture admission; callers must wait for Ready and inspect
`accepted()` before using the model.

The positive suite's largest job is8,573,499units. A separate eight-partial
probe including20/28Hz reaches12,207,759units; the conservative65536-sample,
eight-partial bound is135,551,711units. Final positive-suite whole-job maxima
are25.8831ms Release and109.611ms Debug. Complete jobs must be distributed;
they are not suitable for one audio callback.

Separate serial budget probes cover two/eight partials at8192samples and two
partials at65536samples. Taking the largest p99/max across those three jobs:

| Build | Work per call | Largest p99 µs | Largest call µs |
|---|---:|---:|---:|
|Release|4096|32.542|45.375|
|Release|16384|129.959|131.958|
|Debug|4096|70.458|106.333|
|Debug|16384|278.417|294.959|

These are primitive diagnostics with prepared storage; whole-state copies and
other DSP are outside the timed advance calls. No combined callback deadline,
public work budget or full renderer claim follows from these samples. Ordinary
Release-O3-DNDEBUG and Debug-g remain in effect; no special optimization was
added. Formal benchmark gates were not bypassed.

## Reproduction and remaining integration

Build `talas_incremental_sinusoid_fit_tests` and run it directly or through CTest.
Use `--dump` to export68 JSONL profiles, then
`tools/check_sinusoid_fit_probe.py --input <export.jsonl>` with the bundled NumPy
runtime and one BLAS thread. `--budget-probe` separately measures capture and
incremental work. All logs/exports are in registered build/workbench-debug,
prefix `time-frequency-scale-native-fit-`; `staged-*` files are the current
capture implementation, and `final-release-numpy` includes the final condition
comparison. Original initial/final-before-staging timing records are retained.

Automatic original-rate alias selection/model order now uses the verified
subspace/mode/fit helpers. Next: phase continuity, current-source validation of
completed jobs, confidence/fallback transitions and full callback/checkpoint
integration. Source authority, existing N8192 delay10239 and all64 original
output profiles remain required. Nothing here closes the production34/64 gap.
