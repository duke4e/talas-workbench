# TimeFrequencyScale component synthesis integration

2026-09-27. **Bounded integration verified in both builds.** The quality repair,
semantic snapshots and public failure/retry gates pass. After two measured copy
reductions, the latest callback diagnostics report no wall/CPU deadline misses
in Release or Debug. Earlier misses remain below; formal performance and broader
hardware/source acceptance are still open.

The original nearby-partial production gate improves from34/64 to64/64 in
both builds with unchanged N8192/H2048/D10239 and unchanged independent thresholds.

Estimator evidence: `docs/testing/time-frequency-scale-native-model.md`.

## Production behavior

`PreparedTimeFrequencyScale` prepares an owned estimator, N/4 FFT plan and N raw
samples outside processing. Each capture stores raw and Hann-windowed samples
from the same current source reads. An estimator owns a complete source-window
snapshot and advances64 weighted units per output sample across hops. Startup
padding does not start a discovery job. No source frequency/count/amplitude
truth enters the estimator.

A completed model is only a candidate. Its original-frequency recurrence must
predict the current owned raw frame with RMS<=1e-4 of current standard deviation
(physical1e-12 floor). The stricter constant residual rule remains. Invalid or
nonstationary input keeps the legacy spectral path current, fades the component
contribution, and permits discovery again. Stale jobs cannot reintroduce a source
that a live writer has replaced with silence.

For fractional capture offset alpha, linear interpolation applies
H(alpha)=1-alpha+alpha*exp(i*omega). The fitted phase contains the offset
arg(H)-omega*alpha and fitted amplitude contains |H|. Candidate conversion removes
both; validation applies the current window's response. Reject an inverse with
gain<sqrt(double epsilon) or nonfinite amplitude/phase. Synthesis uses the
underlying source gain, independent of Rate.

Each component has its own output phase, advanced by original frequency*Scale*H.
Rate only advances source time. A newly admitted unmatched component is seeded
from the current legacy lobe center after the spectral phase/peak stages:
canonical source-center phase + current interpolation phase + legacy peak phase
correction + pi*(mappedPeak-sourcePeak). Matching active components preserve
their already advanced output phase. Seed flags reset at each capture.

Component recurrence runs inside the original overlap-add stage and output
timeline. Its Hann-squared contributions use the existing1.5 normalization.
Complementary linear mixture changes by.25 per hop; translated components above
Nyquist are cropped. Spectral processing continues even while mixture is1.

## State and scheduling

Frame stages share256 weighted units per output sample. Validation and synthesis
cost2+K per sample; one new phase alignment costs32 units per component. At the
largest N65536/K8 with the extra reference FFT, explicit frame work is3,997,976
units versus4,194,304 supplied by256*H. Separate synchronous capture and estimator
input copying are outside that accounting; it is not a CPU-time guarantee.

Reset invalidates the job/model/phase/confidence logically without clearing large
buffers. Frame checkpoints include unread validation raw samples, the exact
oscillator recurrence and all pending legacy spectral state. Pending estimator
checkpointing initially copied the full1,983,616-byte worker. Immutable FFT plans
are prepared separately. Heap ownership uses `OfflineStateJournal`'s existing
semantic clone/copy participant strategy.

## Functional evidence

Release full `talas_time_frequency_scale_dsp_tests` passes: original64 nearby-partial
profiles, low/DC/Nyquist output,32 independent Scale/Rate profiles, changing-source
and Rate trajectories, all existing dirty checkpoints/reset/live-snapshot cases,
plus the following new integration gates:

- Three dirty continuation checkpoints at25000,60000,144013 output frames;
  pending worker and active synthesis; bit-exact continuation across128/7-frame
  partitions, no processing/copy/reset allocation.
- Source replacement with silence while the old snapshot is being estimated;
  no old sound after the existing output tail, exact reset silence.
- Six fractional-clock profiles: Rates.73/1.371, source2203/7883/17011Hz,
  Scale.25. Full confidence persists; amplitude.25+/- .001, DC.1+/- .0001.
- Eight components at N8192 and65536 with Rate2 and the extra reference FFT;
  ready at145792/543360 output frames, all component gains retained, no frame
  overrun or allocation.

