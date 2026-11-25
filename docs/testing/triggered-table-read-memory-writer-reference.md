# TriggeredTableRead live MemoryWriter evidence

2026-09-27. Native candidate implemented and verified.

TriggeredTableRead can read a real-time MemoryWriter recording selected through
FromMemoryWriter. MemoryWriter supports named capture, reading while writing,
recapture and cyclic overwrite. The initial independent named-recording fixture
saved/reopened and compiled successfully, then failed because the offline reader
unconditionally rejected the live source. The original prepared admission also
unconditionally rejected it.

The compiler now orders a reachable same-scope named writer before this reader.
Both renderers include TriggeredTableRead in writer storage activation and
per-sample live-reader scheduling. Offline rendering resolves the unique prior
writer's vector; prepared rendering authenticates and retains its operation
index. Same-named static resources cannot replace a selected live recording.
Only successful live-writer resolution bypasses static table admission; exact
descriptor/plan/settings/resource seals and static snapshot checks remain.

The existing checked cursor/held-value/Gate kernel is unchanged. Its checkpoints
and reset combine with the writer's existing storage journal and capture state.
The descriptor and compiler seal now describe mutable resource access. This
package adds no separate copy of the recording.

Positive-Trigger reading, Gate edge ordering, mono storage, same-scope naming,
write-before-read timing, unwritten zeros, retained old slots on recapture and
whole-table loop/EOF behavior remain native candidate conventions. In particular,
reading while Trigger stays positive rather than only when it becomes positive
is a model choice. Header loops, cross-Sound storage, multichannel recording and
exact timing are not modelled.

## Verification

All **eight focused groups** pass in **0.90 seconds**:

1. The reader starts two samples before the writer. Independent output
   `[0,0,0,0,.25,.25,.25,.5]` proves no future-slot leakage, Trigger hold,
   Gate reset and static-decoy exclusion. Native save/reopen and prepared
   1/3/7 callback partitions preserve the vector without allocations.
2. Trigger holds the reader until the recording completes, then plays the
   four recorded samples in order through both renderers.
3. Cyclic overwrite changes all four slots during playback. Both offline reset
   and allocation-free prepared reset reproduce the independent eight-sample
   vector; prepared 1/3/7 partitions agree.
4. A noncyclic recapture preserves the old final slot until it is overwritten.
   The EOF output changes from1 to.375 as the new final sample arrives; a
   subsequent Gate edge replays the recaptured table from its beginning.
5. A prepared seven-frame call fails downstream on its final sample after a
   delayed capture and repeated writes to a three-slot cyclic recording.
   Output is zeroed. Repairing the gain and retrying restores both writer and
   reader state and the scheduled capture, matching a pristine session and
   independent `[0,0,0,.25,.5,.75,2]` vector. All attempts allocate zero.
6. Offline failure after five frames follows a completed three-slot recording.
   Retrying three frames returns initial zeros, proving stale failed-call
   contents are absent; the next frame matches the fresh-session continuation.
7. Successfully compiled missing, duplicate, parent-scope and zero-duration
   writer graphs reject in both renderers despite the static decoy.
8. Tampering with live-source selection, writer identity or the sealed static
   decoy after compilation rejects in both paths; the live route retains
   compiler-owned plan/settings/resource authority.

Build and test logs are retained under the registered disposable
`build/workbench-debug` root, with `triggered-table-read-memory-writer-` prefix.
`red-build.log` records successful fixture compilation and `red-test.log`
records the original execution rejection. `runtime-build.log` and
`regression-build.log` pass after the source-route changes, including Workbench.
All eight broader CTest groups pass in173.02 seconds (`regression-tests.log`):
catalog, compile, offline, prepared scope, conformance, realtime, CLI examples
and Sound Library. Generated catalog/help/capability checks pass. Tracked and new-file whitespace
checks pass. A source-only review found no route or checkpoint omission. No UI changed.

Test drafts needed corrections: heterogeneous partition-array sizes, vector/array
comparison, a missing third argument to the borrowed-input render overload,
and explicit caller-owned input buffers with advancing offsets. Scheduled writer
Trigger uses the existing connected Control path; direct Trigger/Gate scheduling
belongs to the reader. The recapture fixture also had to schedule second-callback
Gate changes relative to that callback, within its seven-frame capacity. Runtime
admission and numeric expectations were not weakened. Initial failures are
retained in `reference-build.log`, `reference-tests-2.log`,
`reference-build-3.log` and `reference-tests-5.log`. Final acceptance is
`reference-build-6.log` / `reference-tests-6.log`.

An adjacent audit found TimeFrequencyScale's live MemoryWriter source still
rejected in both renderers at this point; that separate gap is handled in
[its own package](time-frequency-scale-memory-writer-reference.md).
