# Common audio and terminal-event conformance harness

Updated 2026-09-05. This implements the initial P1 audio/note/control, terminal-event,
reset/replay and terminal failure/retry extensions; the full
P1 roadmap remains open. No node capability is promoted, and agreement between
the two internal paths is not taken as evidence of any external reference.

`compareOfflineAndPreparedRealtime` remains the compatibility entry point.
It compares one offline transaction with the supplied realtime partition.
`runRuntimeConformance` additionally accepts `ConformanceRenderOptions`:

- `offlineBlockPartition`: an explicit offline transaction sequence. Empty
  retains the single transaction. Both partitions must cover exactly the
  positive requested frame count. Realtime blocks cannot exceed preparation.
- `midiNotes`: ordered absolute-frame MIDI note-on/off events. Array order is
  retained for equal-frame events. A note on a callback boundary belongs to
  the following positive callback; empty callbacks do not consume it.
- `referenceChannels`: optional independent expected samples for every output
  channel and requested frame. Both paths must match these, not merely each
  other. Mismatch diagnostics identify comparison, channel and frame.
- `realtimeZeroFrames`: require success (default) or rejection of an actual
  zero-frame call. Existing profiles have different empty-transaction
  contracts. Calls are exercised rather than skipped; continuation samples
  reveal unintended state advancement. Offline empty calls must succeed and
  publish no samples/events in this initial interface.
- `referenceMidiEvents` and `referenceGlobalControllerEvents`: optional
  independent terminal traces. A disengaged option makes no independent claim;
  an engaged empty span requires silence. Frames, sequence, MIDI bytes and
  controller names must match exactly. Controller values use the declared
  finite numeric tolerance. Oracles must be ordered and inside the render
  horizon; the harness never sorts them into agreement.
- `verifyResetReplay`: after a successful first pass, call the same offline
  session's `reset()` and realtime session's `resetTransport()`, then replay
  the same partitions and inputs. The second pass must satisfy the same
  independent oracles and match each path's first audio/event trace. A fresh
  realtime object is not substituted for its reset API. MIDI ingress is
  rescheduled because offline reset discards that schedule. Success retains
  the first-pass result and sets `resetReplayVerified`; failure identifies
  the replay phase and returns its diagnostic/output evidence.
- `variableAssignments`: ordered absolute-frame assignments to exact prepared
  project, workspace or Sound-instance numeric cells. Same-frame array order
  is retained (the last assignment to a cell wins), and the entire frame is
  published as one typed transaction. When mixed with `midiNotes`, controls
  precede those notes in the same frame's transaction. Empty callbacks consume
  neither list. Invalid identity, domain, frame or nonfinite value fails the
  run. Voice-owned cells are not admitted by this non-voice convenience schedule.
- `inputEvents`: an alternative ordered variant stream of notes, numeric
  variables, Sound-variable selections and Event Value assignments. It cannot be combined with the two convenience
  schedules. Frames must be nondecreasing; within a frame, array order becomes
  the exact typed-event sequence in one atomic transaction. The stream admits
  voice-owned numeric/Sound assignments with an explicit nonzero `voiceGeneration`.
  Static preparation identity/domain or candidate definition are checked first; runtime transaction
  validation checks the final slot generation after accounting for notes in that
  frame. This admits a variable assignment following the note that creates its
  voice without falsely requiring a live publication before rendering starts.
  Non-voice cells require zero generation. No event sorting is performed by
  the runner. The native executor applies notes before validating assignments
  against the final generation of the atomic batch; therefore an assignment
  may also precede its creating note in the same frame. Exact sequence does not
  imply sequential cross-kind evaluation.
  `ConformanceEventValueAssignment` resolves an exact prepared scope, name and
  domain (global scope by default). Values must be finite. Derived projections
  cannot be assigned directly; their writable source must be changed instead.
  Targets requiring preparation reject at typed dispatch. Event Value scopes
  belong to this prepared graph and do not supply voice-generation authority.
- `terminalCapacityProbes`: ordered realtime partition indices with reduced
  caller MIDI-event, controller-event or controller-name capacities. Indices
  include empty partition entries, but a probe must select a positive
  event-only callback. Repeated indices request multiple failed attempts;
  unspecified capacities retain normal storage. A probe must actually return
  `CapacityExceeded`, preserve every caller storage field/byte and publication
  metadata, then permit the regular callback to retry without reset, clock
  advancement or input rescheduling. The offline path remains the unmodified
  reference. `terminalFailureProbesVerified` counts completed failure/retry
  probes, including both passes when reset/replay succeeds. Interpret it
  together with `ok()` and the complete trace/oracle checks.


- `soundStateCheckpoints`: independent, complete ordered Sound-variable
  publications at strictly increasing public boundaries shared by both
  partitions. Frame zero observes state before input scheduling, and the final
  boundary is allowed. Callbacks are never split to accommodate checkpoints;
  zero-frame calls do not duplicate an observation. Each cell's exact identity
  and selected definition must match. `expectedAfterReset` optionally replaces
  that checkpoint's expectation in the replay phase and requires reset/replay
  to be enabled. Omission repeats the original expectation.

`offlineSoundStates` and `realtimeSoundStates` retain requested observations,
including their phase and publication validity. A successful replay appends its
observations to the first pass. `stateMismatch` identifies the path, frame,
cell index, field and reset phase. Realtime publication must be valid; offline
publication must contain the prepared cell count. These traces intentionally
cover identities/selections, not voice metadata or arbitrary hidden DSP state.
When checkpoints are enabled and an offline public render fails,
`offlineFailureSoundState` retains its requested start/frame count and actual
getter values before/after the call. These coordinates describe the attempted
call, not the internal progress of its DSP. Failure evidence is diagnostic;
it does not turn an unsuccessful render into a passing conformance result.

Reset/replay is opt-in because transport reset deliberately retains some
variable state and has profile-specific resource behavior. It asserts repeatable
output for the chosen fixture, not universal equivalence between transport
reset and document reload. Explicit Sound expectations can verify retained
selections even when frame-zero inputs restore repeatable audio. Other hidden
state still requires output oracles that expose the relevant behavior.

Outputs follow the prepared session's actual ordered channel count. The
harness no longer imposes a one/two-channel restriction; native four/eight
channel terminals are covered. It allocates on the caller's control/test
thread and is not an allocation-free audio callback.

Event-only terminals use `renderTerminalEvents` and retain empty audio channel
lists. The legacy offline zero-filled mono view is checked for silence before
being discarded. Result event traces use absolute frames; callback-local
frames are validated before rebasing. Both execution traces are compared,
then each is compared with any independent oracle. Event mismatches identify
the comparison, stream, event index and differing field. MIDI and controller
streams are compared independently; no cross-stream ordering is inferred.

Realtime event storage uses the graph's prepared terminal-event capacity.
Controller name storage is sized from the observed offline names; an unexpected
larger realtime name fails publication instead of truncating into a pass.
Capacity exhaustion is a failed run, not an empty successful trace. Empty
terminal callbacks require the explicit rejection policy and the runtime's
`InvalidArguments` result. Only the explicitly requested capacity probes inject
failed attempts before retrying; other unexpected failures stop the run.

Probe guards cover the whole backing buffers, including storage beyond the
reduced spans. Metadata starts with nonzero sentinels, and comparisons use
fields rather than struct padding. This catches partial publication and
overwrites outside the offered prefix without relying on an empty event count.
Probes do not inspect arbitrary internal journals: independent output and
continued input schedules expose state or queue consumption on failed calls.

The convenience note-only path uses the existing offline MIDI Voice/Mapper
scheduling API and prepared realtime note ingress API. Mixed notes/variables
and all explicit input streams use the typed transaction API in realtime.
The offline combined schedule also admits exact
non-voice numeric and Sound cells with zero voice generation; voice-owned assignments
still require a nonzero generation. Event Values use exact scope/name/domain
and zero voice generation. Combined control schedules do not
require a MIDI owner. Pure non-voice numeric or Sound schedules retain their
respective independent offline drivers. The MIDI path admits only notes handled by a
prepared region; unmatched notes are diagnosed rather than silently omitted.
The offline combined backend itself shares realtime allocator/execution machinery.
It supports both audio and event-only terminals. Terminal storage is bounded
by the prepared event capacity; controller-name storage uses the immutable
source text length as an upper bound for the trimmed prepared name. Events are
rebased from each internal callback to the public offline transaction before
the common runner converts them to absolute positions. A failed offline call
clears all returned audio/events, including any earlier output from that call.
This does not establish rollback of the entire multi-callback offline state.
Consequently this comparison alone is not an independent mixed-input oracle. The
analytical gate/oscillator fixture supplies independent expected values and
rejects a shared wrong result.

Control fixtures use a variable-driven oscillator gain alone, with a MIDI
Mapper gate and inside a MIDI Voice source. The Voice expectation retains the
native fixed-frequency oscillator's running phase while silent; this is the
native phase-reset contract. Independent samples cover boundary/interior updates,
same-frame last-wins order, irregular/empty callbacks and reset/replay with a
frame-zero baseline assignment. A two-cell expression remains finite only
when both same-frame updates are applied atomically. Negative cases reject
unsorted/out-of-range inputs, stale owner paths, incorrect domains and
nonfinite values. Direct offline MIDI admission also rejects forged non-voice
generations, invalid identities/domains and nonfinite assignments.

## Distinguishing checks

The explicit ordered-input regression fails when the new stream is ignored.
Its independent oscillator/gain oracle exercises note creation, a following
assignment to that generation, later control updates and same-frame release/
retrigger with the next generation. Reversing note-on/off order fails the
expected samples. Stale/unborn/zero generations, wrong owner/domain, nonfinite
values, backwards/out-of-range frames and ambiguous mixed APIs are rejected.
An explicit note-only stream retains the analytical gate oracle, and explicit
non-voice assignments retain the independent offline batch check. Transport
reset preserves voice generations: replaying this exact voice schedule with
stale generation numbers is explicitly expected to fail in the replay phase.

Sound-selector fixtures use independently calculated sinusoidal branches and
128-sample native crossfades, checking the first blend sample, complete fades,
same-frame last-wins selection and a later reverse fade across different
offline/realtime partitions. Pure Sound schedules use independent execution;
Sound/numeric schedules, with and without a MIDI Mapper, exercise the combined
adapter and reset/replay. A voice-owned selector exercises generation-aware
selection in the same transaction as voice creation, unborn/zero generations,
final-batch generation validation and expected stale-generation reset failure.
Unknown candidates, forged instance paths, non-voice generations and unknown
offline event kinds reject. The 128-sample fade is a native contract, not a
measured reference.

Direct offline regressions also check committed Sound publication and reset
retention. The combined backend supplies the getter's published values; reset
retains non-voice selections while rebuilding fresh voice state. Before any
new inputs, an internal preparation-only transfer seeds retained non-voice
selections in all realtime cell copies and publication atomics. It renders no
hidden samples and consumes no event slots. A post-reset numeric/MIDI schedule
that omits Sound assignments must still render the retained branch with reset
oscillator phase. Invalid admission leaves the session usable.

Event Value fixtures compare an independently calculated gain waveform against
live assignments and separately compiled native offline automation. The latter
admits one point per binding/frame; independently authored final points match
the live stream's duplicate assignments and last-wins order. Same-frame batches
evaluate their final snapshot: a temporary zero denominator followed by a valid
value succeeds, while the zero in its own transaction fails. Numeric controls,
Sound selections and MIDI Voice/Mapper notes compose in the same input stream.
Two original Sound instances use distinct oscillator frequencies to expose
wrong-scope updates; global-to-local projections preserve sparse overrides and
reject direct writes to derived bindings. Wrong names/scopes/domains, nonfinite
values, backwards/out-of-range frames and forged offline generation fields reject.

Live terminal inputs use hand-authored controller and MIDI-byte traces, including
same-frame last-wins order and unchanged-value suppression. Ten additional
capacity-failure probes across reset/replay cover queued Event Value/numeric
inputs, MIDI storage and trimmed controller names. Zero-frame calls consume no
inputs. A late invalid MIDI byte verifies that an offline failure returns no
partial event prefix. General offline rollback/retry remains open.
An unrelated live control splits MIDI-file execution into additional internal
callbacks while the hand-authored same-time message order and file cursor trace
remain unchanged across continuation/reset.

The initial multichannel regression failed on the old two-channel limit.
Current fixtures compare independent sinusoidal expectations for four and
eight distinct ordered channels; swap two reference channels; corrupt the
last sample of the last channel; and reject incomplete/nonfinite oracles.

The MIDI fixture applies a note-controlled gate and independent stereo gains
to an original oscillator. It checks notes inside callbacks and exactly on
boundaries, same-frame note-off/on ordering, offline continuation, empty
transactions and fresh-session repeatability. Reordering the equal-frame
notes must produce a reference mismatch. Out-of-range times, unsorted notes
and invalid channels fail admission.

Finite extreme amplitudes exercise overflowing subtraction and tolerance
arithmetic. A normalized comparison prevents `infinity > infinity` from
turning an actual error into a pass. Nonfinite samples are always mismatches.

The event-only admission regression failed on the former audio-terminal
requirement. Existing raw MIDI, controller, note, MIDI-file and generated
controller partition fixtures also run through the common runner with offline
continuation. The hand-authored MIDI-file expectations cover formats 0/1/2,
tempo/running status, channel filtering and SMPTE timing in both paths. These
retain the parser's documented channel-message profile; they do not establish
SysEx playback. A linear ramp supplies an
analytical controller oracle. Mutations of equal-frame order, final payload,
time, count, controller name/value and explicit silence must fail. Invalid
times, unsorted events and nonfinite controller references are rejected.

Reset/replay fixtures include eight distinct oscillator phases, historical
Filter and ADSR sample baselines, the independent MIDI gate schedule, terminal
MIDI-file expectations, timed terminal control changes and the analytical
controller ramp. Non-period-aligned horizons and initial terminal emissions
expose stale oscillator, delay, envelope and change-suppression state.
Three compiled mutation canaries omit the offline reset, realtime reset or
both. All fail; omitting both still fails the independent controller count
oracle even when both paths agree. The reset calls are restored before final
Debug/Release verification.

Failure/retry fixtures exercise queued variable-controlled output, raw MIDI
change suppression and MIDI-file cursor/equal-time ordering. Reduced event
capacity fails after partial evaluation; a missing controller-name buffer
also exercises publication admission. Repeated failures precede the successful
retry, with an initial 14 positive probes across those fixtures' first/reset passes. Invalid
indices/order, empty callbacks, oversized spans, omitted capacities and probes
that actually succeed are rejected. Mutation canaries insert a reset before
retry or corrupt an unused backing byte; both must fail the suite.


Sound state fixtures observe both execution paths before inputs, at shared
boundaries with differently partitioned offline calls, and immediately after
reset. Negative oracles change selection, identity, count or reset expectation;
malformed boundaries and unused reset expectations are rejected. An explicitly
empty oracle checks a valid graph without Sound cells. The ignored-option
regression failed before implementation.

A late combined-input failure fixture selects `toneB` at frame 2, then makes
its amplitude division invalid at frame 6 inside a 9-frame offline public call
(maximum internal block 4). Before the P2 native checkpoint this exposed
`toneA -> toneB` despite render failure. The regression now requires `toneA ->
toneA`, repeated failure, a committed prefix, zero-frame calls and shorter
retries with independent oscillator phase/crossfade samples. Raw MIDI checks
initial emission, suppression and future queue preservation. MIDI Voice checks
that retry recreates the original generations and gain/gate trace after a late
stale-generation rejection. An allocation-failure sweep exercises all 89
ordinary allocation sites observed in one nonzero-boundary terminal transaction,
including result assembly after successful internal callbacks.

The private offline checkpoint copies independently owned native value state,
queue/projection state and atomic publications; rollback uses nonthrowing swaps
and stores. It preserves live expression identities and immutable borrowed
storage. Capture allocates on the exclusive offline/control thread, not in a
realtime callback. The public cursor commits after output assembly succeeds.

The complex-spectrum participant now deep-copies FFT frame payloads and all
sequence metadata into independent storage. Rollback appends that baseline into
the original prepared sequence, preserving its storage identity and borrowed
views without allocating. Native FFT/IFFT tests use rectangular/hop-64 and
Hann/hop-32/16 configurations with frame size 64. After committing 193 samples,
two failed 75-sample calls encounter an invalid control at frame 251; retrying
58 samples matches an independent 63-sample-delayed sine oracle, and the invalid
control remains queued. A separate Hann/hop-16 test sweeps all 103 observed
ordinary allocation sites while retaining a pending gain change; each retry
matches the independently validated delayed gain/sine trace. This extends the
owned-state guarantee to complex-spectrum sequences, not to all spectral DSP.

Static and dynamic phase-vocoder participants now clone processing state using
the immutable initial interval, sample rate, maximum block size and envelope
policy. Their current target interval, ramps, phase/overlap history and transport
state are copied separately through the existing semantic state-copy contract.
Rollback restores into the live processor without allocating; it does not swap
span-backed spectral owners. Capture failures return empty failed offline output.

Five combined-input fixtures cover static/dynamic Simple Pitch Shifter,
Monotonizer, Frequency Scale and Time Frequency Scale. A control changes at
frame 4096; after a committed 4209-frame prefix, two failed 1000-frame calls
encounter an invalid control at frame 4986. Retrying 777 frames matches a
separately uninterrupted audible reference and leaves the invalid control
queued. A dynamic fixture sweeps all 147 observed ordinary allocation sites
during an active interval transition. DSP tests independently prime a reference,
clone and overwrite the candidate, restore without allocation, destroy the
snapshot and verify exact continuation across further interval changes. They
cover both processor types, initial intervals 0/7 and both envelope policies.
These are state-continuation tests, not evidence of external reference behaviour.

Partial-spectrum sequences and the live spectral analyzer now participate in
whole-call offline rollback. The sequence journal copies timestamps, source
sequence numbers and every partial payload into independent storage, restoring
into the original frame/payload owners. The analyzer clone uses its exact
configuration plus the semantic state-copy contract for STFT history, metadata
ring/cursors, source sequence and transport state. Scratch is regenerated before
use; restoring does not borrow snapshot storage or allocate.

The live-analysis fixtures deliberately use a non-bin-centered 7.3 Hz source
and a later source-gain change: a steady bin-centered tone had concealed the
state error. Both fundamental modes now pass repeated late failure and shorter
retry against separately uninterrupted ordered outputs. Tests additionally
verify empty/populated sequence rollback, exact frame and point metadata,
original storage addresses, and all 144 observed ordinary allocation-failure
sites. DSP clone tests verify all frame/point fields after snapshot destruction,
both fundamental modes, input latencies 0/7, and independent source-time and
frame-start amplitude/frequency-scale expectations. External analysis records
are handled by the subsequent publication increment described below.

Partial-track resynthesis now has an explicit allocating processing clone. A
private copy duplicates all owned prepared segments, tracks, points, interpolation
modes, phases, cursors and transport state; ordinary public copying remains
unavailable. Rollback uses the existing nonthrowing move assignment into the
original processor. This processor exposes no borrowed views into those vectors.

The combined-input fixture crosses track and segment boundaries after a
41-frame committed prefix. Two failed 90-frame calls encounter an invalid
control at frame 121; retries of 19 and 61 frames match a separately uninterrupted
reference, including the pending gain change at frame 57. All 111 observed
ordinary allocation-failure sites recover the same 100-frame continuation,
including an empty terminal segment. DSP tests cover Step/Linear interpolation,
all segments or one selected segment, checkpoints at frames 0/41/99/140,
overwriting state through end-of-file, and exact continuation after destroying
the snapshot. They clear source inputs after preparation and verify allocation-free
restore and processing. A separate 7.3 Hz sine oracle checks phase and output
gain; invalid preparations remain invalid after cloning.

