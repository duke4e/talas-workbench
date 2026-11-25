# ADR-0007: Explicit Delayed Feedback and SCC Execution

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Talas currently rejects direct graph cycles. The historical
`FeedbackLoopInput` and `FeedbackLoopOutput` descriptors exist only as inert
offline compatibility subsets: blank input consumes its upstream graph, blank
output emits silence, named pairing and delay memory are rejected, and
prepared realtime is unsupported.

M2 requires useful feedback without legalizing algebraic loops, introducing a
callback-sized hidden delay, sharing state across nested instances, or
duplicating offline/realtime behavior.

## Decision

### Pair contract

A feedback pair is identified by `(structural Sound scope, connection)` where
`connection` is a non-empty stable text identifier. Exactly one
`talas.feedbackLoopInput` and one `talas.feedbackLoopOutput` must exist for the pair.
Names do not cross Sound scopes.

The implicit delayed edge runs from `FeedbackLoopInput.audioIn` to
`FeedbackLoopOutput.audioOut`. The ordinary explicit graph remains an
instantaneous DAG after delayed edges are removed. Blank legacy nodes retain
their current inert behavior until and after named feedback is added.

Missing, duplicate, cross-scope, type-mismatched, or ambiguous endpoints fail
during compilation. A direct ordinary cycle remains illegal.

### Delay semantics

`FeedbackLoopOutput.delay` is a finite static Seconds-domain value resolved at
preparation. The effective whole-frame delay is:

```text
max(1, round(delaySeconds * sampleRate))
```

Thus a configured zero delay is an explicit one-sample delayed edge, never an
algebraic connection. M2 does not modulate, resize, or fractionally interpolate
feedback delay on the callback.

### SCC compilation and execution

The compiler constructs a dependency graph including the implicit delayed
edges, computes strongly connected components deterministically, and proves
that every cyclic component contains at least one declared delayed edge. It
then removes delayed edges to obtain the instantaneous execution order.

Each feedback pair owns a preallocated circular buffer and cursor in its
prepared Sound scope. For every frame in a cyclic SCC the driver:

1. reads each pair's delayed output state for that frame;
2. evaluates the instantaneous SCC operations in deterministic topological
   order; and
3. commits each feedback input into its pair state.

Frame-stripe execution is the correctness baseline. A later chunked
optimization is allowed only when it proves the same trace for all valid block
partitions and delays.

Offline and prepared realtime consume the same compiled feedback/SCC plan and
the same pair-state implementation. Callback boundaries never define delay.

### Lifecycle

Pair state is allocated during preparation, uniquely owned by one prepared
scope, reset on the descriptor-declared transport discontinuity, and destroyed
off the callback after graph retirement. Pause and loop wrap preserve state.
Graph replacement begins with fresh feedback state; M2 performs no feedback
state transfer.

Feedback topology delay is not feed-forward path latency and is excluded from
automatic fan-in compensation claims.

## Failure and realtime policy

- Invalid pairs, delay overflow, SCC budget overflow, or an algebraic cycle
  fail preparation transactionally and leave the previous graph active.
- Pair buffers are bounded both per pair and by the configured aggregate
  feedback-frame budget; feedback SCC analysis has a configured prepared-step
  budget, and runtime metadata is allocated before playback.
- A failed frame does not publish partially updated pair state.
- The callback performs no allocation, free, lock, wait, parsing, formatting,
  logging, file/network I/O, or UI access.

## Rejected alternatives

- Allowing all graph cycles and hoping node order supplies a delay would make
  semantics insertion-order and callback-size dependent.
- Treating one callback as the minimum delay would fail partition invariance.
- Reusing one global connection-name map would leak feedback between nested
  Sound instances.
- Inserting an undocumented delay into arbitrary cycles would hide modeling
  mistakes and make diagnostics dishonest.
- Separate offline and realtime feedback engines would make offline/realtime
  equivalence unprovable.

## Acceptance evidence

- Direct zero-delay cycles still reject with an algebraic-loop diagnostic.
- One named pair returns an impulse at the exact effective delay frame.
- One-sample and longer feedback traces agree across contiguous rendering and
  irregular partitions `{3, 7, 1, 9, 4, 16}`.
- Two nested instances using the same local connection name have independent
  state.
- Missing/duplicate/cross-scope pairs and delay/SCC budget overflow fail closed.
- Pause/loop preserve state; stop/reset and replacement produce fresh state.
- Offline and prepared realtime use the same compiled plan and allocate zero
  bytes on the callback.
