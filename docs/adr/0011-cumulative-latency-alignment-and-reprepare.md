# ADR-0011: Cumulative Latency, Fan-In Alignment, and Reprepare

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Node capability metadata currently distinguishes no, fixed, and
variable/unknown latency. Compiled steps do not store resolved latency, graph
paths do not accumulate it, and multi-input processors receive unequal paths
unaligned. M3 FFT/STFT, streaming/resampling, and multichannel work would make
that omission audible. Latency-affecting parameters also cannot resize delay
state safely on the callback.

Explicit delayed feedback from M2 is topology state, not ordinary feed-forward
latency. Any latency algorithm must preserve that distinction and work after
reusable Sound expansion without collapsing structural identity.

## Decision

### Latency authority

Descriptor latency has four states:

- None: exactly zero frames;
- Fixed: a descriptor-owned whole-frame constant;
- PreparationTime: resolved to a whole-frame constant from validated prepared
  settings/parameters for one graph generation;
- VariableOrUnknown: not eligible for automatic aligned-support claims.

The compiled step records resolved intrinsic latency, resolution source, and
whether a change requires reprepare. Fractional values are unsupported in M3
and never silently rounded into a fixed claim.

History/lookback does not imply latency. A node declares latency only when the
semantic output timeline is delayed relative to its input.

### Propagation

After hierarchy expansion and legal-feedback SCC planning, the compiler walks
the feed-forward condensation graph in deterministic topological order.

Every port contract declares whether it carries the sample timeline. Audio,
partial-spectrum, and complex-spectrum ports do so by default; a Control port
may opt in explicitly. Propagation follows every connected timeline-bearing
buffer, including Audio -> ComplexSpectrum -> Audio paths.

For an ordinary multi-input timeline-bearing fan-in whose relevant arrivals
are all known, the target arrival is their maximum. Each shorter input receives
compensation equal to the difference. Target output cumulative latency is
aligned arrival plus target intrinsic latency. If any relevant arrival or the
target intrinsic latency is VariableOrUnknown, the target and every dependent
timeline-bearing output become unknown, no compensation is inserted at that
fan-in, and the plan records an `uncompensatedUnknownLatency` warning. Unknown
is never treated as zero or omitted from the maximum.

Source nodes begin at zero. Terminal plan latency is the maximum emitted output
latency. Nested Sound boundaries preserve the internal compiled values and map
diagnostics to the expansion manifest. Named feedback edges and their explicit
delay are excluded from this feed-forward calculation; illegal algebraic loops
still fail before latency planning.

No automatic compensation is inserted on an edge inside a feedback SCC or on
an ordinary edge whose target lies inside that SCC. This preserves the exact M2
loop trace and effective pair delay. Every timeline-bearing output leaving a
cyclic feedback SCC is marked VariableOrUnknown for downstream alignment unless
a future reviewed SCC transfer contract proves a finite latency. The plan emits
an uncompensated-feedback warning rather than modifying the loop.

Automatic alignment is the default for audio fan-in. A descriptor may opt out
only for a documented semantic operation where relative arrival is itself the
meaning. Such a node remains visibly uncompensated and cannot support a
phase-aligned claim.

### Compensation plan and state

Compensation is represented by explicit `LatencyCompensationPlan` entries,
not persisted hidden nodes. Each entry identifies source/target structural
bindings, input/output buffers, channel layout, and fixed delay frames.

Offline and prepared realtime allocate identical circular-delay state before
render. The scheduler applies compensation before the consuming step. Zero
delay has no state and may bind directly. Delay memory never resizes during
process.

Compensation state is cleared on:

- transport stop/reset;
- seek or any host discontinuity that advances ADR-0005's discontinuity
  sequence;
- sample-rate/device epoch change;
- graph-generation replacement unless an explicit future state-transfer
  contract proves equivalence.

Pause, ordinary playhead loop wrap, and callback partition changes preserve
compensation state, matching ADR-0005 and ADR-0007. Editing a loop region or a
host action that publishes an actual discontinuity clears it. Bypass/dry-wet
paths use the same propagation rule; bypass is not permission to skip declared
latency.

### Dynamic change and staged replacement

A parameter or resource change that can alter intrinsic latency is marked
`requiresReprepare`. Live in-place update rejects it without modifying the
active value. Control-thread flow is:

