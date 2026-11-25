# Workbench and CLI reference

Detailed notes on the Workbench, the command-line tool, node maturity levels and the
component boundary. The [README](../README.md) gives the overview.

## Component ecosystem boundary

The private component pipeline can inspect, verify, owner-authorize, preserve,
probe, and transiently run one exact frozen macOS bundle. F5b Step 4 admits one
root audio-only instance in a separately sandboxed runtime at exact 44.1/48 kHz
and 256/512-frame profiles with fixed one-block latency. F5b Steps 5–8 admit
one atomic ordered set of at most eight unique generation-bound project
snapshots whose closed kinds are `Table`, `Binary`, or `Audio`, with at most two
Audio streams and 64 MiB of aggregate canonical source bytes. Table/Binary use
preparation-time immutable views of bounded canonical F64 bytes or exact raw
octets. Audio accepts only 1–64 MiB mono/stereo PCM16-WAV snapshots: the host
freezes the exact bytes, sends them pathlessly, and the Runtime helper owns the
read-only mappings, independent bounded decoder/refill workers, fixed
caches/queues, and one nonblocking direct ABI-v4 read per Audio member and
component callback. Component code receives no path, descriptor, raw WAV
pointer, decoder, worker, queue, writable storage, enumeration, or general
broker authority.
F5b Step 9 now exposes explicit owner-selected local replacement in the macOS
Workbench. Its owning preview, session-bound move-only receipt, and
receipt-aware Runtime seam are composed by one bounded controller that freezes
one explicitly selected directory,
retains exact owner/signature/probe/runtime evidence, and hands one prepared
binding to the control-thread stage point. The Workbench provides an accessible
root-preserved-node `Replace...` action and compact copyable
approve/preview/confirm/cancel surface; exact approval is session-local and
does not execute or stage the candidate. Project edits, Save As, destructive
decisions, project replacement, and shutdown revoke and join the operation.
Test-only signature/deadline/chooser seams remain absent from Release builds.
Discovery, installation, CLI action, registry promotion, persisted trust,
project-format change, declarative UI, state migration, writable/network/
compressed/reverse streams, multiple component instances, and a public SDK
remain closed.

## Prototype support levels

Talas classifies every built-in node by maturity. Current maturity counts are
listed in the generated
[capability matrix](coverage/capability-matrix-v2.md); candidate execution
profiles must be distinguished from accepted package evidence. “Full runtime” means
that no subset restriction is currently documented; it does not claim more
than the node's documented contract.

The Workbench Prototype Browser labels every node as `FULL`, `SUBSET`,
`PLACEHOLDER`, or `UTILITY`. Its tooltip keeps that maturity summary distinct
from offline and prepared-realtime execution support and translates tracked
limitations into concise product language. The same axes and stable limitation
IDs, plus explicit implementation bindings, typed signal/spectral ports,
resource access, fan-in latency policy, preparation-time latency rules, and
terminal bus layouts, are included in `talas_cli catalog --format json`;
unclassified state, latency, and resource access remain visibly unknown rather
than inheriting optimistic defaults. Detailed evidence lives in
`coverage/capability-matrix-v2.md` and `coverage/README.md`.

Named `FeedbackLoopInput`/`FeedbackLoopOutput` pairs now form explicit delayed
edges scoped to one reusable Sound instance. Compilation keeps ordinary graph
cycles illegal, emits deterministic delayed-SCC metadata, and prepares a
minimum one-sample circular delay. Offline and prepared realtime share that
compiled plan and pair-state driver, remain invariant across render/callback
partitions, isolate same-named nested instances, and allocate nothing on the
audio callback. Their catalog contract reports session-owned pair state,
transport reset support, and no feed-forward latency. Blank legacy feedback
nodes retain their inert compatibility behavior.

