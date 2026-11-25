# ADR-0025: DelayWithFeedback's Wavetable, Prezero and SmoothDelayChanges

Status: Accepted (owner decision 2026-10-04)

## Context

A DelayWithFeedback needs three fields
ours lacked:

- Wavetable: almost always Private, so the delay gets memory of its own;
  delays and resonators that name the same wavetable share a single delay
  line.
- Prezero: checked, the delay line starts empty when the program starts;
  unchecked, it may hold what earlier programs left in it, which models a
  resonator that keeps its state between excitations.
- SmoothDelayChanges: checked, the delay changes slowly to a new value;
  unchecked, it changes at once.

Ours gave every delay a private line, emptied it whenever the transport
restarted, and moved the delay at once.

## Decision

`talas.delayWithFeedback` gains the three fields. Their defaults, Private,
Prezero on and SmoothDelayChanges off, keep older projects' sound, so the
project format does not change.

- Wavetable: any name other than Private shares one delay line among all
  the DelayWithFeedbacks of the Sound that give that name, MIDI voices
  included. The line is one frame longer than the longest of their delays.
  At each frame every one of them reads its own delay from the line and
  adds its write, its scaled Input plus its feedback, to the line's current
  frame, so the line carries the sum of their writes: one with Scale 0 and
  Feedback 0 is a tap on the others' line. Their feedbacks add up, so
  together they should stay at most 1. A frame at which none of them ran
  holds silence. A Sound with a shared line renders one frame at a time in
  both renderers, so each delay hears what the others wrote at earlier
  frames whatever the block size.
- Prezero: we keep no memory between programs, so every line starts empty.
  When the transport restarts in realtime, or an offline render is reset, a
  line is emptied if Prezero is on and kept if it is off, so a delay's tail
  carries over the restart. A shared line is kept only when all of its
  delays have Prezero off.
- SmoothDelayChanges: the delay moves toward a new value by at most half a
  frame per frame, so a change of Delay or DelayScale glides, bending the
  delayed signal's pitch by at most half, instead of jumping. The rate is
  our choice. With Interpolation None the gliding
  delay still reads the nearest frame.

## Consequences

Shared lines cost frame-by-frame rendering. Sounds with other Wavetable
fields, such as the AveragingLowPassFilter, still take only Private.

## Amendment (2026-10-04): ReverbSection

The ReverbSection is the same as DelayWithFeedback except that DecayTime
sets its feedback, and it has the
same Wavetable and Prezero. Under the decision above it now shares a named
Wavetable's line with the DelayWithFeedbacks and ReverbSections naming it,
reading its delay and adding its write as they do, and keeps its line over
a restart when Prezero is off. Its SmoothDelayChanges (this amendment
first missed that field) glides its delay as a
DelayWithFeedback's does from project format 69; older projects keep it
off, since it had no effect before.

## Amendment (2026-10-10): HarmonicResonator

Owner decision 2026-10-10. The HarmonicResonator's Wavetable is the name
of a wavetable to use as a delay line, or Private, and resonators are among
the Sounds that share one. A
HarmonicResonator naming a Wavetable now shares the line of the
DelayWithFeedbacks, ReverbSections and HarmonicResonators naming it: it
reads its period, 1/Frequency, back from the line (its allpass reading
one frame further) and adds its write, its scaled Input plus its feedback,
to the line's current frame, as they do. The line covers its period, or
the bounded history when its Frequency changes, and is kept over a
restart when all its Sounds have Prezero off. The AveragingLowPassFilter
still takes only Private memory.