Analysis capture now joins an eligible combined-input offline transaction by
deferring publication. The adapter copies each successful callback's record and
rejection intent on the offline thread before scratch is reused, then publishes
through the existing capture methods after all throwing output work succeeds.
It preserves typed partial points alongside compatibility values. Failure
discards these copies without touching external queues or consumer cursors;
already committed records and stage/binding authority remain valid. Live realtime
publication is unchanged, and both callback rendering and commit allocate no
memory.

Tests preserve queued prefix evidence across repeated late failures at capacities
1/8. Consumer reads between provisional callbacks return no record. Rollback and
retry match separate direct realtime ComplexFrame, PartialSpectrum, ControlBlock
and EventBatch publishers, including exact headers, all partial point fields,
queue accounting and an independently specified gate-edge trace. A 257-edge
batch defers its single rejection until commit. All 131 audio and 170 partial
ordinary allocation-failure/retry sites suppress provisional audio, values,
typed points and counter changes, then recover the exact continuation.

Full rollback is still open for production isolated components. Bindings without
the optional checkpoint capability retain their existing execution admission and
callback-level behavior. Eligible native streams
now join the outer transaction as described below; other participant eligibility
checks still apply.

The stream prerequisite now provides a private exclusive processing checkpoint
for `AudioStreamConsumer`. Capture/restore synchronizes with the owner worker
mutex and preserves consumer/Impl identity; immutable configuration stays outside
swapped state for concurrent health readers. It covers owned response payloads,
cursor/direction, prepared identities, worker fields and 17 atomic queue/epoch/
health values. Restore allocates nothing. Unit fixtures cover fresh/nonzero/EOF
boundaries, fractional forward/reverse reads, prepared restart/relocation,
Missing results, worker progress, unrelated consumers and 38 ordinary allocation
failures. A real background worker and concurrent health reader exercise 30
restore attempts. That prerequisite alone did not accept whole-call streaming.

The native stream integration captures distinct consumers once per whole call,
including Disk Cache prewarm consumers and Sample Map regions. Health and seek
generation are deferred behind a control-thread inspection mutex until commit or
restore. Audio processing retains a live cursor and uses neither checkpoint mutex.
Sample Map opaque context pointers and the session-level inspection registry
retain their original storage while mutable flags and consumer state are restored.
Tests commit a prefix, fail twice at a later input boundary, and retry against
independent decoded PCM plus exact gain timing. Forward/reverse Sample Map loops
and gate retriggers match explicit sample expectations from fresh and nonzero
boundaries. All 200 ordinary whole-call allocation sites preserve prior analysis
and exact retry. Both checkpoint inspection modes recover all 38 capture sites.
Concurrent worker/consumer inspection and 60 session-level render/abort attempts
exercise retained metadata and storage. Realtime callback guards are unchanged;
this guarantee requires every other participant
to be eligible for the outer transaction.

The isolated-component prerequisite adds an optional private prepared-binding
checkpoint contract. A capable binding must capture all its processing state
before the public call begins; a failed or nested capture rejects the call before
processing. The runtime captures each distinct binding once and restores its
local scratch contents and transport flags in place, retaining borrowed pointer
targets. Checkpoint destruction restores without allocation unless committed.
Revocation and quarantine authority stay outside the processing snapshot.

A stateful one-block pipeline model reproduces a failed call advancing pending
audio. Tests cover fresh and committed-prefix boundaries, repeated late failure,
exact retry against an independent Noise/float/gain oracle, prior analysis,
118 ordinary allocation-failure sites, nested capture and retained revocation.
This verifies the private runtime seam only. The real macOS binding still reports
no checkpoint capability; kernel state, authenticated host/helper transport,
component-hosted streams and deferred health remain open.

The optional kernel ABI checkpoint extension is registered separately from the
frozen V1 module/descriptor array. A validated extension identifies its exact
module/version and a subset of kernel types; absent or omitted types never gain
implicit support. The local kernel host copies callback records before
publication and keeps one opaque baseline on the original prepared instance.
Restore, commit or instance destruction consumes it before module unload.
Capture allocation failure preserves retry; malformed/fatal callbacks and failed
restore/destruction quarantine, and prior quarantine is never undone.

Kernel conformance verifies Oscillator, Filter and scalar/typed-gate ADSR from
fresh and nonzero boundaries. Repeated provisional processing and reset restore
exactly against independently prepared direct kernels, with explicit oscillator
phase and ADSR attack expectations. Tests check allocator failure, absence of
ordinary and host-service allocations during restore/commit, commit continuation,
malformed/duplicate/subset metadata, partial payload cleanup and retirement order.
Separately omitting restore for each of the three builtin families makes its
continuation test fail. These checks cover local kernel host/builtin state;
dynamic extension discovery and real activation host/helper rollback remain open.

## Reproduction

```sh
cmake --build build/core-debug --target talas_runtime_conformance_tests -j 4
ctest --test-dir build/core-debug -R '^talas_runtime_conformance_tests$' --output-on-failure
cmake --build build/core-release --target talas_runtime_conformance_tests -j 4
ctest --test-dir build/core-release -R '^talas_runtime_conformance_tests$' --output-on-failure
```

Changes to offline mixed-input admission also build and run
`talas_offline_render_tests`, `talas_nested_sound_runtime_tests` and
`talas_realtime_render_session_tests` in both configurations. Run those suites
serially after compilation; final Event Value/terminal evidence is Debug 4/4
(80.98 s) and Release 4/4 (9.48 s) including conformance.

The subsequent Sound checkpoint extension changes only the common harness,
its tests and documentation. Complete conformance passes Debug 1/1 (3.20 s)
and Release 1/1 (0.59 s). Independent review found no P0–P3 defects in this
delta.


The native whole-call rollback increment additionally verifies complete Wave
Terrain and Sample Map runtime suites. After final builds, serial Debug runs
pass 6/6 (86.45 s), Release 6/6 (11.01 s), and ASan/UBSan conformance/Wave
Terrain/Sample Map runs pass 3/3 (22.01 s). All configurations exercise 89
ordinary allocation-failure/retry points. Independent review closed two borrowed
owner hazards and has no remaining P0–P3 findings; the Wave Terrain use-after-free
was reproduced under ASan before its stable-backing fix.

The partial-track resynthesis increment passes ten relevant groups in
Debug (100.66 s) and Release (12.57 s), run serially after all builds: native
STFT, phase-vocoder DSP, live spectral DSP, partial-track DSP, offline rendering,
Sample Map, common conformance, nested Sound, realtime sessions and Wave Terrain.
ASan/UBSan passes seven groups across two invocations (68.66 s total): four DSP
suites plus common conformance (57.53 s), then Sample Map/Wave Terrain (11.13 s).
The first sanitizer group has no active timing gates and ran during the
independent Release build; the runtime sanitizer group ran after all builds and
other runtime tests. All configurations exercise 111 partial-track, 144
live-analysis, 147 pitch, 103 STFT and 89 terminal ordinary allocation-failure
sites. Independent review found no P0–P3 defects in the partial-track increment.

The analysis-publication increment passes eleven relevant groups in
Debug (108.07 s) and Release (13.11 s), serially after all builds: the preceding
ten groups plus realtime analysis capture. ASan/UBSan passes eight groups across
two invocations (86.01 s total): the four DSP suites, common conformance and
analysis capture 6/6 (74.99 s), then Sample Map/Wave Terrain 2/2 (11.02 s).
Only the first instrumented group ran during the independent Release build;
it has no active timing gates. Every configuration reports 131 audio and 170
partial publication allocation-failure/retry sites, alongside the existing
111 partial-track, 144 live-analysis, 147 pitch, 103 STFT and 89 terminal sites.
Independent read-only review found no P0–P3 defects in this increment.

The stream checkpoint prerequisite passes six relevant groups in Debug (97.99 s)
and Release (11.85 s): Audio Stream, common conformance, Sample Map, component
activation audio stream, offline rendering and realtime sessions. Both ran
serially after builds. ASan/UBSan passes four groups across two invocations
(60.55 s total): common conformance 1/1 (50.03 s) during the independent Release
build, then Audio Stream/Sample Map/component activation stream 3/3 (10.52 s)
after all builds and other runtime tests. TSan Audio Stream passes 1/1 (5.52 s)
after those tests. All four configurations exercise 38 ordinary checkpoint
allocation-failure/retry sites. Independent review found no P0–P3 defects.
Those prerequisite checks accepted only the private processing primitive.

The subsequent whole-call stream integration passes the same six groups serially
in Debug 6/6 (106.60 s) and Release 6/6 (13.78 s), after all builds. ASan/UBSan
passes common conformance 1/1 (45.25 s invocation) during an independent TSan build,
then Audio Stream/Sample Map/component activation stream 3/3 (24.86 s) after all
builds and native tests: four groups across two invocations, 70.11 s total. TSan
passes Audio Stream and Sample Map 2/2 (56.19 s), run last. All four configurations
exercise 200 whole-call allocation-failure/retry sites and 38 checkpoint sites in
each inspection mode. Independent review identified a swapped inspection registry
and an unquantized test oracle; both were corrected, re-review found no remaining
defects, and all final checks ran against those fixes. This accepts eligible
native stream rollback, not isolated components or full P2.

The private isolated participant prerequisite passes eight groups serially in
Debug (113.14 s) and Release (15.32 s): those six native stream groups plus
transient component graphs and actual macOS/XPC transient graph integration.
ASan/UBSan passes common conformance 1/1 (50.11 s) during the independent Release
test build, then stream/Sample Map/component stream/transient graph 4/4 (40.68 s)
after all builds and native tests: five groups, 90.79 s across two invocations.
TSan passes stream/Sample Map/transient graph 3/3 (98.64 s), run last. All four
configurations exercise 118 isolated participant ordinary allocation-failure
sites alongside the existing 200 stream and 38-per-mode checkpoint sites.
Independent review has no findings. This accepts the private runtime seam;
the real XPC test verifies retained legacy behavior, with production checkpoint
capability still absent and full P2 open.

The local kernel checkpoint extension passes seven groups serially after builds
in Debug (10.23 s) and Release (4.06 s): ABI C header, C++ layout, host lifecycle,
builtin conformance, subprocess probe, transient component graphs and actual
macOS/XPC transient integration. ASan/UBSan passes the four ABI groups (7.18 s).
Separate missing-restore mutations for Oscillator, Filter and ADSR each fail
the continuation oracle. Independent review has no findings. These results
accept the local kernel host and builtin checkpoint contract; actual loaded
extension discovery, helper transport and resource participants remain open.

The loaded kernel checkpoint continuation passes nine groups serially after all
builds in Debug (15.76 s) and Release (12.93 s): ABI C header, C++ layout, host,
builtin conformance, subprocess probe, checkpoint loader, immutable resource
loader, audio/resource-set loader and actual macOS/XPC transient integration.
ASan/UBSan passes host/conformance and the three loader groups 5/5 (19.44 s).
TSan passes the three loader groups 3/3 (16.95 s), run last. The original loaded
fixtures exercise 256 repeated restores, independent recurrence continuation,
capture allocation/retry, malformed/absent discovery, fatal state, bounded
allocator violations and checkpoint/instance/image retirement. Final publication
allocation failure reproduced an instance/image leak before its lifecycle fix.
Missing-restore and wrong-lifetime-budget mutations each fail. Independent review
has no findings. Untrusted fixture bundles retain the existing uninstrumented
loading policy; sanitizer coverage applies to the host loader/tests. These checks
accept loaded kernel snapshots only, leaving resource snapshots, authenticated
helper transport, pending audio and deferred health open.

The private component-hosted resource stream checkpoint passes eight groups
serially after all builds in Debug (15.26 s) and Release (13.06 s): stream,
checkpoint/resource/audio loaders, actual XPC stream and stream-health transient
graphs, stereo 44.1 kHz, and a two-audio-resource transient graph. ASan/UBSan
passes the four stream/loader groups 4/4 (13.97 s); TSan passes those same four
4/4 (19.07 s), run last. Every configuration covers 160 deterministic restores,
60 restores with a background decoder and concurrent health reader, and the
single ordinary capture-allocation site in both health modes. Independent PCM
expectations, wrapped response slots, cache eviction/reuse, pending/stale/full
queues, underrun episodes, EOF and terminal lifecycle distinguish restored
processing history. Missing-cache and missing-response mutations fail after the
latter exposed and closed an insufficient queue-wrap test. Independent review
has no findings. This accepts the private stream participant; coordinated loaded
kernel/resource checkpoints, aggregate publication and helper transport remain
open. Native XPC tests retain legacy behavior rather than proving checkpoint
control messages that do not yet exist.

The coordinated loaded kernel/resource checkpoint passes the same eight native
groups serially after all builds in Debug (17.64 s) and Release (15.44 s), plus
the four stream/loader groups under ASan/UBSan 4/4 (17.33 s) and TSan 4/4
(24.15 s), run last. Original one-stream and Table/Binary/two-stream fixtures add
typed recursive DSP state, exercising 80 repeated fresh/nonzero restores against
independent source/kernel expectations. Every ordinary resource capture site
(one or two per instance) and kernel host-allocation failure permits exact retry.
Tests also cover aggregate health under concurrent inspection, terminal failure
in one slot or kernel restoration, allocation-free restore/commit, empty resources,
mixed ownership, absent legacy capability and shutdown. Missing-resource-restore
and missing-kernel-history mutations both fail. Independent review has no findings.
This accepts coordinated loaded state only; authenticated helper commands,
matching request/response quiescence and pending audio remain open. Native XPC
checks preserve legacy behavior and do not establish the absent checkpoint
control protocol or real prepared-binding capability.

The private host pending-audio checkpoint passes nine native groups in Debug
(18.52 s) and Release (15.87 s), plus five plane/stream/loader groups
under ASan/UBSan (18.48 s) and TSan (26.93 s). All 28 checks ran serially
after every build, TSan last. The independent closed-form oracle exercises 384
rollbacks across 16 mono/stereo, 256/512-frame, 44.1/48 kHz and fresh/nonzero
combinations. It verifies delayed audio, retained initial silence, unchanged
shared slots, monotonic physical identities, zero allocations and missing-response
retry. Edge checks cover commit with an unfinished response, corrupt binding,
transport/sequence/publication, nonfinite output, revocation, quarantine, move
ownership and terminal canary use. Missing logical silence and wrong saved-output
mutations both fail; originals were restored before final verification. Independent
read-only review has no findings. Real worker state is modeled separately in
these tests; matching worker parking, authenticated helper commands and the real
prepared-binding opt-in remain unfinished. Supported profile counts are unchanged.

The private actual-worker checkpoint adds the basic macOS/XPC transient graph
to the preceding nine native groups: Debug 10/10 (21.46 s), Release 10/10
(17.82 s), ASan/UBSan 5/5 (20.09 s) and TSan 5/5 (29.78 s).
All 30 groups ran serially after every build, TSan last. Actual background workers
execute 64 one/two-resource rollbacks at fresh/nonzero histories against independent
recursive source expectations. Saved host output and the first new DSP response
both match after provisional transport changes and response-slot wrap. Shared
health consumers accept monotonic publications carrying captured resource health.
Boundary checks reject an acquired/queued/publishing request and stale/future
completion; tests stop before submission to prove queued-but-unacquired rejection
and exact retry. Allocation failures, commit, revoked plane, one failed resource,
fatal kernel restoration, absent legacy extension and held destruction are covered.
Missing transport restoration and missing queued-request detection mutations both
fail. Independent review has no findings. These tests accept the private actual
worker participant; authenticated helper commands, parent health deferral and real
prepared-binding opt-in remain unfinished. No Supported profile is promoted.

The authenticated helper/host checkpoint adds real XPC control acceptance.

The private macOS host and sandboxed helper now exchange authenticated
Query/Begin/Restore/Commit requests. Separate fixed-size envelopes preserve
existing V1 layouts and bind each reply to the exact operation, next control
sequence, completed physical audio sequence, held checkpoint ID and runtime
authorization. The helper checks identity before touching the same prepared
worker; no Reset or reconstructed instance substitutes for rollback.

The parent drains an in-flight heartbeat, captures logical pending output and
waits for exact audio completion on its control queue under the existing deadline.
A successful remote acknowledgment finishes local state; physical audio and
control identities keep advancing. Ordinary capture failure unwinds and permits
retry. Invalid identity, crash, timeout or terminal restore failure revokes
runtime authority before publishing Fatal. A generation guard retains one
heartbeat timer chain across repeated checkpoint operations.

Real XPC tests exercise 24 repeated one/two-resource rollbacks with independent
recursive PCM expectations at fresh and nonzero histories, provisional transport
changes, pending responses and wrapped slots. Other cases cover Query while held,
commit, absent legacy capability, failed capture and exact retry, held teardown,
replayed control sequence, wrong checkpoint ID, fatal restore and callback
hang/crash during capture and restore. Bounded envelope tests reject malformed
identities, field/type/size/reserved data, invalid operation/status combinations
and valid replies for a different request.

The terminal tests first exposed premature Fatal publication while runtime
authority was still active; corrected publication ordering passes those cases.
Replacing worker restore with commit fails independent audio continuation.
Removing the helper held-ID guard fails the adversarial identity test, whose
parent expected request is also adjusted to prove the helper guard independently.
Both mutations were restored before final verification.

The final serial verification passes all 60 groups: Debug 23/23 (36.16 s),
Release 23/23 (24.34 s), ASan/UBSan 7/7 (21.81 s), and TSan 7/7
(33.07 s). All builds completed first with at most two compilers; TSan ran last.
Native groups include all eleven new actual XPC variants, protocol/envelopes and
the retained plane/stream/loaders and basic/stream/health/stereo/two-resource
integrations. Sanitizers cover protocol/envelopes and the five plane/stream/loader
groups. Independent read-only review found no actionable P1/P2 defects.

One initial Release run failed the retained health transient-graph test because
runtime evidence was still active after its two-second revocation wait. The
unchanged test then passed five serial repetitions and the complete Release
rerun. The cause is not established; neither the deadline nor production behavior
was weakened. Review found no checkpoint/heartbeat guard that strands this path.
This intermittent revocation timing failure remains a follow-up, not a claimed
fix.

This accepts private authenticated host/helper checkpoint control. Complete
parent public-health deferral and the real prepared-binding whole-call opt-in
remain unfinished. Supported profiles stay **55.0% realtime / 50.3% offline**;
full P2 remains open.

The Ready heartbeat/activation drain closes the reproduced control-sequence race.

Activation now waits for any in-flight sealed Ready heartbeat before sending
component bytes. The host owns the prepared load message and materialized image
while waiting, preserves the heartbeat's exact reply token and sequence, and
revalidates live authorization and exact peer identity before dispatching the
load. The existing one-second heartbeat and two-second load deadlines remain
separate bounded phases. Another activation cannot replace queued ownership.
Timeout, shutdown and revocation clean the retained message, materialization and
peer; a late reply cannot revive them.

A deterministic real-XPC hook sends a Ready heartbeat in the same control-queue
turn that accepts activation. The original implementation rejected the subsequent
Prepared transition because it had invalidated the heartbeat reply without
advancing the shared control sequence. The RED test fails at that exact boundary;
the fix preserves complete single/resource-set recursive checkpoint continuation.
Three separate signed test apps hold a real heartbeat reply and verify timeout,
shutdown while waiting and owner revocation before release. All reject duplicate
activation and verify that no component message was sent, runtime evidence is
revoked, the exact peer exits and materialization is removed. Initial repeated
same-app lifecycle setup failed at prewarm before reaching the boundary; separate
service identities follow the existing integration-test structure.

