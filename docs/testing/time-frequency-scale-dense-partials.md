# TimeFrequencyScale dense stationary partials

Status on 2026-09-27: extension in progress.

## Preserved quality results

All 40 profiles, source lengths, N8192/H2048/D10239 and numerical gates were
declared before the initial probe. The full-rate Goertzel oracle accepts two
ideal mixtures and rejects eight deliberately wrong outputs. Complex projections
match analytic coefficients within 1e-6. Measurement uses no decimation.

| Release implementation | Passing profiles | Fraction |
|---|---:|---:|
| Previous eight-component cap | 16/40 | 40% |
| Capacity 16 and bounded frame sharing | 18/40 | 45% |
| Skip redundant ranking for a unique assignment | 21/40 | 52.5% |
| Give the estimator all remaining shared work | 31/40 | 77.5% |

The final shared-work/compact-history Debug gate also passes **31/40 (77.5%)**,
with the same nine failing profile identities. Its intentionally nonzero exit
is retained as a quality failure, not a passing regression test.

Latest breakdown: K4, K8 and K9 each pass 8/8; K12 passes 7/8; K16 passes 0/8.
The remaining K12 case uses Scale .0625, Rate .25 and descending amplitudes.
Its min/max gains .991873/1.00995, pitch and DC pass; residual RMS .0443578 exceeds
.035. These nine failures remain failures. The opt-in gate is not promoted into
passing default coverage. Current callback timing remains open.

## Capacity, work and state

Eigen/mode capacity is 32; sinusoid fitter/model capacity is 16, with 49 columns
in the linearized fitter. The lag matrix stays 97×95. Guide and Cartesian
assignment caps stay 16 and 256. Maximum fitter work at N65536/K16 is 466,971,271,
within 32-bit size_t. The model's combined bound uses 64-bit arithmetic and derives
its maximum order from the nested capacities.

Model copies omit raw/subspace parent buffers during a final fit only when the
next order exceeds capacity or captured subspace rank. The fitter already owns
the samples. Earlier retry-capable fits retain their parents. A unique assignment
now directly runs the same refined/validated fit. Multiple assignments still rank
exclusively on the training prefix.

The scheduled total stays 320 weighted units per output sample. Frame allowance
is max(256,192+8*K), where K is the larger current/active component count. The
estimator receives all units left after that call, including 320 while the frame
is idle. A later capture guard under verification caps this at 64 during the 32
sample positions ending at each atomic capture. It depends only on sample
position, preserving partition-independent scheduling and the 320-unit ceiling.
This changes discovery timing. Source/output clocks, N, H, delay,
quality thresholds and callback deadlines stay fixed. Synchronous capture/copy
remain outside this work count and must be included in timed public callbacks.

The isolated scheduling gate passes **112/112 Release cases**: all seven windows,
K9–16 and Rate 1/2, including activation, current-source validation and no allocation.
Candidates are seeded only by test access; real discovery has separate gates.
The largest N65536/K16/Rate2 frame also gives a pending estimator 112,504 work units
over one hop under the initial sharing rule, and 130,536 under the final shared
budget. Both versions pass all 112 cases. Independent arithmetic simulation
counts 5,112,344 frame units in 16,314 output samples, below H16384. This is not
CPU timing.

Release baseline source tests remain **289/289**: 267 accepted and 22 expected
rejects. After the unique-assignment change, **187 dirty snapshots** pass all 16
parent stages, three FFT lanes, retries, terminal states, reset and no allocation.
The added multi-alias source retains explicit training-only Fit-stage coverage.

## Independent primitive and discovery checks

Expanded eigen/mode JSONL has 129 eigenvalue cases and 37 mode cases (23 clean),
all checked against NumPy in both Release and Debug. It includes 32-dimensional clustered/non-normal
matrices and a 32-mode source. Maximum normalized eigenvalue difference is
3.23104e-11; shift Frobenius difference is 1.86853e-14. Release primitive CTests
pass 2/2 in 1.96 seconds. The fitter's new gate passes K9/K12/K16 refinement,
held-out rejection, dirty continuation across different partitions and no allocation,
in both Release and Debug.

