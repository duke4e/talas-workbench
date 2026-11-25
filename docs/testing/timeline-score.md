# Timeline and Score Testing Contract

This document is the operator-facing authority map for C8. It distinguishes a
persisted or prepared field from a behavior that is actually consumed by
offline and prepared realtime playback. Historical implementation notes are not
acceptance evidence unless the current shared source and a non-vacuous
regression still prove the behavior.

## Source authority

| Contract | Shared authority | Primary regression | Current disposition |
|---|---|---|---|
| Score counts, IDs, time values, routing, overlap, automation, and marker validation | `include/talas/project/ProjectScore.h`, `src/project/ProjectScore.cpp` | `talas_project_score_tests` | Implemented |
| One-Undo Score editing and referenced-Sound deletion guard | `include/talas/project/ProjectEditor.h`, `src/project/ProjectEditor.cpp` | `talas_project_editor_tests`, `talas_project_score_tests` | Implemented |
| Format-21 Score introduction and closed JSON | `src/project/ProjectJson.cpp` | `talas_project_json_property_tests`, `talas_project_document_tests`, `talas_project_migration_compatibility_tests` | Implemented through current format 37; formats 1-32 migrate without invented source-instance or clip-artifact state, while format 33 remains the exact C8 artifact boundary |
| Seconds and musical-tick conversion | `include/talas/project/ProjectMusicalTime.h`, `src/project/ProjectMusicalTime.cpp`, `src/project/ProjectScore.cpp` | `talas_project_musical_time_tests`, `talas_project_score_tests` | Implemented |
| Immutable schedule and binding | `include/talas/runtime/PreparedScore.h`, `src/runtime/PreparedScore.cpp` | `talas_prepared_score_tests` | Implemented through exact source-instance and immutable materialized-audio bindings |
| Offline/prepared graph projection | `src/runtime/ProjectRuntimeBridge.cpp`, `src/runtime/Runtime.cpp`, `src/runtime/ScoreClipMaterialization.cpp` | `talas_prepared_score_tests`, `talas_project_runtime_bridge_tests`, `talas_offline_render_tests` | Exact content offset, repeat restart, fades, scoped automation, routing, seek, and callback partitions are consumed by one prepared Score schedule; ordinary Sounds stay live, while opted-in MemoryWriter sharing clips use stage-owned audio |
| Track MemoryWriter sharing | `include/talas/project/ProjectScore.h`, `src/runtime/Runtime.cpp`, `src/runtime/ScoreClipMaterialization.cpp` | `talas_project_document_tests`, `talas_prepared_score_tests`, `talas_offline_render_tests` | Format 37 opts in per track. A completed mono recording can feed a later non-overlapping Sample or SampleCloud clip; other tracks and private Sounds remain isolated. Public playback uses prepared clip audio; mutable realtime named memory is still open. |
| MIDI Voice literal Script source | `src/runtime/PreparedMidiFileVoiceSources.cpp`, `src/runtime/RealtimeRenderSession.cpp` | `talas_prepared_midi_file_voice_sources_tests`, `talas_midi_file_voice_playback_tests`, `talas_midi_file_source_workflow_tests`, `talas_prepared_timeline_wait_until_tests` | Bounded `self keyDownAt:` notes retain fractional pitch, octave names with `sharp`/`flat`, and exact velocity offline and in prepared realtime. Literal controller set/slide statements and a bounded integer note loop are prepared; a parent WaitUntil holds later events on the composition clock. General Smalltalk, unrestricted EventCollections, and complex loops remain open. |
| Controller recording | `include/talas/project/ControllerRecording.h`, `src/project/ControllerRecording.cpp`, `src/runtime/ProjectRuntimeBridge.cpp` | `talas_controller_recording_tests`, `talas_project_runtime_bridge_tests`, native Timeline smoke | Implemented and previously exact-reviewed |
| Parent Timeline holds and transport loops | `src/runtime/PreparedLiveScoreSources.cpp`, `src/runtime/RealtimeRenderSession.cpp` | `talas_prepared_timeline_wait_until_tests::testParentTimelineLoop`, `testParentCachedPlayback`, `testParentLoopScheduledVoiceSources`, `testParentLoopNestedSources` | Composition-driven wrapping, both Resume polarities, simultaneous gates, oscillator phase, clip automation, Cached/Frozen PCM and 105 MIDI/Analog source/entry combinations match bounded offline/realtime oracles. Nested fixtures verify child clock origins, overlapping parents, parent reuse, scoped controller reset, live input, persistence, public seeks and callback/whole-call rollback. Broader concurrent control/voice histories remain open. |
| MIDI sources in public Score loops | `src/runtime/PreparedScoreClipSource.cpp`, `src/runtime/PreparedLiveScoreSources.cpp`, `src/runtime/RealtimeRenderSession.cpp` | `talas_prepared_score_clip_source_tests::testPublicScoreLoopRestoresMidiSourceClocks`, `talas_prepared_timeline_wait_until_tests::testParentLoopScheduledVoiceSources` | Per-voice WaitUntil and parent Timeline holds have separate loop/entry fixtures for File/Script Voice and Mapper sources. Analytic offline/realtime samples, reset, persistence and failure retry after wraps are verified. Standalone source loops remain open. |
| Live VCA gain expressions | `src/runtime/Runtime.cpp`, `src/runtime/RealtimeRenderSession.cpp` | `talas_prepared_timeline_wait_until_tests::testLiveVcaGainExpression`, `testParentTimeExpressionContinuesDuringWait`, `testParentLoopNestedAnalogSources` | Unconnected gain uses prepared scalar bindings; connected gain keeps precedence. EventValue changes, scoped voice keys and bounded millisecond ramps have numeric offline/realtime evidence, including parent holds, nested loops and reset. |
| Timeline projection, viewport, command truth, focus, and accessibility | `include/talas/app/TimelineScoreProjection.h`, `src/app/TimelineScoreProjection.cpp`, `src/app/workbench/WorkbenchTimelinePanel.cpp` | `talas_timeline_score_projection_tests`, `talas_timeline_score_large_fixture_tests`, native Workbench smoke | Implemented, including source availability and Live/Cached/Stale/Frozen/Missing/Invalid/operation states |
| Export and output-take publication | `src/audio/ProjectWavExporter.cpp`, `src/audio/RealtimeOutputTake.cpp`, `src/app/workbench/WorkbenchProjectLifecycle.cpp` | `talas_project_wav_exporter_tests`, `talas_realtime_output_take_tests`, native Workbench smoke | Implemented, but not a clip cache/freeze authority |
| Clip materialization, cache, and freeze | `include/talas/runtime/ScoreClipMaterialization.h`, `src/runtime/ScoreClipMaterialization.cpp`, `include/talas/runtime/ScoreClipArtifactPublication.h`, `src/runtime/ScoreClipArtifactPublication.cpp`, `src/app/workbench/WorkbenchScoreClipArtifacts.cpp` | `talas_prepared_score_tests`, `talas_project_document_tests`, `talas_project_editor_tests`, native Workbench/Timeline smoke | Implemented and cumulatively accepted locally on implementation commit `b1082d06` |