Removing the live-authorization recheck makes the revoked-owner no-send test fail.
The live-authorization check was restored before final verification. Independent
read-only review found no P1/P2 defects, including the final test-isolation change.

Final verification passes all 66 serial groups: Debug 26/26 (39.44 s),
Release 26/26 (34.42 s), ASan/UBSan 7/7 (22.01 s), and TSan 7/7
(33.41 s). All builds finished before runtime checks, with at most two heavy
compilers; TSan ran last. Native checks include the three new queued-activation
apps, the complete prior checkpoint/legacy XPC suite and plane/stream/loaders.
Sanitizer tests retain the protocol/envelope and plane/stream/loader groups;
the macOS host library also compiles with both sanitizer configurations. These
sanitizer test groups do not claim instrumented host/helper XPC execution.
The retained health-evidence test passes in both native final runs.

This fixes the proven Ready-heartbeat activation race. It does not establish the
cause or repair of the earlier intermittent health-evidence revocation timeout;
that remains explicitly tracked. Parent public-health deferral and the real
prepared-binding whole-call capability are still unfinished. Supported profiles
remain **55.0% realtime / 50.3% offline**; no full P2 claim is made.

Checkpoint health publication and idle helper-failure revocation are accepted.

Successful worker begin, restore and commit now publish one fresh resource-health
record per stream before control returns and the helper acknowledges completion.
The factored publisher retains the existing process-path validation and quarantine
policy. A failed publication consumes checkpoint ownership and prevents restart;
partial publication of a two-resource set cannot preserve audio authority.
Physical publication identities keep advancing and no health consumer is rewound.

The RED actual-worker test exposed stale shared health without another audio
request. Acceptance now checks exact resource health and exactly one new physical
record after control, preserves the independent recursive PCM oracle across 64
worker rollbacks, and retains allocation-free restore/commit. Twelve additional
cases cover one/two resources, all three checkpoint operations, and an odd or
exhausted publication counter. The two-resource failure targets the second slot
to exercise partial publication and prove held-state cleanup.

Actual XPC single/set/retry tests run 24 rollbacks with a provisional block beyond
EOF. Captured and restored processing health hides that block. Commit immediately
exposes exactly 256 EOF frames, one discontinuity and one seek generation per
stream, without a subsequent audio request; independently calculated PCM includes
EOF silence. Removing finish publication fails this real-XPC commit oracle. The
publisher was restored before final verification. Independent read-only review
found no P1/P2 defects.

The retained intermittent evidence-revocation failure was also reproduced and
resolved. Temporary atomic diagnostics showed healthy heartbeat progress while
the shared audio plane was quarantined and the parent callback latch remained
false. A helper could fail between callbacks, so no later process call set the
latch that the heartbeat required. The heartbeat now checks live plane quarantine
as well as that latch, using the existing terminal/revocation path. A deterministic
RED test stops further audio callbacks before waiting for evidence revocation;
the fix passes 20 serial repetitions (10.23 s), then checks silence when callbacks
resume. Deadlines are unchanged and temporary trace instrumentation is removed.
Independent changed-area review found no P1/P2 defects.

Final serial verification passes all 66 groups: Debug 26/26 (42.23 s),
Release 26/26 (36.09 s), ASan/UBSan 7/7 (25.44 s), and TSan 7/7
(38.51 s). All builds finished first with at most two heavy compilers; TSan ran
last. Native checks include the complete actual XPC checkpoint/activation suite
and retained plane/stream/loaders. Sanitizers exercise protocol/envelopes and
plane/stream/loaders, including the actual worker checkpoint and publication
failure cases; host/helper XPC execution is verified natively. `git diff --check`
is clean.

This accepts the worker/shared-plane health boundary observed through actual XPC.
The parent production inspection path is unchanged; its full publication deferral
and real prepared-binding whole-call opt-in remain open. The diagnosed
health-evidence revocation race is fixed above. Supported profiles remain
**55.0% realtime / 50.3% offline**; full P2 remains unproven.

## Parent public-health deferral — accepted 2026-09-05

Parent health inspection now retains one complete public snapshot while an
authenticated checkpoint command is pending and throughout held provisional
processing. A short mutex serializes local consumers and caches. A valid helper
acknowledgment publishes the complete captured/restored/committed health boundary;
inspection still validates fresh shared records and live quarantine before
returning any retained snapshot. The mutex never spans an XPC wait or authority
revocation, and audio callbacks do not acquire it.

For the worker's resource-set profile, every slot is published once per processing
or checkpoint round. The parent compares local accepted consumer sequences and
retains its previous complete set when a read spans different rounds. Busy or
invalid observations do not advance accepted identities. This adds local consumer
metadata only; frozen shared records, ABI layouts and wire commands are unchanged.

The initial RED test reproduced missing parent health during pending control.
Actual XPC tests hold real replies for single/set/legacy/retry cases, including
commit after provisional EOF has already reached shared memory. A concurrent
inspector checks complete healthy snapshots during audio and control. A separate
deterministic test pauses after the first resource slot, completes another real
audio block, and verifies that old/new slots cannot be combined. Terminal failure
bypasses cached health. Mutations removing either publication deferral or the
complete-set guard fail their intended assertions; original code was restored.
Independent read-only review found no important correctness issues.

| Configuration | Result | Elapsed |
| --- | --- | ---: |
| Debug native core and actual macOS/XPC integration | 26/26 | 41.61 s |
| Release native core and actual macOS/XPC integration | 26/26 | 36.30 s |
| ASan/UBSan core plane, stream, loaders, protocol and envelope | 7/7 | 25.29 s |
| TSan same core groups | 7/7 | 39.00 s |

All 66 groups ran serially after all four builds, with at most two compiler
processes; TSan ran last. The host library also compiled under both sanitizer
configurations, but actual XPC host concurrency was exercised natively, not under
TSan. `git diff --check` is clean.

This supersedes earlier notes that parent public-health deferral is unfinished.
The real prepared binding still does not advertise whole-call checkpoints.
Its synchronous participant lifetime, failed offline calls and exact retry/commit
integration remain the next work. Supported profiles remain **55.0% realtime /
50.3% offline**; full P2 remains unproven.

## Checked isolated participant commit — accepted 2026-09-05

The combined-input offline adapter now rejects terminal isolated participant
commit failure instead of returning assembled audio and advancing its public
cursor. Private C++ participant and transaction commit return an explicit boolean
without throwing. The transaction retains each distinct binding, completes
participant control before acquiring final publication stage leases, and commits
stream/analysis publication only after every participant and lease succeeds.

A failed commit or lost publication authority quarantines every captured binding.
Already committed peers cannot be restored as a reusable distributed transaction;
remaining native/stream undo stays armed, provisional output and analysis are
discarded, and repeated terminal commit remains false. Publication leases are
released before quarantine and never cover a helper-control wait. Public cursor
advancement follows successful transaction finalization.

The RED fixture reproduced successful public audio after commit-time revocation.
Regressions cover explicit false with initially live authority, successful commit
followed by revocation, and denied final stage admission; both unread and consumed
prefix analysis remain correct without provisional records or counter changes.
Further checks prove no successful retry after terminal partial commit, and zero
allocations for success/failure/repeated commit and destruction. The existing late
evaluation/allocation-failure rollback and exact retry sweep still pass.
Independent read-only review has no actionable findings.

| Configuration | Result | Elapsed |
| --- | --- | ---: |
| Debug core and retained actual macOS/XPC integration | 10/10 | 81.72 s |
| Release same groups | 10/10 | 13.62 s |
| ASan/UBSan conformance, transient graph and realtime session | 3/3 | 257.57 s |
| TSan same core groups | 3/3 | 662.84 s |

All 26 groups ran serially after every build, with at most two compiler processes;
TSan ran last. Actual XPC execution was native, not instrumented sanitizer XPC.
`git diff --check` is clean.

This accepts checked transaction finalization, using the private stateful binding
fixture for failure injection. The later real-integration section records
production discovery, checkpoint ownership and actual public-call retry evidence,
including the final admission allocation fix found beyond this initial core
fixture. Frozen kernel C ABI and wire records are unchanged. Supported profiles
remain **55.0% realtime / 50.3% offline**; full P2 is still unproven.

## Remaining P1 work

Resource generations, declared latency, broader state traces, seek and general
failure/retry injection remain open. Offline failure evidence currently covers
native whole-call failure/retry scenarios above. Specialized resource and real
isolated-participant rollback tests are now accepted separately; their broader
integration into the common P1 harness remains open. Terminal failure/retry currently covers caller capacity
and publication atomicity, not every runtime failure mechanism. Reset/replay
covers the same stimulus and expected audio/events after a real reset, with
optional distinct Sound-state expectations. Other retained-state scenarios
remain open. Existing
specialized tests still cover many of those behaviors separately. Public
transactions that sample controls only at their entry retain their contract;
an explicit offline partition avoids claiming universal partition invariance.

Future extensions must preserve the independent expected result, distinguish
observed reference captures from analytical/native expectations, and keep queue,
event order, resource and failure behavior explicit. Full P1 acceptance is
not established by these initial extensions.


## Accepted real prepared-binding integration

The real prepared binding now opts into whole-call checkpoints on the existing
combined-input offline path after an authenticated helper capability query.
Storage for the RAII checkpoint is allocated before Begin. Restore/commit run
outside the control queue and publication/inspection gates, using the existing
control watchdogs. Only processing inside an exclusive offline checkpoint waits
for exact helper completion through the production audio-boundary validator.
Ordinary realtime processing preserves its nonwaiting deadline behavior.

Nine actual XPC apps cover single Audio and Table/Binary/two-Audio participants,
ordinary capture failure and exact retry, unsupported legacy capability,
terminal restore/commit loss, capture/restore timeouts, and ordinary realtime
deadline quarantine. After a committed prefix, single/set cases inject caller
allocation failure after real provisional blocks, fail twice after provisional
EOF, then compare 8192 retry samples with independent source PCM, recursive kernel
history, float pipeline and scheduled gain. Prefix analysis remains unchanged on
failure; successful commit exposes the final pending EOF health. Concurrent
public inspection checks live, coherent resource health through those calls.

Two additional defects were reproduced and fixed before acceptance. Preparation
transfer now rejects pending or held raw-host checkpoints without moving their
owner; the original host can restore and retry transfer. Independent review
found allocation in final publication admission after the helper had committed.
The private stage lease now stores authority and four shared locks inline in
their existing order, with moves retaining authority until the old locks release.
A real public-render allocation trap starts immediately after helper commit and
remains armed through transaction cleanup; the entire finish allocates zero
caller C++ objects. Direct restore/commit cleanup and authorization admission/move
tests also prove that boundary. This does not claim C/libdispatch/XPC bookkeeping
is allocation-free or relabel focused XPC faults as a complete allocation-site sweep.

Evidence includes initial missing-capability RED, a Restore-to-Commit mutation
that fails provisional-health rollback, pending/held transfer RED/GREEN (2/2,
2.46 s), and stage-allocation RED (both authorization and actual public render)
followed by focused GREEN (3/3, 17.62 s).
Independent final review found the stage-allocation issue above; targeted
re-review confirms the fix with no new findings.

Fresh final verification after the fixes: **Debug 37/37 (77.61 s),
Release 37/37 (54.69 s), ASan/UBSan 9/9 (78.25 s), TSan 9/9 (154.82 s)** — 92
passing groups in total. Native groups include
all nine whole-call apps, the prior 26 stream/control/health/teardown groups,
the core transient graph and authorization. Sanitizer groups cover the relevant
nine core/protocol/authorization tests; the macOS host is also built with both
sanitizers, while actual XPC execution is verified natively. Builds finish before
serial runtime tests; TSan runs last. The earlier accepted 26-group core commit
verification remains separate; its large unchanged realtime sanitizer suite was
not rerun for this host/authorization increment.

This supersedes historical statements that the real binding has no checkpoint
capability. Kernel C ABI and authenticated wire layouts remain unchanged. Legacy
bindings without the optional extension retain their existing admission.
Terminal participant loss rejects output and analysis and cannot become a reusable
retry. This accepts the described real integration, not universal offline graph
admission or full P2. Supported profiles remain **55.0% realtime / 50.3% offline**.

## Offline checkpoint exclusion audit and native compile guard

The audit distinguishes absent external checkpoint support from the 20 native
owned fields formerly guarded by `OfflineStateJournal::canCapture`. A compiler
probe instantiating a strategy assertion for every guarded native field passes:
13 fields/12 types use copy-and-swap, six fields/three types use semantic processing
clones, and one partial-track field uses its dedicated snapshot. Thus none of
those 20 predicates excluded a currently prepared graph.

The native guard is now enforced in `captureOwned` with a compile-time assertion.
The redundant runtime predicates are removed. A future noncopyable owner without
a checkpoint strategy can neither silently skip its own undo nor switch off the
whole transaction. The same injected opaque participant compiled successfully
before the fix and is rejected by the exact strategy diagnostic afterward.
Current capture/restore dispatch and existing execution admission are unchanged.

Fresh verification: the production translation unit passes syntax/type checks
with the configured Debug, Release, ASan/UBSan and TSan flags; the negative compiler
probe is rejected as intended. A rebuilt Debug runtime passes conformance and
isolated transient-graph tests **2/2 (22.87 s)**. This is not a fresh four-configuration runtime
or sanitizer execution claim; the preceding real-binding 92-group acceptance
remains separate.

A separate public-call reproduction confirms the remaining legacy exclusion.
Using `makeOfflineIsolatedFixture(false)` from the existing transient-graph
fixture, schedule its ordinary Gain/Divisor events, commit one block and request
four more. The call fails with no samples, but the binding stays active and its
processing count advances from **1 to 4**. The rollback expectation fails with
exit 1. The executable uses the current real core adapter and the explicit private
pipeline test binding; this is not an additional actual-XPC execution claim.

After the native guard change, the no-checkpoint return paths are unprepared
session state and missing/unsupported isolated participants. The public adapter
still preserves legacy successful admission by running without an outer journal
when capability is absent. Exact rollback for those calls is therefore unproven
and contradicted by the reproduction above. A fix must account for opaque kernel
state, pipeline, resources and publication together; copying native state, resetting
or replaying a component, or inferring support from absence of the STATEFUL flag
cannot substitute for that contract. Do not silently reject previously admitted
graphs or relabel this boundary as complete. Resolve the remaining legacy contract
alongside wider P2 family-program and shared metadata work.

Supported profiles remain **94/171 realtime (55.0%) / 86/171 offline (50.3%)**.
No full P2 is accepted by this audit.

## Prepared unary control family

The first P2 arithmetic family now routes Absolute Value and ArcTan through
`PreparedUnaryControlProgram` in both offline and prepared realtime execution.
The private program carries immutable kind/capacity configuration and canonical
Control value input/output metadata. Existing descriptor, scope, buffer and
compiler-owned graph authentication remains at the adapters; the program is
not a substitute execution authority.

Both operations still use the original checked arithmetic and its complete-block
validation before output commit. The family has no mutable DSP history or
additional reset/undo work. Exact in-place processing remains valid; partial
overlap and incompatible size/capacity reject before output publication. Offline
diagnostics retain their existing wording. Prepared operation configuration is
copied by the existing whole-call journal; copying the family configuration
introduces no borrowed pointers or additional allocations.

Focused tests cover finite extremes, independent known angles, signed zeros,
late NaN/infinities, unchanged sentinel outputs, exact retry, zero frames, capacity,
aliasing and zero caller C++ allocations. Actual compiler-produced oscillator/VCA
graphs exercise both execution paths against independent sine/gain expectations,
including irregular 1/3/2/2-frame realtime continuation. Reconstructing a graph
from public plan/settings still rejects without compiler authority. Existing
C119/C120 behavior, recovery and mutation tests run in the retained native
offline/realtime suites. Independent final read-only review has no findings.

Fresh final verification: **Debug 4/4 (97.11 s), Release 4/4 (10.75 s),
ASan/UBSan 2/2 (47.97 s), TSan 2/2 (131.37 s)** — 12 passing groups. Native groups are the new family test,
OfflineRenderTests, RealtimeRenderSessionTests and RuntimeConformanceTests.
Sanitizer groups are the new family test and common conformance suite.
Runtime tests run serially after all builds, with TSan last and at most two heavy
compilers.
The paired Release preparation probe uses the same benchmark object linked to
the retained before/after runtime libraries, with five batches of 1000 session
constructions per operation. Median times are Absolute Value **55.60 → 55.54 µs**
and ArcTan **55.36 → 55.17 µs**. This small local measurement shows no preparation
regression; the difference is not a claimed speedup. Incremental selected-target build times were Debug
22.70 s, Release 438.44 s, ASan/UBSan 35.81 s and TSan 17.12 s. Release includes
rebuilding the existing large OfflineRenderTests translation unit; those build
times are recorded evidence, not a paired compiler-performance comparison.

This accepts the first arithmetic-family extraction only. Stateful-family
migration, shared producer/terminal metadata, legacy isolated components without
checkpoint support and wider P1/P2 remain open.
No generated capability metadata changes: **94/171 realtime (55.0%) and
86/171 offline (50.3%)**.

## Prepared Running Min/Max family

The first P2 stateful family now routes Running Min and Running Max through
`PreparedRunningExtremumProgram` in ordinary offline and prepared realtime
execution. Immutable configuration carries kind, capacity and member-specific
Control port enumeration. One internal value record owns initialized/extremum
history in offline continuation, realtime operations and callback checkpoints.
Explicit reset/checkpoint/restore use that record; the existing whole-call value
snapshot also captures it. No program-owned mutable pointers or buffer lifetimes
cross calls, and compiler-owned descriptor/plan/scope/buffer authority stays at
the existing adapters.

The program validates a whole block against a stack candidate before committing
output and state using the original checked helpers. Running Max retains its
positive-zero reset seed; Running Min retains the selected maximumAmplitude seed,
then both include the current sample. Nonzero reset truth, signed-zero ties,
connected control precedence, selected finite checks and offline diagnostics are
preserved. Exact in-place input/control spans work; partial overlap and invalid
size/capacity reject before mutation. Zero frames are inert. The include-tree
detail header supplies private offline storage and does not define a kernel ABI
or grant execution admission.

Focused tests cover independent cumulative/reset expectations, NaN/infinities on
each selected port, inactive-reset amplitude validation, unchanged sentinel
output/state after failure, checkpoints, partitioned continuation, signed zero,
aliasing and zero counted C++ allocations during processing. Compiler-produced
literal graphs exercise ordinary offline and irregular realtime continuation,
both resets and forged public-plan reconstruction rejection against analytical
sine/extremum expectations. Combined-input offline graphs commit a prefix, fail
twice after later extrema have provisionally changed, then retry shorter calls
against an independent oracle. This detects retained later extrema as well as
incorrect event/cursor rollback. Retained native C151/C86 coverage exercises
authority, control schedules, lifecycle and later callback-failure recovery.
Independent final read-only review has no findings.

Fresh final verification: **Debug 4/4 (96.23 s), Release 4/4
(10.74 s), ASan/UBSan 2/2 (47.68 s), TSan 2/2 (131.33 s)** —
12 passing groups. Native groups are the family test, OfflineRenderTests,
RealtimeRenderSessionTests and RuntimeConformanceTests; sanitizer groups are the
family test and common conformance suite. All runtime tests ran serially after
all builds/compile probes, TSan last. Whitespace checks pass. This increment did not rerun the full suite, actual-XPC cases,
Workbench or tests-off install.

