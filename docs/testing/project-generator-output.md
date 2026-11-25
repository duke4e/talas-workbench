# Project generator output

Talas project generators are bounded authoring tools. They are not audio
nodes, background transports, or realtime script engines.

The Workbench Generator inspector exposes four separate operations:

- **Preview Symbols** runs the deterministic grammar and retains a text-only
  preview. It does not author Timeline events, Score content, or audio.
- **Generate Timeline** maps declared terminal actions to canonical Timeline
  Event Value rows. Applying the complete candidate is one Undo step.
- **Generate Score** maps declared terminal actions to musical-tick clips,
  automation points, and markers. Applying the complete candidate is one Undo
  step.
- **Generate Sound** maps only declared `graphNode` actions to one new linear
  reusable Sound. Applying the complete candidate is one Undo step; it does
  not instantiate, stage, or play the Sound.

Each output profile stores exact terminal bindings, integer Timeline and Score
grid steps, and closed action records. The command also supplies an exact base
time or musical tick and a bounded generated-entity namespace. Unbound
terminals are rests but still advance the grid. Repeating the same Score
request therefore collides with the same deterministic IDs instead of silently
overwriting prior output.

Generation and projection happen off the audio callback. The candidate is
published only if its generator, output profile, project authority, canonical
project bytes, targets, capacities, checked time arithmetic, and selected
catalog descriptor fingerprint still match. Cancellation, stale authority,
missing targets, changed catalog authority, duplicate same-time targets,
identity collisions, overflow, or validation failure applies no partial edit
and leaves the active graph unchanged.

Generated Timeline events use the existing project staging and prepared
typed-event scheduler. Generated Score content uses the existing Score
validation, preparation, routing, and playback authorities. No generator-owned
realtime queue, direct graph mutation, wall clock, file, network, process,
plugin, or native-call capability is introduced.

Project format 31 stores generator-library schema version 2 and its output
profiles. Formats 1 through 29 migrate with an empty generator library. Format
30 generators retain their exact grammar and execution settings but migrate as
preview-only, with no invented output actions.

Project format 32 adds the closed `graphNode` action used by the restricted
linear reusable-Sound constructor. The persisted action stores only one
bounded builtin node type ID. Catalog-derived validation rejects unavailable,
external, device, event/transport, executable-text, resource-bearing, and
noncanonical Audio-port shapes before publication. Formats 1 through 31
migrate without graph actions and never invent nodes or Sound definitions.

## Restricted Sound-construction scope

The format-32 constructor is deliberately smaller than a general graph
generator. The first emitted node must be one approved Audio source. Every
later node must be an approved processor with exactly one required Audio input
and one Audio output. Nodes receive deterministic namespace-derived IDs,
consecutive nodes receive exactly one connection, catalog defaults remain the
parameter authority, and the final node becomes the Sound's sole `audioOut`.
The complete definition is validated as ordinary project state before it can
be appended. The fixed graph limit is 256 nodes.

The Workbench picker lists only catalog entries accepted by that same closed
validator. A generated Sound becomes executable only after the user explicitly
instantiates it in an ordinary graph; reusable-Sound expansion, offline render,
and prepared realtime then use the normal runtime authorities.

Branching, fanout, feedback, arbitrary port or connection selection, graph
replacement, mutation of existing graphs, generated parameter overrides,
assets/resources, external or isolated components, Variables, Event Values,
MIDI, Score, snapshots, device I/O, direct staging, and direct callback
generation are deferred. The generator grammar still has no reflection,
object messages, native calls, filesystem, network, or process access.

## Broader script-language decision

C7 deliberately stops at this bounded authoring boundary. The historical
`talas.contextFreeGrammar`, `talas.limeInterpreter`, and `talas.script` catalog entries
remain compatibility placeholders; they are not the generator language and do
not imply Smalltalk, a notation interpreter, or arbitrary script execution. Their descriptors
publish `arbitrary-script-runtime-unsupported`, and the shared restricted-graph
validator excludes all three from both planning and the Workbench picker.

Any future broader language requires a new versioned syntax/VM, explicit
parser/instruction/time/memory/state/capability budgets, migration and rollback
rules, fuzz/sanitizer evidence, and separate security approval under ADR-0015.
Legacy placeholder bytes cannot be reinterpreted to opt into it.
