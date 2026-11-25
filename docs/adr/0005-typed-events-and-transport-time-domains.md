# ADR-0005: Typed Events and Transport Time Domains

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

The runtime has sample-accurate Event Value transactions, MIDI note ingress,
Timeline seconds, engine-local cursors, and a host-owned transport frame. These
paths are bounded but expose different timing vocabularies. M1 utility and ADSR
work needs one explicit event order and clear ownership of time.

## Decision

Kernel-facing events use a fixed-capacity prepared representation with:

- kind: Event Value assignment, gate edge, trigger, MIDI note on/off, transport
  discontinuity, or an explicitly registered internal kind;
- target: a generation-scoped resolved handle;
- frame offset within the current process interval;
- monotonically assigned ingress sequence for stable equal-frame ordering;
- a trivially owned typed payload with no string or heap ownership.

The representation is internal and may use a tagged union. Producers may own
strings and rich diagnostics outside the callback, but preparation/ingress must
resolve them before publication. Unknown kinds or targets fail transactionally.

At each frame, ordered events are applied before kernels process that sample.
An event at the end of a block belongs to the following block. Equal-frame
events apply by ingress sequence; the last assignment to the same target is the
effective value for that sample while all explicit edges remain observable.

## Time domains

The runtime names and does not implicitly mix these domains:

- **Project time:** persisted finite seconds and, later, musical positions.
- **Transport time:** host-owned monotonic frame plus transport epoch and active
  sample rate. It survives graph replacement within an epoch.
- **Playhead time:** transport frame mapped through the persisted half-open loop
  region for UI and recurring Timeline assignment lookup.
- **Session time:** frames since one prepared session became active; used only
  for node-local compatibility behavior.
- **Block time:** `0...frameCount-1` offset for a specific callback/process
  interval.

Sample-rate changes start a new transport epoch. Project seconds are converted
to transport frames during preparation with checked deterministic rounding.
Tempo-dependent expressions use an explicit prepared tempo snapshot; M1 does
not infer a tempo map. No kernel reads wall-clock time.

The `RealtimeGraphHost` remains transport owner. A prepared session receives an
immutable transport snapshot per process segment. Kernels may observe playing,
epoch, absolute transport frame, playhead frame, sample rate, and declared loop,
but may not advance or reset host transport.

## Discontinuity and reset

Pause advances neither transport nor playhead. Stop/reset is adopted at a
callback boundary, increments or explicitly marks a discontinuity, and invokes
only kernels whose descriptor requests transport-reset notification. Loop wrap
changes playhead mapping without rewinding DSP state. Graph activation adopts
the current transport epoch/frame and starts with fresh session DSP state.

## Legacy bridges

- Existing Event Value queues adapt to typed assignment events.
- Existing scalar gate/control buffers adapt to ADR-0003 edge events where a
  kernel requests them.
- MIDI note ingress resolves to typed note events before kernel dispatch.
- Unmigrated nodes keep existing scheduler behavior through the legacy adapter.

## Rejected alternatives

- One global frame counter without an epoch would make sample-rate changes
  ambiguous.
- Passing wall-clock timestamps into kernels would undermine deterministic
  offline/realtime comparison.
- Treating every event as a scalar Event Value would erase note and edge
  semantics needed by envelopes and utilities.

## Acceptance evidence

- The conformance harness produces identical ordering across variable offline
  and realtime block partitions.
- Equal-frame, callback-end, loop-wrap, pause/resume, reset, and graph-swap
  fixtures have explicit expected sequences.
- Typed gate events and scalar-gate adaptation drive ADSR equivalently.
- Event delivery and rejection allocate zero bytes and never block the callback.

## Amendment (2026-10-12): terminal callbacks split at a prepared frame

A replacement staged while a looped or Score project plays is prepared for
one absolute frame, and audio callbacks split there so it becomes audible
exactly at that frame. Terminal event callbacks (MIDI and global
controller output with no audio outputs) now split there too: the
callback renders up to the frame with the incumbent and on from it with
the replacement, each side into storage the bridge allocates when it first
stages a terminal-only engine, and publishes the events as one block, the
later side's frame offsets counted from the callback's start. The block
reports the replacement's stage, the incumbent's (`previousStageId`) and
the frame between them (`adoptionFrameOffset`); when both sent global
controller events under different names, the incumbent's name and event
count come separately. A side that fails leaves the caller's block as it
was. The stopgap that refused a terminal-only restage prepared for any
frame but the next callback's start is gone.

