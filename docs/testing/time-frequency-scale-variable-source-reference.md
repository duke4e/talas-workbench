# TimeFrequencyScale changing-source evidence

2026-09-27. Independent native source-time and pitch measurements extend the
existing [quarter-speed evidence](time-frequency-scale-quarter-speed-reference.md).
Its bounded implementation status and open delivery acceptance remain
unchanged, and it is not checked against an external reference. The fixture,
source-support guard and numerical criteria are described below.

## Oracle and scope

An eight-second48kHz source contains four successive53/173/389/701Hz tones or a
linear80Hz-plus16Hz/second chirp, each with amplitude.25 and DC.08. The source
clock independently integrates six Rate segments (.25/.75/.5/1/.25/.5) with
non-aligned changes. FrequencyScale stays1. The native analysis window8192 and
hop2048 imply delay10239, which is checked against the compiler and silent prefix.

Eight half-second measurement windows test the tone at the independently
predicted source position. Conservative source and synthesis support remain
inside a plateau. The chirp is measured against the integrated expected local
frequency, allowing an arbitrary constant output phase. The alternative
resampled chirp is distinguishable outside Rate1; that mathematical exception
is explicit. Wrong plateau and slowed-tone projections are also tested.
All output samples must be finite, and the last second must be silent after
the independently integrated source clock and support have passed EOF.

The shared test header contains source mathematics and measurement code only.
DSP and both public renderers each face that same analytical oracle. Prepared
callbacks of31 and128 samples schedule Rate changes inside calls, require zero
C++ allocations and compare the whole timeline with offline output. The128-frame
case also resets without allocation and replays the entire source/event timeline.
The agreement comparison supplements the mathematical measurements.

## Results

The first direct DSP probe passed16/16 measurements using criteria fixed before
the run. No production DSP or runtime change was needed for these cases.
After sharing the reference and adding the runtime fixture, ordinary Release
passes both complete test suites: DSP34.58s and quarter-speed runtime6.54s,
41.14s total. All64 analytical measurements pass:

| Path | Tone measurements | Chirp measurements |
|---|---:|---:|
|Direct DSP|8/8|8/8|
|Offline|8/8|8/8|
|Prepared,31 samples|8/8|8/8|
|Prepared,128 samples|8/8|8/8|

All four paths have the same values to the printed precision. Tone projection
minimum is.0624342, versus the predeclared.045 floor, and every tone scan peaks
at the expected frequency. The minimum ratio to a wrong tone is954.1.
Chirp projection minimum is.0478426; the minimum applicable resampling-rejection
ratio is107.3. The largest frequency offset is+1Hz at logical output5.5s,
exactly the allowed bound. That reduced coherent projection and frequency offset
remain visible limitations of the candidate rather than perfect chirp tracking.

Debug also passes both complete suites (DSP36.50s, runtime32.56s,69.08s total)
and all64 measurements with the same printed values. Read-only review confirms
the pre-integration clock indexing, conservative support bounds and Rate1
exception. The5.5s chirp result was identified as the narrowest numerical margin;
its measurement window and predeclared criteria were retained.
Generated catalog, capability matrix and node help freshness checks pass. Production sources and their existing callback
timing evidence are unchanged; only these two affected native suites were rerun.

The fixture does not establish an exact transition waveform, broadband audio
quality, arbitrary chirp slope, simultaneous FrequencyScale automation,
continuous disk streaming or agreement with an external reference.

## Retained logs

All logs are under registered `build/workbench-debug`, prefixed
`time-frequency-scale-variable-source-`:

- `baseline-build.log` and `baseline-probe.log`: initial direct DSP fixture.
- `release-build.log` and `release-tests.log`: shared reference, public runtime
  fixture,64 measurements and both complete Release suites.
- `debug-build.log` and `debug-tests.log`: Debug build and focused verification.
- `generated-check.log`: generated-artifact freshness.

These are functional measurements. Prior callback timing evidence remains in
its own records; no new performance campaign is claimed for this test-only work.
