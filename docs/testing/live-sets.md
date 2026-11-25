# Live Set and Bounded Tool Testing Contract

This document is the operator-facing authority map for C9. A persisted field,
prepared record, UI label, or green timing test is not acceptance evidence by
itself. Current source authority and a non-vacuous regression must prove each
behavior.

## Source authority

| Contract | Shared authority | Primary regression | Current disposition |
|---|---|---|---|
| Model bounds, IDs, target references, complete-set mapping conflicts, and Tool validation | `include/talas/project/ProjectLiveSets.h`, `src/project/ProjectLiveSets.cpp` | `talas_project_live_sets_tests` | Implemented |
| One-Undo structural editing and failure-atomic candidate publication | `include/talas/project/ProjectEditor.h`, `src/project/ProjectEditor.cpp` | `talas_project_editor_tests`, `talas_project_live_sets_tests` | Implemented |
| Format-34 closed JSON and formats 1–33 empty migration | `src/project/ProjectJson.cpp` | `talas_project_json_tests`, `talas_project_document_tests`, `talas_project_migration_compatibility_tests` | Implemented |
| Immutable cell preparation and exact quantization | `include/talas/runtime/PreparedLiveSet.h`, `src/runtime/PreparedLiveSet.cpp` | `talas_prepared_live_set_tests` | Implemented |
| Fixed-attempt ingress, authority matching, coalescing, and scheduling | `include/talas/runtime/RealtimeLiveSetIngress.h`, `src/runtime/RealtimeLiveSetIngress.cpp` | `talas_realtime_live_set_ingress_tests` | Implemented |
| Prepared Tool compilation, fixed runtime storage, guards, rollback, and closed commands | `include/talas/runtime/PreparedLiveTools.h`, `src/runtime/PreparedLiveTools.cpp` | `talas_prepared_live_tools_tests` | Implemented |
| Tool trigger ingress and source/route authority | `include/talas/runtime/RealtimeLiveToolIngress.h`, `src/runtime/RealtimeLiveToolIngress.cpp` | `talas_realtime_live_tool_ingress_tests` | Implemented |
| Stage binding, callback activation, crossfade, Score-region lifetime, Stop, exact failure telemetry, and Tool restore acknowledgment | `include/talas/runtime/ProjectRuntimeBridge.h`, `src/runtime/ProjectRuntimeBridge.cpp`, `src/runtime/RealtimeAudioEngine.cpp`, `src/runtime/RealtimeGraphHost.cpp` | `talas_project_runtime_bridge_tests`, native Workbench smoke | Implemented |
| Shared UI availability/dispatch for keyboard, pointer, MIDI, OSC, and Tools | `include/talas/app/LiveGridWorkbenchModel.h`, `src/app/LiveGridWorkbenchModel.cpp`, `src/app/workbench/WorkbenchSnapshotPanel.cpp` | `talas_live_grid_workbench_model_tests`, native Workbench smoke | Implemented |
| WorkspaceSession schema 13 Tool state and definition fingerprint | `include/talas/project/WorkspaceSessionState.h`, `src/project/WorkspaceSessionState.cpp` | `talas_workspace_session_state_tests`, native Workbench smoke | Implemented |
| Product maximum and positive execution | `tests/app/LiveSetLargeFixtureTests.cpp` | `talas_live_set_large_fixture_tests` | Implemented; final cumulative gate remains Step 6 authority |

## Portable bounds

The fixed project authorities in `ProjectLiveSets.h` are:

- 16 Live Sets;
- 8 rows by 16 columns per set;
- 1,024 aggregate cells;
- 8 mappings per cell;
- 32 Tools, each with at most 32 states, 64 variables, 256 transitions, and
  8 commands per transition;
- 4,096 guard-expression bytes;
- 16,384 aggregate prepared Tool instructions;
- 2,048 fixed runtime Tool variables;
- 64 queued realtime Tool triggers;
- 8,192 aggregate inspected records;
- 128-byte identifiers and 256-byte display names.

The 1,024-cell aggregate cap means a maximum-cardinality project can contain
16 sets but only eight completely populated 128-cell grids; the other sets are
empty or share the remaining aggregate budget. A fixture containing 16 full
grids would be product-invalid and is not performance evidence.

Parsing must apply container and decoded-string bounds before semantic copies.
Public validation and ProjectEditor candidate boundaries reject the first
record beyond the aggregate authority without publishing partial document or
history state.

## Persistence boundary

Format 34 owns a required schema-version-1 `liveSetLibrary` with exact
`liveSets` and `tools` fields. Formats 1–33 migrate to an empty library and
invent no target, mapping, Tool, active state, or executable behavior.
Canonical format-34 serialize/parse/serialize bytes are deterministic.