`Forward FFT v1` (`talas.forwardFftV1`) and `Inverse FFT v1`
(`talas.inverseFftV1`) are separately versioned Talas-native utilities. They
carry bounded `ComplexSpectrumV1` frame sequences in offline and prepared
realtime rendering, use the project-original radix-2 backend, and reconstruct
rectangular or periodic-Hann windows at exactly `N-1` round-trip latency. Frame
size, hop, and window are static prepared configuration; callback partitions,
ordinary loop wraps, and offline render-call partitions preserve frame timing,
while true transport discontinuities reset state. Complex buffers and FFT/OLA
scratch are prepared ahead of the audio callback, which performs no allocation.
The historical Audio-port `talas.fft` now runs the frozen non-overlapping
`PackedRealAudioV1` adapter for length 1 or radix-2 lengths 16–4096. It keeps
independent `N-1` latency per forward or inverse transform and is explicitly
labeled as a compatibility layout rather than native complex frames. The
decision record is
[ADR-0008](adr/0008-partial-spectrum-complex-frames-and-fft.md).

`Pan`, `Crossfade`, `Stereo Mix 2`, `Stereo Mix 4`, and the mono/stereo
`Audio Output` render terminal also run in prepared realtime with the
same static, connected sample-rate, and direct Event Value semantics proven by
offline rendering. Their stateless adapters preserve stereo channel order,
reject invalid controls, and allocate nothing on the audio callback.

## Workbench

The optional JUCE desktop Workbench provides graph editing, realtime transport,
virtual controls and MIDI mapping, Timeline events, analyzers, and external
project assets. `Export Audio...` renders an immutable project snapshot to
PCM16 WAV in the background, with configurable duration and sample rate,
streaming progress, and cancellation-safe destination replacement.
`Package...` similarly creates a self-contained `.talaspkg` directory containing
the project and byte-for-byte copies of every registered asset. Package
publication is cancellable and failure-atomic; opening one keeps later browsed
assets inside its `Assets` directory. Referenced asset files are fingerprinted
around staging and polled once per second on the message thread. A missing,
inaccessible, moved, or externally changed referenced asset opens the
in-context Asset Recovery surface without interrupting the existing realtime
graph. `Locate File...` inspects one explicit candidate; `Search Folder...`
performs a bounded local recursive search using exact accepted SHA-256 identity
only. Exact matches can be applied together as one Undo step. Different bytes
remain visibly blocked behind `Replace...` and an old/new-hash
`Confirm Replace` action that advances the generation exactly once. Ignore and
Cancel never edit the project, and embedded package failures never fall back to
external files.
One immutable prepared-graph status model now drives
the bounded project-state headline, selected-node Inspector contract,
keyboard/pointer-operable Diagnostics rows, Analysis spectral readout, exact
physical-output destinations, and aggregate stream health. It distinguishes
awaiting activation, latency alignment and warnings, spectral frame formats and
timing, resource identity, stream fill, underruns/missing frames, request
pressure, stale blocks, worker state, and decode failures without formatting
or synchronizing on the audio callback. Its New, Open, Save, Save As,
window-close, and application-quit
flows protect dirty documents with an explicit Save/Discard/Cancel decision;
the window title marks unsaved changes with `*`. Dirty projects are also
autosaved after a short idle period to a separate failure-atomic recovery
archive. It retains up to twenty timestamped unsaved versions and suppresses
identical consecutive snapshots. On the next launch, a newest-first version
selector can restore any retained snapshot without silently overwriting a
project that changed on disk; legacy single-snapshot recovery files remain
readable. The Open menu retains up to ten successfully opened or saved projects
across launches; recent-file selection passes through the same unsaved-change
protection.