The separate 27-source gate accepts 18/24 stationary sources and rejects all
three deliberate held-out changes. Nearby K12 at center 3.141 and all nearby
K16 cases fail source discovery; both numerical rank and root admission require
investigation. Direct NumPy FFT-guide/least-squares checks confirm
the 18 accepted outputs and three intended rejects; missing accepted models remain
six failures. Accepted maximum frequency error is 4.53554e-10 Hz, relative amplitude
error 2.80504e-11 and normalized coefficient difference 1.77636e-15.

## Integration and callback timing

The shared-work Release regression passes all six DSP/runtime/offline targets in
118.02 seconds. It retains the previous 64/64 low-partial cases. Real-discovery
maximum-work cases pass K8 and K16 at N8192 and N65536, reaching full confidence
at output frames 57,728 / 232,064 and 152,064 / 479,104, respectively.
After the compact-history edit, Debug passes all nine selected targets in
452.16 seconds: DSP, eigen/modes, fitter, model and five runtime/offline targets.
The DSP gate retains 64/64 low-partial profiles, all 2,913 changing-control
snapshots and exactly the same four real-discovery activation frames. Model
coverage retains 289 source profiles and 187 dirty snapshots.

The first Release diagnostic campaign has one K8 CPU/wall deadline miss:
674.334 / 707.5 us versus 666.667 us, N65536/block32, output offset 98,560.
Untimed replay identifies Analysis/Subspace with 64 model work units/sample.
K16, live and rollback diagnostics have zero misses in that campaign.

A concrete copy reduction replaces the rate history with exact boolean
Scale==Rate bytes, and copies only the next two live frequency capture centers,
using the same liveness rule as clock history. A full N65536 checkpoint copies
1,228,784 fewer bytes. Changing frequency/rate controls, wrap, dirty destinations
and reset pass 2,913 exact, allocation-free snapshots across 21 scenarios (seven
with varying frequency). The five runtime/offline targets pass again in 24.17 s.

The post-change Release campaign retains these observed results:

| Diagnostic | Groups / calls | Wall misses | Thread CPU misses |
|---|---:|---:|---:|
| K8 | 12 / 19,957 | 1 | 1 |
| K16 | 12 / 33,292 | 0 | 0 |
| Live and separated copy/kernel measurements | 11 / 43,771 | 15 | 0 in nine measured groups |
| Late failure / rollback | 40 / 8,576 | 0 | Not measured |

The new K8 outlier is offset 107,680, Inverse/Modes: 732.958 us thread CPU and
756.291 us wall time, again with 64 model units/sample. Its group's CPU p99 falls
from 338.25 to 232.625 us, but the maximum still fails. The 15 live wall misses
include overlapping aggregate/component observations and must not be treated as
15 distinct failed public callbacks. Live wall maxima include scheduler pauses
up to 17,593.6 us; all measured thread CPU groups stay within the deadline.
These are current-host diagnostics. This extension's callback timing gate remains
open; previous zero-miss campaigns do not supersede these failures.

The first Debug campaign with compact history retains further timing failures:
K8 has one CPU/wall miss; K16 has six CPU/seven wall misses; live and separated
measurements have four overlapping CPU/nine wall miss observations. Rollback has
zero wall misses. Group/call counts match the Release table. Both K8 and K16
reach about 955 us at offset 74,496 while idle-frame discovery advances from
Spectrum to Subspace using 320 units/sample. K16's capture block at 131,040
spends 9,984 model units across its 32 samples as well as atomic capture.

The subsequent edit reconstructs the same three FFT lanes with explicit scalar
arithmetic and caps model work at its original 64 units immediately before each
capture, as described above. It changes neither FFT mathematics nor acceptance
limits. Post-edit Debug model checks retain 289 baseline profiles and 187 dirty
snapshots. Integrated quality, state and timing verification are in progress.

After this edit, Debug retains 31/40 dense profiles and 2,913 exact state
continuations. All four real-discovery maximum-work cases pass: N8192 activation
moves by one hop to 59,776 frames for K8 and 154,112 for K16; N65536 remains
232,064 and 479,104. Subsequent Debug diagnostics record:

| Diagnostic | Groups / calls | Wall misses | Thread CPU misses |
|---|---:|---:|---:|
| K8 | 12 / 20,037 | 2 | 0 |
| K16 | 12 / 33,372 | 7 | 1 |
| Live and separated copy/kernel measurements | 11 / 43,771 | 3 | 1 in immutable-source comparison |
| Late failure / rollback | 40 / 8,576 | 0 | Not measured |

