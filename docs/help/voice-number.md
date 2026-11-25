# Vary parameters between MIDI voices

`?VoiceNumber` identifies a prepared MIDI Voice slot, starting at one. The
number belongs to the slot and stays the same when that slot is reused or
stolen for another note.

In the Inspector's expression-language controls, choose **Upgrade to Voice v1**
from a Variables v2 project. The upgrade is undoable. Earlier profiles must
first follow the displayed upgrade steps. New projects still start in Variables
v2, so using this token is an explicit choice.

Set MIDI Voice's `polyphony` to `3` and use the token in a numeric parameter of
a Sound feeding its `audioIn`. For example, Noise's `initialState` can be:

```text
?VoiceNumber * 0.1 + 0.001
```

The three prepared slots receive seeds `0.101`, `0.201` and `0.301`. The value is
specialized before Noise prepares its state, so a static parameter can differ
between voices. Each resulting seed must fit the parameter's legal range.

For another example, Oscillator's `frequency` can be:

```text
?VoiceNumber * 110 hz
```

The slots then use 110, 220 and 330 Hz. Each independent MIDI Voice starts
numbering its own slots at one.

Saving retains the formula, including formulas in reusable Sound definitions
and exposed parameter defaults or overrides. A reusable template can contain
the token before it is instantiated, but each executable instance must belong
to one MIDI Voice input graph. The MIDI Voice's own allocator settings, such as
`polyphony`, cannot use a child voice's number.

The spelling is case-sensitive. This feature supplies a numeric ordinal; it
does not add string concatenation or formulas for resource filenames.