The paired Release preparation probe reuses one benchmark object linked against
retained before/after libraries, five batches of 1000 session constructions per
member. Medians: Running Min **59.43 → 58.26 µs**, Running Max **58.48 → 58.72 µs**.
These small local samples do not establish a speedup or a statistically significant
regression.

Two reversed-order before/after Release object-compilation pairs use identical
configured flags and a retained baseline Runtime.h for the old sources. Median
offline source compilation is **28.52 → 27.21 s**; realtime **42.41 → 41.73 s**.
The probe shared the at-most-two-compiler limit with the remaining native build;
these are local measurements under variable background build load, not isolated
compiler-performance or speedup claims. Final selected-target
incremental builds took Debug 36.07 s, Release 818.70 s, ASan/UBSan 96.08 s and
TSan 58.69 s. The Release rebuild includes both large existing offline/realtime
test translation units after the private offline storage header changed.

This accepts the first P2 stateful-family extraction alongside the previously
accepted unary arithmetic family. Shared producer/terminal metadata, common
scope/lifetime/resource/clock contracts, legacy isolated participants without
checkpoints and broader P1/P2 acceptance remain open. No capability
promotion: **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## Compiled output bindings and secondary-output consumers — accepted 2026-09-05

CompileStep now records each original registry output's ordered port identity
and buffer index as the compiler creates BufferSpecs. Existing plan equality and
compiler-owned snapshots include these records. Synthetic latency-compensation
buffers copy the source description but remain separate producers; they never
enter the original step's output list. Existing terminal channel routing and
TerminalAudioLayout remain intact, with no fictitious generated terminal buffers.

A common private validator compares expected port/type declarations with the
actual output slots already bound by an admitted adapter. It checks ordering,
count, owner/scope, capacity, uniqueness and exclusion of compensation outputs.
Metadata is not execution authority. Missing records retain legacy adapter
validation; removing or modifying records in compiler-sealed graphs rejects
through the existing authority checks. The ordinary offline adapter validates
without allocating additional metadata storage.

The unary and Running Min/Max families and both ScaleAndOffset lanes now bind
owned immutable prepared output indices. These value-owned indices survive
checkpoint copies without borrowed plan pointers. The common producer proof
requires exactly one admitted operation. Other operation layouts retain one
compatibility enumeration pending migration; the separate ScaleAndOffset-right
condition has been removed.

Verification uncovered four local producer-recognition replicas that bypassed
the new records. Actual paired graph probes showed Channel Crosser, High Shelving
Filter, Two Formant Element and Analog Sequencer accepting an affine left control
in both backends but rejecting the identical right control only in realtime.
All four replicas now delegate to the common exactly-one producer proof.
The same probe object linked against before/after runtimes changes from four
right-side rejections to all eight left/right cases rendering successfully.

Permanent tests cover the four consumer groups, audible left/right PCM agreement,
an independent Channel Crosser carrier/gain oracle, all four extracted family
successors consuming affine lanes, irregular continuation, real custom Subset
admission, malformed/omitted sealed records and a compiler-produced compensation
buffer whose descriptor equals its source while its output ownership differs.
The initial read-only review was clean; the later consumer finding was repaired
and the changed-boundary re-review also has no findings.

Fresh final verification: **Debug 10/10 (112.73 s), Release 10/10
(14.78 s), ASan/UBSan 4/4 (90.95 s), TSan 4/4 (224.62 s)** —
28 passing groups. Native groups cover the new output-binding suite, compile
plans, latency compensation, nested sounds, ScaleAndOffset authority, both
extracted families, ordinary offline rendering, realtime sessions and common
conformance. Sanitizer groups cover output bindings, compile plans, latency
compensation and common conformance. All runtime tests and preparation benchmarks
ran serially after all builds/compile probes; TSan was last.
Whitespace checks pass. This increment did not
rerun the full suite, actual-XPC cases, Workbench or the tests-off install.

The same fixed preparation benchmark source was built with matching headers and
libraries for each version because CompileStep's C++ layout gained storage. The
retained before executable and newly rebuilt after executable each measured five
batches of 1000 session constructions per family member. Median Running Min
preparation was **59.01 → 59.76 µs**, Running Max **58.35 → 59.41 µs**.
These small local samples do not establish a significant regression or speedup.

Two reversed-order before/after Release object-compilation pairs use configured
flags and the matching retained Runtime.h for the old sources. Median offline
compilation was **26.35 → 26.64 s**; realtime **39.93 → 39.71 s**.
These are local measurements, not isolated compiler-performance claims.
Initial selected-target builds took
Debug 63.37 s, Release 936.83 s, ASan/UBSan 111.93 s and TSan 73.13 s;
the Release build includes the large existing test translation units rebuilt
after the CompileStep layout change. Final consumer-repair builds took
Debug 12.82 s, Release 62.36 s, ASan/UBSan 48.31 s and TSan 18.48 s.

This accepts the compiled original-output records, migrated family bindings and
the four consumer fixes. Remaining prepared output layouts, terminal metadata
consumption, shared scope/event/voice/resource/clock contracts and legacy isolated
participants without checkpoints remain open. The kernel C ABI and serialized
project format are unchanged; the C++ CompileStep DTO requires matching builds.
No capability promotion: **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## Shared terminal audio bindings — accepted 2026-09-05

A common private terminal resolver now expands the core TerminalAudioLayout
against actual CompileStep inputs into ordered logical channels with owned input
positions and buffer indices. It preserves an unconnected optional right input,
repeated stereo-pair ports and additional Disk Recorder controls. Port identifiers
refer only to static core declarations, never to mutable plan strings. Missing
required or duplicate declared audio bindings reject. The resolver is neither
execution admission nor original-producer ownership; compensation buffers remain
valid separately produced input routes.

The compiler uses the resolver to publish its existing outputBufferIndices.
Shared Audio Output structure authority checks the same channel order and route
agreement, retaining the separate exact physical-input-order requirement.
Realtime Audio Output, Output4/8, paired stereo and recorder channel preparation
consume the shared declarations/bindings. Local numeric channel/name selection
and the separate paired-port array have been removed. Existing descriptor,
producer, scope, latency, terminal-specific and compiler-seal checks remain.
Ordinary offline multichannel validation retains its earlier diagnostics and then
checks the same resolved route contract; legacy matrix fallback and recorder
admission stay at their existing adapters. No terminal gains generated producer
buffers, and no callback path gains resolution work or allocations.

The new focused test exercises missing/duplicate/optional inputs, independent
physical input order, owned indices, repeated channels, compiled routes and
compatibility aliases, invalid buffer/type bindings, recorder controls and
non-audio sinks. Six literal graph fixtures cover mono/stereo Audio Output,
Output4/8 and paired stereo4/8. Their channel counts and source mappings are
independent of the shared declaration table; analytical sine/amplitude oracles
verify every sample over irregular continuation and reset. Sealed route/alias/
input mutations reject in both backends. The focused Debug suite passes before
and after the final order-helper refinement. Independent read-only review has no
findings, including compensation routing, legacy admission and metadata lifetime.


Fresh final verification: **Debug 9/9 (111.09 s), Release 9/9
(14.07 s), ASan/UBSan 4/4 (90.26 s), TSan 4/4 (221.86 s)** —
26 passing groups. Native groups cover terminal and producer output bindings,
compile plans, latency compensation, nested sounds, ordinary offline rendering,
realtime sessions, common conformance and the realtime output recorder.
Sanitizer groups cover terminal bindings, compile plans, latency compensation
and common conformance. All runtime tests and preparation benchmarks ran serially
after all builds/compile probes; TSan was last. Whitespace checks pass. This
increment did not rerun the full suite, actual-XPC cases, Workbench or the tests-off install.

The paired Release preparation probe uses the same fixed five-node
Running Min/Max-to-Audio Output graph source in matching retained before and
rebuilt after executables, five batches of 1000 session constructions per member.
Median preparation was **59.52 → 59.80 µs** for Running Min and
**58.42 → 58.70 µs** for Running Max. These small local samples do not establish
a significant regression or speedup.

Two reversed-order before/after Release object-compilation pairs use configured
flags and coherent baseline source/private AudioOutputAuthority.h copies.
Median offline compilation was **26.34 → 26.64 s**; realtime
**39.32 → 39.46 s**.
Final selected-target builds took Debug **11.63 s**, Release **90.73 s**,
ASan/UBSan **70.23 s** and TSan **29.52 s**. Debug had already built the focused
target during implementation; these incremental build times are not comparable
clean-build benchmarks. No compiler-performance improvement is claimed.

This accepts the shared terminal channel-resolution boundary alongside the
earlier compiled original-output bindings. Remaining prepared producer layouts,
shared scope/event/voice/resource/clock contracts and legacy isolated participants
without checkpoint support remain open. The kernel C ABI, public C++ DTO layout
and serialized project format are unchanged in this increment.
No capability promotion: **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## Producer phases and compensated oscillator rollback — accepted 2026-09-06

Prepared output ownership now covers three distinct execution phases.
Latency-compensation operations bind only their actual synthetic destination,
validated against the compensation record, target input, audio capacity,
uniqueness and exclusion of original compiler output records. FeedbackRead binds
its original declared output. FeedbackCommit and all four audio-terminal
publication paths carry validated empty output lists. Terminal publication is
shared once, and the common exactly-one producer proof no longer needs
FeedbackCommit/AudioOutput kind exclusions. Remaining operation layouts retain
their compatibility enumeration. Metadata is not execution authority.

The real compensated oscillator fixture exposed two existing integration defects.
First, promoted oscillator admission counted the synthetic descriptor copy as an
extra original output. KernelOscillator now binds its original output through
the shared validator, and its already compiler-authenticated owner count excludes
declared synthetic compensation outputs. Untracked original buffers still count
and reject; all existing compiler bundle, control producer, scope and capacity
checks remain. Second, compensation's mutable delay ring/cursor had no checked
callback checkpoint coverage, so oscillator coexistence still rejected it.

LatencyCompensation now has dedicated checkpoint coverage, with ring storage
allocated during both general and oscillator-tail preparation, and ring/cursor
capture and restore on callback boundaries. Existing DelayWithFeedback behavior
is unchanged. The operation is not classified as stateless and the coexistence
guard has not been bypassed. Legacy compensation capacity may exceed the callback
limit; legacy feedback/kernel-oscillator output bindings use admitted buffer
capacity while promoted oscillator retains its exact capacity authority.
The historical oscillator adapter remains on its existing compatibility path.

Permanent tests cover separate original/synthetic ownership despite identical
BufferSpecs, missing/duplicate/conflicting compensation metadata, immutable owned
indices, empty producers at buffer zero and legacy capacity. Real compiler-created
compensation and named-feedback graphs match independent delayed-sine and
read-before-commit recurrence oracles over irregular continuation and reset.
Forged phase plans reject in both backends. A live connected VCA control drives
two repeated late overflow failures after a committed prefix; corrected retries
must recover the earlier delay/oscillator state and match the independent oracle.
All these callbacks count zero C++ allocations.

The focused Debug group passes. An isolated mutation build removes only
compensation's restore case, retaining preparation and processing: the late-failure
test then fails its independent PCM recurrence check. Production source and the
normal Debug library were never replaced for this probe.
Independent final read-only review has no findings, including both checkpoint
profiles, original/synthetic ownership and all empty-output construction paths.


Fresh final verification: **Debug 10/10 (112.21 s), Release 10/10
(14.51 s), ASan/UBSan 5/5 (93.63 s), TSan 5/5 (226.24 s)** —
30 passing groups. Native groups cover producer phases, terminal and original
output bindings, compile plans, latency compensation, nested sounds, ordinary
offline rendering, realtime sessions, common conformance and the realtime output
recorder. Sanitizer groups cover producer phases, original output bindings,
compile plans, latency compensation and common conformance.
All runtime tests and preparation benchmarks ran serially after all builds and
compile probes; TSan was last. Whitespace checks pass. This increment did not rerun the full suite,
actual-XPC cases, Workbench or the tests-off install.

The same small Release probe object linked against retained before and repaired
runtime libraries confirms the admission change: the compensated oscillator graph
was offline=1/realtime=0 before and is offline=1/realtime=1 after; the paired
feedback graph remains offline=1/realtime=1.

The paired preparation benchmark also reuses one object, five batches of 1000
session constructions per graph and version. The feedback graph admits all 5000
sessions on each version; median preparation is **62.85 → 63.06 µs**.
The compensation graph changes from **0/5000 admitted** to **5000/5000 admitted**,
with repaired successful preparation at **59.71 µs**. Its old 14.40 µs early
rejection is not a comparable successful preparation measurement. These small
local samples establish no significant performance regression or speedup.

Two reversed-order before/after Release object-compilation pairs use configured
flags and coherent baseline private PreparedOutputBindings.h/source copies.
Median offline compilation is **26.54 → 26.69 s**; realtime
**39.79 → 40.14 s**. Final selected-target builds took Debug **15.69 s**,
Release **97.23 s**, ASan/UBSan **69.46 s** and TSan **33.77 s**. Debug had
already built the focused target during implementation; these incremental times
are not clean-build benchmarks.

This accepts explicit compensation/feedback/terminal producer phases, the
compensated oscillator admission correction and allocation-free compensation
callback rollback. Other producer layouts, broader scope/event/voice/resource/
clock contracts and legacy isolated participants without checkpoints remain open.
No kernel C ABI, public C++ DTO layout or serialized project format changed.
No capability promotion: **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## Cascaded delay output ownership — accepted 2026-09-06

The real compiled cascade Oscillator → DelayWithFeedback → direct/second
DelayWithFeedback → aligned Mixer was rejected by both adapters. Both delay
nodes honestly declare and process three samples of latency. The compiler adds
three samples of compensation to the short branch, copying the first delay's
buffer identity; raw node/scope counting incorrectly treated that synthetic slot
as a second original delay output.

A shared allocation-free structural validator now checks compensation ownership
before original-output counting excludes a synthetic slot. Untracked originals
still count. DelayWithFeedback validates its actual original output against
shared compiled metadata in both adapters, and realtime stores that binding at
preparation. Promoted realtime Oscillator uses the same validated count.
Compiler/descriptor authentication, existing exact delay buffer capacities,
control producer checks and legacy compensation capacity remain intact. This
metadata is not execution authority. Other legacy output-counting branches have
not been migrated by this increment.

The permanent compiled regression failed on the retained implementation with
`Realtime renderer rejected Delay With Feedback buffer authority`. The retained
same-object Release probe separately recorded both offline and realtime
rejection. Repaired checks cover an independent six-sample delayed sine oracle,
irregular continuation/reset, forged compensation rejection, and two repeated
late overflow failures followed by corrected retry. Cascaded rollback starts
after eight committed samples; overflow occurs at the final sample of the next
five-frame callback. All tested realtime callbacks allocate zero C++ storage.
Helper tests cover malformed/duplicate/conflicting records, untracked original
slots, scope separation and larger legacy capacities.

Fresh verification: **22/22 groups pass**.

| Configuration | Focused groups | Time |
| --- | ---: | ---: |
| Debug | 8/8 | 111.84 s |
| Release | 8/8 | 13.89 s |
| ASan + UBSan | 3/3 | 51.30 s |
| TSan | 3/3 | 137.94 s |

Native groups cover producer phases, original output bindings, compile plans,
latency compensation, offline/realtime rendering, nested sound and runtime
conformance. Sanitizer groups cover producer phases, latency and conformance.

The same Release probe object linked against the retained and repaired runtime
reports cascade offline/realtime 0/0 → 1/1; its feedback comparator remains 1/1.
Five batches of 1,000 realtime preparations keep feedback admission at 5,000/5,000
and median cost at 62.54 → 62.47 µs. Cascade changes from 0/5,000 admissions at
17.56 µs (early rejection) to 5,000/5,000 at 64.74 µs; these are not comparable
successful-preparation costs. One local before/after object-compilation pair
measured offline 28.55 → 26.33 s and realtime 39.51 → 40.24 s; this small sample
is a regression observation, not a speedup claim. Baseline source used its
retained private header. At most two heavy compilers ran concurrently; all
runtime tests and preparation measurements ran serially after builds, TSan last.

Independent read-only review found no P0–P3 issues. Generated capability matrix
checks pass. No full suite, Workbench,
actual XPC helper execution or tests-off install was rerun for this increment.
No public C++ DTO, kernel C ABI, wire format or capability profile changes.
Broader legacy isolated whole-call rollback and remaining P2 ownership/scope
migration remain open. Supported profiles stay **94/171 realtime (55.0%) and
86/171 offline (50.3%)**.

## Ambisonic Encoder downstream producer recognition — accepted 2026-09-06

The horizontal Ambisonic Encoder prepared W/X/Y buffer slots but left its
spatial output count at zero. The common exactly-one producer check therefore
rejected all three outputs when connected to a checked consumer such as
DelayWithFeedback. Direct encoder/decoder graphs did not exercise that check.
The retained compiled probe confirms offline success and realtime rejection for
each of W/X/Y; a VCA comparator admitted all three and did not expose the defect.

Encoder preparation now derives the spatial count from its existing three-port
list and fills exactly those slots. No DSP arithmetic, legacy admission rule,
capacity contract or callback allocation behavior changes. This closes a concrete
producer-recognition defect; it does not complete migration to shared compiled
output metadata for every legacy operation.

The permanent regression failed before the fix with the expected W-output
Delay With Feedback buffer-authority error. It exercises each output through a
one-sample DelayWithFeedback using oscillator input and azimuth 0.2. Independent
W/X/Y coefficients (1/√2, cos(0.4π), sin(0.4π)) are distinct, so swapped channels
cannot pass by symmetry. Both adapters must match the delayed analytical signal
over irregular continuation and reset. Tested realtime callbacks allocate zero
C++ storage.

Fresh verification: **12/12 focused groups pass**.

| Configuration | Groups | Time |
| --- | ---: | ---: |
| Debug | 4/4 | 72.79 s |
| Release | 4/4 | 7.98 s |
| ASan + UBSan | 2/2 | 8.50 s |
| TSan | 2/2 | 13.84 s |

Native groups cover producer phases, four/eight-channel spatial runtime,
realtime rendering and conformance. Sanitizers cover producer phases and spatial
runtime. The identical retained Release probe object linked against the repaired
library changes all three ports from offline/realtime 1/0 to 1/1. Builds were
limited to two heavy compilers; runtime checks ran serially after all builds,
TSan last. Generated capability matrix checks pass.
No full suite, Workbench, actual XPC helper run or tests-off install was rerun
for this local correction.

One independent read-only review found no P0–P3 issues after checking every use
of the spatial output count. No new public DTO, kernel ABI, serialized format or
capability profile. Other P2 ownership/scope work and legacy isolated whole-call
rollback remain open. Supported profiles remain **94/171 realtime (55.0%) and
86/171 offline (50.3%)**.

## Active multichannel outputs and general delay checkpoints — accepted 2026-09-06

Prepared output ownership now covers Matrix4/8, Pan stereo/discrete4/discrete8,
Horizontal Ambisonic Encoder/Decoder and Quad Function Generator. Each adapter
binds its actual written slots before publication. The common exactly-one
producer check no longer enumerates matrix/spatial arrays or special-cases these
operation kinds. Primary, secondary and isolated component layouts retain their
existing compatibility paths; full P2 remains unfinished.

