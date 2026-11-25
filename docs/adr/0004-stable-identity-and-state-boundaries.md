# ADR-0004: Stable Identity and State Boundaries

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Projects already persist node instance IDs, Sound definition IDs, type IDs,
port IDs, parameter IDs, and Event Value names. Runtime scopes and compile-order
indices are implementation details. Automation and state must
survive display renames, nested expansion, and dispatch migration without
turning transient indices into file-format identities.

## Decision

Persisted identity is string-based and structural:

- node type: `NodeTypeDescriptor::typeId`;
- root node: project `instanceId`;
- nested node: ordered Sound-instance path plus definition-local `instanceId`;
- parameter/port: stable descriptor ID within the qualified node identity;
- Sound definition and instance reference: existing `definitionId` and
  `soundDefinitionId`;
- Event Value: name plus value domain within the project namespace.
- scalar variable declaration: variable ID plus its closed project,
  Sound-definition, structural MIDI Voice, or workspace-session owner;
- live MIDI Voice variable: structural owner and expanded Sound-instance path
  plus prepared stage generation, slot ordinal, and slot generation.
- Sound-variable declaration: variable ID plus the same closed owner and an
  ordered bounded set of exact reusable-Sound definition IDs. A prepared
  ordinal is valid only inside that immutable candidate table; persistence,
  transfer, reconciliation, and stale checks compare definition identity.

Display names, categories, vector positions, compile-step indices, buffer
indices, memory addresses, and runtime scope integers are never persisted as
automation identities.

Preparation resolves each structural identity once into compact generation-
scoped integer handles. Handles may change after recompilation and are valid
only with their owning prepared-graph generation. The callback receives only
those handles; it never reconstructs or hashes paths.

Descriptor IDs are append-stable. Renaming or removing one requires an explicit
project-format migration or compatibility alias with collision tests. Reordering
descriptor vectors cannot change identity.

## State layers

Portable and local state have distinct versioned authorities:

1. **Project execution state:** serialized graph topology, parameters, assets,
   controls, Timeline, device preferences, and Sound definitions.
2. **Project presentation state:** portable root/nested Structure coordinates
   and Performance control layout. It is part of the project document,
   undoable, and dirtying, but never executable topology.
3. **Snapshot state:** version-1 project-owned banks of scoped typed parameter
   values, smoothing, ranges, and locks. Project, definition, and node scopes
   use the structural identities above. Resource and Text values are
   ineligible; stale semantic identities remain recoverable diagnostics rather
   than silently rebinding.
4. **WorkspaceSession state:** local view mode, split, explicit visible
   Structure navigation path, bounded graph-context pan/zoom cache, selection,
   Event Value focus, bookmarks, and acknowledged workspace-session scalar
   values. Its independent versioned file never enters project bytes, recovery,
   undo, package, host state, or dirty state. Debounced ordinary writes and
   explicit lifecycle flushes remain outside project persistence. An eligible
   project snapshot may target a workspace-session value explicitly, but recall
   changes only local live/session state rather than the portable declaration.
5. **Preset package state:** portable snapshot/fingerprint/optional-asset
   envelope, reserved empty until its behavior is implemented.
6. **Distribution package state:** complete-project/dependency/asset envelope,
   reserved empty without changing the existing `.talaspkg` path in F1.
7. **Host state:** version-2 stable automation and embed/link policy envelope.
   The plugin-neutral F6 foundation persists append-stable automation IDs,
   exact structural target identities, sticky removal tombstones, explicit
   stable-ID migrations, structural dirty state, and either canonical embedded
   project bytes or an external URI with its expected digest. The version-1
   reserved empty envelope migrates to version 2. Actual plugin/host adapters
   remain deferred until an artifact is approved.

`ProjectEditBoundary` owns only the optional path and canonical clean-project
comparison. It is deliberately not named or modeled as workspace state.

Prepared/runtime state remains separate from all seven persisted authorities:

- **Prepared configuration:** immutable validated plan, resolved resources,
  buffer bindings, capability snapshot, and kernel factories/instances. It is
  rebuilt transactionally and not serialized.
- **Session DSP state:** phase, filter history, envelopes, cursors, and legacy
  adapter state. It belongs to one prepared instance/scope and is destroyed on
  retirement unless a later explicit transfer contract applies.
