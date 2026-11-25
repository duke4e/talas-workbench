# ADR-0009: Immutable Resource Generations and Streaming Ownership

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

`ProjectAsset` currently persists an ID, table/binary kind, and path. Asset
loading produces mutable vectors copied into `RenderSettings`. File watching can
notice changes, but there is no content identity, media shape, generation, or
rule that keeps an active session valid while replacement loads. Real disk
streaming is absent; DiskPlayer reads an in-memory table and DiskCache is a
passthrough subset.

Streaming must not move file I/O, decoding, allocation, or waiting onto the
audio callback. Hot reload must not mutate storage still referenced by an
active graph. Path changes and content changes must also remain different user
decisions.

## Decision

### Canonical format-10 manifest

Project format 10 gives every asset these canonical fields:

- stable project-local `id`;
- media kind: table, binary, or audio;
- project-relative or source `path`;
- disposition: referenced or embedded;
- lowercase SHA-256 `contentHash`, or empty when not yet verified;
- `channelCount`, `sampleRate`, and `frameCount`, each zero when unknown or not
  applicable;
- explicit `encoding`, empty when unknown;
- positive `generation`, with migrated unknown assets starting at 1;
- text `provenance`, empty when not supplied.

ID, path, hash, and generation are separate. Moving bytes with the same verified
hash changes only path. Explicitly accepting different bytes creates a new
generation. Reverting to prior bytes still creates a new generation because the
project-visible replacement event is new.

The manifest never stores a runtime pointer, decoder object, worker, cache
cursor, or host-specific handle.

### Immutable prepared snapshots

Successful local inspection/loading creates a `ResourceSnapshot` identified by
`(assetId, generation)`. Its descriptor and any fully loaded payload are
immutable. Prepared graphs hold `shared_ptr<const ResourceSnapshot>` values.
An active graph therefore retains generation N until it is retired even if a
watcher finds changed bytes or generation N+1 is staged.

Load and replacement are failure-atomic:

1. resolve and inspect off the callback;
2. verify expected hash/shape when present;
3. construct a complete immutable snapshot;
4. prepare all dependent runtime state;
5. stage the new graph/resource set;
6. publish only at a callback boundary;
7. reclaim the old graph and snapshot after audio-thread retirement.

A failure preserves the active generation and reports the reachable structural
source path.

### Streaming ownership

Each prepared `(assetId, generation)` has exactly one non-audio
`DecodedBlockCacheOwner`. It owns the file, decoder, worker, cancellation token,
and a worker-only block-indexed decoded cache. Audio callbacks never reference
that shared cache directly.

Each consuming node owns a separate prepared `StreamConsumerState`: playback
cursor, monotonically increasing seek generation, source-rate conversion
history, prefetch window, telemetry, and bounded request/response queues. A
consumer seek invalidates only requests/responses tagged with that consumer's
older generation; it never moves a shared decoder cursor or invalidates another
consumer. The worker may satisfy several consumers from its block-indexed cache
and copies complete decoded blocks into each consumer's preallocated response
slots.

The callback and worker communicate through bounded SPSC structures and
atomics:

- each consumer publishes requests through its own bounded SPSC request queue;
- worker publishes completely decoded blocks through that consumer's bounded
  SPSC response queue, tagged with consumer and seek generation;
- callback consumes only matching-generation frames without waiting;
- seek/retrigger advances that consumer's generation and ignores older slots;
- worker discards obsolete work per consumer without disturbing others;
- cancellation is cooperative and bounded;
- teardown/join occurs off the callback after the owning graph retires.

All per-consumer queue capacity, block memory, resampler history, and scratch are
prepared in advance. Acquire/release publication protects complete blocks; the
callback never observes a partially written block.

### Initial media and underrun policy

The initial streamed format is mono/stereo PCM16 WAV. Channel count, source
sample rate, frames, and encoding are verified against the manifest. Source-rate
conversion uses the reviewed prepared interpolation path and retains its state
per consumer.

If required source data is absent at callback time:

- emit zero for every missing output frame/channel;
- continue destination transport and playback cursor progression;
- increment atomic underrun-event and missing-frame counters exactly once;
- never block, retry synchronously, decode, read a file, log, or format text.

The policy is deterministic across callback partitions. UI/CLI consumers may
format coalesced telemetry on non-audio threads.

`talas.diskPlayer` consumes this stream contract. `talas.diskCache record=false` may
prewarm or share the read cache while retaining audio passthrough semantics.
Record/write streaming remains unsupported in M3.

### Missing-asset recovery authority

M4 Step 3 extends the same manifest identity into one explicit recovery
workflow. A user-selected file is fully inspected off the message and audio
callbacks. Exact SHA-256 plus kind/media shape creates a same-generation
relocation; a verified different hash creates an inert replacement candidate;
an unverified migrated manifest may accept a first inspection without consuming
a generation.