The private active-output validator distinguishes the complete expected port
schema from the selected layout. When compiler output records exist, it checks
all ordered declarations, including inactive ports, against original scoped
buffers and their type/capacity. It separately validates active membership,
actual slots and uniqueness, then owns only those active indices. Synthetic
compensation cannot impersonate an original. Legacy empty records, absent
inactive buffers and nonuniform capacities above the callback limit remain valid.
Existing exact-capacity validation and compiler/descriptor authentication remain;
metadata is not execution authority. No public DTO, kernel ABI or wire changes.

The new Quad → DelayWithFeedback fixture exposed an existing general transaction
gap: the delay ring/cursor already had capture/restore, but Delay was excluded
from general checked-DSP coverage and its checkpoint storage was prepared only
for oscillator-tail transactions. It now has dedicated coverage and preparation
for both profiles. Neither the coexistence guard nor the stateful operation is
bypassed. A four-times-scaled triangle with live downstream gain overflows at the
last sample of a five-frame callback after five committed samples. Two repeated
silent failures followed by corrected retry must restore both generator phase
and delay history and match the independent oracle without C++ allocations.

Permanent coverage adds active/inactive ownership and adversarial declaration
checks, plus 42 selected-output Matrix/Pan/Decoder/Quad fixtures through checked
delay consumers and four inactive-layout rejection fixtures. Three existing
encoder-output fixtures remain. Independent gain, panning, Ambisonic and triangle
oracles exercise irregular blocks and transport reset. Full declaration checks
cover malformed inactive records, synthetic aliases, wrong scope/type/capacity,
duplicate/unknown active slots and immutable prepared ownership.

Historical C74/C18/C17 tests had used Delay as an example of a stateful
operation without general checkpoint coverage. Those exact interactions are now
positive regressions: a selected Sound candidate retries past filled delay history;
FIR → Delay matches an independent feedback recurrence after repeated late
overflow; Delay → biquad survives two scheduled invalid-formant failures after
12,000 samples of history fill. Reset and callback allocation checks remain.
Real Frequency Scale graphs retain the negative general-checkpoint exclusion,
with standalone admission established and the specific transaction rejection
checked. A focused CTest invocation executes these existing integration functions
under ASan/UBSan and TSan; the full native runtime suite still runs them normally.

Fresh verification: **28/28 focused groups pass**.

| Configuration | Groups | Time |
| --- | ---: | ---: |
| Debug | 8/8 | 82.64 s |
| Release | 8/8 | 11.31 s |
| ASan + UBSan | 6/6 | 67.31 s |
| TSan | 6/6 | 166.47 s |

Native groups cover producer phases, original output bindings, compile plans,
latency compensation, spatial runtime, Quad Function Generator, the full runtime
session test executable and conformance. Sanitizers cover producer phases,
output bindings, spatial/Quad runtime, conformance and the focused checkpoint
coexistence invocation. Two isolated mutations are detected: publishing inactive
outputs fails the ownership oracle; omitting Delay restore fails the independent
Quad/delay oracle and, separately, C74 selected-candidate recovery. Mutations did
not replace production sources or libraries.

The same Release probe object retains Matrix/Pan/Decoder admission in both
adapters and changes Quad → Delay from offline/realtime 1/0 to 1/1. Five batches
of 1,000 realtime preparations retain 5,000/5,000 admissions for Matrix, Pan and
Decoder; median times are respectively 65.99 → 66.04 µs, 58.70 → 58.90 µs and
60.55 → 59.74 µs. Quad changes from 0/5,000 early rejections at 50.26 µs to
5,000/5,000 successful preparations at 53.47 µs; those are not comparable
successful-preparation costs. One local object-compilation pair measured offline
28.95 → 28.18 s and realtime 39.88 → 39.72 s using retained baseline headers.
This small sample is an observation, not a speedup claim.

At most two heavy compilers ran together. Runtime checks and measurements ran
serially after their builds, with TSan last. Generated capability matrix checks
pass.


One independent read-only review found no P0–P3 issues. No full native suite,
Workbench, actual XPC helper execution or tests-off install was rerun for this
increment. Legacy isolated whole-call rollback and common scope/event/voice/
resource/clock contracts remain open. Supported profiles stay **94/171 realtime
(55.0%) and 86/171 offline (50.3%)**.

## Frequency Scale general callback checkpoints — accepted 2026-09-06

Frequency Scale already captured/restored its delay ring/cursor and dynamic phase
vocoder for oscillator-tail rollback, but General preparation omitted storage,
coverage classified it Unsupported, and a pointer exclusion separately blocked
active pitch state. The same compiled Frequency Scale/FIR/biquad regression
rejected before the change with the exact locally-transactional-DSP diagnostic.

General now prepares the same owned checkpoint and classifies Frequency Scale
as DedicatedCheckpoint; only its redundant pitch-pointer exclusion is removed.
Existing shape-checked capture, allocation-free restore and spectral retiming
remain unchanged. No arithmetic, public ABI, descriptor or unsupported participant
exclusion is replaced by a stateless assumption.

Permanent regression coverage includes a neutral delayed-sine/FIR recurrence
with late VCA overflow, plus six Frequency Scale/FIR/biquad combinations: neutral,
static active, and connected ratios 1/1.5/0.75/1.25/1, with FIR before and after.
Each 1280-frame section exceeds the 1071-frame pre-delay plus spectral latency.
After a full 256-frame spectral hop at each ratio boundary, an invalid formant
fails the second segment. Repeated failures silence the whole callback; corrected
continuation is bit-identical to uninterrupted realtime and agrees with the
independent offline renderer. Irregular partitions and reset cross filled history;
all measured realtime callbacks allocate zero C++ storage. Neutral output also
matches a separate analytical sample recurrence.

C74 Sound selection rollback now includes neutral and active Frequency Scale
branches; the active branch fills 80 blocks of 16 frames before selection and
late failure. Original Delay and oscillator variants remain. Existing C74/C18/
C17 rejected Frequency Scale graphs are retained as positive General admission
fixtures, with runtime recovery supplied by the expanded integration tests.

**24/24 focused groups pass:**

| Configuration | Groups | Serial elapsed |
| --- | ---: | ---: |
| Debug | 6/6 | 119.56 s |
| Release | 6/6 | 13.41 s |
| ASan/UBSan | 6/6 | 211.12 s |
| TSan | 6/6 | 700.32 s |

Native groups cover the new checkpoint regressions, phase-vocoder state tests,
native STFT, producer-phase integration, conformance and the full realtime test
executable. Sanitizers run the same focused neighbors and C74/C18/C17 checkpoint
coexistence invocation in place of the full monolithic runtime executable.
All runtime tests and preparation probes ran serially after every build and
mutation compiler had exited; TSan was last. Generated capability matrix checks
pass.


Two isolated production-source mutations are detected. Omitting Frequency Scale
ring restore fails the independent delayed-sine/FIR recurrence; omitting dynamic
pitch restore (including retiming) makes the repaired callback fail to continue.
Both mutation executables exit -6 at those specific assertions, and leave the
normal source and libraries unchanged.

One Debug probe object linked against retained before/after runtime libraries
measures five batches of 200 session constructions/destructions per fixture.
Standalone neutral and active profiles retain 1000/1000 admissions: median costs
are 4158.02 → 4123.03 µs and 4776.76 → 4772.46 µs respectively. FIR combinations
change from 0/1000 admissions to 1000/1000 for both profiles. Neutral early
rejection costs 4166.47 µs versus successful preparation 4168.86 µs; active early
rejection costs 4482.65 µs versus successful preparation 4815.79 µs. Rejection and
success costs are not equivalent work. These local Debug observations are not a
speedup claim or a clean compilation benchmark.

The initial exploratory long test run was deliberately terminated after sampling
showed repeated offline FIR expression parsing dominated its runtime. The final
fixture removes redundant steady frames while retaining every ratio, filled
history and reset, and moves failure after a whole spectral hop. That terminated
run is not counted as a pass. One independent read-only review has no P0–P3
findings.

No full project suite, Workbench, actual XPC helper execution, tests-off install
or performance improvement claim is included in this increment. General native
STFT/other external participants, legacy isolated whole-call rollback and broader
P2 contracts remain open. Supported profiles stay **94/171 realtime (55.0%) and
86/171 offline (50.3%)**.

## Native offline owned checkpoints — accepted 2026-09-06

Native `OfflineRenderSession` public transactions now capture owned processing
state, process only newly requested frames, and commit or restore in place.
Fresh candidate reconstruction and the vector of accepted render partitions are
removed. The shared private journal preserves the combined backend's behavior.
Native copyable values and all 13 owned processors are captured explicitly;
lazily created processors return to absent on rollback. Immutable resource
identities stay live, and spectral payloads restore into original buffer owners.
The private session layout changed, so dependent objects must be recompiled;
the kernel C ABI and public data-transfer records did not change.

The native FIR regression reproduces 576 unchecked frames for 128 output frames
in the retained baseline, and requires exactly 128 after the change. Every PCM
sample also matches an independent FIR recurrence. Active Frequency Scale with
FIR and a scheduled gain change, and lazy Diffuse Spectral Stretch, exercise two
late failures, empty failed payloads, unchanged public position, exact repaired
continuation through populated spectral history, irregular blocks and reset.

Two initial full-offline failures identified historical zero-frame semantics:
stale assignments must not be consumed by a zero seed, and zero-frame Spectrum
Modifier probability state must not survive when historical replay discarded it.
The existing assertions are retained. Explicit zero-frame compatibility covers
the original no-op set, 82 replay-family categories used only for zero validation,
the direct oscillator exception, three spectral commit families and Set Duration
discard. Positive renders never replay audio. Sound selection across reset and
existing combined/isolated backend routing remain unchanged.

**70/70 selected runtime groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 27/27 | 129.17 s |
| Release | 27/27 | 21.57 s |
| ASan/UBSan | 8/8 | 177.17 s |
| TSan | 8/8 | 373.33 s |

Debug and Release include full offline and realtime executables, conformance,
native checkpoint and Frequency Scale integration, producer phases, clocks,
filters, latency, nested Sound, immutable resources, Wave Terrain, Sample Map,
Diffuse processing and the related spectral DSP state tests. Sanitizers include
full offline, new native ownership/recovery, Frequency Scale, Delay coexistence,
producer phases and the three resource-backed native neighbors. All runtime
checks and probes ran serially after all build/mutation compilers exited; TSan
was last. This is selected integration coverage, not the full project suite.

All three isolated production-source mutations are detected with exit -6 at the
intended oracle: missing native values or owned DSP restore breaks exact repaired
continuation; missing presence restore leaves a cold-created Diffuse processor
present after failure. Normal production sources and libraries remain unchanged
by the mutations. One independent read-only review has no P0–P3 findings.
Generated capability matrix checks pass.

Release timing uses three-trial medians, 256-frame public blocks, and includes
independent PCM validation inside the timed loop:

| Fixture | 32 blocks, before → after | 64 blocks | 128 blocks |
| --- | --- | --- | --- |
| Oscillator | 0.436 → 0.809 ms | 0.934 → 0.963 ms | 1.730 → 2.404 ms |
| Oscillator + FIR | 8.061 → 0.808 ms | 30.931 → 1.614 ms | 121.052 → 3.209 ms |

Direct oscillator rendering now pays fixed checkpoint overhead. The 128-block
median adds 0.674 ms (5.26 µs/block); individual trials vary. This is explicitly
not a no-regression result. FIR time scales approximately with new blocks rather
than accumulated history. The work counter is the deterministic regression gate;
these local wall-clock observations are not a timing acceptance threshold.
Debug medians at 128 blocks are 7.557 → 8.107 ms for the oscillator and
840.843 → 15.295 ms with FIR.

Separate Release preparation probes use three batches of 100 graph compilations
or 1000 session constructions/destructions. Oscillator compile costs are
1178.00 → 1168.74 µs and session costs 3.694 → 3.789 µs; FIR compile costs are
1181.41 → 1200.72 µs and session costs 6.639 → 6.769 µs. Every compilation succeeds.
Before/after probe objects use their matching retained/current private headers;
reusing a probe object across this layout change would be invalid. Incremental
selected-target C++ builds take 89.42 s Debug, 847.17 s Release, 123.71 s ASan/UBSan
and 77.64 s TSan. These are not paired clean C++ build benchmarks.

No Workbench, actual XPC helper execution, tests-off install, offline allocation-free guarantee or
full-project acceptance is included. Legacy isolated whole-call rollback,
wider realtime participant combinations and broader P2 contracts remain open.
Supported profiles remain **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## Spectral general callback checkpoints — accepted 2026-09-06

General checked DSP compositions now prepare the existing owned checkpoint for
NativeForwardStft, NativeInverseStft, packed Fft, LiveSpectralAnalysis and
PartialSpectrumLegacyHold. Capture/restore, processor identity and spectral
retiming remain the existing processing authority. Other external participants
and TimeFrequencyScale retain their current admission rules.

Integration exposed two additional defects. Native rendering forced one-frame
blocks for any live-expression dependency even when its value had not changed;
this changed block-rate partial-hold output. Native intervals now end at actual
Event Value steps and Variable/Sound assignments, with one-frame processing
retained during linear ramps and feedback. Second, a successful one-frame repair
cleared recovery suppression while the invalid scheduled entry remained ahead of
the cursor. A prepared atomic watermark per binding now suppresses older queued
publications across later callbacks. Newer publications and unrelated bindings
remain effective; combined offline rollback and explicit reset cover the watermark.

The permanent spectral executable covers 18 composition/recovery scenarios and
two independent step/linear control-boundary cases. STFT/FFT round trips have a
separate delayed-source/FIR oracle. Recovery tests compare exact uninterrupted
PCM and partial frame contents, timestamps, sequence and identities through
populated histories, repeated late failures, a one-frame repair followed by the
remaining 64 frames, changing controls, irregular partitions and reset. Native
PCM is checked independently. Legacy and typed scheduling paths are crossed for
old invalid and newer valid publications, with another binding changing at the
invalid boundary. Successful and failed realtime calls allocate no C++ storage.

**96/96 selected runtime groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 35/35 | 134.83 s |
| Release | 35/35 | 24.23 s |
| ASan/UBSan | 13/13 | 376.57 s |
| TSan | 13/13 | 849.70 s |

Debug and Release include full offline/realtime and conformance, native and
Frequency Scale checkpoints, producer phases, snapshot performance, typed events,
control/live ingress, graph host, clocks, filters, latency, nested Sound,
resource snapshots and related spectral/generative DSP tests. Both sanitizers
include full offline and realtime, conformance, all three new checkpoint suites,
STFT/packed FFT/live analysis, snapshot performance, typed/control ingress and
graph host. Runtime checks and probes ran serially after all build and mutation
compilers exited; TSan was last. Generated capability matrix checks pass. This
is selected integration coverage.

All six isolated production mutations are detected with exit -6: omitted STFT,
packed FFT or live-analysis restore prevents repaired continuation; omitted
partial-hold restore changes exact recovered PCM; the old native segmentation
breaks the step/linear interval oracle; omitted committed suppression replays the
invalid entry after the short retry. Mutations alter only temporary source and
archive copies. One independent read-only final review reports no P0–P3 findings.

The same retained probe object compiles all 18 fixtures before and after. Ready
sessions increase from 8/18 to 16/18. The two live-analysis → selector → FIR
fixtures still fail the separate exact causal-latency guard; no latency is
invented to admit them. Same-object Debug preparation measurements use the median
of 31 session constructions (destruction excluded):

| Fixture (oscillator source) | Before → after | Admission |
| --- | ---: | --- |
| STFT pair | 2393.88 → 2444.50 µs | ready → ready |
| FIR → STFT pair | 2500.58 → 2502.29 µs | rejected → ready |
| Packed FFT pair | 13624.90 → 13649.80 µs | ready → ready |
| Live analysis/hold | 2506.92 → 2506.54 µs | ready → ready |
| FIR → live analysis/hold | 2595.25 → 2634.12 µs | rejected → ready |

These are local observations, including the cost of newly prepared state, not a
performance acceptance threshold or a no-regression claim. The private header
layout is unchanged within this package, allowing the same probe object with the
retained/current libraries. Selected incremental builds take 37.12 s Debug,
83.67 s Release, 63.97 s ASan/UBSan and 40.06 s TSan; these are not clean-build
benchmarks.

No full-project suite, Workbench, actual XPC helper execution or tests-off install is claimed. Legacy
isolated whole-call rollback, wider participant combinations and broader P2
contracts remain open. Supported profiles remain **94/171 realtime (55.0%) and
86/171 offline (50.3%)**.

## Time Frequency Scale callback checkpoints — accepted 2026-09-06

TimeFrequencyScale now participates in the common dedicated callback checkpoint.
The checkpoint captures playback position, active/started flags and the optional
existing dynamic phase-vocoder processor. Preparation reserves a matching
processor; capture validates presence and processing shape, and restore keeps
processor identity while retiming the next process. Immutable sample storage and
fully overwritten source scratch do not become a second history authority.
The original bounded in-memory, exact latency, control, correction and resource
admission checks remain in force. No public or kernel ABI changes.

A retained six-fixture probe compiles all neutral, static pitch/time and connected
scale profiles, standalone and followed by FIR. Before the change only the three
standalone graphs prepare; all FIR compositions reject at the transactional
closure guard. The permanent test also reproduces this rejection before the
production change. All six permanent scenarios pass after implementation: three
profiles with FIR alone and with an additional checked TwoFormant failure source.

The tests fill history, turn Gate off inside a callback that fails later, repeat
the failure, repair a one-frame prefix before the Gate boundary, continue through
Gate off/on and change connected scale/rate. A second failure crosses the finite
sample endpoint. Exact uninterrupted realtime PCM, independent native PCM and
an independent neutral sample/FIR recurrence cover playback and DSP state.
Irregular partitions, filled audible history, zero allocations during successful
and failed realtime callbacks, and reset versus a fresh session pass.

The initial connected test mistakenly requested correction 1.25/0.5 = 2.5,
outside the existing authority. Native rendering correctly rejected it before
frame zero. The fixture uses initial correction 1.25/0.75 = 5/3 and later
0.75/1.25 = 0.6; production range guards and existing negative oracles stay intact.

**26/26 selected runtime groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 9/9 | 101.06 s |
| Release | 9/9 | 12.82 s |
| ASan/UBSan | 4/4 | 41.87 s |
| TSan | 4/4 | 111.65 s |

Debug and Release include full offline/realtime, conformance, Time Frequency
Scale, spectral, Frequency Scale and native offline checkpoints, phase-vocoder
DSP and static FIR authority. Both sanitizers include Time Frequency Scale,
spectral and Frequency Scale checkpoints plus the phase-vocoder DSP suite.
Runtime and probes ran serially after all compilers exited; TSan was last.
Generated capability matrix checks pass.
This is selected integration coverage, not the full project suite.

All three isolated missing-restore mutations fail at the exact recovered-PCM
oracle: omitted playback position, omitted active/started flags and omitted
phase-vocoder restore. Temporary source/archive copies leave production intact.
One independent read-only final review has no P0–P3 findings.

The same retained probe object now prepares all six fixtures (6/6 versus 3/6).
Debug preparation medians of 31 session constructions, excluding destruction:

| Profile | Standalone before → after | FIR before → after |
| --- | ---: | ---: |
| Neutral | 2477.50 → 2474.54 µs | 3584.42 → 3608.00 µs |
| Static pitch/time | 2666.38 → 2664.04 µs | 2700.92 → 2932.42 µs |
| Connected scale | 3269.21 → 3257.96 µs | 2724.50 → 3531.00 µs |

