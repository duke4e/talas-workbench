# ADR-0006: Reusable Sound Interfaces and Structural Scopes

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Format 8 persists reusable Sound definitions, typed boundary ports, instance
references, and internal graphs. Validation rejects missing or recursive
definitions, and compilation deterministically expands reachable instances for
offline and prepared-realtime rendering. The generated flat node IDs currently
carry instance ancestry textually, but definitions expose no stable parameter
interface, ordinary nested nodes retain the global runtime scope, Event Values
remain project-global, and only one prepared MIDI Voice owner is supported.

M2 needs independent instances that can be automated, controlled, migrated,
and diagnosed without turning display labels, vector positions, generated flat
IDs, or transient runtime integers into persisted identity.

## Decision

### Persisted interface identity

Each `ProjectSoundDefinition` has a positive interface version and ordered
exposed descriptors. Stable identity consists of:

- definition ID;
- exposed port or parameter ID;
- binding target kind;
- definition-local node/parameter ID or local Event Value name/domain.

Display names, ordering, canvas positions, compile indices, generated flat
node IDs, and runtime scope integers are not interface identity.

An exposed parameter declares its `ParameterKind`, value domain where
applicable, typed default, display label, and one or more explicit bindings.
Bindings target an internal node parameter or a declared definition-local Event
Value. Implicit capture by equal text is forbidden.

An instance stores sparse overrides keyed by exposed parameter ID. Missing
overrides use the definition default. Unknown/deprecated overrides remain
recoverable document data and fail closed until an explicit compatibility
alias or format migration resolves them. Display renames do not require
migration; stable-ID changes do.

### Structural expansion manifest

Expansion produces both the executable flat graph and an immutable sidecar
manifest. For every expanded node the manifest records:

- generated flat node ID;
- ordered Sound-instance path;
- owning definition ID;
- definition-local node ID;
- deterministic scope ordinal;
- structural source label used by diagnostics and prepared-target resolution.

The manifest, not parsing of the generated ID, is authoritative after
expansion. Runtime scopes are compact preparation-only handles allocated
deterministically from the manifest and tagged by prepared generation. Root
nodes retain `globalRuntimeScope`. MIDI voice scopes use the same checked
allocator and cannot collide with ordinary nested scopes.

### State and control ownership

Each prepared Sound instance owns independent kernel objects, DSP state, local
Event Value projection, feedback state, MIDI voice state, and analysis scope.
Immutable resources may be shared explicitly. Mutable state is never shared in
M2.

Root Event Values remain global for compatibility. A definition-local Event
Value exists only through a declared interface binding and is instantiated once
per Sound scope. Global-to-local control crosses that binding explicitly; local
names neither shadow nor capture global names.

Preparation resolves structural identities into generation-scoped numeric
handles. Callback code never reconstructs paths, hashes names, or traverses the
document.

## Persistence and migration

The interface schema introduces project format 9.

- Versions 1-7 continue their existing migrations.
- Format 8 definitions migrate to interface version one with empty exposed
  parameters, unchanged ports, graphs, and instance behavior.
- Existing flat projects remain root-scoped and behaviorally unchanged.
- Interface aliases are explicit, collision-checked, and one-way; they do not
  silently retarget ambiguous data.
- Unknown future format versions remain rejected.

## Failure and realtime policy

- Duplicate IDs, invalid defaults, incompatible targets, dangling bindings,
  ambiguous aliases, scope exhaustion, or stale prepared generations fail
  before publication.
- Failed expansion/preparation leaves the previous realtime graph active.
- All scope maps, local bindings, and instance state are allocated before
  playback.
- Callback routing uses numeric scope/target handles and remains allocation-,
  lock-, parsing-, logging-, and I/O-free.

## Rejected alternatives

- Treating generated flat IDs as the only structural identity would couple
  persisted automation and diagnostics to one expansion encoding.
- Using display names as interface keys would make ordinary UI renames
  destructive.
- Implicit Event Value capture would create cross-instance crosstalk and make
  global/local ownership unknowable.
- Sharing one mutable definition instance would violate existing kernel-state
  isolation and complicate safe graph retirement.
- Persisting runtime scope integers would leak preparation order into the
  project format.

## Acceptance evidence

- Format 8 migrates canonically to format 9 without changing output.
- Two instances of one definition have deterministic but distinct scopes and
  independent defaults, overrides, local Event Values, DSP state, and MIDI
  state.
- Display rename and descriptor reordering do not change qualified semantic
  identity.
- Explicit alias fixtures migrate; ambiguous or missing bindings preserve data
  and fail closed.
- Offline and irregular prepared-realtime renders use the same structural
  manifest and agree sample-for-sample within the declared tolerance.
- A stale generation/scope handle cannot mutate another instance.

## Amendment (2026-10-13): a Sound's input may bind a Variable

Taken as recommended under the owner's standing approval of 2026-10-12.
A class is made from an example Sound whose varying parameters are
variables, Variables or SoundCollectionVariables; each becomes a parameter
of the class, and a Variable becomes a parameter of type sound, the
class's input: dragging a Chopper into it plays the example with the
Chopper where the Variable was.
Our Sound definitions bound an input only to an interior node's input, and
a Variable has none to stand in for.

- An input boundary may now name a Variable (`talas.variable`) by its output,
  `audioOut`: the boundary binds the Variable itself. No field is added and
  the project format does not change, since such a boundary was refused
  before.
- Expanding an instance whose bound input is connected replaces the
  Variable by the Sound connected there: whatever heard the Variable hears
  that Sound, the Variable is left silent and unheard, and its declaration
  is not consulted. Such an input is optional: an instance that leaves it
  unconnected plays the Variable as before, its declared selection as the
  Default Sound, or silence. Two Sounds connected there are refused.
- Inlining such an instance (Expand) leaves an
  Annotation fed by the connected Sound where the Variable was.

The alternatives were an exposed parameter of a new `sound` kind holding a
Sound definition reference (a format change, and a second way to connect
Sounds besides connections), or leaving the class's input to a Script
(ADR-0015's 2026-10-12 amendment). A SoundCollectionVariable class
parameter, which would need an instance input taking many
connections, is not covered.

## Amendment (2026-10-13, later): a Sound's input may bind a SoundCollectionVariable

Taken as recommended under the owner's standing approval of 2026-10-12.
Each SoundCollectionVariable of an example Sound becomes a parameter of
the class that holds any number of Sounds, as a Variable becomes one that
holds a Sound.

- An input boundary may name a SoundCollectionVariable by its output: the
  instance's input then takes any number of connections (`acceptsMany`)
  and is optional.
- An instance that connects Sounds there plays its own copy of the
  definition in which the collection is gone and each Sound, in the order
  of the connections, reaches every input the collection fed through an
  input of the copy's own; the copy is made as the project compiles, with
  the declarations the definition owns. An instance that connects none plays the collection's
  declaration, as the definition alone does (ADR-0017's 2026-10-12
  amendment).
- Inlining such an instance connects each Sound where the collection was
  and removes the collection.

The alternative was to expand the collection inside the instance expansion
itself, which runs after the collections of definitions are expanded and
would have needed a second expansion of the declared Sounds.