## Nested scheduled Input sources

`testNestedScheduledInputPlaybackLifecycle` covers twelve combinations of
Analog/File/Script parents and File/Script MIDI Voice or MIDIMapper children.
Child clocks begin when the parent's Input starts, continue after Release and
reset on parent reuse. Controller changes stay with the child source's Input
voice slots. Independent numeric offline/realtime audio checks overlap,
save/reopen, callback partitions, reset, compatible transfer and late-failure
rollback/retry. Measured callbacks and retries allocate/deallocate nothing.

The lifecycle fixture also checks 72 compatible performance-transfer cases:
twelve source combinations at six boundaries, covering pending parent/child
attacks, a due/consumed CC, overlapping active children and parent reuse. A direct
VCA probe has independent offline/suffix expectations so source-state transfer
does not depend on a fresh output interpolator. Transfer, frame alignment and
subsequent seven-frame callbacks allocate/deallocate nothing. All 102 playback
groups pass. This is not evidence for arbitrary DSP-history migration.

`testNestedScheduledSiblingControllerScopes` adds six two-child/two-slot cases.
Different source deadlines and controller values prove sibling/global isolation;
local ranged Maps read their source and deeper explicit Sound values retain
precedence. These are local candidate timing/lifetime rules.
Live child MIDI routing and general nested source Sound crossings remain open.