Every before-FIR attempt is rejected and every after-FIR attempt is ready, so
those times compare different preparation outcomes. The extra prepared state
has a measurable cost; these local observations are not a timing threshold or a
no-regression claim. The Runtime.h layout is unchanged within this package, so one probe object can link the retained/current libraries. Selected
incremental builds take 11.07 s Debug, 53.31 s Release, 34.50 s ASan/UBSan and
18.30 s TSan; these are not paired clean-build benchmarks.

This package does not accept disk-backed sources, memory-writer sources, wider TimeFrequencyScale
controls, a full-project suite, Workbench, actual XPC execution or a tests-off
install. Legacy isolated whole-call rollback and broader P2 contracts remain
open. Supported profiles stay **94/171 realtime (55.0%) and 86/171 offline
(50.3%)**.

## Partial Track Player compact callback checkpoints — accepted 2026-09-06

PartialTrackPlayer now participates in the common dedicated callback checkpoint.
The resynthesizer keeps immutable prepared tracks/points and one active-segment
continuation vector. Cursor/phase storage is bounded by the largest selected
segment, including the existing realtime limit of 128 active voices. Entering a
new segment or seeking resets the active vector; prior segments cannot be
revisited without reset under the original next-source-frame contract.
Interpolation, phase arithmetic, seeded bandwidth noise, gain, source-end and
zero-frame behavior are preserved.

An opaque prepared ProcessingCheckpoint owns that compact vector plus current
segment, next source frame and initialization state. Capture/restore validate
preparation identity and shape, then copy in place without allocation. Explicit
owned clones and moves retain identity; an independently prepared processor
cannot import a checkpoint even when its asset/shape matches. The existing
allocating offline clone/move restore continues to own all prepared data. Runtime
admission removes only the stale PartialTrackPlayer pointer exclusion and uses
the owner's compact API. Other external consumers and all resource/port/rate
validation remain unchanged. The private C++ layout changes, so selected
dependents are rebuilt; the kernel C ABI is unchanged.

The retained six-fixture probe compiles tone, multisegment/noisy and empty assets,
standalone and with FIR. The original standalone sessions prepare (3/6), while
all FIR combinations reject at the transactional closure guard. The permanent
runtime regression reproduces the same rejection before the implementation.
Its six scenarios compare exact uninterrupted and native PCM across control
points, segment boundaries, empty segments and EOF. Repeated late failures,
a one-frame repaired prefix, irregular partitions and reset retain exact state
with zero successful/failed callback C++ allocations. The tone/FIR case also has
an independent analytic sine and FIR recurrence.

The compact core suite covers 32 step/linear, all/selected-segment and
continuation-boundary combinations, including zero, reset/seek, EOF and owned
move/clone. Unprepared and foreign checkpoint operations reject before mutation.
Checkpoint creation allocates 112 bytes for both a one-segment asset (3 total
tracks) and a 1024-segment asset (3072 total tracks), each with three active
tracks. Capture/restore/reset/render allocate nothing. Existing owned-clone,
interpolation, bandwidth-noise, adapter and original core assertions remain.

**42/42 selected test groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 15/15 | 96.97 s |
| Release | 15/15 | 13.93 s |
| ASan/UBSan | 6/6 | 133.58 s |
| TSan | 6/6 | 254.06 s |

Debug and Release include all seven Partial Track asset/edit/analysis/resynthesis/
derivative/editor-model/large-fixture suites, the new checkpoint suite, full
native offline/realtime, conformance, native offline/spectral/Time Frequency
Scale checkpoints and resource snapshot runtime tests. Both sanitizers include
Partial Track resynthesis/checkpoint/derivative, full offline, native offline
checkpoint and spectral checkpoint suites. All runtime tests and probes ran
serially after compilation finished, with at most two heavy compilers during
builds and TSan executed last. Generated capability matrix checks pass.

Separate probes compiled against the matching retained/current private headers
and libraries emit byte-identical output (1944 lines) over step/linear and
all/selected segment cases, irregular and zero-length calls, seeks, resets,
noise, transitions and EOF. This retained oracle is independent of both new
render backends. All six isolated mutations are detected: omitted phase, point
cursor, continuation fields, preparation identity, segment initialization and
runtime restore. The segment-initialization mutation still runs successfully
but differs from retained PCM; the other five fail their intended assertions.
Only temporary source/archive copies are mutated.

Debug preparation medians of 31 session constructions, excluding destruction:

| Profile | Standalone before → after | FIR before → after |
| --- | ---: | ---: |
| Tone | 2317.62 → 2327.00 µs | 2347.58 → 2403.33 µs |
| Multisegment/noisy | 2274.92 → 2276.33 µs | 2348.58 → 2359.54 µs |
| Empty | 2266.92 → 2266.71 µs | 2353.75 → 2342.96 µs |

All before-FIR attempts reject, while all after-FIR attempts prepare, so those
times compare different outcomes. These are local observations, not a timing
threshold or a no-regression claim. The private C++ layout changes require
separate before/after probe objects with matching headers. Selected incremental
builds take 55.47 s Debug, 782.79 s Release, 83.87 s ASan/UBSan, 52.75 s TSan; these are not paired clean-build benchmarks.

Independent read-only final review reports no P0–P3 findings. No full-project suite, actual Workbench UI,
XPC helper execution, tests-off install, profile promotion or full P2 acceptance
is implied. Legacy isolated whole-call rollback and broader P2 contracts remain
open. Supported profiles stay **94/171 realtime (55.0%) and 86/171 offline
(50.3%)**.

## Shared prepared scope authority and nested MIDI Sounds — accepted 2026-09-06

A valid nested Sound feeding a root MIDI Voice compiled but failed realtime
preparation. The original Sound had structural/event scope 1, while its two
processing voices had scopes 2 and 3; all three had parent 0. Constant and Filter
preparation incorrectly required the original Sound to be an ancestor of the
processing voice. The retained twelve-fixture probe prepares nine contexts and
rejects the three nested-MIDI Absolute Value, ArcTan and Filter combinations.
The permanent public regression reproduces the original rejection.

PreparedScopeAuthority now names processing, structural and event identity
separately. One bounded, allocation-free membership query replaces sixteen
realtime walks and the native Filter classifier's walk; it retains duplicate
membership counts and half-open range/scope checks. Local identity equality and
the existing bounded ancestry query are shared. Family-specific promotion
exclusions and native Filter's stricter structural/event relationship remain.

A preparation-only VoiceSources view accepts only the private compiler plan
when the executable plan is equal and the caller has verified exact matching
settings, including prepared scope/cell tables. Exactly one voice membership
then provides that step's original structural scope. Event/parameter ancestry
can follow the processing lineage or this compiler-proven source lineage.
Public reconstruction, altered plans/settings and ambiguous ownership cannot
supply the bridge. Variable parent tables, public Runtime.h layout, kernel C ABI,
DSP arithmetic, callback checkpoints, voice generations and event arbitration
are unchanged. The borrowed view exists only during preparation; the session
retains only an optional scope ID, never a borrowed checkpoint payload.

All twelve permanent public contexts pass: Absolute Value, ArcTan and Filter,
each local, nested, MIDI and nested-MIDI. Six non-MIDI contexts match exact
independent native PCM. Unary contexts also match analytic sine/gain and timed
two-voice output, including the global gain change at frame 23. Irregular and
uniform callbacks and held-voice transport reset match exactly with zero callback
C++ allocations. The same retained probe object links old/current runtime
archives: all nine previously ready contexts retain identical initial/reset PCM
(128 samples each), and all three previously rejected contexts now prepare. Their initial/reset
PCM also exactly matches the corresponding retained original root-MIDI context,
independently proving transparent nesting for all three new combinations.

Fixtures supply the existing typed resource/text declarations and explicit
pitch bounds. Controls schedule within the current prepared block. The oracle
also preserves existing lifecycle semantics: retrigger sets KeyDown to -1 before
the delayed attack, so audibility uses KeyVelocity as in existing fixtures.
Oscillators run from transport origin in each voice, and the five-millisecond
attack controls audibility. Transport reset resets DSP but retains held voices.
These are implementation characterizations, not observed reference behaviour.

Direct query tests distinguish the three roles, parent/sibling/root/missing
relations, duplicate ancestry, cycles, reordered tables, voice range endpoints,
scope mismatches, overlapping owners, overflow, absent compiler authority,
modified executable plans and ambiguous source ownership. Twenty-seven public
forgery/reconstruction cases reject across the three nested-MIDI families.
Five isolated mutations are detected: omitted event role, collapsed membership
ambiguity, omitted compiler-plan equality, unrestricted source ancestry and
omitted runtime source bridge. Direct header mutations run standalone without
the runtime archive to avoid mixed inline definitions. The runtime mutation
uses only a temporary source/archive copy and reproduces the original Constant
admission failure. Production files remain intact.

One independent read-only final review reports no P0–P3 findings in compiler
authority, per-step provenance, membership, borrowed lifetime and retained
family restrictions.

**36/36 selected test groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 12/12 | 93.63 s |
| Release | 12/12 | 12.80 s |
| ASan/UBSan | 6/6 | 57.27 s |
| TSan | 6/6 | 145.45 s |

Debug and Release include scope, unary/running-extremum family programs,
project numeric/Sound variables, nested Sound runtime, full realtime/offline,
conformance, MIDI allocator, prepared mapper and compiled output bindings.
Both sanitizers include scope, both family programs, nested Sound runtime,
MIDI allocator and conformance. Builds use at most two heavy compiler frontends;
all probes and runtime tests run serially after compilers finish, with TSan last.
Generated capability matrix checks pass.

Debug preparation medians of 31 session constructions, excluding destruction:

| Family | Context | Before → after |
| --- | --- | ---: |
| Absolute Value | Local | 2452.42 → 2479.92 µs |
| Absolute Value | MIDI | 2511.29 → 2524.50 µs |
| Absolute Value | Nested | 2400.42 → 2457.12 µs |
| Absolute Value | Nested MIDI (rejected → ready) | 156.25 → 2530.08 µs |
| ArcTan | Local | 2448.08 → 2434.96 µs |
| ArcTan | MIDI | 2509.46 → 2527.17 µs |
| ArcTan | Nested | 2401.33 → 2409.08 µs |
| ArcTan | Nested MIDI (rejected → ready) | 156.62 → 2526.46 µs |
| Filter | Local | 2363.42 → 2380.92 µs |
| Filter | MIDI | 2471.79 → 2500.79 µs |
| Filter | Nested | 2370.88 → 2379.75 µs |
| Filter | Nested MIDI (rejected → ready) | 137.42 → 2464.21 µs |

The three nested-MIDI timings compare rejected preparation with newly ready
sessions. These local observations are not timing thresholds or a no-regression
claim. Each before/after probe pair uses the same object because the public C++ layout is
unchanged within this package. Selected incremental builds take 23.48 s Debug, 74.05 s Release, 62.46 s ASan/UBSan, 36.22 s TSan; these are not paired clean-build benchmarks.


No full-project suite, actual Workbench UI, XPC execution, tests-off install or full P2 acceptance is claimed.
Dynamic voice lifetime, stream callback checkpoints, legacy isolated whole-call
rollback and wider scope/resource/clock contracts remain open. Supported
profiles stay **94/171 realtime (55.0%) and 86/171 offline (50.3%)**.

## MIDI input frequency pitch bounds — accepted 2026-09-06

A MIDI pitch limit such as 442 hz failed before runtime preparation because the
parameter expected NoteNumber and the general expression evaluator deliberately
does not convert Hertz to NoteNumber. Even fractional note-number bounds then
failed Mapper plan construction, which required exact integers. The MIDI-In
pitch bounds are specified to accept frequency values rounded to the nearest
equal-tempered MIDI note.

MidiPitchBounds provides an explicit preparation-time conversion for the two
MIDI-In pitch parameters. It first preserves ordinary note-number evaluation;
positive Hertz expressions can then use the equal-tempered logarithmic mapping.
Project-wide, effective-default and single-parameter editor audits share that
rule with compilation. Generic expression domain rules remain unchanged.
Mapper plan construction and realtime validation share finite ordered range
validation followed by nearest rounding. Authored expressions and their
unrounded values remain in the prepared plan. No allocator, callback processing,
voice lifetime, MIDI dispatch, typed-event or persisted-format rules change.

Twenty-eight Mapper contexts cover seven intervals, gate/velocity modes and
root/nested Sounds. Integer and frequency bounds include both inclusive MIDI
endpoints; immediate neighbors/wrong channels remain unhandled in realtime and
rejected by the offline schedule API. Independent stereo sine/gain oracles cover
scheduled note-on/off timing, irregular/uniform partitions and reset. Native
no-MIDI rendering stays silent and closed. Combined offline MIDI uses the
existing realtime-backed scheduled renderer; its agreement alone is not an
independent DSP oracle. Direct callbacks allocate no C++ memory. Two additional
MIDI Voice contexts verify frequency bounds, five-millisecond attack, release
and reset with a positive-KeyDown amplitude gate. This gate preserves existing
negative retrigger and retained velocity behavior rather than assuming fresh
voice state after transport reset.

Ten invalid-unit/range/reversed-input cases and seven inconsistent or malformed
prepared plans reject. Nonfinite range values reject before integer conversion.
A generic Hertz expression still cannot evaluate as NoteNumber outside this
MIDI parameter contract. The retained/current probe links the same probe object
against the corresponding old/new runtime and project archives.

The retained admission probe improves from **8/28 to 28/28**. All eight
previously admitted integer-bound contexts retain byte-identical 16-sample PCM.
Four isolated mutations are detected: floor instead of nearest rounding at the
lower bound, the same error at the upper bound, a wrong equal-tempered conversion
factor and a truncating realtime configuration reconstruction. The unmutated
standalone query control passes; each mutated binary aborts at its oracle. The
realtime mutation uses a temporary source/archive copy and fails the public
session-readiness oracle. No production file is mutated for these checks.

**26/26 selected test groups pass:**

| Configuration | Groups | Elapsed |
| --- | ---: | ---: |
| Debug | 9/9 | 90.85 s |
| Release | 9/9 | 11.57 s |
| ASan/UBSan | 4/4 | 52.73 s |
| TSan | 4/4 | 146.77 s |

Debug and Release cover Mapper, expression evaluation, project/editor expression
audit, MIDI allocator, full realtime/offline suites, common conformance, nested
Sounds and prepared scope authority. Both sanitizer configurations cover Mapper,
project expression audit, MIDI allocator and conformance. These are selected
checks, not a complete Workbench, device or platform recertification. At most two
heavy compilers run together; all runtime probes/tests are serial after all
compilation, with TSan last.

Incremental selected-target builds took Debug 7.68 s / Release 89.65 s / ASan/UBSan 44.03 s / TSan 24.63 s.
These are observations, not clean-build or preparation-performance budgets.

One independent read-only final review reports no P0–P3 findings in domain/audit
consistency, range validation, rounding or retained authority. The generated
capability matrix checks pass, as do all 38 coverage-report Python tests.

This accepts the explicit MIDI pitch-bound behavior in the existing profiles.
Mapper gate/velocity remain original limited mappings; arbitrary local maps,
files, scripts, expressive controllers and VoiceNumber remain open. Generic
Sound interface domains are unchanged. Exact reference half-note ties,
floating-point tuning and out-of-range behavior remain unobserved. Internal
half-note ties use the existing MIDI Voice round-away-from-zero convention;
the original unrounded 0...127 restriction remains. Supported profiles stay
94/171 realtime (55.0%) and 86/171 offline (50.3%).


## VoiceNumber specialization — accepted 2026-09-06

This accepts the bounded numeric VoiceV1 profile.
The exact `?VoiceNumber` token becomes an immutable one-based slot literal before
numeric evaluation and node-specific preparation. Every actual voice is
validated, including later slots whose seed or frequency becomes invalid. Each
independent MIDI Voice owner restarts at one. Reusable Sound definitions,
functions and exposed parameter defaults/overrides retain their authored source.

Format 36 persists `voiceV1` explicitly. All seven previous language identities
and their historical migration boundaries remain valid; new projects and the
unversioned latest-language helper retain VariablesV2. Project/editor audits
permit an unowned reusable template, but executable instances need an actual
unambiguous voice owner. The Inspector offers an explicit, single-Undo upgrade;
save/reopen, stable second serialization, Undo/Redo and fresh-stage invalidation
are covered. Ordinary non-MIDI graphs retain their numerical behavior after the
upgrade. See the [authoring guide](../help/voice-number.md).

Eleven VoiceV1 integration contexts cover ordinary non-MIDI rendering, four
root/nested oscillator cases with or without timed controls, five three-voice
static-noise placements, and independent owners with a function and shadowed
variables. Noise uses separate literal native generators for ordinals 1, 2 and
3; oscillator PCM is analytical. Irregular partitions, reset replay, note reuse,
stealing, invalid later ordinals and allocation-free callbacks are covered.
Twenty-eight forged/unsealed plans reject in each of realtime and native offline
(56 route checks), including public reconstruction combined with language
settings downgrade. The private authority check also inspects embedded expression
language, preserving specialization provenance after the marker is removed.

| Verification | Result |
| --- | --- |
| Full Debug build and serial suite | 380/381 initially; the exact checkpoint-loader test allowlist correction and expanded three-voice Mapper fixture pass the 2/2 focused rerun |
| Final ordinary-graph extension | Complete Mapper executable passes again in Debug and Release |
| Workbench GUI CTest | Pass, 266.53 s; measured full Debug coverage uses a 600 s budget |
| Release selected groups | 47/47, 35.22 s, including every Project group and related runtime/CLI/library coverage |
| ASan/UBSan selected groups | 9/9, 48.65 s |
| TSan selected groups, last native runtime run | 4/4, 48.52 s |
| Coverage tool unit tests | 47/47 using the configured bundled Python runtime |
| Generated capability and help checks | Pass; compiled catalog checks also pass and generated C++ support bytes remain unchanged |
| Retained pre-feature probe object | 28/28 old-profile admissions and all 16-sample PCM outputs remain byte-identical |
| Distinguishing mutations | Forced ordinal one fails core seed math and the three-voice literal-noise oracle; unbound resolver execution fails the inactive-branch core test |
| Independent review | One reproduced public-reconstruction/downgrade finding fixed and rechecked; final policy correction has no findings |

The initial governance failure was an existing test-only loader consumer omitted
from its exact allowlist. Its target remains inside TALAS_BUILD_TESTS; all 117
governance unit tests, including rejection of product consumers, pass afterward.
Production source hashes were retained and unchanged through final runtime
verification. Builds used at most two compiler frontends, with serial native
execution and TSan last.

General green variables, string/collection construction, resource-name formulas,
MIDI file/script/local-map behavior and the broader P0–P11 work remain open.
Profile counts stay 94/171 realtime (55.0%) and 86/171 offline (50.3%).


## Shared SMF decoding — MIDI File source foundation, 2026-09-06

Status: accepted decoder foundation. Debug 4/4, Release 4/4, ASan/UBSan 4/4
and TSan 3/3 selected CTest groups pass.
This is the first implementation stage of the MIDI File source work, not MIDI
Voice file-playback acceptance. Supported profile counts remain unchanged:
94/171 realtime Supported (55.0%) and 86/171 offline Supported (50.3%).