The repeated 955 us Spectrum spike is gone. The remaining K16 CPU outlier is
732.166 us at offset 150,560, without capture. The active live-source public
call has zero CPU misses, while its immutable-source comparison has one. As
before, aggregate and component live observations overlap. Wall-only pauses
remain, including 39,372.9 us in a K16 128-frame callback. Neither these CPU
improvements nor the source-only quality results close the callback timing gate.

Release with scalar guides and the capture guard passes all seven selected
DSP/model/runtime targets in 154.11 seconds, including the original 64/64
nearby-partial profiles and all existing reset, source and transaction checks.
Independent NumPy verification of all 289 source-model dump rows passes after
the scalar guide edit (267 accepted, 22 intended rejects). Maximum guide
magnitude difference is 9.09495e-13 and normalized coefficient difference is
1.30798e-13. Debug's separate 64/64 gate passes again, and Release dense quality
remains 31/40 with the same nine failing profiles.

The subsequent Release diagnostics have zero thread CPU misses for K8, K16 and
all nine CPU-measured live/separated groups. K8 (20,037 calls), live/separated
(43,771 overlapping observations), and rollback (8,576 calls, wall only) have
zero wall misses. K16 (33,372 calls) retains one startup/transition wall miss:
2,133.33 us against 666.667 us, while that group's maximum thread CPU is
359.958 us. This is an observed wall failure; the full deadline gate stays open,
including the remaining Debug CPU failures and broader hardware coverage.

## Bounded investigations

Provenance correction: root review found the saved untrusted-ESPRIT script used
cosine with `n/Fs-center`, whereas the native fixture uses sine with
`center+(n-N/2)/Fs`. Its 24 results are exploratory results for different phases,
not reproductions of the six native failures. The earlier Loewner/Jacobian
experiment's exact sample provenance also needs confirmation. Preserve these
artifacts, but do not use their numbers as evidence about the native failing
profiles. The corrected follow-up loads exact samples from the original native
JSONL and scores with the original phase/DC rules. Root also corrected its
pivot search to start at the current pivot row.
The Loewner investigator could not establish the formula from retained artifacts;
its exact sample provenance remains unconfirmed.

A 64×64 frequency-domain Loewner experiment reported K16 rank 29–30 at the
existing 1e-12 floor and K12 errors above .02 Hz. Its reported full-window
Jacobian condition at known truth is 1.13e7–7.84e7 for K16. These exploratory
numbers are subject to the provenance correction above, and are not proof of
source-only initialization or convergence. No Loewner method was implemented.

A second experiment examined orders 24, 28, 30 and 32 on six related synthetic
profiles with the incorrect phase construction described above. All 24 proposals
failed the existing 1e-4 root-radius gate for those inputs. Nonlinear
refinement was not attempted; its viability is still untested. Scripts and
results preserve these failed hypotheses.

The corrected follow-up loads the exact six failed native JSONL sample arrays,
uses NumPy SVD/eigenvalues for 24 fixed-order speculative proposals, and calls
the unchanged Release C++ fitter for final refinement. Root count/conjugate
checks reject ten proposals; fourteen receive a unique training-derived alias
assignment, so no ranking QR is needed. Three final fits pass the fitter's
acceptance rules. Only one also passes the original count/frequency/amplitude/
phase/DC oracle: native case 12 (descending K12, center 3.141), order 24, with
frequency error 7.08615e-6 Hz, relative amplitude error 9.76961e-7 and phase error
1.85457e-7 rad. The other two accepted fits contain extra components and do not
pass that oracle. All four K16 source profiles remain unresolved.

This is one recovered source out of six in a mixed NumPy/native experiment,
not a production discovery result or an improvement of the integrated 31/40
gate.

The actual native lag-subspace/mode follow-up retains rank admission and every
final fitting gate. Of 24 proposals, 14 exceed the native retained rank and six
fail the real/conjugate-root count checks. Four enter the native final fitter;
all four reject, so this experiment recovers 0/6 sources. The two K12 inputs
retain rank 23; the four K16 inputs retain 31, 28, 31 and 28. Only the root-radius
gate was deferred for speculative initialization, as in the mixed experiment.
NumPy still supplies training FFT guides and any alias ranking; all four usable
assignments are unique, so no ranking QR runs. Artifacts use
`dense-frequency-research-native-seeds-*`. No production algorithm changed.

