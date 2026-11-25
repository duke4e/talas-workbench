# ADR-0017: The Mixer and inputs that take many Sounds

Status: Accepted (owner decision 2026-09-29)

## Context

A Mixer should add every Sound in its Inputs field, then set the level of
each output channel with Left and Right. The old
Mixer, `talas.mixer`, took exactly two inputs weighted by two gains and had one
output, and graph validation allowed one connection per input port. Mixing
three Sounds needed a chain of Mixers.

## Decision

A port descriptor may declare `acceptsMany`. Such an input takes any number
of connections; validation still rejects the same connection twice and a
Sound definition mapping two inputs to one port. Compiling binds every
connection, in connection order, as its own input binding, each with its own
signal adapter and latency compensation.

`talas.mixerV2` is the Mixer: a required `inputs` port that accepts many
Audio connections, optional Control `left` and `right` inputs over Scalar
`left` and `right` levels (default 1), and `leftOut` and `rightOut`. It sums
its inputs in binding order with checked finite arithmetic, then multiplies
the sum by each level. The Mixer prototype maps to `talas.mixerV2`.

`talas.mixer` stays registered as the "Two-Input Mixer" so current graphs that
name it still compile, but it records `replacedBy: talas.mixerV2` and the
prototype palette no longer offers it. Project format 43 migrates older
projects: an input whose gain is a plain unit gain connects straight to
`inputs`; any other gain, including a connected, bound or snapshotted one,
moves with its references to a VCA inserted on that input. Consumers of
`audioOut` read `leftOut`, which carries the same mono sum.

## Consequences

- A migrated project plays the same samples: each gain multiplies its input
  once and the two products are added, as before.
- Stereo Inputs (amended 2026-09-30): a second many-connection port,
  `rightInputs`, takes the right channels of stereo Inputs. The left output
  adds `inputs`; the right output adds, for each `inputs` connection, the
  `rightInputs` connection from the same source in its place, or the input
  itself when that source sent nothing to `rightInputs` (a mono Input is
  heard on both sides), then any `rightInputs` source with no `inputs`
  connection. With nothing on `rightInputs` both outputs carry the one sum,
  as before.
- Other Inputs fields (Product, VCA, OverlappingMixer) can use `acceptsMany`
  without a new mechanism.
- A snapshot bank scoped to a migrated Mixer keeps its gain targets, which
  no longer resolve.

## Amendment (2026-10-12): a SoundCollectionVariable in a many-Sound field

Owner decision 2026-10-12. A SoundCollectionVariable stands for a
collection of Sounds and belongs only in fields that take more than one
Sound, such as the Inputs of a Sum; a Script reaches its Sounds as
`orchestra at: n`. The old implementation stood for the one Sound of its
declaration a typed event selected.

- From project format 90, compiling replaces a SoundCollectionVariable
  connected only to `acceptsMany` inputs by the Sounds its visible
  SoundCollection declaration lists, in order, each an instance of its
  Sound connected where the variable was; a Mixer then mixes them all and
  a Script's Inputs count them in order. A collection nothing declares is
  empty.
- Connected anywhere else, or heard directly, it is refused.
- Older projects acquire `members` `legacySelected`, which keeps the
  one-of-N selector with its live selection and crossfade.
- Declarations are looked up in the project and in the Sound whose graph
  holds the variable; a MIDIVoice's own declarations are not yet
  consulted for a collection.

## Amendment (2026-10-13): collections a Script sets or a MIDIVoice declares

Owner decision 2026-10-12, completed. A Script can also set the value of a
SoundCollectionVariable to a collection of Sounds, such as an Array; a
Script's parameter values may be any Smalltalk object, arrays among them.

- A Script's event keyword naming a SoundCollectionVariable in its Input
  takes Smalltalk-80's `Array with: ... with:` of other Inputs, by name or
  `orchestra at: n`, each perhaps given values but no start of its own;
  the event's copy of its Input hears a copy of each, started with the
  event, where the collection was. A collection inside a Script's Input
  waits for the Script's events, and one its event gives nothing plays its
  declaration. Still compile-time expansion of the Script (ADR-0015).
- A collection inside the Sound of a MIDIVoice, MIDIMapper or
  AnalogSequencer finds that voice owner's own declaration (format 29's
  MIDI Voice owner) before the Sound's and the project's.

## Amendment (2026-10-18): the range of Left and Right

A Mixer's Left and Right range from -1 to 1. A level that evaluates beyond
either bound, or
is not finite, is refused as it compiles, as StereoMix's and
OverlappingMixer's already were. The Mixer a Script or ContextFreeGrammar
becomes takes their Left and Right, so it refuses theirs alike.
