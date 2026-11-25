# ADR-0022: RE resonator tables and analysis

Status: Accepted (owner decision 2026-10-03)

## Context

An REResonator is a time-varying filter whose coefficients come from
analyzing a recording with the RE Analysis Tool. RE analysis treats a sound as an excitation feeding a
resonator. The REResonator is the resonator and its Input the new
excitation; its Wavetable holds the analysis's sequence of coefficient sets
and its TimeIndex, from -1 to 1, reads from the first set to the last. The
RE Analysis Tool takes a Filter order of about twice
the number of resonant peaks, a number of filter updates per second and an
averaging time of about twice the period of the sound. It writes the
resonator into an RE file and, if asked, the excitation that recreates the
sample exactly into an EX file. The filter overflows easily
unless its Input is attenuated, by as much as 0.001.

Talas had no RE file format and read the Wavetable as one fixed set of FIR
coefficients, a filter that neither resonates nor changes.

## Decision

An RE table is a numeric table resource of our own format: its first value
is the filter order, an integer from 1 to 128, and the rest are the
coefficient sets in time order, each the order's number of reflection
coefficients, every one finite and between -1 and 1 exclusive.

The REResonator is the all-pole lattice filter those coefficients describe,
without gain, so the EX file recreates the sample and other inputs need the
attenuation described above. TimeIndex -1 reads the first set and 1 the
last; between two sets the coefficients are interpolated linearly, which
keeps the filter stable. TimeIndex may be an expression, an Event Value or a
connected or pasted Sound, read at every sample.

The RE analysis (`talas_cli re-analysis`) computes one coefficient set per update
from the autocorrelation of a Hann-windowed stretch of the averaging time
centred on it (Levinson-Durbin), and the EX file as the sample through the
inverse lattice whose coefficients move as a TimeIndex ramp from -1 to 1
over the sample's duration would move them.

Projects before format 63 keep the static FIR reading (`filterForm`
`legacyFir`).

## Consequences

An REResonator resonates and follows its TimeIndex alike offline and in
realtime. RE files in other formats do not load; analyses must be made
with the `re-analysis` command. The analysis parameters' defaults (order 32, 100 updates per second,
20 ms) are our choice.