Native File, Edit, Transport, View, and Help menus expose the complete command
set;
the compact toolbar keeps transport, Save, and overflow visible while moving
secondary actions into menus. Standard platform shortcuts cover New, Open,
Save, Save As, Undo, Redo, Delete, Play/Pause, and direct Graph/Timeline focus.
The Workbench keeps a usable `960 × 640` content minimum, adapts side-zone and
measurement-panel sizes continuously, and switches among Patch, Automation,
Analysis, and Performance workspaces. Performance projects the stable exposed
controls of the selected reusable Sound instance without exposing internal
node identity. Compact mode protects the complete patch by making the
side workspaces summonable; wheel/Option-drag pan, Command-wheel zoom, and
Command-0 zoom-to-fit preserve graph continuity. The Prototype Browser adds
search, collapsible categories, Favorites, Recent, and maturity filtering.
The Performance workspace also has a Snapshot tab. It creates project-,
definition-, or selected-node banks; stores, recalls, renames, clones,
initializes, reorders, and deletes snapshots; morphs between two endpoints;
and applies seeded randomize or random merge with explicit ranges and locks.
Store and library changes are ordinary undoable project edits. Recall, morph,
and randomization are live actions and do not dirty the project; `Capture
Current` is the explicit one-undo path back into a stored snapshot. Missing or
changed parameters remain visible with migration diagnostics, and a missing
numeric range disables randomization instead of inventing a unitless default.
Create and Store use only values from the current accepted runtime stage; an
unstaged authoring edit asks the user to stage the project instead of silently
capturing descriptor defaults or stale performance values.
The Performance workspace also provides a Live Grid mode with stable 8×16
navigation. A cell launches one exact prepared Sound, Snapshot, ranged Score
marker, or Stop action using Immediate, Next Beat, Next Bar, or bounded
musical-tick timing. Pointer, keyboard, exact MIDI Program Change, OSC, and
bounded Tool commands consume the same availability and generation-bound
dispatcher. Live Tools are finite state machines over fixed numeric/boolean
state and the closed activate-cell, Stop, approved scalar-variable, and Event
Value command vocabulary; they are not a general scripting or I/O facility.
Definitions and mappings are format-34 project state, while current Tool
values and active/queued presentation are WorkspaceSession state and never
dirty the project. See `help/live-grid-and-bounded-tools.md` for the
operator contract and `testing/live-sets.md` for the acceptance map.
WorkspaceSession format 13 keeps those local choices in an independent file:
view mode, the accessible draggable/keyboard pane
split, explicit visible root/Sound navigation path, bounded pan/zoom context
cache, selection, focused Event Value, and bookmarks never enter project bytes
or dirty state. Root-only reopen remains distinct from cached definition views.
A missing or malformed local session falls back to defaults and cannot block
Open or Recovery. High-frequency local changes are coalesced behind a 250 ms
idle window, with explicit flushes at Save, project replacement, and shutdown.
Persistent toolbar text separates saved/unsaved, compile, and transport state
from transient messages. Core controls have explicit accessible names,
high-contrast keyboard focus, and status announcements. Graph nodes and typed
ports are keyboard-operable; visible `A/C/E/D` port prefixes reinforce their
audio/control/event/data colors without relying on color alone. Timeline events
and loop boundaries retain their pointer workflow while the event list and
persistently labeled numeric fields provide the equivalent keyboard editing
path.

Open and each successful Save also retain the exact raw project-file revision.
An external edit, reformat, replacement, or removal adds `[Changed on Disk]` to
the window title; ordinary Save then fails closed without changing the external
file, while Save As remains the explicit copy-or-replace workflow.

Event Value references such as `!Gain` and `!Frequency` automatically create
persisted controls with inferred scalar, hertz, seconds, note-number, or sample
domains. The Event Values panel also supports explicit domain and
Fader/Gate/Toggle creation, typed numeric fader entry, MIDI learn, and
undo/redo-safe edits. Timeline markers can be selected and dragged directly;
they snap to an adaptive time grid by default, with Shift temporarily enabling
free positioning, and each drag remains one undo action. The track derives a
stable labeled lane for each distinct Event Value name, so simultaneous events
remain independently visible and selectable; dense lane sets scroll vertically.
Project-persisted half-open loop regions repeat Timeline Event Values
sample-accurately while keeping DSP state continuous; their start/end handles
use the same snap, Shift-bypass, and single-undo interaction.
Each Timeline keyframe can hold its value with Step interpolation or ramp
sample-accurately to the next same-lane keyframe with Linear interpolation.
The lane view draws the selected segment shape and offline/realtime playback
share the same endpoint and loop-boundary rules.

