# TimeFrequencyScale state-copy evidence

2026-09-27. Native performance/correctness work on existing candidates.

## Diagnosis and scope

The earlier [Gate/reset repair](time-frequency-scale-gate-reference.md) left
occasional active48k/32,N65536 deadline misses. The optional
`--component-deadline-probe` retains real cyclic live capture, a fresh immutable
source comparison, exact silent latency prefix, independent40Hz pitch/gain and
zero C++ allocations. Public live and immutable calls are measured first. A
separate direct DSP pass times state-copy and processing calls individually and
compares every output sample to the public live result (existing1e-10 tolerance).
All passes use the same recording and source-clock trajectory.

Timings are divided by whether the callback contains an original source-window
capture atN-1+kH. The13 capture blocks all fit before the change; the3 live and4
immutable misses occur among the8,690 other blocks. One live miss occurs at
prefix13,376, before any FFT. Separate kernel capture maximum is.100ms, other
kernel maximum.076ms, while copy maximum is.615ms. The passes have different
cache/scheduling histories; their values are not an additive profile and do not
prove that every outlier is caused by a specific operation.

The change preserves atomic capture, DSP arithmetic, nativeN+H-1 latency and
all source/control admission. Histories and scratch now copy only live values
for the current processing stage; full overlap and persistent phase state are
retained where continuation needs them. The source clock retains at most two
already-written future centers. No callback checkpoint or validation is removed.

## Release results

Apple M1 arm64, AppleClang21, ordinary Release configuration. This is not the
formal clean-source benchmark campaign or hardware xrun certification.
Each public active pass has8,703 full32-frame callbacks; deadline.666667ms.

| Measurement | Before p50/p99/max, ms | After p50/p99/max, ms | Misses before → after |
|---|---:|---:|---:|
|Public live, all active|.197/.462/.812|.147/.316/.514|3 → 0|
|Public live, capture only|.245/.295/.375|.219/.229/.242|0 → 0|
|Public immutable, non-capture|.196/.463/.942|.143/.312/.497|4 → 0|
|DSP state copy, non-capture|.109/.377/.615|.068/.184/.460|0 → 0|
|DSP processing, capture only|.093/.098/.100|.095/.098/.155|0 → 0|
|DSP processing, non-capture|.0004/.062/.076|.0004/.062/.080|0 → 0|

The active public live median falls25.4%; the state-copy non-capture p99 falls
51.3%. The repaired stopped/pulsed measurements also have zero misses. Pitch
projections remain.0624999 at40Hz versus.00000550786 at10Hz; latency, complete
waveform comparisons and zero-allocation checks pass. Raw outliers are retained.
These finite measurements do not prove all graphs, sample rates or OS behavior.

Release DSP/checkpoint/live suites pass3/3 in35.67s. New DSP regression checks
1,942 copied states across all seven window sizes and two changing-rate profiles,
with unrelated nonzero destination histories and two-window audible continuation
comparisons at1e-12 tolerance. Smallest-window hops use every-sample checkpoints;
larger windows include first/wrapped states andH/32 offsets, plus owned clones.
Existing reset, first-frame, source capture and independent tone oracles remain.

The expanded48k/128 checkpoint probe also passes all40 window/rate/category
rows with zero misses. Maximum success.519458ms, maximum across successes and
failure/repair outcomes.5365ms, versus2.667ms. N8192/16384/32768/65536, Rate.25/2,
FrequencyScale1 retain boundary and mid-hop failures, entire-output silence,
exact repaired continuation and zero C++ allocations.

## Final Debug and integration gates

All11 Debug suites pass190.24s: DSP, compile, offline, TFS checkpoint, scope
authority, conformance, realtime, all three TFS source-reference suites and
resource snapshots. CLI/Workbench rebuild succeeds with only the existing
duplicate-library linker warnings. Generated catalog, capability and help
freshness checks pass. The existing live and quarter-speed sources gain fixture
evidence; their status and admission remain unchanged. Whitespace and
final-newline checks pass.

The final Debug component probe has zero deadline misses in every row and
passes all waveform, latency, source identity, pitch/gain and allocation checks:

| Measurement | p50/p99/max, ms | Full calls |
|---|---:|---:|
|Public live, all active|.180/.356/.471|8703|
|Public live, capture only|.256/.302/.319|13|
|Public immutable, non-capture|.152/.325/.536|8690|
|DSP state copy, non-capture|.067/.192/.540|8690|
|DSP processing, capture only|.097/.117/.152|13|
|DSP processing, non-capture|.0005/.062/.082|8690|

Stopped/pulsed public Debug maxima are.175/.101ms. All rows retain the.667ms
comparison for48k/32,N65536. This resolves the measured profile in both builds;
broader graph/platform deadlines and agreement with an external reference remain
unverified. The earlier failing baseline and every outlier remain in the logs.
Read-only review found no concrete flaw in the two-clock formulas, stage ranges,
phase history or dirty-destination continuation invariants.

## Logs

All logs are under registered `build/workbench-debug`:

- `time-frequency-scale-component-{build,release-probe}.log`: initial diagnostic
  and baseline.
- `time-frequency-scale-state-copy-{release-build,release-tests,release-probe,
  release-retry-probe}.log`: corrected build, three suites,32-frame component
  timing and128-frame complete rollback timing.
- `time-frequency-scale-state-copy-{debug-build,debug-tests,debug-probe}.log`:
  complete Debug build,11-suite verification and final component timing.
- `time-frequency-scale-state-copy-generated-check.log`: final generated-artifact
  checks.
