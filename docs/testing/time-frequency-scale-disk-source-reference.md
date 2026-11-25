# TimeFrequencyScale prepared disk source evidence

Historical source-route evidence follows. The [staged followup](time-frequency-scale-staged-reference.md) now uses1279 active latency at8192Hz/min20; original1023-frame measurements below describe the earlier synchronous processor.

2026-09-27. Bounded implementation and verification complete.

TimeFrequencyScale selects a disk recording through FromDisk, and Sample names
that file. The first real PCM16 WAV fixture reproduced compilation rejection of
an Audio resource referenced by TimeFrequencyScale. Both original renderers
also rejected FromDisk. Wavetable and live MemoryWriter selection did not
implement this separate source.

## Implemented candidate and limits

Compilation now permits a distinct Audio snapshot when FromDisk is true and
FromMemoryWriter false, limited to mono PCM16 WAV at the exact render sample
rate with1..1,048,576 frames. Existing AudioStream preparation authenticates the
hash and media shape and retains private immutable backing. Offline uses the
already decoded graph resources. Realtime preparation decodes that private
backing outside playback and copies the samples into the operation's existing
vector. No new WAV parser, callback IO, source cursor or DSP algorithm is added.

This is bounded prepared-file playback. **Continuous disk streaming for this
node remains unimplemented.** Direct disk reading is broader than this
candidate. File formats beyond mono PCM16 WAV, channel/rate policy, Gate
release, detectors, phase, latency and scaling DSP are not calibrated against
an external reference. Static and live
memory sources retain their existing routes and native conventions.

Two integration defects were found and fixed:

- Standalone TimeFrequencyScale was omitted from the complete callback rollback
  range. A late Rate failure advanced its cursor/vocoder and broke corrected
  retry. It now selects the existing whole-call checkpoint driver, including
  cursor and vocoder copy/retiming. The fixture checks file and table modes.
- TimeFrequencyScale was omitted from the compiler-owned sampling source seal.
  A substituted snapshot generation was accepted by both renderers. The compiler
  now retains its plan/settings/deep resources; offline admission and realtime's
  sampling classifier and admission require their exact authority. Connected
  unused fallback handling is preserved. Source identity and retained bytes
  cannot be changed after compilation.

## Focused verification

All seven groups pass in2.14 seconds with the native project-loader fixture:

1. Independent PCM16 samples survive JSON roundtrip and replacement of the source
   pathname after compilation. Offline and prepared output retain the accepted
   recording and EOF zeros; prepared playback allocates nothing. The fixture
   loads the persisted Audio manifest through the native project asset loader.
2. Active scale/rate profiles1/.5,2/1 and.5/1 at8192Hz match an equivalent immutable
   table containing decoded PCM16 values. All preserve1023-frame active latency
   and produce nonzero DSP audio. Prepared1/7/128 partitions and reset agree.
3. An explicit20-frame vector checks fractional raw interpolation, delayed Gate,
   interruption/restart and retrigger after EOF. Offline reset and allocation-free
   prepared1/3/7/31 partitions/reset reproduce the vector.
4. File and table modes first advance2040 active samples. Rate0 on the last frame
   of a128-frame call fails after cursor/transform advancement and clears output.
   A repaired retry and2304 later frames match pristine sessions, without allocation.
5. Offline gain overflow occurs after valid disk samples have advanced playback.
   Retrying a two-frame valid prefix matches the fresh session and independent
   expected values; the failed call does not leave the cursor advanced.
6. Audio used in wavetable mode, simultaneous disk/live selection, a real source
   above the frame bound, differing rates, stereo and unsupported WAV format all
   reject compilation. Missing disk resources and same-name tables compile but
   reject both rendering paths.
7. Altered source selection, snapshot generation/hash/retained bytes, and active
   latency all reject both renderers. Authentic authority is admitted first.

The accepted live MemoryWriter suite also passes again: eight groups in8.72
seconds after the final runtime changes. The file loader uses the existing Audio
PCM16 convention: negative codes divide by32768, nonnegative by32767. An initial
test incorrectly expected WavReader's negative scaling; the explicit-16384 oracle
was corrected to-.5 without changing either decoder.

## Retained verification history

All logs use `time-frequency-scale-disk-source-` under registered disposable
`build/workbench-debug`. The first `red-build.log` records fixture mistakes in
ProjectJson API names and initializer-to-span construction; `red-build-2.log`
compiles and `red-test.log` is the genuine missing-source RED.
`reference-test.log` records the PCM oracle mismatch. `reference-tests-2.log`
and`-3.log` record failed active retry. `reference-tests-4.log` reached the next
fixture, which needed the mixer's required second input wired. `reference-tests-5.log`
and`-6.log` expose the snapshot generation admission defect. `reference-tests-7.log`
caught the realtime sampling classifier missing the new compiler seal; that
classifier now includes TimeFrequencyScale. Both suites pass in
`reference-build-8.log` / `reference-tests-8.log`. The final extended disk fixture
passes in `reference-build-9.log` / `reference-tests-9.log`.

All ten broader groups pass in183.85 seconds (`regression-tests.log`): catalog,
compiler, offline, prepared scope, conformance, realtime, resource snapshots,
AudioStream, CLI examples and Sound Library. Runtime, CLI and Workbench rebuild
successfully (`regression-build.log`), with only preexisting duplicate-library
warnings. No UI changed.

Generated catalog, capability matrix and node help checks pass. The first
metadata check found stale generated output; that failure is retained in
`generated-check.log`, and the final pass is `generated-check-2.log`. Tracked
and new-file whitespace checks pass.

The disk source is a bounded candidate. Continuous streaming for this node
remains open.