`NestedSoundRuntimeTests.cpp::testNestedMidiVoiceSoundSelectionIsPerVoiceAndTransactional`
uses a Script parent with two slots, each owning one Script child. Exact child
selector identities retain their parent ancestry. Constant 0/0.5 candidates prove
independent selection and a combined value of 1.0. A valid assignment followed by
a stale-generation assignment rejects the whole callback and preserves both
selections. Complete callback rollback retains the invalid queue, so an unchanged
retry rejects again; transport reset cancels it and reactivation advances the
generations. Parent slot reuse resets only that child's selection and generation,
leaving its sibling at 0.5. Sibling cell/scope substitutions reject both realtime
and offline admission. The selector accepts its compiler-owned voice or structural
EventValue scope; full plan and ordered candidate authority remain required.
The same shared fixture now covers two further placements through
`testNestedMidiVoiceSoundSelectionUsesProjectedSoundCell` and
`testNestedMidiVoiceSoundSelectionUsesNearestSoundAncestor`. The former declares
the selection in the Input Sound; the latter reads its enclosing Sound's
declaration from a further child Sound. Assertions distinguish the physical
voice, projected declaration and projected reader scopes, and preserve the full
parent/child ancestry. Runtime admission resolves the nearest visible cell from
that compiler-owned lexical scope, while reset stays attached to the physical
voice. All three placements pass the lifecycle checks above and reject sibling
cell/scope substitutions in both admission paths. An initially audible variant
also checks independent 32-frame offline stereo output: 0 until the first parent
Input starts at frame 5, 0.5 until the second starts at 15, then 1.0.

`testParentLoopNestedSources` also verifies these scheduled children under public
Score Timeline holds and loops. Thirty-six new combinations cover both sources,
both Voice/Mapper child owners, three parent kinds and three loop entries.
The independent 96-frame oracle checks local CC deadlines, delayed attacks,
parent cursors, an enclosing CC monitor and current device input. Save/reopen,
offline rendering, 1/7/32-frame callbacks, reset, public interior/wrapped seeks,
callback failure after wraps and whole-call abort/retry pass without callback
heap activity. The VCA gain probe reads controls directly; ValToSound-based
probes have a separate interpolation delay at 1000 Hz.

The same helper's overlapping-parent mode adds 45 combinations with parent notes
at 0/8/16 and two slots. Notes use slots 0/1/0; the last cancels the first child's
pending delayed Attack while the other child retains its clock and controller.
For scheduled children, one CC deadline coincides with that reset; a second CC
reveals the reused slot's initial value before its next scheduled assignment.
Non-vacuity assertions require audible child overlap and an observable reset
default. The full persistence/offline/partition/reset/rollback/public-seek checks
above pass for this matrix as well. All **48/48 Timeline groups pass**.

## Nested Analog live controls

`checkNestedLiveRateAndRange` covers twelve Analog/File/Script-parent combinations
with direct/connected Rate and optional GeneratedIndex. Parent velocity gives each
child its own Rate multiplier and range offset. Independent 128-frame pulses
verify global speed/sign and range changes, held-stage continuity, clamping,
wrapping and parent reuse. Save/reopen, offline audio, 1/7/64-frame callbacks,
reset, 36 compatible transfers and failure/retry pass without measured callback
allocation or deallocation. These standalone fixtures do not claim every Timeline
or feedback combination.

GeneratedIndex preparation creates a writable scalar in the containing parent
Input scope when inheriting an outer mutable target. Explicit local bindings keep
precedence; read-only and mistyped targets reject. A regression first caught one
parent copy overwriting another's stage index; the corrected twelve-case matrix
and all 102 playback groups pass.

