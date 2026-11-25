# TimeFrequencyScale complete callback measurements

2026-09-27. Apple M1, arm64, AppleClang21.0.0. Bounded local observations;
no external reference or hardware xrun claim.

This records the synchronous baseline and initial Debug optimization. The
[current staged followup](time-frequency-scale-staged-reference.md) distributes
work across calls and adds an explicit synthesis-hop delay.

## Reproduction

Build `talas_time_frequency_scale_checkpoint_tests` and
`talas_time_frequency_scale_dsp_tests` in the existing ordinary Debug or Release
configuration. Run the checkpoint executable with `--deadline-probe`. Default
CTest continues to run its six normal checkpoint cases. This opt-in mode is
separate from the formal clean-source performance campaign; its gate is intact.

The probe measures public128-frame `render` calls at48kHz, including rollback
snapshots. Each window/rate profile warms up2N frames, times1024 calls, then
measures16 late downstream failures and8 repaired retries across synthesis
boundaries. It checks independent40Hz pitch/gain/DC, zero C++ allocations,
whole-call silence on failure, and exact repaired output plus another window of
continuation against a pristine session. Read-only review verified the boundary
alignment, source support and snapshot inclusion.

## Observed successful-call maxima

All values are milliseconds; the128/48k deadline is **2.667ms**. Each entry is
the maximum of1024 successive measured calls, not an average. Rates are at
FrequencyScale1. Every retained run passes its non-timing assertions.

| N | Rate | Initial Debug | Optimized Debug | Optimized Release | Optimized misses Debug / Release |
|---:|---:|---:|---:|---:|---:|
|8192|.25|6.816|.706|.776|0 / 0|
|8192|2|8.698|.824|1.331|0 / 0|
|16384|.25|13.059|1.423|1.359|0 / 0|
|16384|2|18.382|1.733|1.680|0 / 0|
|32768|.25|27.285|2.906|2.816|2 / 1|
|32768|2|39.132|3.765|3.580|16 / 16|
|65536|.25|57.938|5.894|5.816|8 / 8|
|65536|2|81.810|8.247|7.123|8 / 8|

ForN8192/16384, failure restoration and repaired retry also have zero measured
deadline misses. Their largest observed call is1.855ms in Debug and1.737ms in
Release. Larger-window failure/retry calls still miss deadlines. All raw
p50/p95/p99/max values and misses are retained, including initial Debug outliers
above58ms. SinceN65536 synthesizes in only8 of1024 calls, its low p99 alone would
hide every expensive frame. The peak and forced-boundary measurements matter.

The initial measurements bracketed a wrapper containing allocation-sentinel
bookkeeping. Current measurements time public `render` directly, with that
bookkeeping outside the timestamps. Both include the full runtime snapshot.
These observations are not a controlled five-report optimization campaign.

## Changes and correctness

- Debug builds on AppleClang/Clang/GNU optimize only the TFS source processor and
  FFT translation unit with`-O2`; existing`-g` remains. Standard FFT consumers
  therefore also receive the Debug optimization. No fast-math flags are added;
  the compiler's default FP-contraction policy is retained. Other compiler IDs
  have no new Debug flag or deadline claim.
- Explicit LongAnalysis transforms use real/imaginary butterflies. The Standard
  profile's algorithm, constructor admission and4096 limit stay intact.
- Float/double long-profile transforms match an independent direct DFT at16/64,
  the standard profile through4096 and an arbitrary-complex inverse roundtrip.
  Existing8192/65536 known-bin and roundtrip tests remain. Source NaN/infinity
  and finite-input FFT overflow reject without allocations.
- No source modes, accepted windows, latency, control mapping or snapshot
  contents were reduced or changed. The complete large-window deadline issue
  remains open; a scheduling repair must preserve current-source snapshot
  semantics and checkpoint any in-progress spectral work.

## Retained logs

Logs are in the registered `build/workbench-debug` root, prefixed
`time-frequency-scale-callback-`:

- `debug-build.log`, `debug-probe.log`: initial full-session failure evidence.
- `release-build.log`: checkpoint build succeeds, then Make cannot resolve the
  newly configured DSP target in that first multi-target invocation.
  `release-dsp-build.log` succeeds; `release-probe.log` is the initial Release
  measurement, also showing genuine large-window deadline misses.
- `fft-release-build.log`, `fft-release-build-2.log`, `fft-release-tests.log`:
  final Release correctness/checkpoint groups pass2/2 in1.17s.
  `fft-release-probe.log` contains the current Release observations above.
- `final-debug-build.log` rebuilds the targeted suites, CLI and Workbench;
  `fft-consumers-build.log` rebuilds the remaining direct FFT consumers.
  Existing duplicate-library linker warnings remain; application build succeeds.
  `final-debug-probe.log` contains current Debug observations above.
- `final-tests.log`:17/17 regression groups pass160.34s, including spectral
  primitives, STFT, generic pitch, TFS DSP, live analysis, packed FFT, diffuse
  stretch, compile/offline/realtime, conformance, scope authority, both phase
  checkpoint suites and live/file/quarter-speed TFS references.
- `generated-check.log` passes after the final CLI rebuild. Whitespace and
  final-newline checks pass.

These measurements improve evidence and Debug execution; they do not close the
remaining large-window performance work.