`StandardMidiFile.cpp` now owns the private shared decoder previously embedded in
`Runtime.cpp`. Both the existing native offline MIDI File Echo route and prepared
realtime Echo route call `parseStandardMidiFileChannelEvents`. The corrected
chunk scan respects extended headers, unknown chunks and odd payload lengths,
checks declared track counts and uses subtraction-based length validation to
avoid 32-bit header-length wraparound. Format 0 requires one track. Tempo events
use their known three-byte prefix when the declared payload contains extensions;
short/zero tempo values, invalid meta-event types and invalid preparation inputs
fail with a diagnostic. A successful retry clears an earlier error.

These changes follow the official RP-001 v1.0 Standard MIDI File
specification. The decoder retains the existing Echo
projection: channel messages only, stable track ordering for equal sample frames,
exclusive horizon, channel filtering and selected-event budget. Format-2 tracks
retain their separate tempo maps and existing Echo merge; the existing -29 SMPTE
interpretation is unchanged. End-of-track handling remains the previous lenient
projection. This package does not establish a complete SMF conformance validator,
Voice pattern selection, controller mapping or composition-clock semantics.

`talas_standard_midi_file_tests` contains thirteen focused groups. Tests cover
running status (including one-byte channel messages and cancellation), default
and changing tempo, independent format-2 timing, SMPTE rates, channel/horizon/
budget boundaries, malformed chunks and every truncated prefix of the canonical
fixture. The integration fixture has extended header/chunks/tempo and two note
channels: independently expected messages fall at samples 0, 50, 100 and 150.
Native offline and realtime output match those bytes and times. Realtime blocks
of 1, 17 and 151 samples, reset, insufficient output capacity, unchanged caller
storage on rejection and successful retry preserve the sequence.

The initial tests failed in six groups against both
the old archive and an unchanged extraction; five controls passed. The final
thirteen-group test object linked against the retained before archives fails in
eight groups, including the real offline Echo integration, while the same tests
pass against the current library. This supplies distinguishing regression
evidence rather than relying only on agreement between two shared-code routes.

Debug and Release each pass four
selected CTest groups (decoder, full offline rendering, full realtime rendering,
and prepared MIDI Mapper/Voice integration); ASan/UBSan passes the same four.
TSan passes decoder, full realtime rendering and prepared MIDI Mapper/Voice
integration as the final native verification. Exact production/test hashes
remain unchanged through all four configurations.
Coverage Python tests pass 47/47, and all four generated checks pass. An
independent read-only review found no P1/P2 findings in the production decoder
and extraction delta. No project-format, public DTO or C ABI change is made.


## Immutable MIDI Voice file source preparation — core verified 2026-09-06

The private `PreparedMidiFileVoiceSources` prepares immutable channel-message
streams outside the callback and is retained by realtime session preparation,
separately from mutable checkpoint journals. Source labels, original nested owner
identity, exact binary resource selection, selected channels and aggregate event
budgets are explicit. The data retains non-note channel messages and byte-level
note distinctions for later dispatch. It does not itself grant execution authority.

Seven focused groups cover source selection (including historical long blank
fields), immutable ownership after inputs are mutated, inactive-file arbitration,
twelve resource-identity rejections plus generation-zero compatibility, aggregate
limits across owners, malformed data/format-2 policy and invalid owner bindings.
The initially unimplemented factory fails all seven groups; the implementation
passes them. Debug 3/3 and Release 3/3 also include the full realtime regression
and existing prepared MIDI Mapper/Voice tests. ASan/UBSan 2/2 and TSan 2/2 cover
the new preparation tests and Mapper/Voice integration; TSan is the last native
verification. Code/test hashes are unchanged throughout final verification. Four
generated checks and independent review pass.

At this preparation-only stage, both compiler and runtime blank-source guards
still rejected MIDI Voice file playback. No audio playback, authoring or
composition-clock claim is made from these preparation tests. All coverage
metrics remain unchanged.


## MIDI Voice file note playback — runtime verified, 2026-09-06

`talas_midi_file_voice_playback_tests` executes compiler-sealed file instruments
through automatic offline and realtime paths. Nine groups cover an independent
sine/velocity oracle; 5 ms retrigger delay; note-off and zero-velocity Note On;
filtering, cancelled attacks, reuse and stealing; same-frame polyphony; isolated
live/file and nested two-file owners; 1/7/64-frame partitions; reset, host repair
and discontinuity; rejected public-plan forgery and preparation budgets; callback
failure/retry without allocation; file performance transfer; and offline Sound
selection preservation. Public offline validation selects the same backend.

The old-runtime instrument failed source admission. Three review regressions
were independently reproduced before correction: public offline validation
bypassed the file backend, performance alignment discarded transferred notes,
and offline reset lost retained non-voice Sound selection. Review corrections
have no remaining findings. The allocator test adds allocation-free reset,
stale-generation/rollback rejection and exhaustion boundaries.

Same-frame precedence is prepared controls/live batches, one file batch, then
pending attacks. Reset restarts the file and retires notes while preserving
allocator generation authority. Forward host repair retires file notes and
skips unrendered events; it does not synthesize historical audio or infer held
notes at a seek target. Performance transfer of an equivalent source retains
its clock and active note state. These are tested native contracts, not observed
reference measurements.

The initial profile rejects selected non-note channel messages, format-2
independent patterns, prepared Score, loops and WaitUntil composition clocks.
Script, controller routing and authoring/persistence/export are open. No atomic
or node promotion is made from this runtime-only evidence.

Final Debug 6/6 and Release 6/6 pass, including the full realtime regression and
shared SMF integration. ASan/UBSan 4/4 and final TSan 4/4 pass for playback,
allocator, immutable source preparation and Mapper/Voice. All nine focused
playback groups pass. Blank and explicit Live MIDI also ignore inactive file
fields in native offline rendering; that regression was independently reproduced
before the narrow Voice-only fix. Source hashes stayed unchanged across all
final runs. Four generated checks pass; focused independent reviews have no
remaining findings.


## MIDI Voice source authoring and export, 2026-09-06

The shared canonical source parser now drives resource discovery as well as
immutable runtime preparation. Discovery uses expanded Sound defaults and
instance overrides. Live MIDI, historical blank source and Script retain their
authored file fields but exclude inactive files from loading and watching.
Selected files retain normal missing-resource diagnostics with nested source
paths; invalid source fields fail atomically. MIDI Mapper is unchanged.

Five workflow groups cover root/nested source selection, missing inactive files,
source errors, exposed defaults and independent instance overrides, editor
Undo/Redo and exact save/reopen, project-relative immutable loading and actual
CLI PCM16 export. The export uses an independent sine/gain/note/rest oracle and
also proves Live MIDI export succeeds after its inactive MIDI file is removed.
The retained pre-change test program fails the distinguishing source-selection
and editor cases while preserving its initial CLI control.

The Workbench fixture edits the real Inspector Source field, calls the Browse
selection binding callback, exercises resource/source Undo/Redo, saves/reopens
and stages the file instrument. It measures actual rendered PCM after host
transition settles and saves a readable graph/Inspector screenshot. The native
file chooser itself is not automated. The resource picker admits MIDI files
through its existing all-files filter.

Final Debug 6/6 (6.34 s), Release 6/6 (2.60 s), ASan/UBSan 6/6
(14.10 s) and final TSan 6/6 (26.18 s) pass. The six programs cover workflow,
playback, immutable preparation, resource loading, asset watching and CLI.
The complete Workbench smoke passes in 266.41 s; its MIDI fixture screenshot is
`build/core-debug/workbench-smoke/workbench-midi-voice.png`. All 47 coverage-tool
tests and four generated checks pass. Independent final review has no P1/P2
findings. Production/test hashes stayed unchanged through final TSan. The
metadata tests were updated for the accurate file-note support description.

Supported profiles remain 94/171 realtime (55.0%) and 86/171 offline (50.3%).
Controllers, format-2 pattern selection, Script, local maps and
Score/loop/WaitUntil clock integration remain open.


## Live Score source compilation foundation, 2026-09-06

The private `PreparedScoreClipSource` boundary compiles one exact repeat's source
without rendering/prerolling its DSP or consuming audio input. The existing
materializer now uses the same boundary for source compilation. Source-instance
connections, exposed defaults/overrides, global and scoped automation, repeat
origins and content offsets retain their existing meaning. The caller still
owns activation, source-offset advancement, fades and parent transactions.
Materialization can refresh content under caller-owned schedule generation
labels; the parent compile entry point owns full document/schedule validation.

Four focused groups cover new device input after preparation (including an
upstream connected Sound instance), content-offset/repeat automation with an
independent oscillator oracle, two independently scoped instances, live controls
published after playback starts, callback allocation counts and invalid
clip/repeat/schedule/sample-rate selection. Cleared authored definitions do not
invalidate the already prepared source. The initial boundary rejects the two
new capability groups and preserves invalid-selection rejection; the completed
boundary passes all four groups.

Final Debug 5/5 (6.90 s), Release 5/5 (2.77 s) and final TSan 5/5 (50.18 s)
pass. These include source preparation, full PreparedScore, ProjectRuntimeBridge,
MIDI File authoring and CLI regression programs. ASan/UBSan passes 4/5; the
existing bridge bounded-worker-stack test aborts. Its failure is also reproduced
by a standalone root oscillator with no Score and no call to the new helper:
a 536,576-byte macOS worker stack enters the much larger instrumented
`RealtimeRenderSession::Impl::prepare` frame. This pre-existing baseline remains
an open failure, not a sanitizer pass or a disabled assertion. Debug, Release and TSan cover the same bridge test successfully.
Four generated checks and independent review pass. Exact code/test hashes remain
unchanged through final TSan.

The actual parent Score acceptance probe remains **red**:
`talas_prepared_score_clip_source_tests --parent-score-probe`. It demonstrates that
ordinary parent Score playback still uses preparation-time PCM rather than
new device samples. This is the next integration requirement; the private source
foundation does not provide live input after staging, WaitUntil or a shared
composition time. No Supported count is promoted.

No new GUI journey was run for this private preparation seam; the parent integration requires its own Workbench acceptance.


## Parent-owned source callback transaction, 2026-09-06

The internal `PreparedSourceCallback` can retain one successful tentative audio
callback until its parent commits or rolls it back. Its preallocated holder keeps
the ordinary callback's nine rollback guards alive in the same destruction order.
It includes source DSP/history, automation cursors, MIDI File readers and voices,
and the source frame cursor. Analysis and timing publication wait for commit.
A second tentative callback cannot overwrite the outstanding transaction. Parent
candidate audio remains private until every source and parent operation succeeds.
External-output and isolated participants are explicitly rejected until their own
deferred transaction integration is implemented.

Six focused groups cover rollback/retry and continuation with an independent sine
oracle; a later source's overflow after an earlier source succeeded; pending-call
rejection; analysis absent before commit and after cancellation, followed by exact
committed records; FIR/feedback history across 3/8/5/8-frame callbacks using an
independent recurrence; and MIDI note/attack/voice rollback across 17/24/23-frame
callbacks using an independent two-pitch oracle. The guarded paths count zero C++
allocations. The initial nontransactional adapter fails all three original
rollback tests. Debug passes all six focused groups and 6/6 regression programs,
including the full realtime renderer, PreparedScore, source compilation, MIDI File
playback and the runtime bridge. Four generated checks and an independent
production/analysis/history review pass without P1/P2 findings.

Final Debug and Release pass 6/6 programs (49.82 s and 6.51 s); final TSan
passes 6/6 (423.80 s) and is the last native verification. ASan/UBSan passes 5/6
(183.64 s), including the complete realtime renderer and the new source callback
program. The known `testBridgeStagesDocumentOnBoundedWorkerStack` BUS failure
remains open, as described above; no assertion is disabled or relaxed. A separate
ASan run with `detect_stack_use_after_return=1` passes all six new source callback
groups. Code/test hashes remain unchanged through final TSan.

Actual parent Score playback is not yet wired to this internal adapter. Its live
input probe remains red. Parent activation, repeat ownership, content-offset
preroll, live control/transport/stage/offline integration and common composition
time remain open. No node support count is promoted; no new Workbench journey is claimed for this private boundary.


## Live parent Score integration in progress (2026-09-06)

Ordinary Score clips now execute private per-repeat source graphs through the
parent routing. Public plan/settings reconstruction cannot provide or downgrade
that authority, including after removing both public schedule and marker. Explicit
Cached/Frozen clips still load validated artifact PCM. Realtime input remains live
after compilation; offline double input retains its exact precision and range.

Parent/source callback commits and whole offline call journals include children.
DiskPlayer participants retain published response-slot ownership until commit,
defer their refill/seek/counter publication and restore audio-owned candidate
state on abort. They do not rewind a concurrently running worker. Deterministic
offline reads borrow a decoded immutable source owned by the exclusive checkpoint;
decode and release occur outside the audio callback. Allocation failure preserves
its diagnostic category, and decode errors remain inside the public render
failure boundary.

Preparation bounds: 64 source instances, 4096 operations, 2^21 callback audio values,
1,000,000 offset frames, plus aggregate 8192 retained stream blocks/16 MiB. Connected
rate/position requires the complete source file window to fit; static starts and
post-offset positions have bounded prepared windows. These limits do not establish
exhaustive aggregate DSP memory accounting. Temporary offline file decodes use
the existing full-resource path and are outside the retained-window budget.
Sustained realtime streams can still underrun beyond retained windows. No ordinary clip is replaced by rendered PCM.

Debug, Release and final TSan each pass 4/6 programs (52.09 s, 8.82 s and 461.03 s).
The full realtime, bridge, private-source-callback and AudioStream programs pass.
TSan reports no race diagnostic and is the last native verification; source/test
hashes are unchanged through that run. ASan/UBSan passes 3/6 (201.50 s): its additional
failure is the known bounded-worker-stack bridge BUS in
RealtimeRenderSession::Impl::prepare; no additional UBSan diagnostic appears. PreparedScore has 25/27
groups and PreparedScoreClipSource has 11/12. The remaining failures are required
parent reset/seek/loop acceptance; no assertion was disabled. The existing C17
stateless Score predecessor test now authors an explicit Cached clip, retaining
its original filter/placement/allocation assertions.

Distinguishing evidence includes new device samples after compile; per-repeat
origins, offsets and whole-clip fades; exact double offline input; stripped-public-
authority rejection; later-source failure and retry for oscillator and DiskPlayer;
stream content offset 2048 beyond prewarm; connected rate 512; deterministic reads
with a stopped ManualTest worker; typed decode allocation faults; and 8192-frame
parent offline comparison at rate 2 against the independent old materializer beyond
its first prepared window. Callback paths retain allocation counters.

Review found and corrected public authority downgrade, double narrowing, stream
participant rejection, deterministic offline horizon, decode exception category
and connected-rate initial-window defects. The final focused production review
found no additional P1/P2 issue in the last two corrections. Reset, seek, loop,
parent controls/MIDI/stage/analysis wiring and common composition time remain open.
Stage 2 is not complete; no node metric is promoted.

Four generated checks pass; no new Workbench journey was performed for this increment.

### Reusable live Score source reset (2026-09-06)

Each live source captures an owned, settled reset point after its content-offset
preroll and stream-window preparation on the control thread. Parent reset restores
all source positions and the Score schedule cursor. The audio thread reuses
existing semantic DSP checkpoints and fixed-size control/history storage; it does
not replay the skipped clip, decode a resource or allocate an offline journal.
Source restoration preserves compiled automation cursors, MIDI readers, active
voices, pending attacks, mapper state and the exact prepared stream boundary.
Queued live-input read indices are retired by reset rather than resurrected from
the preparation snapshot. The reset point remains reusable after callbacks and
whole-call offline transactions.

Debug source acceptance passes 12/12 groups and private source callback acceptance
passes 7/7. Tests cover offsets 0/2/17 with automation and FIR/feedback history;
nine separate DSP kinds compared with uninterrupted rendering across changed
callback partitions; active MIDI-voice restore across release and a delayed next
attack; and parent DiskPlayer rollback, continuation and repeated resets at
offsets 0/2048, actual file EOF, and connected rate 512. The varying negative
PCM16 stream oracle distinguishes the exact source cursor. Callback paths check
allocation counts. Independent review has no concrete P1/P2 findings.

The prior parent-integration record above remains historical evidence for its
own source hashes.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 5/6 | 52.40 s |
| Release | 5/6 | 9.10 s |
| ASan/UBSan | 4/6 | 203.06 s |
| Final TSan | 5/6 | 465.08 s |

Both source programs pass in all four configurations. The remaining Score
failures are the two seek/loop acceptance groups. ASan/UBSan additionally
reproduces the existing bounded-worker-stack bridge BUS in
`RealtimeRenderSession::Impl::prepare`; its full realtime and AudioStream
programs pass, with no additional UBSan diagnostic. Final TSan reports no race
diagnostic and is the last native verification. Source/test hashes are unchanged
through it. Four generated checks and the focused whitespace check pass; no new
Workbench journey was performed.

Seek/loop/pause, parent control/stage/analysis propagation, common composition
time and the known ASan bounded-worker-stack preparation failure remain open.
No node metric is promoted by this reset increment.

### Bounded runtime preparation stack (2026-09-06)

The ASan worker-stack failure above is reproduced independently by the existing
bridge worker regression and a new live Score worker regression. The latter runs
the parent reset matrix with offsets, automation and FIR/feedback history on an
ordinary `std::thread`, including actual nested source preparation. Both failed
with an ASan BUS before the change; neither worker stack nor sanitizer
instrumentation is enlarged or disabled.

Each of the 154 operation-dispatch branches now executes in its own non-inlined
control-thread helper instantiation. A completion flag preserves original early
returns. The Cached Score and live Score branches retain their explicit skip of
per-step reset postprocessing. Dispatch conditions, compensation preparation,
authority checks and branch bodies preserve their original order and lifetime.
An inverse of the extraction reproduces the entire before source byte for byte.
Independent review has no concrete P1/P2 findings.

Fresh ASan object inspection shows `Impl::prepare` reserving 165888 bytes instead
of 999200, excluding saved registers/alignment, on the existing 536576-byte worker
stack. Both worker regression programs pass (2/2, 10.05 s). The broader ASan/UBSan
suite passes 5/6 (199.95 s), with only the two existing Score seek/loop failures;
the bridge now passes and no ASan/UBSan diagnostic is reported. Source acceptance
has 13/13 groups, including the added worker case, and private callback acceptance
has 7/7.

The expanded Debug run initially passes 83/85 programs (205.35 s). Alongside the
two known Score seek/loop groups, CompilePlan exposes a stale negative fixture:
blank/default Live MIDI with a retained, inactive file name was expected to fail.
The already accepted MIDI-file workflow deliberately ignores inactive references.
The corrected fixture asserts success for that case and rejection for active
MIDI File with either a missing resource or an empty name. The full CompilePlan
program then passes its focused rerun (1/1, 9.87 s). Independent focused review
confirms that rejection coverage is preserved. This is combined acceptance of
84/85 programs, not a claim that the initial 85-program invocation was all green.
Release passes 5/6 (9.01 s), with only the Score transport acceptance failures.
Final TSan passes 5/6 (468.77 s), with the same two Score seek/loop failures and
no race diagnostic. It is the last native verification; production and runtime
test source hashes remain unchanged through that run. Four generated checks and
the focused whitespace check pass. No new Workbench journey is claimed.

Remaining Score transport, parent controls and common composition-clock work
remain required. This fix does not promote node coverage.


### Private source composition clock (2026-09-06)

A parent-owned source callback can now hold composition time while its sample
cursor continues. Compiled automation points and linear ramps, together with
MIDI file reader deadlines, use the composition cursor. Their deadlines are
translated into sample positions inside the existing callback scheduler. The
five-millisecond MIDI retrigger attack, DSP processing, device input and live
scheduled-event deadlines retain sample time. Ordinary public realtime callbacks
retain their existing clock relationship. No seek replay, PCM replacement or
callback allocation is introduced.