The selected-node analyzer supports 64/128/256-sample scope and spectrum
lengths, Rectangular/Hann/Blackman spectrum windows, exponential averaging,
peak hold/reset, adjustable rising/falling continuous and single-shot scope
triggers, 1x/2x/4x mode-specific zoom, linear/logarithmic spectrum axes with
pan, exact hover or pinned draggable cursors, and a Freeze mode that continues
draining realtime capture. Single-shot capture also keeps draining the
realtime queue after its first matching edge and can be explicitly rearmed.
The analyzer cursor is focusable and keyboard-operable with Arrow, Shift,
Home, End, Return, and Escape commands; its frequency/time and magnitude value
is visible text and exposed to assistive technology.

Project format v5 stores the Workbench's selected Audio/MIDI device
preferences, sample rate, buffer size, and active channels. Format v6 adds the
Timeline loop region, and format v7 adds per-keyframe Step/Linear
interpolation. Format v8 adds persisted reusable Sound definitions with typed
input/output boundaries and explicit Sound-instance references. Definitions
are validated for endpoint types, duplicate input bindings, missing
dependencies, direct graph cycles, and recursive definition dependencies,
then deterministically expanded before both offline and prepared-realtime
compilation. Format v9 adds a positive versioned interface to each definition:
stable exposed parameter IDs, display labels, typed defaults, explicit
internal bindings, compatibility aliases, and sparse per-instance overrides.
Choice, Boolean, Resource, Text, and domain-aware Expression parameters are
validated against their internal targets before expansion; display-only
renames preserve identity, while unknown or ambiguous overrides fail closed.
Format v10 introduced the canonical asset manifest
with independent ID, path, referenced/embedded disposition, lowercase SHA-256,
audio media shape, encoding, positive generation, and provenance. Versions 1–9
migrate in memory without filesystem inspection or invented hashes. Local
same-content relocation preserves generation; explicitly accepted different
bytes advance it. Format v10 also persists typed `complexSpectrum` Sound
boundaries, while format v9 keeps rejecting that future token.
Format v11 moved every root, nested, and Performance
coordinate into one versioned portable `presentation` authority, removes inline
positions from topology and controls, and owns a populated version-1
`snapshotLibrary` with project/definition/node scopes, stable typed target
identities, smoothing, ranges, locks, and ordered banks/snapshots. Formats 1–10
migrate positions exactly in memory and acquire an empty snapshot library;
future/unknown presentation or snapshot versions, malformed/over-capacity
banks, legacy inline positions, duplicate identities, and invalid typed values
fail closed. Preset, distribution, and host schemas likewise accept only their
empty version-1 envelopes until their behavior is implemented. Format v12
retains those authorities while adding the required
version-1 `sampleMapLibrary` boundary with exact note, velocity, direction, and
source-frame loop metadata.
Missing-asset recovery uses the same canonical manifest without persisted
chooser/search state. Save As rebases accepted recovered paths;
autosave and recovery retain only the accepted one-Undo document transaction.
The project-native `talas.sampleMapPlayer` resolves stable map/region identities,
prepares map-only reachable immutable Audio generations, and renders the same
note/velocity layers, root-note transposition, reverse direction, and exact
sustain/release loops offline and in prepared realtime. The historical
`talas.keyMappedMultisample` identifier remains its documented compatibility
subset; it is not promoted by the new authoring workflow.
Reachable table and binary assets are prepared as immutable
`(asset ID, kind, generation, SHA-256)` snapshots. Prepared graphs and render
sessions pin those exact payloads; same-generation content changes, generation
rollback, and generation increments without new content fail closed. Migrated
unknown hashes are adopted on their first valid load without consuming a
generation. Reachable mono/stereo PCM16-WAV Audio assets now use the same
immutable generation contract with one graph-scoped worker/cache owner and
independent bounded consumers. `DiskPlayer` supports deterministic offline
full-load and prepared-realtime positive-rate playback with linear source-rate
conversion, trigger/retrigger and seek-on-change, stereo midpoint reduction to
mono, and zero-fill underrun telemetry. `DiskCache record=false` remains mono
passthrough while sharing and prewarming that owner; recording, reverse Audio,
compressed codecs, stereo output, and remote streams remain unsupported. The
decision record is
[ADR-0009](adr/0009-immutable-resource-generations-and-streaming.md).
Each instance receives distinct runtime node identities, prepared scopes, and
DSP state; flat versions 1-7 retain their previous execution path, while v8
definitions migrate to interface version one with an empty parameter list. The
Workbench Reusable Sounds panel can search all, kept, or recent definitions;
create, rename, delete, instantiate, extract, inline, and audition them; and
edit definition graphs directly in a two-pane Root/parent/child Structure
view. Shift-click extraction supports a selected root subgraph and substitutes
typed boundary inputs. Double-click opens a nested Sound opposite its parent,
while Parent and Root navigation retain selection per structural context.
Audition stages an isolated prepared preview and keeps the active project graph
unchanged on failure. Shared mutable Sound state remains a later hierarchy
milestone. Declared definition-local Event Values are instantiated
per Sound scope; explicit exposed defaults/overrides can project global or
ancestor controls into that scope without equal-name capture, while legacy
name-based scheduling remains global-only. Multiple disjoint reachable prepared
MIDI Voice regions receive independent allocator, pending-attack, voice-scope,
and mix state; matching channel/range routing can deliberately address more
than one region without callback allocation. Versions 1-6 migrate with Step interpolation, and versions 1-7
migrate with an empty Sound-definition list, preserving their existing
Timeline sound; unavailable saved hardware falls back to the working host
configuration with a visible warning.

