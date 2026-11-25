# ADR-0027: GA analyses are tables of our own layout

Status: accepted (2026-10-19, taken as recommended under the owner's standing
approval of 2026-10-12).

## Context

A GAOscillators adds oscillators of
complex waveforms, each with its own amplitude envelope, all following one
frequency deviation envelope; TimeIndex moves through the envelopes, -1
their beginning, 0 their middle, 1 their end, and Morph crossfades each
waveform and envelope of Analysis0 with those of Analysis1. A GA analysis
holds the waveforms (4096 samples each), then an amplitude envelope for
each, then the deviation envelope, the envelopes of one length that varies
from analysis to analysis. Until now our GAOscillators played computed harmonic amplitude
tables as a bank of sines, its TimeIndex reading constant envelopes.

## Decision

- A GA analysis is a table (`talas/dsp/GaTable.h`): a header of six numbers
  (a magic number, version 1, the waveform count N, the envelope length L,
  the waveform length 4096 and the base pitch in Hz), then N waveforms of
  4096 samples, N amplitude envelopes of L frames and the deviation
  envelope of L frames (the pitch over the base pitch). Its file is a
  32-bit float WAV of those numbers, so the resource loader reads it as any
  table; AIFF GA files are not read.
- A GAOscillators whose Analysis0 is a GA table plays it: each oscillator's
  waveform, crossfaded toward Analysis1's by Morph, read at one shared
  phase, times its amplitude envelope at TimeIndex, crossfaded alike; the
  phase advances by Frequency times the deviation at TimeIndex. Each
  analysis's envelopes are read over their own length, so analyses of
  different lengths morph; both must hold as many waveforms. TimeIndex is a
  field or, since a Sound may drive it, a new optional control input.
  Harmonic amplitude tables keep playing as before.
- `talas_cli ga-analysis` and the Workbench's Tools > GA Analysis make a GA
  analysis from a sample: the pitch over the whole sound (CDP's getpitch), each harmonic's
  amplitude in each frame (100 a second by default), the harmonics grouped
  into waveforms by k-means over their envelopes normalised to their peaks,
  each harmonic's share of its group fitted by least squares, the waveform
  normalised to a peak of 1; the Workbench leaves an example GAOscillators
  at the base pitch, a FullRamp moving its TimeIndex over the sound.

## Consequences

No project format change: no older project names a GA table. GA files in
other formats need converting to this layout. The harmonic grouping is our
choice. Alternatives: reading AIFF GA files by guessing their header; a
JSON analysis format, which the table loader would not read.
