# TimeFrequencyScale staged processing evidence

2026-09-27. Bounded native verification complete.

The later [live Gate/reset audit](time-frequency-scale-gate-reference.md) finds
and repairs repeated large-vector clearing while a live reader is stopped. It
also records the remaining32-frame active callback limit; the128-frame results
below retain their original measurement scope.

The original-rate DSP and source-window capture times are preserved. A pending
job distributes spectral computation across a hop, addingH to native latency.
The exact sealed active delay isN+N/4-1; static neutral playback retains0.

## Focused verification

Five groups pass15.21s: DSP, TimeFrequencyScale checkpoint, live MemoryWriter,
prepared disk and quarter-speed references. They preserve the original tone,
gain, DC, EOF, source-authority and rollback assertions, with explicit expected
latency updated to1279/5119/10239 forN1024/4096/8192. No numerical threshold was
relaxed. The overflow oracle observes through completion of the staged frame
and requires the same nonfinite/overflow status.

New checks cover:

- Incremental FFT budgets1/7/257 atN16/1024/8192/65536, exact operation counts,
  zero-work preservation, storage rejection, full-spectrum equality and copied
  mid-inverse cursors with different subsequent budgets.
- TFS snapshots every31 samples across a hop containing the auxiliary FFT,
  copied state/owned clone/retiming, allocation-free continuation and comparison
  to an uninterrupted independently partitioned session.
- A live source replaced immediately after capture. The already captured
  audible frame remains unchanged while its FFT is pending. The initial fixture
  checked an early silent part of the first half-speed window; its audible
  assertion failed. Moving capture to a fully populated window at3N preserves
  that assertion and supplies a meaningful source-retention oracle.

The initial staged Debug probe covers all eight48k/128 window/rate profiles with
zero measured deadline misses. Max successful call is0.620ms, versus the2.667ms
deadline. Both late failures and repaired retry retain exact state, silence and
allocation checks. A subsequent expanded probe also tests different mid-hop
failure points. The expanded Release probe also has zero deadline misses across
all eight profiles and all five timing categories (success, boundary failure,
boundary repair, mid-hop failure and mid-hop repair). Its maximum successful
call is0.783ms; mid-hop failure restoration reaches0.749ms. Every measured
failure silences the complete block and every repaired continuation is exact.
The same Release DSP/checkpoint suites pass2/2 in1.01s.

The final expanded Debug probe likewise passes all40 category/profile rows with
zero misses. Maximum success is0.724ms; maximum across successes, failures and
repairs is0.903ms. Final successful-call maxima in milliseconds:

| N | Rate | Debug | Release | Active delay, frames |
|---:|---:|---:|---:|---:|
|8192|.25|.260|.474|10239|
|8192|2|.279|.279|10239|
|16384|.25|.683|.269|20479|
|16384|2|.308|.401|20479|
|32768|.25|.372|.465|40959|
|32768|2|.410|.479|40959|
|65536|.25|.722|.707|81919|
|65536|2|.724|.783|81919|

Each success row contains1024 successive public calls after2N warmup. The source
is40Hz plus0.1DC, FrequencyScale1, sample rate48kHz, block128. These are ordinary
local diagnostics on Apple M1/AppleClang21, including synchronous capture and
full runtime snapshots. All raw outliers remain in the logs. The formal
clean-source benchmark campaign and its provenance rules remain unchanged.

Read-only review checked capture/ring indices, overlap ordering, the64N work
budget, reset and all copied cursor/buffer state. It identified the synchronous
capture cost as a remaining component to measure; the public probe includes it.
Finite measurements do not prove all graphs, devices or OS scheduling behavior.

## Logs

All logs are in registered `build/workbench-debug`, prefixed
`time-frequency-scale-staged-`:

- `dsp-build.log`, `dsp-tests.log`: first build and retained silent-fixture failure.
- `focused-build.log`, `focused-tests.log`: final focused pass5/5.
- `debug-probe.log`: first staged full-session Debug timing.
- `release-build.log`: optimized runtime and DSP test build.
- `release-probe.log`, `release-tests.log`: expanded Release timing and2/2 pass.
- `final-build.log`:22 targeted test executables, CLI and Workbench rebuild;
  only existing duplicate-library linker warnings.
- `final-debug-probe.log`: expanded final Debug timing, including mid-hop retry.
- `final-tests.log`:22/22 regression groups pass204.04s, covering catalog, FFT/
  STFT/generic pitch and diffuse/live spectral consumers, compile/offline/
  realtime, scope/conformance, both phase checkpoints, all three TFS source
  reference suites, resource snapshots, AudioStream, examples and Sound Library.
- Catalog, capability and help regenerate in dependency order.
  `generated-check.log` passes with the final rebuilt CLI.
- `coverage-tests.log`:14/14 pass0.058s. Whitespace and newline checks pass.