The persistent composition cursor belongs to both the session value journal and
the paired callback cursor. Parent abort restores it with automation, MIDI voices
and DSP state; offline abort and reusable source reset also restore distinct
composition/sample positions. The per-call advancing flag is private and is reset
after synchronous source rendering. It does not reinterpret public host pause,
local WaitUntil nodes or the parent Score scheduler.

Fresh focused Debug acceptance passes 10/10 private callback groups. New independent
numeric checks cover step and linear automation under repeated holds, a hold on an
exact automation boundary, and continuing oscillator phase. A MIDI-file fixture
holds at composition frame 0 or 2 and later at 31; both delayed attacks occur
during a hold, while note-offs wait for composition time. Sample-by-sample oracles
match partitions 1/7/64, stereo output and abort/retry. Distinct cursor reset and
offline rollback repeat exact continuation. The existing FIR/feedback live-input
oracle now also exercises held composition time. Callback allocation counts stay
zero. Independent review has no concrete P1/P2 findings.

Consuming held-boundary events once is this native callback contract.
Composition time and sample time remain separate clocks; cursor jumps while
playing or paused are not taken to reconstruct arbitrary stateful DSP history.
Parent Score scheduling, WaitUntil gate activation/release, parent control/MIDI/
stage propagation and the two seek/loop regressions remain required integration.
No new GUI journey or Supported count is claimed.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 6/7 | 58.11 s |
| Release | 6/7 | 9.60 s |
| ASan/UBSan | 6/7 | 207.33 s |
| Final TSan | 6/7 | 471.75 s |

All four builds pass. The only failed program in each configuration contains the
two existing Score seek/loop regressions. Private callback acceptance passes all
10 groups, live Score source acceptance all 13 groups, and the MIDI file playback,
full realtime, bridge and stream programs pass. ASan/UBSan and final TSan report
no sanitizer diagnostic. TSan is the last native verification; production and
runtime test hashes remain unchanged through it. Four generated checks and
whitespace pass.


### Segmented source and parent Score transactions (2026-09-06)

A private source transaction now accepts multiple parent-directed segments while
retaining its initial nine rollback guards, pinned stream checkpoint and frozen
ingress write limits. Each segment can hold or advance composition time; DSP,
input and sample-time attacks continue. Aggregate frame capacity is bounded by
maxBlockFrames. A failed append aborts the entire transaction. Audio stays in
private candidate storage, and analysis accumulates all segments with the original
callback timestamp and publishes only on parent commit.

The existing scheduler was extracted into reusable member operations with an
explicit scheduling window. Its initial mechanical extraction was checked with a
byte-identical inverse proof. The shared frame
scheduler preserves ordinary public event ordering and is used by both the first
source segment and subsequent appends. Source input offsets and aggregate output/
analysis offsets are distinct, so continued segments do not overwrite earlier
analysis. No DSP history recapture, allocation or intermediate stream commit is
introduced by appending.

Parent Score source execution now follows scheduled spans instead of eagerly
rendering the entire callback. Each source tracks pending DSP frames separately
from composition advance. Clip activation, repeats, content offsets and fades use
composition position, while continuing source output remains shared by its channel
projections. Parent commit requires every requested frame to have rendered. Offline
checkpoints reject capture/commit during an open callback; abort closes any partial
source transaction before restoring its storage and source positions.

Focused Debug acceptance passes 15/15 private callback groups and 14/14 Score
source groups. Independent numeric oracles cover an intra-callback hold/resume,
step/linear automation, MIDI file note-offs and continuing delayed attacks, live
FIR/feedback history, repeat/fade/content-offset positions, full analysis visibility,
late segment failure and bounded-capacity rejection. Nine DSP reset families compare
appended transactions against uninterrupted output, including whole-abort retry
without an intervening reset. Later sibling failures occur after an earlier source
segment; stream cases retain offset 0/2048/actual EOF and connected-rate coverage.
The new offline lifecycle regression first failed on an abandoned partial callback
and passes after the owner explicitly aborts that callback. Allocation counts stay
zero in the exercised realtime paths. Independent review has no concrete P1/P2
findings.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 12/13 | 110.15 s |
| Release | 12/13 | 16.71 s |
| ASan/UBSan | 12/13 | 395.63 s |
| Final TSan | 12/13 | 887.17 s |

All four builds pass. The 13-program set covers private sources, parent Score,
streaming, full realtime scheduling, bridge, MIDI file playback, performance
snapshots, offline rendering, native offline checkpoints, conformance, typed events
and resource snapshots. Only the two existing Score seek/loop acceptance groups
fail; they remain required work. ASan/UBSan and final TSan report no sanitizer
diagnostic. An additional ASan stack-use-after-return run passes both source programs
(2/2, 3.80 s), exercising retained guards after the first segment returns. Native
tests run serially after all compilation; TSan is the last native verification,
with unchanged production and runtime test hashes. Four generated checks and
whitespace pass.

This increment does not yet activate/release global Timeline WaitUntil gates or
wire public parent suspension, controls, live MIDI, stage transfer and source
analysis identity. Before adding live producer routing, audit producer-visible
legacy/typed queue retirement across tentative segments; frozen write limits alone
do not provide that retirement contract. Public host pause remains distinct from
Timeline suspension. Seek/loop, aggregate DSP memory accounting and larger stream
windows remain open. There is no new GUI acceptance and no Supported-count
promotion: 94/171 realtime (55.0%), 86/171 offline (50.3%).


### Timeline WaitUntil acceptance — RED (2026-09-06)

The previous segmented transaction is unchanged in production. New default CTest
`talas_prepared_timeline_wait_until_tests` exercises the missing behavior through
public compilation and the actual parent Score. WaitUntil must hold the
composition cursor and automation while active synthesis continues, and must
delay Input activation until Resume. The existing local audio-history implementation conflicts with that second contract:
old device samples and pre-start control values can be heard after release, and
waiting unnecessarily exhausts bounded audio history.

Two positive controls pass: immediate parent release with normal automation, and
immediate local device input. Seven distinguishing requirements remain RED:

- Parent cursor holds at frame 2, with callback partitions 1 and 3; it currently
  advances to frame 3.
- ResumeOnZero and simultaneous independent gates hold composition automation;
  automation currently advances at sample 3.
- Offline rendering keeps the same held automation and continuing synthesis;
  its independent numeric oracle currently fails at sample 3.
- Local live Input can wait longer than the old history capacity and then reads
  current device samples; waiting currently exhausts the audio queue.
- A control change before Input starts affects its first oscillator samples;
  the old queued waveform has stale gain at sample 6.

The parent oracle computes composition advancement separately from oscillator
phase. It covers repeat activation, a gate releasing while another is still closed,
clip-local automation, repeated reset and allocation-free callbacks. Condition
input uses a 1 ms PeakDetector and Threshold 0.5 at 1000 Hz; the binary fixture
crosses that threshold on the first changed sample in either direction. The
immediate controls verify the same compilation, signal path and oracle without a
hold. Same-sample release is explicitly the native ordering requirement.
Independent review finds no concrete P1/P2
problem in the fixtures or CMake registration.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 2/3 | 2.07 s |
| Release | 2/3 | 0.94 s |
| ASan/UBSan | 2/3 | 6.36 s |
| Final TSan | 2/3 | 14.00 s |

All four builds pass. In each run, both existing private/source programs pass
(15/15 and 14/14 groups). The new WaitUntil program passes 2/9 groups and fails the
same seven functional requirements. ASan/UBSan and final TSan report no diagnostic;
TSan is last native verification, and production/test hashes remain unchanged
through it. This is confirmed RED acceptance, not a runtime fix. The entire project
suite was not rerun for this test-only increment. The preceding 13-program results
remain evidence for the unchanged production code; registering the new target adds
an explicit failing acceptance program to the default suite.

No Supported profile is promoted. Counts remain 94/171 realtime (55.0%) and
86/171 offline (50.3%). The shared condition/Input activation regions, parent
clock, live control/MIDI routing, seek/loop and GUI journey remain open. Four
generated checks and whitespace pass.


### Local WaitUntil dependency activation (2026-09-06)

Compiler-sealed built-in WaitUntil now delays its Input dependency region instead
of recording and replaying early audio. Resume dependencies execute while closed;
once released, the latch stays open. Shared producers execute once per sample,
inner gates do not evaluate before their outer Input starts, and named feedback
read/write pairs follow the same active ownership. Per-frame traversal scratch is
prepared in advance for realtime and uses no callback allocation. Existing whole-call
DSP/control/feedback journals include activation latches and preserve failure/retry
and reset behavior. The legacy public custom/Subset route retains its old history
implementation; exact built-in wait duration no longer consumes that history limit.

The offline renderer uses the same dependency scheduler inside its existing native
processing and rollback path. It retains decoded WAV resources, variable and Sound
assignment APIs, preflight and node/port diagnostics. Device input is indexed by
absolute sample time when it starts; stateful synthesis and file playback start at
their own initial state. Review identified two regressions in an intermediate common
backend route (WAV stream support and offline assignment APIs); preserving native
rendering fixes both, and actual WAV plus pre-start variable regressions now pass.
The repeated independent review has no new concrete P1/P2 finding.

Existing per-operation rendering bodies were mechanically extracted. Inverse
comparison proves the original realtime body and 1,887-line offline body unchanged
apart from the intentional WaitUntil and device-cursor changes. Non-Wait graphs
retain their original operation order. The previous queue-overflow assertions now
require indefinite closed silence and no queue allocation; finite-value rejection,
source diagnostics, forged authority, numeric output, partition/reset and rollback
assertions remain. Tests are not disabled to make the acceptance program green.

The expanded Timeline program passes **13/18 groups (72.2%)**, including all
**12/12 local cases**. Five actual parent clock cases remain RED: partitions 1/3,
ResumeOnZero, simultaneous gates and offline composition automation. Existing
Score seek/loop acceptance remains open. This implements local DSP activation,
not complete global Timeline gating or all local MIDI-reader/time-expression
lifecycle semantics. Sound-valued Resume completion remains unverified. No
descriptor promotion is made; counts remain 94/171 realtime (55.0%) and 86/171
offline (50.3%).

All four builds pass. The following 16-program runs include the full realtime
and offline session programs, common conformance, both catalog checks, Score,
source/private transactions, streams, MIDI voices, bridge, typed events,
performance, resource snapshots and native offline checkpoints.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 14/16 | 113.93 s |
| Release | 14/16 | 18.04 s |
| ASan/UBSan | 14/16 | 404.15 s |
| Final TSan | 14/16 | 901.72 s |

Only the existing two Score seek/loop groups and the five parent WaitUntil clock
groups fail. Both sanitizers report no diagnostic. The additional ASan
stack-use-after-return run passes 2/3 programs (4.59 s), with only the
same five parent-clock assertions failing; all local activation and both source
programs pass. Native tests run serially after compilation; final TSan is last
native verification, with unchanged runtime and compiled test hashes.

The compiler-owned descriptor limitations and generated catalog/help now describe
delayed Input activation and retain explicit global/MIDI/expression lifecycle
exclusions. Supported counts do not change. The example display title is updated
to **Wait Until Delayed Input** while preserving its filename. The live CLI catalog
matches the artifact, four generated checks and whitespace pass, and the focused
release-policy check verifies the example-title update.


### Parent Timeline WaitUntil composition clock (2026-09-06)

The parent live Score now resolves one composition decision after all active
sources and parent DSP successfully render a sample. Reached WaitUntil gates in
the clip's primary Sound owner can hold that clock. Resume conditions, live input
and synthesis run once on sample time; Score event boundaries, source automation,
MIDI File readers and the published composition cursor follow the shared held
clock. Each repeat retains its own latch. Simultaneous gates release independently.
The deferred decision stays inside the existing source/parent transaction, so a
later failure restores gate latches, source DSP and parent/child composition
positions together. Normal host snapshots do not overwrite a held composition
cursor with the progressing sample position.

Compiler scope identity binds primary owners by definition, instance path and
structural/processing ownership. MIDI processing clones with the same path do not
become extra primary owners. A nested Sound's local WaitUntil remains local even
when another primary gate enables the global clock. This owner rule and the
same-sample advance after Resume are explicit native semantics.

Explicit Cached/Frozen clips retain validated immutable PCM. Composition time
starts and stops the clip; once active, playback advances on sample time through
a hold, stops at EOF and does not repeat the held cursor's sample. Its playback
position and started flag are covered by the callback checkpoint, whole offline
value snapshot and transport reset. A cached clip whose start lies beyond a held
cursor remains silent until composition reaches it. Ordinary live clips continue
to execute their programs and do not acquire implicit PCM caches.

The Timeline acceptance program passes **24/24 groups (100%)**, replacing all five
previous parent RED cases and retaining all twelve local activation groups. New
cases cover normal host snapshots, late parent failure after repeated holds, an
actual MIDI File Voice whose note-off waits while synthesis and delayed attack
continue, primary-versus-nested ownership, Cached/Frozen playback, delayed cache
start, EOF, callback failure/retry/reset and rollback of multiple successful
callbacks under one offline checkpoint. Realtime callbacks assert no allocation.
Initial cache test diagnostics exposed a PCM quantization expectation mismatch;
the final timing oracle uses the compiler's authoritative decoded PCM. The
corrected RED reproducer then fails at sample 3 by repeating the held sample.
Independent parent integration review and focused cache/owner re-review report
no concrete P1/P2 findings.

All four builds pass. The same 16-program integration set covers Score, live
source/private callbacks, streams, full realtime/offline programs, bridge, MIDI
File voices, typed events, performance, resource snapshots, native offline
checkpoints, common conformance and both catalog programs.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 15/16 | 113.09 s |
| Release | 15/16 | 17.32 s |
| ASan/UBSan | 15/16 | 406.05 s |
| Final TSan | 15/16 | 904.88 s |

Each configuration passes **15/16 programs (93.8%)**, up from 14/16 at the local
activation boundary. Only the two existing Score seek/loop assertions fail; all
Timeline groups pass. Both sanitizers report no diagnostic. Additional ASan
stack-use-after-return passes **3/3 programs** in 5.90 s. Tests run
serially after all compilation, with at most two compiler frontends during
builds. Final TSan is the last native verification; all 157 runtime, compiled-test
and related source hashes remain unchanged. CLI catalog equality, four generated
checks, focused release example-title policy and whitespace pass.


Remaining work includes the existing two Score seek/loop failures, host
pause/discontinuity/forward repair, parent live control/stage/analysis integration,
local MIDI-reader/time-expression lifecycle inside a delayed Input, Sound-valued
Resume completion, other Timeline time operators and GUI journeys. This increment
does not complete the Timeline. Supported profiles are unchanged: realtime 94/171
(55.0%), offline 86/171 (50.3%).


### Prepared live Score transport points (2026-09-06)

Transport reconstruction now has a control-thread preparation boundary and
bounded realtime adoption. The initial post-offset source point is retained;
an independently captured target restores DSP histories, feedback, automation,
MIDI readers/voices and stream cursors together. Every point belongs to an exact
source identity token, so it cannot be applied to an identical graph or a new
object occupying the retired owner's address. Capture and restore reject an
outstanding callback decision. Stream readiness and generation headroom are
checked for all affected sources before realtime state changes.

`RealtimeRenderSession::prepareTransportFrameBeforeFirstRender` prepares one
target on an unpublished, unrendered live Score owner. It reconstructs each
active iteration from the existing post-offset point, using immutable supplied
input history (silence when absent) and unheld composition traversal. Successful
source journals remain open until every source candidate is ready. Failure
restores earlier source state, retained stream windows/cursor identities and the
shared preparation budget. The audio-thread transport setter adopts the exact
prepared target with no allocation, decoding or seek-distance-dependent replay.
Reset still returns to the initial point; the prepared target remains reusable.

Preparation is bounded to one additional target per owner, one million aggregate
preroll frames, and the existing shared 8192-block/16 MiB stream-window budget.
Existing five-cursor stream identity limits also apply. A different target needs
a fresh standby owner. This explicit native policy cannot reconstruct an
unrecorded live performance or infer elapsed synthesis time from a composition
cursor that previously waited.

Nine new acceptance groups pass (100%):

- Three private-source groups cover multiple composition/sample positions,
  repeated restoration, rollback/retry, pending decisions, retired-owner address
  reuse, MIDI future events, active voices and delayed attacks.
- Five actual-parent groups cover repeated clip origins and offsets, changing
  live input after preparation, streamed/connected-rate/EOF reads, recursive
  FIR/feedback history, later sibling failure, failed preparation and work limits.
  Five failed preparations followed by a valid one specifically check that stream
  cursor/window capacity is returned on failure.
- One Timeline group jumps beyond a WaitUntil's right edge, checks independent
  oscillator phase and automation, and reuses the point on host discontinuities.

The focused programs pass **19/19 live-source**, **18/18 private-source** and
**25/25 Timeline** groups. Numeric oracles, callback allocation counters, later
failure/retry and repeated reset/adoption cover the new execution paths.
Independent final review has no concrete P1/P2 finding in the source-point,
preparation transaction or adoption boundary.

All four builds pass. The same 16-program integration set covers Score,
live/private source callbacks, streams, full realtime/offline programs, bridge,
MIDI File voices, typed events, performance, resource snapshots, native offline
checkpoints, common conformance and both catalog programs.

| Fresh configuration | Programs passing | Duration |
| --- | --- | --- |
| Debug | 15/16 | 112.11 s |
| Release | 15/16 | 17.46 s |
| ASan/UBSan | 15/16 | 406.17 s |
| Final TSan | 15/16 | 909.29 s |

Each configuration passes **15/16 programs (93.8%)**. Only the two existing Score
seek/loop assertions fail; no additional integration program regresses. Both
sanitizers report no diagnostic. Additional ASan stack-use-after-return passes
**3/3 programs** in 7.40 s. Native tests run serially after all
compilation, with at most two compiler frontends during builds. Final TSan is the
last native verification; all 157 tracked runtime,
compiled-test and related source hashes remain unchanged. CLI catalog equality,
four generated checks, focused release example-title policy and whitespace pass.


This is a verified preparation primitive, not complete public seek or loop
integration. The two existing Score transport assertions remain required and
fail unchanged. Engine/bridge preparation and publication, stage/authority
epochs, cancellation, loop-entry restoration and full host transport acceptance
remain open. So do delayed Input MIDI/expression lifecycle, Sound-valued Resume,
other Timeline time operators, parent live controls/stage/analysis publication
and GUI acceptance. No Supported profile is promoted; counts remain 55.0%
realtime Supported and 50.3% offline Supported.

## QuadOscillator output correction (2026-09-27)

QuadOscillator's output is the sum of a sine-modulated left Envelope and a
cosine-modulated right Envelope. `audioOut` now emits that mono sum; the retained
`rightOut` mirrors it for compatibility. The previous tests asserting separate
product channels encoded an incorrect local assumption.

`testRealtimeQuadOscillatorCancelsTheOppositeSideband` uses independent cos/sin
Envelope channels and compares the output to sin at the sum frequency. Positive
and negative carrier frequency select +11 Hz and -5 Hz from a 3 Hz Envelope and
+/-8 Hz carrier. This tests actual cancellation without an extra downstream mixer.
`testCheckedQuadOscillatorMathRejectsBeforeCommit` adds finite products whose sum
overflows. The common conformance fixture requires audible, equal output channels.
Phase initialization, aliasing, exact PCM and packed stereo Sound representation
remain unverified.