`testGeneratedIndexInCopiedAmplitudeSource` covers an amplitude sequencer inside
each of two MIDI Input copies. An explicit KeyDown Gate starts their clocks at
frames 5 and 9; ordinary Input DSP continues before Attack and between notes.
A downstream VCA reads each just-published index, giving independent 0.25/1.5
stage expectations. Preparation assigns the output and reader to their exact
voice environment while preserving the enclosing index. Save/reopen, offline
stereo, 1/7/32-frame callbacks, reset and late failure/retry pass without measured
callback allocation or deallocation. This also catches a previous admission
failure caused by an amplitude clone retaining its canonical EventValue scope.

`checkNestedProjectedAmplitudeSource` adds four File/Script-parent and optional
GeneratedIndex combinations below a true nested Script child. The amplitude node
and reader live in a reusable Input Sound; assertions distinguish each physical
voice scope from its projected Sound scope and from the sibling copy. Independent
mutable index defaults and a keyboard-derived Sound-local default remain scoped.
A 96-frame stereo oracle covers overlapping eight-sample stages, two five-sample
Attack delays and selective parent reuse. Persistence, offline rendering,
1/7/32-frame callbacks, reset and late controller-triggered failure/retry pass
without measured callback allocations/deallocations. Forged sibling EventValue
scopes reject both execution paths. All 104 playback groups pass. Arbitrary
amplitude DSP-phase transfer is not established here.

`checkNestedProjectedQuadOscillator` covers four File/Script-parent and
direct/connected-frequency cases below a Script child Input Sound. The output is
the documented sum of independent sine/cosine Envelope products; rightOut mirrors
that mono sum. A 96-frame analytic oracle tracks local frequency and envelope
projections, global direction changes, overlapping child notes, parent Release
and reuse, and the zero-frequency hold caused by the pending KeyDown=-1 marker.
Ordinary DSP phase continues before Attack and across reuse. The fixture exercises
persistence, offline audio, unequal callbacks, reset, late controller-triggered
failure/retry, global isolation and rejection of sibling EventValue/parameter
scopes. It does not establish arbitrary DSP-state transfer.

## Prepared source analysis transactions

`testLoopSourceOfflineAnalysisPublicationOrder` in
`tests/runtime/PreparedScoreClipSourceTests.cpp` exercises the private source
manager across three rendered segments and two reused loop owners. Its eight
variants cover both source commit orders, 2/8-record queues, an independent
parent probe or no parent capture, relocation, source/parent positions,
overflow and whole-call abort/retry. Callback rendering and relocation perform
no allocation or deallocation.

Source and parent analysis records share a whole-call publication journal.
Collecting records preserves render order, and committing a source alone cannot
publish them. A failed whole call leaves queue, sequence and drop evidence intact.
`testPublicScoreSourceAnalysisTargets` additionally observes public source
captures during an overlapping offline Score. Authored root-instance targets
are routed only to matching isolated sources. Optional `scoreClipId` and
`scoreIterationOrdinal` select a clip and repeat; their instance path is relative
to the clip's Sound. Every requested target must resolve to an audible live
source. Unknown clips, repeats and nodes reject; standalone graphs reject Score
selectors. Original targets remain in parent settings and prepared fingerprints.

`PreparedRuntimeGraph::scoreSourceAnalysisCaptures()` and the bridge's
`latestScoreSourceAnalysisCaptures()` expose separate captures with clip, repeat,
Sound and original instance identity. Record positions remain source-local.
Loop standby owners share their corresponding source capture, and binding visits
each capture once per stage. `testPublicScoreSourceAnalysisLoopAndSeek` checks
new device input, source-local record positions, inactive repeats, stage identity,
silent seek preparation and allocation-free callback/seek adoption.

Workbench's Analysis source selector chooses one exact clip/repeat while retaining
the existing pinned trace controls. Switching clears prior-source evidence;
refresh preserves selection, and replacement retains the clip/repeat key with
new-stage evidence. `talas_workbench_score_analysis_tests` verifies real rendered
records and source selection at 640/320px panel widths. The existing root graph
capture accessor continues to expose only the parent graph.