Native offline recovery adds three pending/active checkpoints, six deliberately
late rejected calls, no published failure output, exact retry continuation and
reset. Release passes. The existing four runtime CTest groups (checkpoint,
MemoryWriter, disk source, quarter-speed reference) pass4/4 in32.54seconds.

Failures were retained and corrected without relaxing an oracle:

1. Initial integration already passed64/64, but fractional clocks failed0/6.
   Removing/reapplying the known interpolation response makes them6/6.
2. Full regression then failed3/32 independent controls atRate.125 because
   unrelated activation phases cancelled during the crossfade. Seeding from
   current legacy phase makes32/32 and the full DSP executable pass.
3. Initial runtime compilation rejected the newly noncopyable owner. Explicit
   semantic snapshot registration fixes the required offline checkpoint path.

## Complete callback diagnostics: Release

Ordinary Release is-O3 -DNDEBUG. Debug configuration is-g; the previously
established PreparedTimeFrequencyScale/FFT TU-O2 settings are unchanged. No new
per-file optimization was added. Formal benchmark targets remain OFF. These are
native test diagnostics, not a formal benchmark-gate exemption.

`--model-deadline-probe` measures the public renderer atN8192/65536,32/128-frame
blocks,8 components andRate2. A separate kernel pass classifies startup/pending/
active blocks and supplies a bit-exact output comparison; eight independent
frequency projections gate every component gain. All12 state groups pass output
and allocation checks.

The initial run records18/13112 wall-deadline misses in N65536/block32 pending
analysis (max12.5823ms); other11 groups have zero. The diagnostic repeat adds a
thread CPU clock and records zero CPU-deadline misses across all12 groups, but
five wall misses in N8192/block128 pending analysis. Its longest wall call is
14.3138ms with230.125us of thread CPU time. Other long calls are also retained;
this does not prove the cause of the initial run's delays.

| Repeat profile, pending analysis | Samples | p99 wall us | Max wall us | Max thread CPU us | Wall misses |
| --- | ---: | ---: | ---: | ---: | ---: |
| N8192/block32 | 4038 | 130.083 | 201.000 | 201.166 | 0 |
| N8192/block128 | 1010 | 681.875 | 14313.8 | 638.875 | 5 |
| N65536/block32 | 13112 | 415.833 | 665.291 | 592.167 | 0 |
| N65536/block128 | 3278 | 553.417 | 746.625 | 746.917 | 0 |

Deadlines are666.667us at32 frames and2666.667us at128 frames/48kHz. Averages,
p99 or CPU time do not erase observed wall misses. No deadline-green claim.

The existing128-frame rollback diagnostic passes independent audio, failure
silence, exact retry and noalloc; one of8576 measured calls exceeds the deadline
(max2708.33us); the later semantic-copy run has0/8576 misses. The live
MemoryWriter32/N65536 diagnostic passes source identity, gate/reset silence,
gain/pitch and noalloc, but records2/8703 restarted-active wall misses(max1.14692ms).
Its separate immutable pass records one miss. Separate component passes locate
substantial copy cost: copy_other p99~342us versus kernel_other p99~67us. They are
not an additive profiler. A stage-aware estimator snapshot is under review.

## Smaller pending-worker snapshot

The measured copy cost and baseline Debug CPU miss motivate a named semantic
copy method. Default assignment remains complete and trivially copyable. The
method retains all small counters/candidates/guides/model results and:

- Every Running state's raw count-sample capture, needed by future alias fits.
- The used three FFT lanes only during FillSpectrum/Transform/Spectrum. A
  completed lane remains live until the full guide scan ends. The maximum FFT
  buffer is786432bytes(0.75MiB); no unsafe lane-prefix omission is made.
- The full subspace from Subspace onward, except the constant bypass; modes
  during Modes/Roots; the full fitter during Fit/FinalFit. Omitted children reset
  and their later begin operations reinitialize before use.

An independent read-only field/liveness review found no omitted live member.
New Release tests pass87 dirty snapshots through all16 parent stages, three FFT
lanes and both scan segments, including mid-stage positions. Fixtures cover
N8192/eight components, N65536/two components, the constant bypass and impulse
rejection, with32/32/6/17 snapshots. Five terminal classes and self-copy pass.
The original source is replaced before snapshots continue; output, work counts
and guides must be bit exact. Every copy/continuation is allocation-free.