Format 28 adds the closed VariablesV1 scalar library. Project and exact
expanded-Sound declarations use explicit capture/snapshot policy; each
structural MIDI Voice owner receives independent generation-bound cells for
every prepared slot; and Workspace Session declarations persist acknowledged
values only in the local workspace file. The Workbench Variables section shows
the exact owner, current local value, and active/releasing voice slots. Stale
stage or slot generations fail closed, voice values never enter snapshots or
portable bytes, and workspace values never enter project, recovery, package,
component, or host state.

Format 35 adds the frozen VariablesV2 profile. It retains VariablesV1 exactly
and admits only the checked Scalar binary `rem:` token with
truncation-toward-zero remainder semantics and positive-zero normalization.
Formats 1–34 preserve their exact language and expression bytes. A VariablesV1
project moves to VariablesV2 only through the explicit audited Workbench
upgrade transaction; arbitrary Smalltalk selectors and general message sends
remain unsupported.

The Workbench `Samples` workspace exposes the map library, a keyboard/velocity
region canvas, exact-frame waveform and loop markers, and a region inspector.
Reference/Embed imports inspect PCM16-WAV bytes off-thread and publish Merge or
explicit typed-confirmation Replace as one undoable transaction. Map and marker
drags have keyboard/numeric equivalents; multi-selection never invents a
single value; missing/changed assets reuse Relink, Reload, Show, and immutable
generation authorities. Audition preparation is generation-tagged and staged
without file work or allocation in the audio callback. Compact windows keep
the map/waveform central and summon the library/inspector as docks.

For offline development, configure the Workbench against an existing JUCE
checkout:

```bash
cmake -S . -B build/workbench-debug \
  -DTALAS_BUILD_WORKBENCH=ON \
  -DTALAS_FETCH_JUCE=OFF \
  -DTALAS_JUCE_PATH=/path/to/JUCE
cmake --build build/workbench-debug --target talas_workbench
```