Replacement Score stages now prepare source history even without transport loops.
The control thread rejects a non-looping replacement if transport moves during
history preparation. `testPublicScoreReplacementPreparesActiveRepeat` covers
content offset, repeat phase and new device input at a stable replacement boundary.
`testPublicScoreReplacementRejectsMissedBoundary` freezes the incumbent callback
before publishing a candidate, so transport advances after the preparation check.
At the next boundary, the host rejects source history prepared for another exact
non-looping frame. The incumbent retains its cursor and audio; the candidate is
retired on control, and the pending slot is available for an explicit retry.
`PreparedGraphStatusSnapshot::activationRejected` and stage rejection telemetry
report that outcome. Rejected captures are not advertised as current analysis.
Workbench clears that evidence and offers Retry without changing playback,
including when external MIDI Clock owns transport. No automatic live-stage
retry or catch-up guarantee is claimed.

The regression checks independent content-offset/repeat audio, fresh input,
silent rejected captures, status/telemetry, successful retry, paused transport and
zero callback allocations/deallocations. `testMissedPreparedStageRetiresWithoutBlockingRetry`
also covers normal and transition candidate retirement, separate control/audio
rejection counts and host reset. Loop candidates keep their existing phase gate.
UI authoring of new clip-relative traces in implicit
source definitions, automatic nested display selection and Cached/Frozen source
probes are also still open.

## Persisted Score bounds

The portable model owns these fixed limits in `ProjectScore.h`:

- 256 tracks;
- 4,096 clips;
- 2,048 automation lanes;
- 65,536 aggregate automation points;
- 4,096 markers;
- 128-byte stable identifiers;
- 256-byte display and marker text;
- one to eight channels;
- one to 1,024 clip repetitions;
- 8,192 entities in one edit batch.

`projectScoreValidationError` is the semantic authority for accepted in-memory
state. JSON parsing must reject malformed counts and strings before unbounded
copy/traversal, then run the same semantic authority. ProjectEditor candidate
operations must bound raw inputs before copying and validate the complete
candidate before history or document publication.

The format-33 extension adds one nullable 128-byte exact root Sound-instance ID
and at most eight distinct channel asset IDs per clip. Each artifact has a
canonical 64-byte lowercase SHA-256 source fingerprint, a nonzero exact
integral sample rate, a nonzero exact frame count, and one verified mono
PCM16-WAV Audio asset per track channel. JSON rejects these strings and channel
counts at parse limits before semantic copying; `projectScoreValidationError`,
`validateProjectScoreSourceBindings`, and
`validateProjectScoreRenderArtifacts` close the accepted in-memory model.

## Time and scheduling contract

`ScoreTime` stores either finite non-negative seconds or an unsigned musical
tick. Ruler selection is presentation only. Conversion to sample frames uses
the accepted sample rate and the persisted tempo/time-signature map; no display
mode rewrites canonical intent.

`PreparedScoreSchedule` owns immutable tracks, clips, clip iterations, globally
ordered events, automation points, and markers. Its `PreparedScoreBinding`
binds document, stage, sample-rate, tempo, routing, asset, parameter, and source
generations. A schedule with a mismatched binding must be rejected before graph
publication.

For a selected MIDI Voice `source` of `Script`, the bounded literal form is
`self keyDownAt: 10 ms duration: 20 ms frequency: 445 hz velocity: 0.7.`
Multiple statements may be used. `s` and `ms` are accepted for times; `beat`
and `beats` require an explicit `MM := 120.` declaration. Frequencies use `hz`
or `nn`. Unsupported Smalltalk syntax fails at preparation instead of running
in the audio callback.

Prepared metadata alone is not proof of audible behavior. The current shared
materializer additionally proves with non-periodic audio that:

- `contentOffset` selects the intended source frame;
- every repeat starts from the same accepted reset/source position;
- tempo-crossing iterations land on the exact prepared frame boundaries;
- fades and relevant automation are applied exactly once;
- offline and irregular callback partitions produce the same samples.

## Routing and overlap

Every valid Score contains exactly one Master. Track and Submix routes form an
acyclic closed graph with exact channel counts. Mute/solo resolution is derived
by `resolveScoreTrackAudibility`. `Reject`, `Layer`, and `Replace` overlap modes
are validated and prepared deterministically; Replace uses document order only
where the model explicitly defines it.

Graph projection may construct transient nodes, but it may not mutate the
portable Score or invent persisted identities. Generator-authored Score output
enters only through the same bounded `ProjectEditor` candidate/Undo boundary.