- **Host/live runtime state:** transport epoch, current frame, ingress queues,
  live Event Value projection, device timing, and active/pending generations.
  It is neither silently copied into the document nor shared across project
  loads.
- **Prepared scalar-variable state:** project cells, exact expanded
  Sound-instance cells, local workspace-session cells, and per-MIDI-Voice-slot
  cells use immutable prepared handles. Voice cells are initialized before the
  new note becomes observable, survive their release tail, and reset on exact
  slot-generation reuse. They are never project, workspace, host, or snapshot
  state. Workspace-session cells become local persisted state only after the
  exact bridge request is acknowledged.
- **Snapshot performance state:** current typed live values, selected bank and
  snapshot, morph endpoints/position, random seed, and owning project revision.
  Recall, morph, randomize, random merge, and reset change this control-thread
  state without dirtying the project. Only explicit capture writes it back as
  one undoable snapshot edit.

Prepared snapshot projection follows the same identity boundary. Direct Event
Value-backed targets resolve once into generation-scoped handles and bounded
ramp commands. Targets that require preparation use one complete ephemeral
same-topology overlay and a preallocated host crossfade. Stored target strings,
project editors, allocators, locks, and file I/O never enter the callback. The
execution-topology fingerprint rejects structural, resource, interface, bus,
and Sound-reference changes before overlay staging.

No M1 graph replacement transfers DSP state by matching IDs. The old graph
continues until the new graph is completely prepared, then the host swaps at a
callback boundary. A future state-transfer feature requires its own versioned
schema and compatibility checks.

## Automation identity

M1 parameter automation targets a resolved structural `ParameterIdentity`, not
a UI row or compile index. Current Timeline Event Values remain name/domain
targets. Bridging an Event Value into a parameter retains both identities in
prepared metadata so diagnostics can name the project target without callback
string work.

## Rejected alternatives

- Persisted random runtime UUIDs would duplicate existing stable project IDs
  and complicate deterministic fixtures.
- Hash-only identities risk collisions and make migration diagnostics opaque.
- Serializing kernel state would couple project files to internal DSP layout
  before a state ABI exists.

## Acceptance evidence

- Save/reload and nested expansion produce the same structural identities.
- Display/category/vector-order changes do not retarget automation.
- Duplicate or ambiguous structural IDs fail before preparation.
- A stale generation handle is rejected without touching the active graph.
- Oscillator, Filter, and ADSR instances in separate nested scopes retain
  independent mutable state.
- Format 11 has one validated presentation authority and migrates formats 1–10
  without retaining inline coordinates.
- Format 11 round-trips populated version-1 snapshot banks canonically; exact
  structural target identities survive vector reordering and display renames.
- Snapshot editor commands are failure-atomic one-undo edits, while recall,
  morph, and deterministic randomization preserve canonical project bytes.
- Direct transactions reject stale generations atomically; parameter overlays
  reject topology changes and transition through bounded preallocated host
  state.
- WorkspaceSession mutations preserve exact project bytes and clean/dirty
  identity; root-only visibility round-trips independently from cached
  definitions, and malformed local sessions cannot block Open or Recovery.
- Format 28 variable-library version 2 resolves project, exact Sound instance,
  structural MIDI Voice, and workspace-session declarations without display-
  name or compile-index identity. Separate Voice owners and slot generations do
  not alias; stale stage/slot requests leave last-good state unchanged; local
  session values persist only after callback acknowledgment and cannot leak
  into project, recovery, package, component, or host-state bytes.
- Preset and distribution envelopes reject every non-empty or future payload
  until their defining milestones. Host state version 2 round-trips bounded
  automation/storage state under one 16 MiB envelope limit and a 12 MiB
  embedded-project cap; exact canonical structural resolution survives vector
  reorder, linked storage accepts only local authority-free `file:///` URIs,
  missing targets become non-rebinding tombstones, and only an explicit
  stable-ID migration can redirect one.

## Amendment (2026-10-13): a voice sets its Sound up afresh as it starts

Owner decision 2026-10-12. Constant fields are set when the Sound compiles
and cannot change while it plays;
inside a MIDIVoice, MIDIMapper or AnalogSequencer one is read once as each
voice starts, with that voice's Event Values. A probe showed that realtime
voices never restarted a node's setup per note: a Low-Pass Gate's
OpenFrequency of `!KeyNumber * 20 hz` played the compile's key 60 for every
note.

