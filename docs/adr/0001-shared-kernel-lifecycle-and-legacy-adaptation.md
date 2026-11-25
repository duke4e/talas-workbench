# ADR-0001: Shared Kernel Lifecycle and Legacy Adaptation

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Offline rendering and prepared realtime currently implement overlapping node
behavior in separate large dispatch units. Rewriting either runtime would put
working projects, callback safety, and honest support labels at risk. Keeping
two independent implementations would make offline/realtime equivalence
unprovable.

## Decision

Migrated node types use one internal DSP kernel implementation in both offline
and prepared-realtime sessions. Session drivers own scheduling, graph order,
buffer routing, and host interaction; kernels own only node-local behavior and
state.

The lifecycle is:

1. `describe`: immutable registry metadata identifies the node contract and
   kernel binding.
2. `prepare`: on a non-audio thread, validate compiled parameters, resources,
   sample rate, maximum block size, and port layouts; allocate all state and
   scratch; return either one ready instance or structured diagnostics.
3. `reset`: at a declared safe boundary, restore node-local state for a reset
   reason and an explicit time snapshot. Reset is bounded and allocation-free.
4. `process`: consume prevalidated buffer, parameter, event, and transport
   views for a non-empty half-open frame range. It is `noexcept`, bounded, and
   allocation-free, and returns a small status code rather than constructing a
   diagnostic.
5. `destroy`: outside the audio callback after retirement/collection.

An instance is uniquely owned by one prepared graph scope and is neither copied
nor shared between nested Sound instances or MIDI voices. Immutable resources
may be shared explicitly. Mutable DSP state never is.

Offline and realtime drivers may choose different callback block boundaries,
but must call the same kernel with equivalent ordered inputs and time/event
views. Segmentation at an event boundary must not reset kernel state.

## Legacy adaptation

The kernel registry is sparse. A compiled step uses its registered kernel when
one exists; every unmigrated type continues through the existing offline and
realtime central dispatch. The compatibility adapter translates the existing
`CompileStep`, buffers, parameters, and resources into the new prepare contract
away from the callback. A step is executed by exactly one path.

No bulk dispatch rewrite is allowed. Oscillator migrates first; Filter and ADSR
must then prove that the seam supports a stateful processor and an event-driven
control generator. A node may be routed back through the legacy path without
removing its fixture if a migration regresses behavior.

## Failure and ownership rules

- A prepare failure publishes no partial graph and leaves the previous active
  realtime graph running.
- A process failure returns failure to its session driver without allocating,
  logging, throwing, locking, or touching UI/file/network state.
- The driver owns output clearing and device-time advancement on failure; the
  kernel contract does not silently rewind external time.
- Kernel registration and lookup happen before rendering. The callback never
  performs string lookup, factory invocation, container growth, or destruction.
- Existing project JSON and public runtime entry points remain compatible.

## Rejected alternatives

- A new parallel runtime would duplicate scheduling and delay useful behavior
  coverage.
- A virtual per-sample object hierarchy would add callback indirection and
  unclear ownership without solving capability drift.
- Migrating every node before enabling the registry would make rollback and
  differential diagnosis impractical.

## Acceptance evidence

- One fixture drives each migrated kernel offline and realtime from identical
  inputs and compares output/state-visible behavior under ADR-0005 ordering.
- Oscillator, Filter, and ADSR each pass variable block-size and event-boundary
  tests.
- Callback allocation counters remain zero on success and rejection paths.
- Unmigrated projects render through the legacy adapter without output changes.