The forward/backward investigation compares the original real Hankel trajectory
with `[H,J_rows H]` using the same raw training samples, Frobenius relative rank
floor and final acceptance gates. Root review caught a guide-indexing bug in
its first script (`TRAIN/4` instead of `N/4` for each three-lane FFT). That run
is invalid for candidate conclusions and preserved with the
`incorrect-guide-wrap` suffix. Corrected guides match the prior reviewed driver
exactly on all six sources.

Corrected NumPy forward/backward SVD retains ranks 24,24,32,32,32,32 versus
24,24,31,28,31,28 for the original geometry. The same Frobenius and maximum-
singular rank definitions agree on these matrices, confirming that the native
rank discrepancy came from premature Jacobi skipping. Twelve fixed-order
proposals pass the unchanged root-radius/count/conjugate and alias rules and
receive training-only QR ranking, with no truth or held-out selection. Twelve
native final fits produce six accepts, but only four pass every original
count/frequency/amplitude/phase/DC oracle: cases10,12,19,21. Their maximum
frequency error is 5.70805e-6 Hz. Uniform K16 case18 misses the phase limit
(3.34780e-4 rad versus1e-4); case20 misses frequency (.022265 Hz versus.02) and
phase (.00144069 rad). Both stop with zero accepted refinements, so increasing
the refinement-count cap alone would not address the observed problem.

This recovers 4/6 sources only in a mixed NumPy/native experiment. Integrated
quality remains31/40. Artifacts use
`dense-frequency-research-forward-backward-*`, including separate native-fit
results and post-fit scoring. Truth is used only after candidate selection.

The subsequent C++ augmented initializer also recovers4/6 sources:12 fits,
6 accepts, and the same four complete oracle passes. Its original-H control
matches all24 current native results exactly for status, rank, conditions,
roots and consumed work. The augmented SVD and row-shift solve are private
research implementations; roots and final fitting use the actual native
components. Training FFT guides and alias QR still use NumPy. Every original
rank, root-radius, conjugate, trust, condition and final validation gate stays
enabled. Case18 still fails phase (.000248821rad); case20 fails frequency
(.020862Hz) and phase(.00135009rad), with zero accepted refinements for both.

Root review repaired three adapter defects before execution: a shadowed row
constant, an eigen-solver variable-name error, and elimination beginning at the
pivot instead of updating all non-pivot rows. The claimed maximum was also
corrected: full augmented Jacobi permits84,102,756 work units, not3,574,581.
Measured subspace work is39,427,266..46,569,511 units per source, in17..21 sweeps;
all24 recorded work bounds hold. This is too expensive to assume callback
integration is ready. A research follow-up splits the real forward/backward
matrix into even and odd row spaces, retaining the same global rank threshold.
The full C++ prototype and results use `forward-backward-native-*`.

The even/odd C++ split preserves the same4/6 complete source passes and all24
original-H control records exactly. All96 subspace/preparation/shift/eigen work
bounds hold. It consumes6,744,659..7,278,659 subspace units per source, totaling
42,582,038 versus256,053,873 for the full augmented version (about83% less).
Its maximum is13,304,351 units. This is measured arithmetic work, not callback
latency. The fixed-storage incremental implementation is being prepared;
integrated audio quality has not yet changed.

A separate fixed32-sample block-mean trajectory uses more of the existing
training samples before differencing. NumPy seeds and the unchanged native
final fitter still recover4/6 sources; remaining phase errors improve to
1.21126e-4 and1.31653e-4 but stay above1e-4. Its point-sampled control replays
all12 original NumPy-seed results exactly. No filter parameters are selected
using truth or held-out data. Higher-frequency alias behavior is unverified.

Another source-only experiment uses projected frequency QR with at most three
accepted steps and fixed backtracking scales1,.5,.25,.125. Raw full-Jacobian
conditions are recorded, while the projected system receives its own1e12
condition gate. This changes the numerical system being admitted; it is not
equivalent to retaining the original raw-Jacobian gate. The unchanged native
final fitter then validates each polished candidate. Complete source recovery
is6/6 for NumPy seeds,5/6 for full C++ seeds, and6/6 for NumPy block-mean seeds.
Every group submits all12 original candidates. Truth is used only afterward.
The two-stage research pipeline can perform up to six refinements and has
sequential trust regions. It is not a production three-step/one-bin result.
An isolated native fitter prototype will test projected conditioning and
backtracking inside the original three-step and one-bin limits. Artifacts use
`dense-frequency-research-projected-seed-*`.