Complete Release model tests retain289/289 profiles(267 accepted,22 expected
rejected) and the older16 dirty pending copies. Production processing-state
regressions pass, and the five runtime/offline CTests pass5/5 in23.14s. After the
copy change, Debug model plus five runtime/offline CTests pass6/6 in136.12s.
The separate Debug DSP processing-state probe also passes, including three
component checkpoints, current-source mutation, reset and no-allocation checks.
The preceding full functional Debug pass belongs to the initial integration.

### Measurements after the copy change

Release K8 public callback:12 groups,35557 calls,0 wall/CPU deadline misses.
AtN65536/block32 pending analysis, p99wall328.834us, maxwall532.5us and
maxCPU495.375us. Across128-frame profiles maxwall698.917us. Audio is bit exact
against the separate kernel pass, all eight gains pass and no allocation occurs.
Release128-frame failure/repair diagnostic:40 groups,8576 calls,0 wall misses,
max833.791us, with independent audio and exact retry checks retained.

The first post-change live run still records5/8703 wall misses(max24.2247ms),
and its immutable comparison has6. Copy_other p99 drops342.417->199.291us, and
its maximum414.459us has0 misses. This prompted thread-CPU instrumentation of
the same live diagnostic, rather than assuming a cause for those wall stalls.

The first instrumented live run correctly catches a diagnostic allocation: this
test's require accepts std::string, so a successful clock assertion constructed
a message inside the allocation guard. Constructing its exception only on clock
failure fixes the probe; production code and allocation criteria are unchanged.
The failed log remains.

The corrected Release live run has0/8703 wall/CPU misses, maxwall508.416us and
maxCPU508.583us. Separate immutable comparison has one wall miss(max951.958us)
and0CPU misses; copy/kernel passes have0 wall/CPU misses. Copy_other p99wall is
204.292us/max309.583us. These observations show lower copy cost and more CPU
margin, but do not erase the retained wall outliers or establish hardware-wide
deadline behavior.

Debug K8 public callback retains all12 audio/no-allocation groups and35557 calls.
N65536/block32 pending analysis has one wall and one CPU miss: output offset345568,
wall974.208us/CPU700.958us versus666.667us. Its p99wall is391.291us; the remaining
11 groups have no wall/CPU misses. Thus the copy change has not closed the Debug
CPU deadline counterexample. No result is replaced by a cleaner repeat.

Debug live-source active calls have0/8703 wall/CPU misses, maxwall556.125us and
maxCPU539us. The separate immutable comparison has one wall miss(max2037.25us)
and0CPU misses. Copy_other has p99wall222.75us/max398.041us; kernel_other has
p99wall76.667us/max255.333us, both with0 wall/CPU misses. These are separate runs,
not an additive profile. Debug128-frame failure/repair covers40 groups and8576
calls with0 wall misses, max1548.42us, retaining exact retry and no-allocation gates.

## Last-attempt snapshot investigation

An untimed replay of the recorded Debug miss at output345568 observes frame
Analysis and parent FinalFit, after eight attempted orders. Work advances from
17922112 to17924160 across the32-frame block. Both source review and independent
review establish that the parent raw capture/subspace are dead only during
FinalFit when constant_ is true or order_==2*maximumPartials. Earlier final fits
can retry and must retain both buffers. The full fitter remains live in all cases.
At the last allowed order, rejection advances to Order18 and hits the maximum-
order guard before reading either omitted buffer, including at exhausted work.

The narrower copy retains289/289 Release model profiles. Its initial128 dirty
continuations compare exactly, including last-order and constant success/rejection,
a maximum-window last fit and Order18 recopy, but the new coverage assertion
correctly fails: the strong eight-tone fixture skips FinalFit at order14.

An independent NumPy seven-tone/DC least-squares calculation supplies the missing
source: retain the first seven existing tones and add20.3Hz at amplitude3.1e-5,
sine phase5.779335406794897, center3.141. Fixed-frequency training-relative RMS is
9.682411343e-5 and held-out RMS1.082493938e-4. A separate native diagnostic confirms
actual FinalFit visits at14 and16, followed by Ready/eight components with
training2.25967e-13/validation2.931e-13. The source fixture, not any gate, changes.

