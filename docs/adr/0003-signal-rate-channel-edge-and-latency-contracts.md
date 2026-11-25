# ADR-0003: Signal, Rate, Channel, Edge, and Latency Contracts

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

`PortType` identifies broad data families but does not state evaluation rate,
channel cardinality, gate/trigger interpretation, or latency. Numeric parameter
domains carry units, while connected controls and temporal nodes rely on
implementation conventions. A shared kernel needs those conventions explicit.

## Decision

Each kernel-facing port has a prepared `SignalContract` derived from its node
descriptor. The contract contains:

- data family: the existing `PortType`;
- rate: `Constant`, `Block`, `Sample`, or `Event`;
- numeric domain where applicable: the existing scalar, hertz, seconds, note
  number, or samples domain;
- channel layout: fixed cardinality for M1, with named mono ports preferred to
  implicit packed stereo;
- edge semantics: `Continuous`, `Gate`, or `Trigger`;
- latency: `None`, fixed whole frames, or explicitly variable/unknown.

M1 kernels accept fixed prepared layouts only. Dynamic buses and packed
multichannel Sound objects remain deferred. A future extension may add channel
negotiation, but it must not reinterpret an M1 mono port silently.

## Frame and buffer rules

- A process call covers the non-empty half-open interval
  `[absoluteFrame, absoluteFrame + frameCount)`.
- All audio/control buffers expose exactly `frameCount` readable or writable
  frames. No kernel owns or resizes them.
- Constant and block values are immutable for the process call. Sample-rate
  values expose one finite value per frame. Connected input precedence remains
  connected sample value, then live prepared fallback, then static compiled
  fallback.
- Numeric conversion requiring sample rate or tempo occurs during preparation
  or through a prevalidated parameter update, never by parsing in `process`.
- Non-finite connected data fails according to the node's declared policy; it
  is never silently promoted to valid capability evidence.

## Edge rules

- A Gate is closed at `<= 0` and open at `> 0`. State changes are observed
  before processing the sample at which the value changes.
- A Trigger fires on a closed-to-open transition. Holding a positive value does
  not retrigger unless a descriptor explicitly declares level-triggered legacy
  behavior as a limitation.
- Typed events may supply explicit gate/trigger edges. A legacy scalar gate is
  adapted to the same edge sequence using node-local prior-sample state.
- Events at equal frame offsets retain ingress sequence order. An event at
  `frameCount` belongs to the next process call.

## Latency rules

Latency is an immutable prepared property. Fixed latency is represented as a
whole-frame count in the active sample-rate epoch. `None` means zero frames;
`VariableOrUnknown` prevents a node from claiming latency-aligned capability.
M1 records latency but does not add graph compensation. Fractional delay and
dynamic latency require a later ADR and must not be rounded into a false fixed
claim.

Stateful lookback or internal history is not automatically output latency. A
kernel declares latency only when its output time relationship is delayed.

## Rejected alternatives

- Inferring rate from `PortType::Control` would preserve the current ambiguity.
- Treating every control as sample-rate would waste work and obscure update
  semantics.
- A single integer channel count without layout policy would allow accidental
  packed-stereo compatibility claims.

## Acceptance evidence

- Descriptor validation rejects nonsensical combinations such as Event data at
  Sample rate or Trigger semantics on a SampleBuffer.
- Oscillator proves sample-rate frequency/modulation and trigger reset.
- Filter proves sample-rate controls, persistent history, and declared latency.
- ADSR proves scalar-gate adaptation and explicit typed gate events produce the
  same edge sequence.

## Amendment (2026-10-10): positive, not nonzero

A trigger could fire when a value becomes nonzero or when it becomes
positive. A note-on, including a script note, sets `!KeyDown` to -1 and,
five milliseconds later, to +1; the -1 tells an envelope to decay rapidly
to zero and the +1 begins its attack. Under a
nonzero rule the -1 would fire and the +1 could not. Triggers and Gates
therefore keep firing on becoming positive, as the edge rules above say:
a -1 closes a Gate and fires no Trigger, and a held key that dips to -1
and back fires once more (owner decision 2026-10-10). The rapid decay
of an envelope on a -1 Gate is a separate change.

## Amendment (2026-10-10): a negative Gate decays an envelope rapidly

From project format 88, while an ADSR's or AR's Gate is negative its
release runs in at most 2 ms instead of ReleaseTime (the time is ours,
well inside the 5 ms dip of a note-on), so the envelope falls to zero and
the following +1 attacks from zero. A Gate of 0 still
releases over ReleaseTime, and typed gate events, which carry no sign,
are unchanged. Older projects keep the release over ReleaseTime
(`negativeGate` `legacyReleases`). The MultiSegmentEnvelope and
GraphicalEnvelope, which have no release, do not decay on a negative
Gate yet.

## Amendment (2026-10-12): segment envelopes decay too

A -1 Gate tells any envelope, not only an ADSR, to decay rapidly to zero,
so the rule applies to every envelope Sound. A
MultisegmentEnvelope or GraphicalEnvelope has no release to speed up: from
project format 88, when its Gate goes negative while it plays it falls in
a straight line from its current output to zero in 2 ms (the same time as
above, ours), whether its lists are static or hot, and waits there; the
next positive Gate starts it again from its first breakpoint. Once begun
the fall runs to zero even if the Gate returns to 0, and a Gate that
becomes positive first cuts it short with a new start. The Prototypes
Reference pages say nothing more. Formats 1 through 87
acquire `negativeGate` `legacyReleases` and play on through the dip as
before; format 88 is reused because no format 88 project left this
branch before the change (owner plan 2026-10-12).