Automatic discovery searches only a user-selected local directory. Traversal
is deterministic, contained below the canonical root, does not follow directory
symlinks, and is bounded to 65,536 visited entries, 4,096 hashed regular files,
and 256 diagnostics plus a truncation marker. More than one exact path remains
ambiguous and requires owner selection. Cancellation, capacity exhaustion, and
failure publish no partial document edit.

`ProjectEditor` validates a complete resolution batch before one failure-atomic
transaction. Exact relocations preserve every identity field except path;
confirmed different bytes advance the generation exactly once. Resolver work
never changes the runtime identity ledger or active prepared graph. Embedded
package assets never fall back to external resolution: missing or tampered
embedded bytes remain package-integrity failures.

## Compatibility and migration

- Versions 1-9 migrate assets to generation 1 with unknown hash/media metadata
  unless a later explicit inspection fills it.
- Existing table and binary resources remain loadable as immutable snapshots.
- Existing projects do not silently hash or rewrite external assets during
  parse; inspection is a deliberate loading/import step.
- Path rebasing and packaging preserve identity/generation. Embedded packages
  verify content against the manifest before staging.
- The current table-based DiskPlayer subset remains available as a compatibility
  path until the streaming defining tests pass.

## Realtime impact

The audio callback may dereference immutable descriptors, consume prepared
blocks, update consumer cursors, and increment atomics. It may not touch the
filesystem, decoder, allocator, mutex, condition variable, worker lifecycle,
hash function, JSON, error string, or logger.

## Alternatives rejected

- Mutating a shared vector in place would race active readers and erase
  generation identity.
- Treating a modification timestamp as content identity would confuse moves,
  copies, and same-timestamp replacements.
- Using path as identity would break relocation and packaging.
- A callback-owned decoder or blocking cache miss would violate realtime
  safety.
- An unbounded worker queue would exchange callback safety for unbounded memory
  and cancellation latency.
- Auto-accepting changed bytes would hide a material project change.
- Searching by basename, extension, size, timestamp, or directory order would
  turn a path guess into false content identity.
- Falling back from an invalid embedded package asset to external bytes would
  weaken package integrity and portability.

## Acceptance evidence

- canonical format-10 migration/round-trip and package verification tests;
- same-content relocation and changed-content generation tests;
- active-generation lifetime and failed-replacement tests;
- full-load versus streamed PCM16 mono/stereo/source-rate fixtures;
- forced underrun, seek generation, cancellation, restart, and teardown stress;
- structural diagnostics for missing/changed resources inside reusable Sounds;
- bounded exact-hash directory recovery, ambiguous-match selection, one-Undo
  multi-asset relocation, explicit changed-byte confirmation, Save As/recovery,
  package portability/integrity, and ordinary CLI workflow tests;
- callback instrumentation proving no allocation, lock/wait, file/network I/O,
  decode, parse, format, or log.

## Amendment (2026-10-12): a stream joins each callback's transaction

A failed realtime callback rolls back the checked DSP state it touched and
plays the block again. A DiskPlayer's stream consumer took part in that
only when a parent owned the callback (a source session), so a live
MIDIVoice, whose voices need the rollback, refused a DiskPlayer. Now every
session no parent owns opens a callback checkpoint on each DiskPlayer
consumer as a callback starts: the consumer keeps its cursor, direction
and the response blocks it read provisional, and the callback either
commits them (publishing its refill requests and health counters) or puts
them back for the retry. The checkpoint is a few scalars per consumer, so
the callback still allocates, locks and waits for nothing. A DiskPlayer
then plays inside a live MIDIVoice as it plays alone, and offline (whose
MIDI voices render through a realtime session) as in realtime.


## Amendment (2026-10-19): stereo files play in stereo

Taken as recommended (standing approval 2026-10-12). A KeyMappedMultisample's
files are all mono or all stereo, and Sounds are stereo, so from project
format 95
a Sample, Multisample, KeyMappedMultisample, MultifileDiskPlayer and
DiskPlayer play a stereo file's left channel on `audioOut` and its right
channel on a new `rightOut`; a mono file plays on both. A table player renders
the right channel from the same state as the left (its checked kernel run a
second time on the right table, the candidate state kept from the left), so
both channels start, loop and release together. A DiskPlayer streams each
channel of a stereo file through a stream of its own: the graph prepares a
left and a right owner beside the averaged one (`AudioStreamChannel`), the
two consumers are seeked, restarted, read and checkpointed in step, and
offline reads the same channels' full loads. Each node takes `channels`,
`stereo` (the default) or `legacyMono`; formats 1 through 94 acquire
`legacyMono`, under which they play as before: a stereo table refused where a
field names it (Sample, KeyMappedMultisample, a DiskPlayer's table), its left
channel alone where a list does (Multisample, MultifileDiskPlayer), and a
streamed file's channels averaged.
