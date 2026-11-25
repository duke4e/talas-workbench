# Waveshaper live MemoryWriter function verification

Verified native candidate, 2026-09-27.

## Source and correction

When ShapeFrom is Wavetable, FromMemoryWriter makes Waveshaper use a shaping
function recorded in real time instead of a disk file. MemoryWriter names the
shared recording and supports recapture, cyclic overwrite and reading during
recording. The former native Waveshaper
rejected the checkbox in both execution paths. The new focused test first
failed at that admission boundary.

The compiler now orders the named writer before a Waveshaper reader even when
they occupy separate active audio branches. Both rendering paths use the
existing per-sample memory-reader schedule and the writer's actual mono
storage. Preparation authenticates one prior writer with matching scope and
name; it does not copy or substitute an immutable table. Current and past
writes are visible, and future callback samples cannot leak backward. Existing
writer reset and transactional journals own the state; Waveshaper adds no
private recording history. Both shaping lanes can index the same live mono
curve independently. File-backed stereo curves and polynomial shaping retain
their existing paths; polynomial plus FromMemoryWriter rejects.

An initial fixture's concurrent-read expectation inadvertently included an
unwritten future slot. The independent correction is explicit: after two
writes, the four-slot curve is [-1, -.5, 0, 0]. Input -.25 selects coordinate
1.125, giving -.4375. The completed curve would instead give -.375. The fixture
must distinguish those values rather than reproduce the native implementation.

Same-Sound name isolation, write-before-read visibility, zeros before the first
write, mono fallback, positive bounded capture and bipolar clamped lookup are
native conventions. Stereo MemoryWriter curves, cross-Sound storage,
zero-duration live capture and exact timing/channel rules are not modelled.

## Verification

All **seven focused groups** pass in **0.87s**:

1. Concurrent and completed linear lookup match independent vectors despite
   the immutable 99-valued decoy. The distinction at input -.25 proves no
   future-slot visibility. Native save/reopen, 1/7 callbacks and reset reproduce
   the result; prepared callbacks and reset allocate zero.
2. Nearest lookup with an independent right input produces different lanes,
   including zeros for not-yet-written slots. Offline and prepared 1/3/7
   partitions agree without callback allocation.
3. A second cyclic pass overwrites the curve. The next four samples are
   [.25, .5, -.15625, .375] under the declared current-write convention.
   Offline and prepared 1/7 callbacks match the independent vector.
4. A non-cyclic trigger-low interval retains the completed curve; the next
   rising edge records new values. Unwritten positions keep their old contents
   until overwritten. Prepared output follows each change without allocation.
5. A scheduled gain change causes downstream overflow at the fourth sample,
   after recording updates. Both outputs are silenced, and a repaired retry
   matches a pristine session with zero allocations on every attempt.
6. An offline third-sample failure follows a completed three-slot recording.
   Retrying a valid two-sample prefix matches a fresh session and independently
   proves restoration of unwritten slots, capture state and input position.
7. Validly compiled missing/duplicate/wrong-scope writer graphs, a zero-duration
   live capture and polynomial-plus-memory mode reject in both renderers.

All **eight broader CTest groups** pass in **172.80s**: catalog, compile,
offline, prepared scope authority, runtime conformance, realtime, CLI examples
and Sound Library. Runtime/CLI/Workbench builds pass, with only existing
duplicate-library warnings. Source-only integration review found no routing,
snapshot, reset or rollback omission. No UI code changed.

The test draft needed fixes before acceptance: missing ProjectJson declarations,
vector/array comparisons, an input-pointer offset, an oversized reset callback,
indistinguishable right-channel expectations, and an explicit empty coefficient
list rejected by native project serialization. The missing-writer fixture also
had a duplicate edge; all rejection cases now require successful compilation
before checking runtime rejection. These were fixture corrections, not weaker
numeric acceptance or runtime workarounds.

Catalog/help/coverage now describe the live route and Waveshaper's mutable
resource dependency. MemoryWriter's stale monitor-only and no-prepared-table
descriptions were also corrected to match its existing tested readers and
cyclic recording implementation.

Generated catalog/help/capability freshness passes. Tracked and new-file
whitespace checks pass. A separate TriggeredTableRead MemoryWriter route was
still rejected by both renderers at this point; it is handled in
[its own package](triggered-table-read-memory-writer-reference.md).

Logs under the registered disposable `build/workbench-debug` root use the
`waveshaper-memory-writer-` prefix: `red-build.log`, `red-test.log`,
`runtime-build.log`, `regression-build.log`, `regression-tests.log`,
`reference-build.log` (draft compile failure), `reference-build-2.log`,
`reference-tests.log` (draft serialization failure), `reference-build-3.log`,
`reference-tests-2.log` (final focused pass), and generated metadata check logs.
