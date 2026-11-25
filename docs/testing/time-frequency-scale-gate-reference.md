# TimeFrequencyScale live Gate/reset evidence

2026-09-27. Native candidate work. The source, control and latency contracts
are unchanged.

The later [state-copy investigation](time-frequency-scale-state-copy-reference.md)
addresses the32-frame active misses recorded below. This record preserves the
original Gate measurements and their limits.

## Reproduced fault and repair

Live capture splits reader processing into one-sample segments. Inactive Gate
called reset on every segment; each reset filled seven vectors proportional to
the prepared window. In ordinary Release, all128 stopped and all128 pulsed
blocks miss their deadline at each of the three N65536 configurations. The
same numerical, zero-allocation and restart checks pass before the repair.

Reset now changes only scalar/cursor state. Old storage is overwritten before
read, initial output is explicitly silent, and the first overlap frame assigns
its contribution. Its first-frame flag participates in pending checkpoints.
Later overlap frames retain addition. No native latency, admission bound,
source authority, spectral algorithm or numerical threshold changes.

DSP regression covers all seven powers of two1024..65536 and active, neutral and
fast-source profiles. Eight dirty prefix lengths cover initial history, first
jobs and later jobs after ring wrap. Fresh output is compared to restarted
output, with repeated resets,64 one-sample pulses, reset-state copies and a
pending first-frame copy. A zero-source oracle requires no residual output;
owned clones at eight first-hop offsets retain the same restart. C++ allocation
checks cover reset, state copy and processing. Fresh DSP comparisons retain the
existing1e-12 absolute tolerance; silence is required to be exactly zero.

## Public live-source timings

Apple M1 arm64, AppleClang21.0.0, ordinary Release `-O3 -DNDEBUG`. Every measured
call includes full runtime rollback snapshots and real cyclic MemoryWriter
capture. Input is a40Hz+0.1DC float waveform; its two-second recording repeats
exactly at cyclic slots. Timers surround public render; allocation bookkeeping,
input generation, event updates and comparisons are outside the timer.

Warmup fills the entire recording while the reader is active. Each stopped and
pulsed row has128 callbacks; the latter raises Gate for the first sample and
lowers it for the rest. Both require exactly silent output. Subsequent active
restart is compared samplewise (existing1e-10 absolute tolerance) with a fresh immutable-source session, including
the exactN+N/4-1 silent prefix. Independent Hann projections require40Hz gain
above.035 and at least10 times the unwanted10Hz component. Observed wanted
projections remain.0624994–.0624999, unwanted at most.0000258162.

Initial and repaired Release measurements, milliseconds (median/max):

| Sample rate/block | N | Deadline | Stopped before | Stopped after | Pulsed before | Pulsed after |
|---|---:|---:|---:|---:|---:|---:|
|48k/128|8192|2.667|.598/.748|.024/.039|.622/.772|.025/.028|
|48k/128|65536|2.667|5.300/6.515|.113/.268|5.190/5.869|.112/.249|
|48k/32|65536|.667|1.411/1.649|.106/.274|1.371/1.577|.106/.284|
|96k/128|65536|1.333|5.282/5.897|.117/.273|5.211/5.875|.116/.356|

All eight repaired stopped/pulsed rows have zero misses and zero C++ allocations.
The128-frame48k/N65536 stopped median falls by97.88%. Timing bounds remain
observations, not a guarantee about OS scheduling or audio hardware.

Active restart has395 full calls at48k/128,N8192;2175 at both N65536/128
profiles;8703 at48k/32,N65536. The repaired128-frame profiles have zero misses
and maximum calls.369/.869/.818ms respectively. At32 frames the repaired run
records3/8703 misses, maximum.755ms against.667ms; its baseline run recorded
zero misses, max.650ms. This remaining tight active deadline is explicitly open;
the stopped/reset repair does not certify it. All waveform, latency, pitch/gain
and allocation assertions pass in both runs. Raw outliers are retained.

These are ordinary diagnostic runs. The formal clean-source benchmark campaign
and its provenance gate remain unchanged. No comparison of timing or audio
with an external reference is claimed.

The final Debug probe also has zero stopped/pulsed misses in all eight rows,
with the same sound/state/allocation checks. Its active32-frame run has1/8703
misses, max.735ms; all three128-frame active rows have zero misses. Maxima in
milliseconds:

| Sample rate/block | N | Stopped | Pulsed | Active restart |
|---|---:|---:|---:|---:|
|48k/128|8192|.176|.177|.426|
|48k/128|65536|.466|.472|.922|
|48k/32|65536|.317|.297|.735|
|96k/128|65536|.451|.442|.951|

The small-block active limit therefore remains observable in both builds.
Next diagnosis should separate full-state snapshot cost, synchronous source
capture and OS outliers before choosing another change. Reset's measured
improvement does not remove these other costs.

## Verification and retained logs

All logs are under registered `build/workbench-debug`, prefixed
`time-frequency-scale-gate-`:

- `red-build.log`, `red-probe.log`: successful initial build and measured fault.
- `green-build.log`, `green-tests.log`: first repaired Release DSP/live suites,
  2/2 pass9.51s before adding zero-source and first-hop clone coverage.
- `green-probe.log`: repaired Release public-call timings and restart oracles.
- `release-final-build.log`, `release-final-tests.log`: expanded DSP/checkpoint/
  live suites3/3 pass10.70s, including zero-source and first-hop clone coverage.
- `debug-build.log`: all11 selected regression targets plusCLI/Workbench build;
  only pre-existing duplicate-library linker warnings.
- `debug-tests.log`:11/11 pass164.27s: DSP, compile, offline, TFS checkpoint,
  scope authority, conformance, realtime, all three TFS source suites and
  resource snapshots. No source or numerical assertions were weakened.
- `debug-probe.log`: final Debug timings and restart oracles above.
- `generated-check.log`: catalog, capability and help pass.
  Whitespace and final-newline checks pass for changed authored files.

Read-only review confirms the first-frame ring/read and checkpoint invariants.
The clone grid includes partial first OLA atN16384/32768; it is not an exhaustive
sample of every internal stage at every window. All native processes completed.
