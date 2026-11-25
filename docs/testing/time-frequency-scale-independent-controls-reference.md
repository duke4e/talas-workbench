# TimeFrequencyScale independent-control evidence

2026-09-27. Rate and Scale are decoupled for the live, file and quarter-speed
sources. All remain bounded candidates, not checked against an external
reference, with open delivery acceptance.

## Reproduced failures and changes

The initial Scale2/Rate.25 fixture fails direct-DSP construction and runtime
compilation. The latter explicitly reports the old.5..4 correction bound.
Current original-rate analysis never uses that quotient in synthesis: Rate
selects source centers, while Scale changes phase advance and peak placement.
A shared finite-positive control predicate replaces TFS-only quotient checks in
DSP, compilation, latency, offline/prepared plan verification and render calls.
Neutral detection uses exact control equality. MaxScale, Nyquist, source authority,
window capacity, reset and rollback checks remain in place. Generic PhaseVocoder
correction APIs and their numerical limits are unchanged.

The initial expanded cross product passes30/30 profiles. A warmed source slowed
to Rate1e-12 then fails the independent346Hz pitch measurement: projection.000616053
and a peak search reaching its+2Hz edge. Clock addition rounds stationary, making
the old phase estimator use FFT-bin frequencies. The existing auxiliary window,
one hop earlier in original source time, now also handles source advances below
one sample. It already handles advances exceeding a synthesis hop.

The first tiny-rate probe also included a measurement error: its nearly-zero-Hz
resampling rival included the fixture's DC.08. The corrected projection subtracts
only this known offset and retains the separate DC-preservation requirement.
Rate1e-5 passes that correction; Rate1e-12 still fails until the DSP repair.
Both logs remain available, and all amplitude/frequency thresholds are unchanged.
After repair,32/32 DSP profiles pass, including both tiny rates. At1e-12 the
projection is.0571497 and the scanned pitch offset is0Hz. Invalid zero, negative
and non-finite controls reject before direct-DSP output or state changes; corrected
continuation matches a pristine processor, with zero processing allocations.

## Connected-control transition scope

The public fixture combines independently scheduled pitch and time controls with
four known source tones. Its first4s measurement overlaps a declared native
spectral-to-raw blend: Rate becomes.5 at frame192037 while Scale is.5. The two
paths have the same steady86.5Hz frequency and potentially different phase.
That mixed window has projection.0353216 and scanned offset-1.25Hz.

A diagnostic measures.0611209 immediately before the blend and.0624957 after it;
every inspected sample after theN-frame blend equals independently computed raw
source interpolation, with maximum error0. The failed mixed measurement is
retained as transition-quality evidence. It is not a settled-pitch measurement.
This package does not fix or claim an exact phase/gain waveform inside that blend.

The shared oracle now explicitly excludes neutral-path mix intervals as well as
the declared four-hop Scale ramp and conservative synthesis support. Only this
new joint-control fixture uses centers4.5/5.75 in place of4/5.5, with the Scale4
event moved to246037. These satisfy the stated support bounds; numerical pitch
and gain criteria remain unchanged. Earlier F1 tone/chirp schedules stay intact.
Read-only review confirms the source-clock, neutral-mix and scale-ramp bounds.

The corrected joint-control fixture also injects invalid Rate0 at frame300017,
while the valid profile is Scale4/Rate.5. Complete-call silence, corrected retry,
31/128-frame partitions and full reset replay are required. File and completed
live-recording suites additionally cover Scale2/Rate.25 and Scale.25/Rate1.

## Release verification

All six complete Release suites pass51.87s: general PhaseVocoder1.39s, source-window
DSP35.94s, compilation1.74s, live recording1.74s, file source.74s and quarter-speed
runtime10.31s. The corrected joint-control oracle passes24/24 measurements across
offline and prepared31/128-frame execution. Its minimum desired projection is
.0581947 and every scanned frequency offset is0Hz. Complete reset replay and
late failure/retry comparisons pass in both partitions.

Together with the32 DSP profiles, all56 new independent measurements pass.
DSP minimum desired projection is.0571469, maximum scanned offset0Hz and maximum
mean-DC error.0032898, within the existing.006 bound. Previous variable-source
tone/chirp tests remain enabled. The coverage-prose suite passes14/14 tests.
Generated help was refreshed during an intermediate build; the final six-target
build ran again afterward to ensure every test links the final core metadata.

## Debug and final integration verification

All 12 selected Debug suites pass in 247.52s: general PhaseVocoder, source-window
DSP, compilation, offline rendering, checkpoints, prepared scope authority,
runtime conformance, realtime rendering, live recording, file source,
quarter-speed runtime and resource snapshots. The 32 DSP and 24 joint-control
measurements also pass in Debug. CLI and Workbench build successfully; the build
retains only the previously recorded duplicate-library linker warnings.

Generated catalog, capability matrix and node-help checks pass. All native
work ran in the root's serial
lane and has completed. No new hardware deadline measurement was taken for the
expanded control profiles; the existing staged three-transform work bound remains.

## Retained diagnostics

Logs are under registered `build/workbench-debug`, with prefix
`time-frequency-scale-independent-controls-`:

- `red-build.log`, `red-dsp.log`, `red-runtime.log`: original coupled rejection.
- `dsp-build.log`, `domain-probe.log`: decoupled domain and initial tiny-rate probe.
- `tiny-clock-build.log`, `tiny-clock-red.log`: corrected DC measurement and
  remaining actual tiny-clock pitch failure.
- `dsp-probe.log`: repaired32-profile DSP result and invalid-control checks.
- `runtime-probe.log`, `neutral-diagnostic.log`: original joint-control window
  over the mix, followed by the independent endpoint/interpolation diagnostic.
- `release-build*.log`, `release-tests.log`: Release integration builds and tests.
- `debug-build.log`, `debug-tests.log`: Debug regressions and product builds.
- `capability-write.log`, `help-write.log`, `coverage-tests.log`: generated
  evidence and prose checks.
- `generated-check.log`: final generated-artifact freshness checks.

The finite-positive admission domain is broader than these measured cases.
Transition quality, arbitrary spectra and continuous streaming remain open.
Functional tests and native source comparisons are not external reference
captures or hardware deadline certification.