The two initializer drivers accidentally shared the same final-fit filenames:
the NumPy prefix plus `-native-fit` equals the C++ prefix plus `-fit`. The later
C++ run overwrote those two files. Both complete reports retained their seed
frequencies and fit records. Root reconstructed each input from its saved seeds
and original raw JSONL samples, recovered both result sets, and gave the drivers
distinct output names. The reconstructed C++ input is byte-identical to the
overwritten file. The repair audit stores SHA256 hashes; original reports and
the overwritten files remain available. New names are `forward-backward-numpy-fit-*`
and `forward-backward-native-fitter-*`. A subsequent side-by-side native replay
matches every shared baseline result field exactly for all24 candidates across
the two input sets. Earlier training diagnostics that paired the shared TSV
with the NumPy report mixed candidates; do not use those pairings as evidence.

The isolated `ResearchSinusoidFit` appends K zero-right-hand-side Jacobian rows,
penalizing frequency steps with one predeclared scale `64*double epsilon` and
training derivative-column norms. It retains the original trust region,
three-step cap, condition bound, training decrease and final acceptance gates.
Root repaired an adapter enum-type typo, compiled it separately, and tested
all12 candidates from each initializer. This variant is rejected: complete
source recovery falls from4/6 to3/6 for BOTH input sets, losing case21 without
recovering18 or20. With C++ seeds, training RMS for18/20 falls to4.08795e-11 /
1.85864e-11, but held-out RMS rises to3.56798e-5 /9.90217e-5 and phase errors
remain2.54472e-4 /1.39084e-3. NumPy-seeded case20 additionally fails the original
held-out1e-4 limit. Better training residual is insufficient for this task.
No production fitter change or relaxed gate is selected. Evidence is saved in
`dense-frequency-research-conditioned-refinement-postfit-results.json`.

### Premature Jacobi column skipping

The native rank discrepancy exposed a separate numerical defect. The Jacobi
solver stopped rotating a column below the final rank floor before several weak
columns could combine into an admitted singular mode. A new analytical
regression, `1 + 4e-12*sin(2*pi*7*i/97)` with 191 samples and a 97x95 Hankel
matrix, fails on the original code: it must retain DC and both sinusoidal modes.
An independent centered two-by-two Gram approximation checks the weak singular
values to 0.1%; its higher-order correction is below input rounding here. A
ten-times weaker sinusoid must still leave only DC under the unchanged floor.

A research-only machine-epsilon internal cutoff restores native K12 ranks
23→24 and recovers case 12 in the speculative final-fit experiment, but costs
88,518,757 lag work units across six sources versus the original 51,745,384.
In production it regresses dense quality to 25/40 through slower discovery.
Caching column norms alone with that cutoff also gives 25/40. Both failures are
preserved; neither is the selected implementation.

The selected implementation caches each column's squared norm during matrix
fill and after each complete rotation. It rebuilds sums in row order, so no
subtractive norm update accumulates drift. A skipped pair now consumes one work
unit before the row scan. Its internal squared cutoff is
`energy * relativeSingularFloor^2 / columns^2`, while final rank admission stays
unchanged. Frozen columns together have Frobenius norm at most the final rank
floor divided by `sqrt(columns)`, up to rounding. This bounds discarded energy;
it is not an exact-rank guarantee for arbitrarily borderline inputs.