For a reproducible unsigned release build on macOS or Windows, the checked-in
preset fetches JUCE 8.0.15 and creates a portable ZIP:

```bash
cmake --preset workbench-release
cmake --build --preset workbench-release
ctest --test-dir build/workbench-release --no-tests=error \
  --output-on-failure -C Release \
  -R '^talas_workbench_app_smoke$'
cpack --preset workbench-release
```

The archive is written under `build/workbench-release/packages`. Git and
network access are required only for this fetched-JUCE path. On macOS, an
unsigned drag-and-drop DMG can also be requested with
`-DTALAS_ENABLE_DMG_PACKAGE=ON` when configuring without the preset, then built
with the `package` target.

On macOS, a release owner with a Developer ID Application certificate and a
preconfigured `notarytool` keychain profile can create a signed, hardened,
notarized, stapled, and Gatekeeper-verified DMG:

```bash
tools/notarize_macos_release.sh \
  --keychain-profile talas-notary
```

The script autodetects a unique Developer ID Application identity, works only
on a new staging copy, and writes timestamped output under
`build/workbench-release/notarized`. Use `--identity` when more than one valid
identity exists and `--cmake` when CMake is not on `PATH`. No Apple account
password, API key, certificate, or notary credential is stored in the source
tree. The notarized DMG is the preferred macOS distribution artifact; the
companion ZIP contains the same signed app and CLI, but a ZIP itself cannot
carry a stapled ticket.

The app-smoke gate constructs the real Workbench without opening host
audio/MIDI devices or user state, then creates and connects a graph, edits an
Event Value expression, extracts and edits a reusable Sound, verifies scoped
selection through undo/redo and Performance navigation, checks local Event
Value focus plus default/expanded Structure pane splits without dirtying the
project, applies an exposed override, inlines the instance, stages and renders
playback, verifies the Snapshot tab's empty and compact layouts, creates a
node-scope bank, supplies a required range, stores, recalls, morphs, and
randomizes without live actions changing project bytes, then saves, reopens,
and verifies that the graph, generated control, and populated snapshot library
restore. It also enters Samples, imports original PCM16-WAV candidates, proves
cancel-without-publication, Merge and confirmed Replace with Undo/Redo, edits
map/loop state as one gesture, exercises honest multi-selection and regular/
compact layouts, stages/cancels/stops audition, and restores the Samples
workspace session after save/reopen. It can also be run directly as
`talas_workbench --smoke-test <absolute-existing-directory>`.

## CLI

The [examples index](../examples/README.md) lists the runnable projects, expected
prepared-status evidence, exact render commands, and Workbench inspection
targets. Its five M3 demonstrators are current format-10 projects covering
native and historical FFT paths, immutable streaming health, four-channel
routing, and native partial-spectrum frames. The separate
`m4_state_layers.talas.json` fixture is canonical format 13 and proves independent
topology, portable presentation, empty snapshot/sample-map libraries, and local-only
WorkspaceSession behavior. The separate original
`m4_snapshot_performance.talas.json` fixture proves canonical populated snapshot
state, exact target resolution, direct recall, midpoint morph, seeded bounded
randomization, and unchanged authoring bytes. The original
`m4_sample_map_instrument.talas.json` fixture proves a populated map over one
map-only reachable immutable PCM16-WAV generation, exact forward/alternate and
reverse loop metadata, deterministic CLI rendering, checked Save/Save As,
autosave/recovery, external-conflict detection, and portable package integrity.
The original `m4_missing_component_preservation.talas.json` fixture proves the
separate format-13 preservation boundary: an unavailable synthetic component
retains frozen ports and parameters, opaque non-UTF8 state, graph edges, a
snapshot target, and portable coordinates through Save, recovery, and a
data-only package. Validation intentionally returns nonzero because the
document is portable but not executable without an exact trusted descriptor;
rendering fails before publishing output.

