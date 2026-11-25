# Live Grid and Bounded Tools

Live Grid is a Performance-workspace mode for launching one exact prepared
program at a time. It is not a multi-track clip launcher and does not run a
general scripting language.

## Cells and launch timing

Each visible cell has one stable project identity and one closed target:

- a reusable Sound, optionally bound to one exact root Sound instance;
- a Snapshot in one exact bank;
- one ranged Score marker;
- Stop.

Launch timing is Immediate, Next Beat, Next Bar, or a bounded musical-tick
interval. The accepted project, prepared stage, transport discontinuity,
input-source generation, route generation, and request generation all belong
to the launch authority. If any of them changes before execution, the request
becomes stale and has no side effect.

The grid has eight rows and sixteen columns. Arrow keys skip empty positions,
Return or Space activates the focused cell, and the compact view scrolls the
active cell into view. Pointer, keyboard, exact MIDI Program Change, OSC, and
bounded Tool commands use the same availability and dispatch decision.

## Project mappings and local availability

Mappings stored in the project may be:

- Workbench activation;
- exact MIDI input identifier, channel, and Program Change number;
- exact OSC address on the configured local-loopback ingress.

Mappings are validated as one conflict-checked set. Connected MIDI inputs,
listener availability, selected cell, queued request, and active cell are
session/runtime facts rather than project definitions. A disconnected or
stale mapping stays visible with its reason and cannot silently bind to a new
device or route.

## Bounded Tools

A Live Tool is a finite-state machine compiled before the audio callback. It
may react only to Manual, Event Value, local-variable, exact MIDI Program
Change, exact OSC, or periodic musical-tick triggers. Guards use the bounded
expression authority. A transition may emit only these commands:

- activate one exact Live Grid cell;
- Stop;
- set one approved project scalar variable;
- set one approved WorkspaceSession scalar variable;
- set one approved Event Value.

The first matching true transition wins. Invalid, stale, guard-rejected,
command-rejected, over-budget, or queue-full work holds the last-good state
and publishes a bounded reason. The Performance panel reports aggregate
queue/stale/rejected counts and the exact no-match, invalid, stale-trigger,
guard, command, or initialization category.

## Persistence and lifecycle

Format 34 stores Live Set and Tool definitions, mappings, initial states, and
initial values. Structural changes use ordinary ProjectEditor Undo and dirty
the project.

Current Tool state and values, selected set/cell/tool, grid view state, and
other performance presentation are WorkspaceSession state. Live activation
does not dirty the project. Save, Save As, project replacement, and shutdown
flush the latest callback-acknowledged Tool state. Reopen and recovery may
restore definition-compatible Tool state, but never auto-start a cell, retain
a queued launch, or resume an old active program.

Project replacement, stage adoption, transport seek/discontinuity, device
stop/restart, MIDI source-set replacement, OSC profile replacement, and
shutdown revoke prior-authority requests. Failed staging or activation keeps
the last-good graph/program.

## Fixed limits

- 16 Live Sets and 1,024 aggregate cells;
- 8 rows by 16 columns per set;
- 8 mappings per cell;
- 32 Tools;
- 32 states, 64 variables, and 256 transitions per Tool;
- 8 commands per transition;
- 4,096 bytes per guard expression;
- 16,384 aggregate prepared Tool instructions;
- 2,048 fixed runtime Tool variables;
- 64 queued realtime Tool triggers;
- 8,192 aggregate inspected Live Set/Tool records.

Stable identifiers are at most 128 bytes and display names at most 256 bytes.
Malformed and over-bound state is rejected before unbounded semantic work.

## Capability boundary

Live Grid and Tools reuse existing prepared Sound, Snapshot, Score, transport,
variable, Event Value, MIDI, and OSC authorities. They add no node descriptor
or node execution promotion, so the generated node capability matrix does not
change.

They do not provide Smalltalk, Lime, arbitrary scripting, direct graph
mutation, filesystem/network/process/native access, plugin loading, UI-object
access, arbitrary I/O, or an implicit multi-track mixer. Those capabilities
must not be inferred from the words “compiled” or “tool”.

## Deployment boundary

Live Grid/Tools are part of the locally verified standalone Workbench and
prepared runtime. Their local proof includes exact Project/WorkspaceSession
round-trip, a maximum 1,024-cell/32-tool/8,192-record fixture, callback and
projection budgets, source replacement, restore acknowledgement, device
restart, and native Workbench interaction. This does not make the grid a DAW
plugin surface, remote service, public scripting SDK, or certified
cross-platform/accessibility product. A missing external/manual environment
must remain visible as unavailable rather than being treated as passed.