Project bytes own definitions, mappings, initial states, and initial values.
WorkspaceSession schema 13 separately owns selected identities and
definition-fingerprint-bound current Tool state. Save and Save As flush the
last callback-acknowledged state to the destination session identity; project
replacement and shutdown flush the old project session first. Reopen/recovery
clear active and queued cell identity and never auto-start a program.

An accepted restore carries a monotonic request generation and the audio
callback publishes its applied generation. UI/session synchronization clears
pending restore by that acknowledgment, not by eventual value equality, so a
newer user transition between callback adoption and the next UI poll remains
authoritative.

## Scheduler and callback contract

Every request carries document, stage, transport epoch/discontinuity, source,
route, and request generations plus one prepared numeric cell/tool index.
Immediate, Next Beat, Next Bar, and Tick Interval resolve against the immutable
accepted musical map and exact integral sample rate. Same-authority same-frame
requests have a deterministic last-wins reduction before side effects.

The callback may perform no allocation, hashing, parsing, filesystem/network
I/O, UI work, locking, or waiting. Tool execution uses fixed state arrays,
fixed-attempt ingress, bounded guard instructions, first-true transition order,
and at most eight prebound commands. Every rejected transition retains the
last-good state while anti-replay sequence authority still prevents duplicate
execution.

## Lifecycle authority map

| Boundary | Required outcome | Direct evidence |
|---|---|---|
| Project edit before restage | queued prior-document work is stale; current graph may continue | `testBridgeRevokesLiveSetRequestsAfterEditorDocumentChange` |
| Stage adoption/crossfade | old active/queued identity is revoked; commands address only the adopted control target | `testBridgeRevokesMappedLiveSetRequestsOnRouteReplacement`, prepared Live Set bridge tests |
| Transport seek/discontinuity | queued quantized work and bounded Score-region end authority do not survive | Score-region bridge regression |
| MIDI hotplug/slot reuse | source generation and route generation must both match | MIDI registry smoke plus mapped-route bridge regression |
| OSC profile replacement | retained endpoint is generation-bound; ingress is no-echo | `talas_workbench_osc_loopback_profile_tests`, native OSC smoke |
| Device stop/restart | sources are revoked and prepared/state publications are cleared before restage | `audioDeviceStopped`, `clearAfterAudioStopped`, native Workbench smoke |
| Open/New/Recovery replacement | old session flushes first; active/queued identity clears; new session reconciles by definition fingerprint | WorkspaceSession tests and native Workbench lifecycle smoke |
| Save/Save As/autosave/package | project structure remains project state; live current state remains session-only; package does not invent session execution | Project file, recovery, package, WorkspaceSession tests |
| Shutdown | worker/ingress ownership quiesces, session flushes, audio callback is removed, runtime state clears | Workbench destructor path and native smoke |

## Maximum fixture

`talas_live_set_large_fixture_tests` constructs a valid exact-bound project with:

- 16 Live Sets, eight full 8×16 grids, and exactly 1,024 cells;
- 4,459 mappings chosen so the complete model consumes exactly 8,192
  inspected records;
- 32 Tools and exactly 2,048 runtime variables;
- positive prepared Sound, Snapshot, ranged Score, Stop, and Tool callback
  execution;
- canonical format-34 round-trip;
- a first-over-bound aggregate rejection;
- measured validation, codec, preparation, UI projection p95, and callback
  p99 work.

Zero-cell, zero-candidate, unprepared, rejected-only, or model-only timing is
vacuous. The fixture asserts all four cell kinds are ready and executed and
that all Tool variables are materialized before accepting its measurements.

## Local acceptance commands

Before C9 completion, one exact candidate must freshly run:

- the complete serial Debug suite;
- focused Live Set/Tool model, parser, migration, editor, preparation, ingress,
  bridge, WorkspaceSession, OSC/MIDI, Workbench model, and maximum-fixture
  tests;
- focused ASan/UBSan and bounded TSan for model/publication/ingress/scheduler;
- fixed-seed parser and guard replay fuzz;
- optimized maximum-fixture and callback budgets;
- Debug native Workbench smoke;
- a physical tests-off Release CLI/Workbench build, catalog/help checks, and
  native smoke;
- generated, governance, release, protected-path, and diff checks;
- `git diff --check`;
- one independent exact-commit review with no remaining P0–P3.

GitHub Actions, remotes, hosted CI, publication, notarization, signing, and
other cloud/release-only work are deferred and do not count as local evidence.