A less conservative `/columns` variant restores 31/40 and uses 41,798,669 lag
units, but fails the new 0.1% weak-singular-value check. It is not selected.
The `/columns^2` variant uses 66,523,956 units on the six source fixtures, passes
the new analytical test plus the existing primitive suite, and retains 31/40
Release dense quality. Release passes all nine selected DSP/primitive/model/
runtime targets in161.54seconds, preserving64/64 older quality cases,187 dirty
model snapshots and2913 processing snapshots. Its four real-discovery cases
remain inside16seconds: K8 readiness at49536/232064 andK16 at150016/479104
forN8192/65536. Independent NumPy checks pass36/36 subspaces (maximum relative
singular error1.621e-14, projector error3.016e-12) and289/289 source models.
Debug passes all four selected DSP/lag/modes/model targets in328.68seconds,
including the same64/64 older quality cases,187 model copies and2913 processing
snapshots. Its dense gate also remains31/40, with the same nine failed profiles.
The current Release source-only dump still accepts18/24 stationary sources and
rejects all3 held-out changes; the six source failures remain.
No input shape, allocation, state size, maximum sweeps, final numerical gate or
declared maximum-work formula changes. State-copy coverage still requires the
known rejected seven-tone fit to retry at order16; other fixtures may accept
before exhausting their retained rank and are no longer incorrectly required
to retry merely because they happen to be penultimate.

### Callback diagnostics after the Jacobi repair

All calls retain exact reference output and no render allocation. These are
current-host observations, not formal performance acceptance; exit0 does not
assert deadlines. Keep the preceding failed campaigns as historical evidence.

| Build / lane | Groups / observations | Thread CPU misses | Wall misses |
| --- | ---: | ---: | ---: |
| Release K8 | 12 / 19637 | 1 | 3 |
| Release K16 | 12 / 33212 | 0 | 12 |
| Release live/separated | 11 / 43771 | 2 overlapping | 3 overlapping |
| Release rollback | 40 / 8576 | unmeasured | 0 |
| Debug K8 | 12 / 19637 | 0 | 1 |
| Debug K16 | 12 / 33212 | 2 | 11 |
| Debug live/separated | 11 / 43771 | 1 | 8 |
| Debug rollback | 40 / 8576 | unmeasured | 0 |

Live/separated groups are overlapping aggregate and component observations;
nine groups measure CPU. Release's two CPU counts are one actual live callback
(718.709us) also reported in `live_other`. Debug's CPU count is the immutable
comparison (748.458us); actual public live callbacks have no CPU/wall misses in
that run. Wall-only pauses include51,951.4us in a separated Debug kernel call.

Release K8's CPU miss is704.792us atN65536/block32/offset231680, versus666.667us.
Untimed replay finds frame Analysis and the estimator idle before and after the
block: this block does not execute the Jacobi solver. Debug K16's two CPU misses
are atN8192/block128:4385.96us at67328 and4226.17us at63360, versus2666.67us.
Untimed replay finds Modes in both blocks; the first consumes40960 estimator
units with idle frame work, and the second32768 units ending in capture.
These observed CPU and wall failures keep the callback gate open. No further
timing rerun is justified without a concrete change or unresolved measurement
question. Logs use `dense-frequency-research-lag-rank-final-*-deadline-*`.

An independent review derived a necessary lower-order bound from the singular
tail of the differenced Hankel matrix. In exact arithmetic an accepted K-partial
model requires tail(2K) <= 2*sqrt(95)*sqrt(trainingCount)*1e-4*normalizer. A NumPy
diagnostic suggests 244/325 attempted lower orders could be skipped. The current
Jacobi norms and residual arithmetic need conservative error bounds before using
this to reject orders permanently. No production change implements this idea.
The 244/325 diagnostic used raw native JSONL samples and is unaffected by the
separate synthetic-phase error above. A subsequent review proposes using the
last zero-rotation Jacobi sweep to bound Gram off-diagonal energy, plus source,
rotation and residual-evaluation rounding bounds. The product norm bound must
include the unchanged directions of each plane rotation (at least one per
factor). A portable proof still lacks a tight `std::sin`/`std::cos` error
guarantee; without that guarantee, uncertain orders must remain eligible.

## Artifacts and pending checks

Logs live under the registered `build/workbench-debug` root with prefix
`time-frequency-scale-dense-partials-`. Quality suffixes are `initial-release`,
`capacity-release`, `unique-fit-release` and `shared-work-release`. Source/eigen
JSONL, stderr and NumPy logs use `model-initial-*` and `modes-*`; state results use
`unique-fit-model-release`; scheduling uses `frame-progress-release`. Numerical
research files use `dense-frequency-research-*` in the same root.

Pending: the nine dense quality failures, six source-model failures, remaining
callback timing failures, bounded initializer research and artifact observation.
Formal benchmark targets remain OFF and their existing
admission gates remain unchanged.
