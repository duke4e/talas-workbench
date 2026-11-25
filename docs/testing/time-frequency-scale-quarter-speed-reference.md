# TimeFrequencyScale quarter-speed evidence

2026-09-27. Bounded candidate verification complete. Quarter-speed playback is
a bounded candidate; it is not checked against an external reference, and
delivery acceptance remains open.

This records the initial package. The subsequent
[low-source audit](time-frequency-scale-low-source-reference.md) changes the
active DSP to original-rate source windows. Its latency depends on sample rate
and MinInputFreq; subsequent staged work adds a synthesis hop, so the current
8192Hz/min20 fixtures use1279 frames. Fixed-transform
descriptions and test timings below are historical evidence for that package.

## Source and independent failure

Rate0.25 means quarter-speed playback, with independent frequency scaling. FrequencyScale1 therefore requires retained
pitch. The first fixture failed compilation because correction4 lay outside the
old0.5..2 candidate. It is retained in `time-frequency-scale-quarter-speed-red-test.log`.

## DSP investigation

An explicit TimeFrequencyCorrection profile extends only this consumer to
correction0.5..4. It travels through preparation, process, reset, clone, state
copy and realtime rollback. General frequency-scale and known-pitch helpers
retain their prior limits. Source lookup, cursor interpolation, fixed transform
storage and1023-frame active latency remain unchanged.

The initial range-only change passed the native runtime fixture but failed a
separate93.75Hz→375Hz DSP test at48k. Measured amplitudes were0.06351 at375Hz,
0.05736 at187.5Hz and0.05568 at562.5Hz. Merely enabling single-peak identity locking
still failed periodicity. The cause was dilation of the Hann lobe for a fixed
synthesis hop. The extended path translates peak regions without changing lobe
width and uses scaled instantaneous phase advances. It then measured375Hz at
amplitude0.25, with inspected spurious components below1.3e-16. The projection
record is saved separately from CTest's replaceable LastTest log.

Additional DSP checks exercise nine combinations of55/110/173Hz and2.5/3.5/4
correction, two simultaneous independent peaks, exact partition agreement,
default-consumer rejection, profile-mismatch copying and changed-state clones.

Review identified DC being moved with the first positive-frequency peak. DC's
periodic-Hann support at bins0-1 now retains its original complex coefficients,
outside harmonic phase locking. Keeping only this support avoids a whole-spectrum
bypass. Absolute source/mapped peak admission also retains a quiet AC partial
under dominant DC. Fixtures pass for both constant polarities, DC plus a tone,
and an0.001-amplitude tone over0.5 DC. A2→4→3.5→2 correction timeline crosses the
mapping boundary and rounded translations, with equal arbitrary partitions,
bounded output and the existing hop-continuity gate.

## Runtime checks

Four focused runtime groups pass with the final DSP changes:

1. Two successive269/509Hz source tones appear at their stretched positions
   with measured pitch peaks within1Hz, while slowed and wrong-segment tones are
   at least10 times weaker. Playback outlasts the source duration and reaches
   EOF plus its transform tail. Both renderers agree for1/7/128-frame partitions,
   with exact native startup latency and allocation-free callbacks/reset.
2. Connected Rate changes0.5→0.25→0.5→0.25 occur within callback boundaries.
   Gate release/restart matches fresh quarter-speed playback; offline reset
   and arbitrary prepared partitions reproduce the full event timeline.
3. A late unsupported correction fails after127 progressed samples and a live
   correction transition. Corrected retry plus4096 later frames matches pristine
   state. Offline late failure followed by a valid prefix tests owned DSP clones.
4. Zero/negative rates and corrections outside the expanded bounds reject.

The existing disk and live MemoryWriter suites now include1/0.25 alongside their
previous active profiles. They pass with complete source duration, partition,
reset, source authority and transaction checks. These compare native sources;
they are not external reference captures.

## Retained verification history

All logs are inside registered `build/workbench-debug`, prefixed
`time-frequency-scale-quarter-speed-`:

- `red-build`/`red-test`: real compilation rejection of the quarter-speed profile.
- `reference-build`/`reference-test`: first independent runtime pitch/duration pass.
- `reference-build-2`: test-only `std::min` integer type mismatch, repaired.
- `reference-build-3`/`reference-tests-3`: source suites pass; low-tone DSP failure.
- `diagnostic-build`/`diagnostic-test`: measured severe hop sidebands.
- `single-peak-build`/`single-peak-test`: insufficient phase-lock-only attempt.
- `peak-region-build`/`peak-region-tests`: four suites pass27.62 seconds.
- `peak-projections`: retained amplitude projections and source-suite output.
- `reference-build-4`/`reference-tests-4`: broader tone/range DSP checks pass9.57 seconds.
- `reference-build-5`/`reference-tests-5`: final DSP/DC/transition checks and all
  three runtime source suites pass28.52 seconds (DSP9.96s, live13.87s, file2.75s,
  quarter-speed1.94s).
- `regression-build`/`regression-tests`: twelve broader groups pass186.99 seconds
  before the final DC fix; this is intermediate evidence only.

- `regression-build-2`/`regression-tests-2`: final app/CLI rebuild and all twelve
  broader groups pass186.18 seconds after the DC/quiet-partial fix. These include
  both scaling checkpoint suites, catalog, compile, offline, prepared scope,
  conformance, realtime, AudioStream, snapshots, examples and Sound Library.
- `coverage-tests`: stale prose assertion still expected correction2; updated to
  the new native profile. `coverage-tests-2`: all14 tests pass.

Phase, gain, transients, detectors and latency are not calibrated against an
external reference. Rate values above one have no defined error/clamp policy.
Continuous disk streaming and an arbitrary correction range remain open.
Very low tones within the transform's DC lobe, weak sub-bin mixtures and broader
audio-quality behavior remain unaudited; these focused cases do not prove them.

Generated catalog, capability matrix and node help checks pass. No UI was
changed. The CLI and Workbench rebuild reports only the prior duplicate-library
linker warnings.
