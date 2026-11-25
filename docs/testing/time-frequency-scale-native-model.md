# Native automatic stationary component estimation

Date: 2026-09-27. `IncrementalStationaryModel` combines the verified subspace,
mode extraction and sinusoid fitting primitives. Its only analysis inputs are
owned original-rate samples and sample rate. Candidate frequencies, component
count, amplitudes, Rate and Scale are not supplied. It is linked into talas_dsp,
but is **not called by the audio processor**. The nearby-partial production
output gate remains **34/64 (53.125%)**.

## Analysis and acceptance

The helper captures a power-of-two window N1024..65536 into fixed owned
storage. Capture checks shape/sample rate and copies raw data; sample checks,
normalization and all numerical analysis run incrementally. The first3N/4
samples train. The final quarter validates the best selected model and can
cause a higher order to be tried. It does not choose aliases or update fitted
parameters, and it is not an untouched blind test.

The periodic-Hann training spectrum has length3N/4, not a zero-padded radix-2
grid. Three N/4 FFTs of interleaved, appropriately windowed samples reconstruct
that spectrum with the radix-three identity. A caller-prepared FFT plan is
passed to advance and never retained. Larger plans use LongAnalysis.
Up to16 interior guides survive the1% peak-amplitude threshold.

Differences of192 strided training values feed the verified97x95 lag-subspace
kernel. Even mode orders2..16 undergo strength/prefix condition checks, unit
root checks, and conjugate pairing. Each mode expands to original-rate aliases
within two training bins of a guide plus its nearest assignment. Orders with
more than256 Cartesian assignments are explicitly skipped in full.

Alias fits use `IncrementalSinusoidFit` with `refine=false, validate=false`:
the heldout quarter is not inspected, even for finite validation or scaling.
Numerical Ready allows ranking by training RMS, while `accepted()` remains
false and `validationPerformed()` reports false. Only the best training model
is refined and validated. Existing conditioning, at most three bounded
Gauss--Newton refinements, RMS<=1e-4 and component-cancellation<=4 gates remain.
The default fitter API still validates as before; both build variants passed
its68 positive profiles, ten rejection controls and exact state checks after
the optional training-only mode was added. Heldout NaN/huge-value changes leave
training-only parameters, diagnostics and work count exactly unchanged.

The model owns **1,983,616 bytes**, is trivially copyable, and has no processing
allocations or retained source pointer. Reset invalidates scalar state without
clearing large buffers. No pending/rejected fit is exposed. Work accounting
delegates exact consumed units to FFT/subspace/modes/fit jobs and includes
bounded setup and sample copies. Its conservative total bound uses64-bit
arithmetic; work units are not CPU-time guarantees.

## Predeclared gates and numerical results

The predeclared criteria retain the original output and analytic thresholds,
plus all test-driven revisions.
The initial282 native cases passed ordinary Release and Debug. Additional
alias-search cases expanded this to289, with267 accurate acceptances and22
expected rejections. Independent final verification is recorded below.

| Profiles | Accurate acceptances | Expected rejections |
|---|---:|---:|
| Selected nearby-partial source windows |96|0|
| Broader one-to-eight partial profiles |32|4|
| Input precision/noise sensitivity |108|0|
| Nonstationary/noise/changed-validation controls |0|8|
| Zero/positive/negative constants |3|0|
| Nyquist endpoint |0|1|
| Seven window sizes, three sample rates |21|0|
| Uniform finite scale1e200 |1|0|
| Full/half decimation-alias boundaries |0|8|
| Multiple alias assignments and over-cap search |6|1|
| **Total** |**267**|**22**|

The selected96 omit source-identical duplicates across Scale from the earlier
offline192 profile gate. Sensitivity includes18 profiles for each of double,
float32, PCM24, PCM16, noiseRMS1e-6 and noiseRMS1e-5. Native deterministic
Box-Muller noise uses a different sequence from the earlier NumPy probe.
The alias-search additions exercise4,27 and256 actual candidate combinations;
the five-partial over-cap case records one skipped alias order and rejects.

| Analytic measurement | Maximum error | Unchanged limit |
|---|---:|---:|
| Frequency |.000679617987Hz|.02Hz|
| Relative amplitude |.000142082785 (.0142083%)|.002 (.2%)|
| DC |2.75778458e-7|1e-6|
| Clean source-center phase |5.54578606e-11rad|1e-4rad|

NumPy independently computes a complete training-length FFT and SVD-based
linear least-squares fit at the native output frequencies. It verifies guide
bins/magnitudes, fitted coefficients, residual diagnostics, normal-equation
residual, analytic truth and explicit rejection behavior. No native FFT/QR
implementation is reused. The checker streams strict JSONL, enforces the whole
profile inventory and treats warnings as errors. It is a separate command,
not an automatic CTest dependency.

