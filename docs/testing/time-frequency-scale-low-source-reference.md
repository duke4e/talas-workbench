# TimeFrequencyScale original-rate source analysis

2026-09-27. Bounded native verification complete. This extends the
bounded quarter-speed candidate; it is not checked against an external
reference.

## Independent failures and repair

The existing runtime passed renderer-equivalence tests but failed all six
40/80/160Hz quarter-speed cases at48kHz, with and without0.1DC. The first40Hz
case projected0.000185686 at the requested40Hz versus0.0617084 at the unwanted10Hz.
The original oracle and thresholds remain. With original-rate analysis and
N8192, that same runtime window measures0.0606182 versus0.000659044; all six
frequency peaks lie at their original frequencies. The DC case measures
0.0608606 versus0.00037728. These are Hann/N projections, not peak amplitudes.

Standalone steady-window tests add20/40/80/160/269Hz with and without DC. For20Hz
over0.1DC the desired projection is0.0624717, slowed pitch0.000938446, and mean
0.100261. Two simultaneous80/233Hz partials survive five frequency/rate profiles,
including active down/up shifts and a4x frequency scale.

A separate admitted Rate8/Scale4 case exposed source-hop phase ambiguity:
original108Hz should become432Hz but projected0.000161204 there and0.0512607 at
420.28125Hz. An auxiliary original-rate window one hop earlier removes that
ambiguity. The first repair still lost gain because the translated lobe followed
an integer FFT bin. Measured continuous frequency now sets its displacement;
the final projection is0.0603697 at432Hz versus0.000189817 at the old alias.
Neither the original frequency oracle nor its gain threshold was relaxed.

## State, sources and boundaries

- Opt-in long FFTs pass a known137-bin complex tone and roundtrip at8192 and65536;
  the default4096 limit, non-power-of-two and invalid-profile rejection remain.
- Source hints select8191/4095/1023 latency at48k for20/40/160Hz. Connected hints
  always prepare against20Hz, independently of unused0.1Hz/900Hz fallbacks.
  A late unsupported hint fails without allocation; repaired playback matches
  pristine long clock, phase and overlap histories for9000 further samples.
- Callbacks, reset and state copies allocate zero. Partitions1/7/128, owned clone,
  mismatched-window copy rejection and transport retiming preserve output. The
  connected-neutral path equals independently computed delayed interpolation.
- A recording delivered sample by sample matches a completed recording at
  Rate.25/.5/1 with actual low-tone output. Existing runtime table/file/live
  source authority, Gate, cyclic/partial recording and failure fixtures remain.
  Fast readers use current zero/retained live slots; no future recording values
  are promised. Read-only review confirmed vector lifetime and bounded access.
- Centered source and synthesis windows have finite support. EOF observation
  lengths now include the derived N/2*(1+1/Rate) tail and another silent interval;
  the1e-12 silence threshold is unchanged. Earlier too-short observation failures
  remain in the logs. Gates and broad tests were lengthened and given audible
  assertions where the larger latency could otherwise hide failures in silence.

## Reproducible evidence

All logs and the optimized probe are in the registered `build/workbench-debug`
artifact root, prefixed `time-frequency-scale-low-source-`:

- `red-test.log`: genuine original low-source failure.
- `dsp-tests.log` / `runtime-tests.log`: initial corrected-pitch passes followed
  by too-short EOF observation failures; live and disk suites already pass.
- `regression-tests.log`: six groups pass123.36s before the later fast-hop audit.
- `reference-tests-3.log`: correct low-tone projections; a new control fixture
  used a Hertz event in scalar Constant and was corrected to scalar units.
- `fast-hop-red.log`: real phase alias; `fast-hop-fix-tests.log`: correct pitch
  but insufficient gain; `fast-hop-gain-tests.log`: complete DSP suite passes3.56s.
- `final-build-2.log`: all selected native targets, CLI and Workbench rebuild
  after the final kernel/header changes. Only pre-existing duplicate-library
  linker warnings; no UI changes.
- `final-tests.log`:18 of19 groups pass in a211.62s invocation. The remaining
  conformance fixture checked audibility before the new latency. It now uses a
  longer recording and commits113 samples before a real synthesis boundary,
  then fails777 samples later while the delayed control transition is active.
  Its audible assertion and exact retry comparison remain. The sole changed
  test was rebuilt and rerun: `conformance-build-2.log` /
  `conformance-tests-2.log`, full conformance group passes26.19s. All19 selected
  groups thus have final passing evidence; no production code changed between
  those two invocations.
- Catalog, capability and help outputs were generated in dependency order;
  generated-artifact freshness checks pass. The first coverage test preceded
  report regeneration and retained its expected stale-report failure;
  `coverage-tests-2.log` is final.

The19 groups cover catalog, NativeStft, generic pitch, TFS DSP, packed FFT,
compile, offline, TFS/FrequencyScale checkpoints, scope authority, conformance,
realtime, live/file/quarter-speed reference suites, snapshots, AudioStream,
examples and Sound Library. No broader unrun suite is implied.

## Timing and remaining performance boundary

The fast-hop Debug kernel reached8.529ms in a128-sample call at48kHz, exceeding
its2.667ms audio-block budget. A standalone `-O3 -DNDEBUG` probe compiles the same
test, `PreparedTimeFrequencyScale.cpp` and `Radix2Fft.cpp`, linking existing DSP
helpers. It passes the same numerical/state/no-allocation tests; its worst
observed fast-hop kernel call was892.875microseconds. Logs:
`optimized-build.log`, `optimized-probe.log`. This demonstrates an optimized
kernel margin for the tested N8192 case, not whole-graph/hardware deadlines or
performance of every supported window. Debug small-buffer use and larger-window
deadline coverage remain explicit open work.

Followup full-session measurements and the bounded Debug repair are recorded in
[complete callback evidence](time-frequency-scale-callback-reference.md).
N8192/16384 now meet the measured128/48k deadline in Debug and Release, including
failure restoration; N32768/65536 still have genuine frame-boundary misses.

The [staged followup](time-frequency-scale-staged-reference.md) addsH to output
latency and distributes spectral work while preserving the original capture
instant. Earlier N-1 latency measurements above are historical.

Detector behavior, exact algorithm/latency, transient/broadband quality and
continuous disk streaming remain open.