Export the built-in node catalog:

```bash
./build/core-debug/talas_cli catalog --format json
```

Render a project to CSV on stdout:

```bash
./build/core-debug/talas_cli render --project examples/oscillator.talas.json --duration 1 --sample-rate 8
```

Render a structured JSON preview on stdout:

```bash
./build/core-debug/talas_cli render --project examples/oscillator.talas.json --duration 1 --sample-rate 8 --format json
```

Validate a project without writing audio:

```bash
./build/core-debug/talas_cli validate --project examples/oscillator.talas.json
```

The original format-9 help patch demonstrates two independent instances of one
reusable Sound, stable exposed overrides, and scope-local named delayed
feedback:

```bash
./build/core-debug/talas_cli validate --project examples/reusable_feedback_help.talas.json
./build/core-debug/talas_cli render --project examples/reusable_feedback_help.talas.json --frames 256 --sample-rate 8000 --format json
```

Export a project graph as Graphviz DOT:

```bash
./build/core-debug/talas_cli graph --project examples/oscillator.talas.json --format dot
```

Create a runnable starter project:

```bash
./build/core-debug/talas_cli init --template oscillator --output /tmp/talas-oscillator.talas.json
```

Render a project to a PCM16 WAV file:

```bash
./build/core-debug/talas_cli render --project examples/oscillator.talas.json --duration 1 --sample-rate 48000 --output /tmp/talas-oscillator.wav
```

Render with a numeric table resource:

```bash
./build/core-debug/talas_cli render --project examples/sample_text_table.talas.json --frames 4 --sample-rate 4 --table Samples/Hit=examples/resources/hit.txt
```

`--table` accepts mono PCM16 WAV files and numeric text files. Numeric text tables support whitespace, commas, semicolons, and `#` line comments.

Project format v4 can persist these table and binary resource bindings in its
`assets` array. Relative asset paths resolve from the project file, and both the
Workbench and CLI load referenced assets automatically. Explicit `--table` and
`--binary-resource` arguments override a persisted asset with the same ID.
The CLI `graph`, `validate`, and `render` commands also accept a marked
`.talaspkg` directory produced by the Workbench; packaged assets are validated to
remain below that package's `Assets` directory before use. Newly written
packages mark assets embedded and verify their accepted SHA-256 and audio shape
both before publication and when opened; tampered content fails closed.
CLI validation and rendering apply the same persisted virtual controls,
discovered Event Values, built-in MIDI controls, and Timeline settings as the
Workbench runtime bridge, including references inside reusable Sound
definitions.

Render with a binary resource such as a MIDI file referenced by a `talas.midiFileEcho` `fileName` parameter:

```bash
./build/core-debug/talas_cli render --project path/to/midi-file-echo.talas.json --frames 48000 --sample-rate 48000 --format json --binary-resource Sequences/Part.mid=path/to/Part.mid
```

Use the same resource flags with `validate` when a project depends on external tables, binary files, or audio inputs:

```bash
./build/core-debug/talas_cli validate --project path/to/midi-file-echo.talas.json --binary-resource Sequences/Part.mid=path/to/Part.mid
```

Add `--status` to inspect the deterministic prepared contract used by the
runtime without attaching a hardware device:

```bash
./build/core-debug/talas_cli validate --status --project examples/oscillator.talas.json
```

Ordinary successful validation remains byte-compatible and writes exactly
`OK\n`. Status mode additionally reports prepared outputs, latency,
compensations and warnings, spectral formats/timing/limitations, immutable
resource health, and static stream contracts.

`--audio-input` accepts mono PCM16 WAV files and maps them to 1-based runtime input channels:

```bash
./build/core-debug/talas_cli render --project path/to/input-project.talas.json --frames 48000 --sample-rate 48000 --audio-input 1=path/to/input.wav --output /tmp/render.wav
```
