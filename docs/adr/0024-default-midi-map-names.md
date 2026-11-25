# ADR-0024: Default MIDI map names

Status: Accepted (owner decision 2026-10-04)

## Context

A global map associates memorable Event Value names with MIDI
controllers, and the Default map is the global map until another is chosen. A Sound can read `!Volume` or `!DamperPedal` and
play from a MIDI fader without a map of its own. Ours names a controller
only by number (`!cc07`, with `!bt07` and `!sw07` as button and switch) and
pitch bend as `!MIDIPitchBend`, so a Sound written with such names hears
nothing from MIDI.

The Default map names `!PitchBend`, which ranges from -1 to 1, and these
controllers:

| Event Value | Controller | Displayed as |
| --- | --- | --- |
| `!DamperPedal` | 64 | gate |
| `!DetuneDepth` | 94 | fader |
| `!EffectControl1` | 12 | fader |
| `!EffectControl2` | 13 | fader |
| `!ExpressionController` | 11 | fader |
| `!ExternalEffectsDepth` | 91 | fader |
| `!FootController` | 4 | fader |
| `!HarmonicContent` | 71 | fader |
| `!Hold2` | 69 | toggle |

`!Volume` is the conventional Event Value name for amplitude; MIDI's
channel volume is controller 7.

## Decision

These names, and only these, follow MIDI through the Default map:

- A controller on the default MIDI channel sets its name's Event Value to
  the controller's value from 0 to 1, as it sets `!ccNN`; pitch bend sets
  `!PitchBend` from -1 to 1, as it sets `!MIDIPitchBend`. Live MIDI and
  MIDI files alike.
- How a name is displayed concerns the Virtual Control Surface only: a
  control made for `!DamperPedal` is a gate and one for `!Hold2` a toggle,
  but both carry the controller's value.
- A MIDIMapper's Map that names the same Event Value takes precedence for
  the Sounds it maps, as a local map overrides the global map, and
  so does a Virtual Control of that name with its own MIDI or OSC mapping.

No other controller names are mapped by default; they are not guessed.

## Consequences

Sounds written with these names play from MIDI controllers without a map.
The names stay ordinary Event Values otherwise: the Virtual Control Surface,
snapshots and OSC set them as before.

## Amendment (2026-10-19): an editable global map

Taken as recommended (Workbench survey item 8; standing approval
2026-10-12). A global map is a text of
associations, which a project may now hold as its own (format 98,
`globalMap`), edited in Tools > Global Map; the Default map above is its
text when the project has none (`defaultGlobalMapText`), and a project's
own map replaces it.

- One association a line, in the syntax a MIDIMapper's Map reads:
  `!Name is: `MIDIControllerNN.`, `!Name is: `MIDIPitchBend.`, or a
  controller with options in parentheses, `channel:`, `min:` `max:`
  `grid:`, `taper: #log` and `displayAs:` separated by semicolons.
  Lines in double quotes are comments. Key numbers and other sources are
  refused; at most 128 associations and 16 KiB.
- A controller sets its name's Event Value on the association's channel
  (else the default one), scaled from 0 to 1 to the range by the taper and
  kept on the grid counted from the minimum, live and from MIDI files
  alike; the pitch wheel sets its names from -1 to 1. `displayAs:` gives
  the Virtual Control's kind (`#nothing` makes none), and a fader the
  range.
- As before, a Virtual Control with its own MIDI or OSC mapping and a
  MIDIMapper's Map take precedence for their names.