## Automation and controller recording

Automation scope is exact: clip scope owns a clip ID, track scope owns a
non-Master track ID, and master scope owns the Master. A target/domain pair may
have only one lane. Point positions are strictly increasing in the lane's
canonical domain and are converted against the owner/time-map authority.

Controller recording retains callback frame, source, route, take generation,
stage generation, domain, and clip punch range. UI coalescing never replaces
raw accepted timestamps. Preview and approval share one eligibility predicate;
Cancelled, SourceLost, StageChanged, stale-stage, empty, or otherwise rejected
takes cannot be approved. Approval is one Score edit transaction.

## Clip cache/freeze contract

Live, cached, and frozen playback share `ScoreClipMaterialization`; artifact
files use one private multi-channel staging directory and an atomic no-replace
directory adoption; ProjectEditor then publishes every manifest plus the clip
record as one generation-and-byte-bound Undo transaction. Cached source
mismatch renders live and remains visibly stale. Frozen verified bytes remain
authoritative even if the live source changes or becomes unavailable. Missing,
mutated, wrong-rate, wrong-length, or wrong-channel bytes fail closed.

C8 is complete and independently accepted locally. Existing project export,
package, output-take, generic prepared cache, and graph staging are supporting
infrastructure, not substitutes for this clip authority.

## Workbench truth

Timeline labels, enabled actions, context menus, keyboard dispatch, pointer
gestures, accessibility nodes, Context Help, operation progress, and failure
diagnostics must consume the same projection/action DTOs. A command shown as
available must be accepted for the same identity and authority, not merely fail
later in ProjectEditor. Virtualized item identity never uses vector index as
the durable key.

Cache/freeze adds explicit Live, Rendering, Cached, Cache stale, Frozen,
Artifact missing/invalid, Cancelling, Cancelled, and Failed states. Neither a
stale cache nor changed source under a frozen clip may be hidden behind a
generic “ready” label.

## Large-score and realtime evidence

The product-valid large fixture contains 128 ordinary tracks plus Master,
2,048 clips, 128 automation lanes with 4,096 points, and 1,024 markers.
Viewport tests require positive visible clip/automation/marker results and
candidate counts; zero-result timing is vacuous. Projection, hit testing,
selection, focus, and ordinary nonstructural refresh must remain candidate- or
changed-entity-bounded rather than scanning the full Score at interaction rate.

The prepared audio callback performs no allocation, file access, hashing,
locking, waiting, graph construction, or UI work. Cache/freeze materialization
and publication run off-callback with bounded progress and cancellation.

## Local acceptance commands

The final C8 candidate must freshly run, on the exact reviewed commit:

- focused Score, musical-time, controller-recording, prepared-score,
  projection, runtime-bridge, offline-render, WAV-export, output-take,
  generator-output, migration, and large-fixture tests;
- ASan/UBSan for portable model/projection/materialization and TSan for
  recording/publication seams where supported;
- Debug Workbench rebuild plus native smoke;
- generated, governance, release, and protected-path checks;
- the complete serial Debug suite;
- a physical tests-off Release build, catalog/help checks, and Workbench smoke;
- `git diff --check`;
- one independent exact-commit review with no remaining P0-P3.

GitHub Actions, remotes, hosted CI, publication, notarization, and production
signing remain deferred and do not count as local acceptance evidence.

## C8 completion evidence

The implementation candidate `b1082d0640eed595c520ce3aff957dd9d649e981`
was independently reviewed with no remaining P0-P3 findings. Fresh local gates
on 2026-08-16 proved:

- the complete serial Debug suite: 301/301;
- focused ASan/UBSan boundaries: 7/7, and TSan concurrency seams: 4/4;
- Debug native Workbench smoke;
- the product-valid large Score fixture containing 2,048 clips, with a bounded
  render horizon and positive viewport candidates;
- generated, governance, and release checks: 3/3;
- a tests-off Release CLI/Workbench build, catalog/help queries, and physical
  Workbench smoke;
- clean cumulative diff checks.

These are local acceptance results only. Hosted CI, remote publication,
notarization, production signing, and C9 are not implied.
