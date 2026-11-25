# ADR-0023: TimeOffset starts its Input late

Status: Accepted (owner decision 2026-10-03)

## Context

A TimeOffset offsets the start time of its Input by its SilentTime: the
Input starts SilentTime later. Ours delays the Input's samples through a delay line. A Sound that
depends only on its own time sounds the same either way, but a live Input
does not: an audio input, MIDI and Event Values are heard SilentTime late
instead of live once the Input has started, and the delay line's memory
grows with SilentTime.

WaitUntil already starts its Input late: the Sounds that reach an output
only through its Input do not run until its Resume opens it.

## Decision

A TimeOffset with a fixed SilentTime starts its Input as a WaitUntil does,
at the SilentTime's frame:

- Until then the Sounds that reach an output only through the TimeOffset's
  Input do not run and keep their initial state; the TimeOffset is silent.
- From then on they run from their initial state, and the TimeOffset passes
  its Input through. Live inputs, MIDI and Event Values reach the Input as
  they arrive, not delayed.
- Sounds that also reach an output another way already run, so the
  TimeOffset lets them through from SilentTime without restarting them.
- Both renderers split their blocks at the starting frame, so the start is
  sample-accurate without rendering one frame at a time.

A SilentTime fed by a Sound, Reverse or Retrograde (but not both), which put
the silence after the Input, and a TimeOffset inside a MIDIVoice keep the
delay line. A TimeControl with a fixed Rate still paces SilentTime
(ADR-0019). Projects before format 64 keep the delay line (`offsetMeans`
`legacyDelay`).

## Consequences

A TimeOffset needs no memory for its SilentTime and plays live Inputs live,
alike offline and in realtime. Time functions such as `!LocalTime` inside
the Input still count from the start of the whole Sound.

## Amendment (2026-10-04): SetDuration's StartTime

Owner decision 2026-10-04. A SetDuration sets the duration and start
time of its Input, as dragging the Input into a timeline would: StartTime is when the Input starts, relative to the
SetDuration. Ours delayed the Input's samples by StartTime, as the
TimeOffset did.

A SetDuration with a fixed, positive StartTime starts its Input as a
TimeOffset does: the Sounds that reach an output only through its Input run
from StartTime, and it passes them through for Duration, silent before and
after. It nests with TimeOffsets and WaitUntils as they nest with each
other. A StartTime of 0 has nothing to start late. A StartTime fed by a
Sound or reading Event Values, variables or time, and a SetDuration inside a
MIDIVoice keep the delay line, as do projects before format 66
(`startTimeMeans` `legacyDelay`).

## Amendment (2026-10-10): voices and moving or Sound-fed fields

Owner decision 2026-10-10. A MIDIVoice's Input runs from the MIDIVoice's
start, not from each note, and a field that is not
hot holds one value for the whole Sound.

- A TimeOffset or SetDuration in a flat MIDIVoice's Input starts its Input
  late as it does outside one, counted from the MIDIVoice's start: a voice
  belongs where its MIDIVoice does, so inside a TimeOffset's Input it
  counts from when that TimeOffset starts it. Later notes are heard as
  they come. Sounds with nested MIDIVoices still delay their Input's
  samples.
- A SilentTime or StartTime fed by a Sound, or following Event Values,
  variables or time, is read once, when the activation schedule first
  reaches the step (from the Sound's own buffer at that frame), and the
  step then starts its Input that much later. Such a step has no fixed
  starting frame, so both renderers follow its Sound a frame at a time,
  as they do a WaitUntil.
- Reverse or Retrograde (but not both) still keep the delay line.

Projects from format 64 (TimeOffset) or 66 (SetDuration) through 86 keep
the earlier rule through `offsetMeans` or `startTimeMeans`
`legacyFixedStartsLate`.

## Amendment (2026-10-12): nested MIDIVoices

Owner plan 2026-10-12. From project format 89 a TimeOffset or SetDuration
starts its Input late in a Sound with nested MIDIVoices as well. A nested
MIDIVoice belongs where its own MIDIVoice does, as a voice belongs where
its MIDIVoice does, so a TimeOffset anywhere inside counts from where the
outermost MIDIVoice starts: a nested voice's prepared DSP
runs on between its parent's notes, and the step's start is never reset
by them. The activation schedule already drives nested voices' Inputs (a
WaitUntil in one waits as it does elsewhere), so both renderers start
such Inputs through it. Reverse or Retrograde still keep the delay line.
Projects of formats 87 and 88 keep starting an Input late only in Sounds
without nested MIDIVoices through `offsetMeans` or `startTimeMeans`
`legacyFlatVoicesOnly`; older projects keep their earlier choices.