From project format 91 an operation inside a voice whose constant fields
follow an Event Value or time names them as fields read as the voice starts.
Each Attack stamps its voice with a number no other Attack in the session
shares (never rolled back, so an Attack played again after a failed callback
stamps it afresh); an operation whose voice carries a stamp it has not
applied resets its processing state as when it was prepared and takes the
fields' values, each saturated at its range as a live value is: seeds and
initial states seed it again, setup values take their new value, and sizes
change within the size prepared for them. Taken as recommended (standing
approval 2026-10-12): a sizing field is prepared for its largest value where
that is small (a Filter's 16 poles, 64 grains, 64 breakpoints, 64 masses, an
EuSeq's 256 steps, a SyntheticSpectrumFromArray's array), and a count that
depends on another field saturates against that field's value (an EuSeq's
Pulses within its Steps and Rotation below them, StepsPerOctave from
ScaleSteps, an OpenFrequency from the ClosedFrequency, a MaxInputPitch above
the MinInputPitch); the alternative was to refuse a voice value above the
size prepared for the compile's value. A field that fixes a latency, a
structure, a table, a bank's size or a processor when the Sound compiles, or
a count our renderers hold (a TimeFrequencyScale's and FrequencyTracker's ten
detectors), cannot be set up again in a callback that may not allocate; given
an Event Value or time inside a voice it is refused by name
(`start_only_fields::fixedWhenCompiled`). Notes reach voices through the
realtime engine in both renderers, so offline and realtime play alike.
Formats 1 to 90 acquire `constantFieldsRead` `legacyWhenCompiled` on their
MIDIVoices, MIDIMapper and AnalogSequencers, which read such a field once, as
the Sound compiles, as before.

Later the same day (2026-10-13, taken as recommended under the standing
approval of 2026-10-12) three of the refused fields became read per voice
within what the voice prepares, so that the callback still allocates
nothing: a Vocoder's, TunableVocoder's and ScaleVocoder's NbrBands retunes
the bank, which reserves room for its 64 bands (a value saturates within 1
to 64); a SumOfSines's AnalysisFrames divide the analyses anew, kept whole
with phases for the widest frame one frame gives (a count that does not
divide an analysis stops the Sound, as the same value alone is refused, and
a NbrPartials past the partials a frame holds plays them all); an
AntialiasedOscillator's DutyCycle warps the cycle anew through a transform
planned when the voice is prepared (its chirp and kernel depend only on the
cycle's length). The alternative, keeping them refused, was the earlier
state. Still refused by name in a voice: fields that set the Sound's latency
(a FrequencyScale's MaxScale, CDP's division, a TimeFrequencyScale's
MinInputFreq, which sizes its analysis window, and MaxFreqScale, which
decides whether a fixed FrequencyScale uses the vocoder at all, so whether
the Sound has latency), structures (a ContextFreeGrammar's expansion, a
feedback loop's shared delay, a processor placement, an oscilloscope), and
the detector counts of a TimeFrequencyScale and FrequencyTracker, whose ten
detectors are the only count either renderer plays, so a voice could only
be refused later.

## Amendment (2026-10-13): the setup of banks and spectra is constant

Taken as recommended under the standing approval of 2026-10-12. The setup of
a bank or a spectrum joins the constant fields (the fields that decide the
memory or DSP requirements of the program): the sizes and counts of the CloudBank, FilterBank, FormantBank,
OscillatorBank, SOSOscillators and VocoderChannelBank and their elements, a spectrum's
partials and analysis metadata (SpectralShape, SpectrumInRAM, SpectrumOnDisk,
SyntheticSpectrumFromSounds, HarmonicSpectrum), a
selected track, a WaveTerrainOscillator's terrain size, a
FrameSynchronizer's frame, the seeds of a CloudBank and a SpectrumModifier, a CloudBankElement's InitialDelay, and a
LiveSpectralAnalysis's harmonic tracker setup (InitFreq, TrackedHarmonic,
LowFreq, HighFreq, UnpitchedOnly and UnpitchedThreshold, which a Harmonic
analysis must be given; the alternative, following the
last two live through a setter on the analysis, can come later).
Given a time function or an Event Value each compiles to its starting value
in both renderers, where both refused a later value that was not whole or
moved the prepared configuration. Inside a voice each is refused by name, as
a bank's size is; the alternative was to read each per voice within a
prepared maximum, which no project needs yet.
