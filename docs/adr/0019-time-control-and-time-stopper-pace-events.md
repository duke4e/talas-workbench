# ADR-0019: TimeControl and TimeStopper pace their Input's events

Status: Accepted

## Context

Every Sound has a start time and a duration on its Input's clock.
TimeControl sets how fast that clock runs: it
moves the start times of the events in its Input and their durations, not
the sample rate. TimeStopper lets its Input start and then stops
its clock until Resume: the Input keeps sounding and cannot end, and later
events wait.

Talas replayed a TimeControl's Input from a history at Rate, which changed
its pitch (and failed above Rate 1, which would need future samples), and a
TimeStopper held its Input's current sample until Resume, which silenced an
oscillator.

## Decision

TimeControl and TimeStopper pass their Input's samples through. A new
choice records the behaviour: TimeControl `rateChanges` (`eventTimes`, or
`legacyPlaybackSpeed` for the replay) and TimeStopper `whileStopped`
(`inputPlays`, or `legacyHold`). Project format 45 gives every TimeControl
and TimeStopper in an older project the legacy choice, so it plays the same
samples; formats before 45 reject the new parameters.

## Consequences

- An oscillator under a TimeStopper sounds while time is stopped, and a
  TimeControl no longer transposes its Input.
- Rate and Resume do not yet reach the timing inside the Input: a
  SetDuration or TimeOffset under them still counts real time. Pacing those
  on the enclosing clock is the next step of this decision.

## Amendment (2026-09-30): a fixed Rate paces the Input's timing

Compiling divides the StartTime and Duration of every SetDuration, and the
offset of every TimeOffset, inside a TimeControl with a fixed Rate by that
Rate, when all the node feeds reaches an output only through the
TimeControl's Input. Nested TimeControls multiply. A Rate that moves (an
Event Value or a connected Sound) and TimeStopper's Resume still leave the
timing inside at real time.

## Amendment (2026-10-05): a moving Rate and a TimeStopper's Resume pace it

A TimeControl whose Rate is fed by a Sound or follows Event Values,
variables or time, and a TimeStopper, run a clock for the timing nodes
inside their Input, chosen as the fixed-Rate amendment chooses them. The clock advances by Rate each
sample; a TimeStopper's stands until Resume opens, as a WaitUntil's does,
and then advances one frame a frame; nested clocks multiply. A TimeOffset
or SetDuration that starts its Input late (ADR-0023) starts it when its
clock, running since the activation schedule first reaches it, passes its
delay; a SetDuration counts its Duration on its clock. Both renderers
follow such a Sound a sample at a time, as they do a WaitUntil, so offline
and realtime start and stop at the same frames.

Each frame's Rate and Resume are read once the frame has rendered, from
the field or from the Sound feeding it, so a clock reads a Sound's value
as the Sound makes it. Timing inside a MIDIVoice, and a TimeOffset or
SetDuration that delays its Input's samples, still count real time. A TimeControl that replays its Input (`legacyPlaybackSpeed`) still
takes a fixed Rate.

## Amendment (2026-10-10): voices and timing read as it starts

With ADR-0023's 2026-10-10 amendment, the timing a clock paces includes a
TimeOffset or SetDuration in a flat MIDIVoice's Input and one whose
SilentTime or StartTime is read as it starts; that delay, once read, is
compared with the clock. TimeControls and TimeStoppers in a flat
MIDIVoice's Input run clocks for its timing. Timing in Sounds with nested
MIDIVoices, timing that delays its Input's samples (`legacyDelay`, and
`legacyFixedStartsLate` projects' moving or Sound-fed fields and voices),
and a Sound-fed field under a TimeControl with a fixed Rate (paced when
compiling, which reads only expressions) still count real time.

## Amendment (2026-10-12): nested voices and Sound-fed timing under a fixed Rate

Compiling paces timing under a TimeControl with a fixed Rate by dividing
the timing fields' expressions by Rate, which leaves a SilentTime or
StartTime fed by a Sound in real time. From project format 89 the value
such a step reads from its Sound as it starts is divided by the fixed
Rates of the TimeControls whose Input the step reaches an output only
through, as compiling finds them, so it lasts as long as the same value
written as an expression. Clocks also pace timing in Sounds with nested
MIDIVoices (ADR-0023's 2026-10-12 amendment), and a TimeControl or
TimeStopper whose Input holds a MIDIVoice paces the timing in its voices,
whose outputs reach the MIDIVoice outside the plan's inputs, as compiling
already paced an expression there. Projects of formats 87 and 88
(`legacyFlatVoicesOnly`) keep reading such a Sound in real time, run no
clocks for timing in Sounds with nested MIDIVoices, and pace only timing
inside a TimeControl or TimeStopper in the same voice. A SetDuration's
Duration fed by a Sound is still not paced by a fixed Rate.


## Amendment (2026-10-19): a Sound-fed Duration under a fixed Rate

Taken as recommended (standing approval 2026-10-12). From project format 96 a SetDuration's Duration fed by a
Sound is divided, frame by frame, by the fixed Rates of the TimeControls
the SetDuration reaches an output only through, as compiling divides a
Duration expression, so it lasts as long as the same value written as an
expression. Projects of formats 89 to 95 whose SetDuration starts its Input
late (ADR-0023) acquire `startTimeMeans` `legacyDurationUnpaced`, which
starts it alike and counts the Duration in real time; older choices
already read a Sound in real time.