Final Release and Debug comparisons: **289/289 PASS in each build**, with
identical reported maxima. Maximum normalized errors:
guide magnitude9.09494702e-13 (limit1e-9), coefficient1.30798151e-13,
residual diagnostic1.66928895e-13 and normal-equation residual1.04345519e-13
(each limit1e-8). NumPy version2.3.5, single BLAS thread.

Both Release and Debug contiguous gates pass **5984/5984 windows (100%)** across32 source
profiles and187 successive N8192/H2048 centers between logical3 and11 seconds.
Maximum errors: frequency1.6378e-12Hz, relative amplitude1.51767e-13,
DC2.41474e-15 and center phase3.48166e-13rad, identical in both builds. No window
rejects. Maximum work is4,911,311 units; whole-job maxima are73.6728ms Release
and115.962ms Debug. These are offline source estimates, not output rendering.

## Independent comparison exposed unstable flat-spectrum peaks

The first direct-FFT comparison failed at case244, the impulse control. Its
mathematically flat spectrum had tiny different local maxima under the two FFT
algorithms. The original native output still rejected the impulse, but its
guide selection depended on roundoff. That failing log is retained.

Peak detection now requires an increase over the left bin of64*epsilon*T in
training-normalized FFT units and permits that tolerance against the right
bin. With normalized samples bounded by1, this suppresses accumulation noise
and makes the first plateau edge deterministic. The checker applies the same
declared peak-selection contract to the independently computed spectrum.
Explicit impulse and Nyquist assertions require zero spurious interior guides.
No fitting, acceptance or output threshold was relaxed.

## State, scheduling and limits

Both final build variants passed16 dirty pending copies across capture,
statistics, FFT and solver/fit progress, exact work partitions1/97/65536,
source mutation after capture, reset/reuse, hidden outputs, wrong FFT dimensions
and changes of FFT profile during a pending transform. Nonfinite samples,
invalid shapes/rates and normalization loss fail closed. A source entirely
below the physical1e-12 floor may be approximated by a constant, explicitly
tested. Capture/advance/copy/reset allocation counts are zero.

The corrected expanded alias fixture reaches **125,519,134 work units**. The selected
20/28Hz reference uses **3,534,349**. An entire job must span multiple audio
calls; no complete renderer deadline follows from this helper's measurements.

Final focused CTest: Release1/1 in6.86s, Debug1/1 in18.08s. The expanded
256-assignment job takes up to496.22ms Release and1850.41ms Debug when completed
without audio scheduling. Ordinary-O3-DNDEBUG and-g compiler flags remain;
no new per-file optimization or formal benchmark bypass was introduced.

Serial budget diagnostics cover two/eight partials atN8192 and two atN65536.
The largest p99/max across the three jobs is:

| Build | Work per call | Largest p99 µs | Largest call µs |
|---|---:|---:|---:|
|Release|4096|34.375|354.333|
|Release|16384|132.042|221.541|
|Debug|4096|167.875|455.500|
|Debug|16384|631.625|1777.46|

Raw capture measured1.417..9.625µs Release and2.292..8.917µs Debug atN8192;
N65536 measured10.709µs and8.875µs respectively. Timing-fixture work ranged
from3,927,188 to7,982,011 units. Complete checkpoint copies and other DSP are
outside these timed calls. In particular, the observed1777.46µs Debug call at
16384 units exceeds the666.667µs duration of a48kHz/32-frame block. This budget
is unsuitable for that callback. Smaller budgets and combined callback/state
copy verification remain required; the4096-unit samples alone do not establish
a complete renderer deadline. The large call is retained, without attributing
it to an unmeasured operating-system cause.

Known coverage limits remain explicit: exact decimation collisions; full
integer aliases annihilated by differencing; half-integer aliases that collapse
to a real mode; more than eight components; over256 alias combinations; and
any signal failing stationarity/conditioning/guide rules. The tested boundary
frequencies include1500/750Hz atN8192 and187.5/93.75Hz atN65536/Fs48000.
Conservative rejection is not general stationary-signal support. The work cap
does not reinterpret the Detectors control.

## Reproduction and next integration

Build/run `talas_incremental_stationary_model_tests` or its CTest entry (300s
timeout). `--dump` exports289 JSONL profiles; run
`tools/check_stationary_model_probe.py --input <export.jsonl>` with bundled
NumPy and one BLAS thread. `--stress` checks5984 consecutive source windows.
`--budget-probe` times three fixture shapes separately from the full renderer.
Logs/exports are under registered build/workbench-debug with prefix
`time-frequency-scale-native-model-`; original and expanded failed/successful
results are retained, and `stable-guides-*` identifies the corrected peak rule.

Next: retain an owned unwindowed source capture in PreparedTimeFrequencyScale,
schedule analysis across calls, validate completed models against the current
source, preserve phase/component identity, and handle confidence/fallback
transitions. Then verify full callback/checkpoint cost and all64 original
production output cases. ExistingN8192, latencyD10239, source authority and
rollback contracts remain required. Automatic source analysis alone does not
close the34/64 output failure.