The complete Release semantic-copy probe now passes162 dirty continuations through
all16 stages and five terminal classes. It explicitly covers two successful
order14-to16 retries, six accepted last-order snapshots, two rejected last-order
snapshots, two exhausted-work Order18 recopies, and two constant snapshots in
each acceptance class. The separate Release production processing-state probe
passes. Five Release runtime/offline CTests pass5/5 in22.94s. Debug full model
plus five runtime/offline CTests pass6/6 in149.45s, retaining289 profiles and all162
dirty snapshots with the same explicit retry/boundary counters. Initial coverage
failure and all previous timing misses remain evidence. The separate Debug DSP
state probe also passes, including current-source replacement, reset and exact
pending/active continuation without allocation.

After this concrete copy change, Release K8 callback measurements cover12 groups/
35557 calls with0 wall/CPU misses. AtN65536/block32 pending analysis, p99wall is
303.625us, maxwall439.334us and maxCPU439.542us. The live diagnostic has0 wall/CPU
misses in all reported groups; active live calls cover8703, maxwall467.25us and
maxCPU467.417us. Separate copy_other p99wall210.583us/max326.792us and kernel_other
p99wall64.875us/max94.917us remain separate measurements. Release128-frame rollback
has0/8576 wall misses across40 groups, max801.167us.

The final Debug K8 run covers12 groups/35557 calls with0 wall/CPU misses. Its
N65536/block32 pending p99wall is353.667us, maxwall519.667us and maxCPU508.791us,
below666.667us. Debug live-source active calls cover8703 with0 wall/CPU misses,
maxwall544.792us/maxCPU545us; every other reported live/immutable/copy/kernel
group also has0 misses. Copy_other p99wall224.5us/max379.375us; kernel_other
p99wall76.667us/max247.334us. Debug128-frame rollback has0/8576 wall misses across
40 groups, max1064.21us. Audio, exact retry and no-allocation gates remain in place.
These are measurements after a concrete state-copy change, not replacement runs
of the previously failing binary and not a hardware-wide deadline guarantee.

## Logs and remaining work

All logs are in registered `build/workbench-debug`, with prefix
`time-frequency-scale-component-synthesis-`: initial Release build/low-partials/
state; fractional-repro build/test; corrected build/DSP/fractional;
strengthened build/work; phase-aligned build/controls/DSP/offline;
final-release-runtime build/ctest; Release model-deadline and model-deadline-cpu;
Release rollback-deadline and live-component-deadline; final-debug build/ctest;
debug-model-deadline-cpu; live-copy-release build/probe/model/runtime-build/state/
runtime-ctest/model-deadline-cpu/live-deadline/rollback-deadline;
live-cpu-release-build; live-copy-release-live-deadline-cpu (diagnostic allocation
failure); live-cpu-noalloc-release-build; live-copy-release-live-deadline-cpu-final;
live-copy-debug build/ctest/state/model-deadline-cpu/live-deadline-cpu/
rollback-deadline; state-trace-debug build/test; final-attempt-release-build
(wrong build-directory invocation), final-attempt-release-native-build/model
(coverage assertion failure), retry-fixture-build/20hz;
final-attempt-retry-release build/copy; final-attempt-release runtime-build/state/
runtime-ctest/model-deadline-cpu/live-deadline-cpu/rollback-deadline;
final-attempt-debug build/ctest/state/model-deadline-cpu/live-deadline-cpu/
rollback-deadline. The standalone retry-fixture source and
binary also live in the registered build root.
Initial failures and all measured misses remain evidence.

Functional verification is complete for this integration and semantic-copy edit.
Latest host-local deadline diagnostics pass; a formal performance campaign and
broader hardware/source coverage remain open. Existing conservative estimator
gaps remain: >8 components,
decimation collisions, integer/half aliases, >256 alias products and rejection
of nonstationary/ill-conditioned windows. The full legacy fallback still runs;
none of these tests establishes agreement with an external reference.
