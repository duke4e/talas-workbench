# ADR-0020: Sounds in parameter fields

Status: Accepted (owner decision 2026-10-02)

## Context

One Sound should be able to control a parameter of another, like a control voltage:
the Sound is pasted into the parameter field and named there with `L`, `R`
or `M` for its left channel, its right channel or their mix, and the field
can do arithmetic with it, for example `Oscillator1 L nn + 2 c` or
`GraphicalEnvelope1 L * 12 nn + 2 c`. The field
is re-evaluated once per millisecond; `Oscillator L: 5` re-evaluates it
every 5 ms.

Talas could only connect a signal to a parameter's own control input, which
replaces the field: no arithmetic, units or channel choice, and parameters
without such an input took no signal at all.

## Decision

An identifier followed by `L`, `R` or `M`, or by the keyword `L:`, `R:` or
`M:` and a number of milliseconds, is a Sound reference. The expression
language records it as one identifier whose name is the Sound and the
channel (`Oscillator1 L`, `Oscillator1 L: 5`), so parsing, the identifier
list and evaluation need no new value kind.

Before expanding Sounds, compiling finds every parameter expression whose
Sound references name nodes of the same graph. For each such parameter it
adds a `talas.pastedSoundExpressionV1` node that takes the referenced outputs
on its `signals` input (left for `L`, the right output or else the left for
`R`, both for `M`), evaluates the field's expression in the parameter's
domain each millisecond, or at the shortest period the references name,
holding each reference's value between its own ticks, and drives the
parameter's control input with the result. The parameter keeps its value
with every reference at 0 as its static value. The project document keeps
the field as written.

Parameters without a control input are refused with a diagnostic naming the
field.

Amendment (2026-10-02): a field splits into a skeleton and live parts. Each
largest part without a Sound reference that reads Event Values or variables
or calls a function becomes one of up to eight live fields of the added node
(`live1` to `live8`), evaluated like any field, with the unit its value
carries; the skeleton names them `__kc_live_1` and so on, and holds only the
Sound references, literals, operators and units, evaluated each millisecond
with the live parts' latest values. `lfo L * 12 nn + !Pitch nn` follows
Pitch at the next millisecond. Fields that apply a list, a user word or a
function that keeps state or follows time to a Sound reference, that need
more than eight live parts, or that name a Sound outside the graph are
refused.

Amendment (2026-10-18): a field that holds only another Sound's name, its
`L` deleted, takes that Sound at the audio rate (for example a VCF's
Amplitude and Resonance): compiling connects the Sound's left output
to the parameter's control input, sample for sample, as a connection does.
A live part keeps the unit its value carries: Val converts a frequency
to a pitch and back, so the part takes the unit in which its value is its
own number (`SignalProcessor halfSampleRate hz` stays 24000 hz, where it had
become 138.2 nn). A field its Sounds leave undefined for a moment, such as
the pitch of a pasted SpectrumFundamental's 0 hz before the analysis has a
frame, holds its last value, 0 at the start, instead of stopping the
Sound; the parameter saturates it at its range, a FormantBank's Frequency at
its lowest, as a live value does (owner decision 2026-10-12).

Amendment (2026-10-22): a field may apply a function that keeps state or
follows time to its Sounds, such as
`((Threshold2 L nextRandom * 6) of: #(0 2 4 7 9 12)) nn + !Pitch`. The
skeleton keeps an expression state of its own, fixed in size, from one
millisecond's evaluation to the next, its clock the frames the field has
played; the parameter's own value is the field evaluated from a fresh
state with its Sounds at 0. An `of:` list beside a Sound splits like any
other part, its index and elements examined for Sounds and live parts.

Amendment (2026-11-16): a Constant's Value may hold a pasted Sound. Compiling then puts the Pasted Sound Expression in the
Constant's place under its name, its output `value` as the Constant's was,
so that its connections and the fields reading it stand; the field's value
loses its units, as a Constant's does, and under the Constant's `valueRange`
choice `unitRange` it is kept from -1 to 1.
Two project choices of format 156, each off by default so that projects
play as they did, set how fields read their pasted Sounds, passed to the
expression as its `range` and `reads` choices: `pastedSoundsSaturateAtUnity`
reads each Sound saturated at -1 and 1, a Sound's output range; `pastedSoundsReadThePreviousMillisecond` reads each as it stood on
the frame before its tick, so that a field lags its Sound by up to a
millisecond and each further paste, a Constant pasted into a Constant and
so on, adds a millisecond. By default a Sound is read on its tick
itself, so that a chain of pasted Sounds changes on one boundary. A field
holding only a Sound's name, taken at the audio rate, is a connection and
reads neither way.

## Consequences

- A field with a pasted Sound changes at 1 kHz, not at the
  sample rate, and lags its Sound by up to one period.
- The referenced Sound plays even when nothing else uses its output, since
  the added node consumes it.
- An Event Value beside a Sound reference reaches the field at the next
  millisecond, not on the frame it changes.