1. apply the project edit in a candidate document;
2. resolve all preparation-time latencies;
3. compile propagation and compensation;
4. allocate and prepare replacement state;
5. stage the complete graph;
6. activate at a callback boundary;
7. retire and reclaim the old graph off the callback.

Failure leaves the active graph and its latency summary unchanged. UI reports
candidate failure separately from active latency.

### Representation and UI

`RuntimePlan` exposes intrinsic step latency, cumulative buffer latency,
compensation entries, critical path, uncompensated warnings, and terminal
latency. CLI and Workbench display frames and derived milliseconds for the
active sample-rate epoch. Derived milliseconds are presentation only; frames
remain authority.

## Compatibility and migration

- Existing None/Fixed descriptors preserve meaning.
- Existing VariableOrUnknown nodes stay unaligned/unsupported where their
  latency matters; infrastructure does not promote them.
- Flat and nested projects do not persist compensation nodes or buffers.
- M2 feedback delay remains exactly its prior topology contract and is not
  double-counted as path latency.
- Old render APIs may ignore the summary, but output samples must use the same
  compensation once the feature is enabled for a proven path.

## Realtime impact

All propagation, memory sizing, buffer routing, and delay construction occur
during compilation/preparation. Callback processing performs bounded circular
buffer reads/writes only. No allocation, resize, lock, wait, parse, format, log,
or recompile occurs on the callback.

## Alternatives rejected

- UI-only latency labels would not align audio.
- Aligning only terminal outputs would leave internal dry/wet and sidechain
  fan-ins phase-incoherent.
- Counting feedback delay as feed-forward latency would double-count a
  different scheduling contract.
- Callback-time delay resize would violate realtime safety and create partial
  state transitions.
- Silently treating unknown latency as zero would create false support claims.
- Persisting compiler-inserted nodes would pollute project identity and undo.

## Acceptance evidence

- unequal synthetic paths align impulses at the exact frame;
- nested, dry/wet, and Discrete4 path accumulation tests;
- offline/prepared equality under irregular partitions;
- FFT native and legacy round-trip latency tests;
- stop/seek/loop/reset/replacement state tests;
- latency-affecting live update rejection and staged replacement tests;
- callback instrumentation proving zero forbidden work.

## Amendment (2026-09-29): a Sound's timing is not latency

SetDuration's start time, TimeOffset, WaitUntil, TimeStopper and TimeControl
shift or pace their Input on purpose: that timing is what the Sound means, and
nothing should be aligned to it. They now declare no latency instead of
VariableOrUnknown. Nothing is aligned to their timing, while a processor's
latency beside them (an FFT path mixed with one) is now compensated as on any
other path instead of leaving the whole fan-in uncompensated. Processors after
them keep a known timeline: a Filter, VCF, FFT or pitch shifter after a Sound
with a start time used to refuse to prepare, in realtime and for several even
offline. Latency stays what a processor adds as a side effect of its work (FFT
frames, lookahead), which alignment should undo.

## Amendment (2026-09-30): processors take any latency before them

A processor that adds no latency (a filter, VCF, resonator, gate, delay,
time node) or adds a known amount (FFT, pitch shifter) now takes whatever
latency reaches its input, known or unknown, and passes it on: known plus
its own, or unknown. Its admission checks only that its own latency and the
latency it reports follow from its inputs. Before, 23 such processors
refused anything after an FFT, pitch shifter or unclassified node (such as
an Oscilloscope display) because they demanded zero latency all the way
back to the source. A packed FFT still needs a known input latency as its
phase reference.

## Amendment (2026-10-10): a packed FFT after an unknown latency

A packed FFT's input latency is its phase reference: its frames start
where its Input's content starts. After an unknown latency (a feedback
read inside a cycle, or an unclassified node such as an Oscilloscope
display) it had none, and both renderers refused it. An FFT's output
repeats every Length/2 samples, so its frames can count from the FFT's own
start without any compensated latency. A packed FFT after an unknown
latency now takes its phase reference from its own start, offline and in
realtime: the unknown latency counts as zero, while known latencies after
it still count, so an inverse FFT behind a forward one takes its frames
where the forward FFT puts them and the pair plays as it does without the
unknown latency. Its output's latency stays unknown, so nothing is aligned
to it and no alignment is claimed (owner decision 2026-10-10).
