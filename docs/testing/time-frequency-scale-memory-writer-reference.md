# TimeFrequencyScale live MemoryWriter evidence

Historical source-route evidence follows. The [staged followup](time-frequency-scale-staged-reference.md) now uses1279 active latency at8192Hz/min20; original1023-frame measurements below describe the earlier synchronous processor.

2026-09-27. Native implementation and verification complete.

TimeFrequencyScale has independent Rate and FrequencyScale and may read a
Sample recorded by MemoryWriter before or in parallel with the Sound. FromMemoryWriter selects that real-time recording. The first
named-recording fixture serialized/reopened and compiled, then failed at the
unconditional live-source rejection. Both original renderers rejected the mode.

The compiler now places an active same-scope named writer before the reader.
Both renderers activate bounded writer storage and schedule its source per
sample. Offline resolves the unique prior writer's vector; prepared rendering
authenticates and retains its operation index. Neutral interpolation and the
existing phase vocoder both read that storage. The immutable-table path remains,
and a same-name static decoy cannot replace a selected live recording. FromDisk
remains rejected. No new recording copy or DSP algorithm is introduced.

The writer's journal and existing reader cursor, Gate and phase-vocoder
checkpoints retain whole-call rollback and reset. The descriptor reports mutable
resource access. Native same-scope ownership, mono storage, current-write
visibility, unwritten zeros, retained recapture slots, whole-table boundaries
and existing phase-vocoder mapping remain native candidate conventions.
Detector interpretation, live interpolation timing, header loops,
cross-Sound/global storage and multichannel recording remain open.

## Verification

All **eight focused groups** pass in **8.64 seconds**:

1. Native save/reopen selects the writer's `.25,.5,.75,1` recording instead of
   a same-named static table filled with99. Both renderers agree.
2. The reader starts before capture. Independent initial zeros prove a large
   callback cannot read future writes. Gate restart after EOF plays the completed
   recording. Offline reset and prepared1/3/7/31 partitions preserve the vector.
3. Fractional raw playback observes cyclic overwrite and partial noncyclic
   recapture. Explicit16-frame vectors distinguish retained old slots, new
   writes, interpolation and Gate restart. All callback partitions agree.
4. Active scale/rate profiles1/.5,2/1 and.5/1 at8192Hz consume a completed
   4096-sample recording. They match the existing static-source DSP reference,
   preserve1023-frame latency, and produce actual nonzero audio after capture
   and latency. Prepared1/7/128 partitions and reset agree without allocation.
   This is source-route equivalence, not an independent oracle for the DSP.
5. A cyclic writer and active vocoder first produce nonzero audio. A128-frame
   callback then fails on its final frame due to a connected Rate of0, after
   new writes and a transform boundary. Output is zeroed; repairing Rate and
   retrying matches a pristine session through2304 further samples. Failure,
   retry and all partitions allocate zero.
6. Offline downstream overflow occurs after a three-slot recording completes.
   Retrying a two-frame valid prefix returns initial zeros; the next two frames
   match a fresh session and the explicit expected values. Failed-call capture
   slots, input position and control timelines are restored.
7. Successfully compiled missing, duplicate, parent-scope and zero-duration
   writer graphs reject in both paths, as does FromDisk plus FromMemoryWriter.
8. The compiler's authentic writer latency compensation is admitted. An
   untracked duplicate writer output, a self-referencing compensation record
   and forged active-reader latency all reject prepared admission; forged
   latency also rejects offline rendering.

The active profile test exposed a genuine prepared integration issue:
compensation duplicates the writer's BufferSpec, and MemoryWriter counted it
as a second original output. It now uses the existing structurally validated
`countOriginalOutputs` and `matchesOriginalBuffer` helpers. A source-only review
confirmed that exact original output ownership and malformed-compensation
rejection remain enforced. The negative tests above protect this change.

Logs are retained under registered disposable `build/workbench-debug` with
`time-frequency-scale-memory-writer-` prefix. `red-build.log` / `red-test.log`
record the original rejection. `reference-tests-2.log` and
`diagnostic-test-2.log` preserve the compensation failure. A temporary diagnostic
print initially failed to compile because an optional latency needed value
access; `diagnostic-build.log` retains that failure. The print is removed.
All eight final groups pass in `reference-build-4.log` / `reference-tests-4.log`.

The earlier eight broader groups passed173.97 seconds before the compensation
fix. All eight groups pass again in172.92 seconds after the final runtime change:
catalog, compiler, offline, prepared scope, conformance, realtime, CLI examples
and Sound Library (`regression-tests-2.log`). Runtime, CLI and Workbench rebuild
successfully (`regression-build-2.log`); only the existing duplicate-library
linker warnings remain. No UI changed.

Generated catalog/help/capability freshness passes. Tracked and new-file
whitespace and final-newline checks pass.

The TimeFrequencyScale live MemoryWriter source is now a bounded candidate.
