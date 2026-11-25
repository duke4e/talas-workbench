# SingleSideBandRM reference candidate

## Behavior

SingleSideBandRM is a stereo phase shift followed by quadrature
modulation: multiply the left phase-shifted channel by sine, the right by cosine,
then add them. The selected Frequency has the strongest opposite-sideband
cancellation; cancellation becomes weaker away from it. FreqScale is the ratio
by which content at Frequency is scaled. The construction expands into
PhaseShiftBy90 and QuadOscillator stages.

The previous local implementation used one -90-degree allpass and a
dry*cosine-minus-quadrature*sine mix. It could shift the tuned sinusoid but did
not equal the expanded PhaseShiftBy90/QuadOscillator pair, and had no
separate right input. The
new direct-versus-expanded test failed against that implementation before the
correction and passes after it.

## Explicit candidate choices

`CheckedSingleSideBandRmState.h` shares the checked two-filter phase pair and
summed quadrature carrier implementations. Carrier frequency is
`Frequency * (FreqScale - 1)`. Optional `rightIn` supplies an independent right
channel; absent `rightIn`, both filters receive `audioIn`. Their two modulation
products produce one mono `audioOut`. Independent stereo output is not implied.

The [phase-pair model](phase-shift-by-90-reference.md) specifies first-order,
unit-gain filters, left +45 degrees and right -45 degrees at Frequency, with
zero initial histories. Carrier phase starts at zero. At the tuning frequency,
a settled sine input therefore yields the scaled-frequency sine with a
-45-degree phase offset. These coefficients, gain, channel orientation,
initialization and mono expansion are candidate choices, not measured against
an external reference.

Existing admission remains: finite positive Frequency below Nyquist and finite
positive FreqScale, with representable finite intermediate arithmetic. A scale
between zero and one creates a negative carrier; scale one creates zero carrier.
Connected controls take precedence over authored fallbacks. Direct EventValue
frequency/scale schedules retain their existing sample timing. The internal SSB
carrier retains finite aliased frequencies admitted by the earlier implementation;
the public QuadOscillator still requires absolute frequency below Nyquist.
Direct/expanded equality is consequently tested within their common domain.

Zero and negative FreqScale are not admitted. QuadOscillator accepts a
negative carrier frequency, but that does not define negative SingleSideBandRM
FreqScale behavior; the positive-scale restriction is a deliberate bound.
Coefficients, nonlinear gain, transients, alias policy, total latency and
broadband behavior are not calibrated against an external reference.

## State and authority

Both filter histories and carrier phase commit only after a valid transition.
Offline and prepared block processing use candidate state; whole-call rollback
and reset cover all three parts. The optional right binding participates in
descriptor sealing, exact compiler-owned plans, buffer shape and prior-producer
checks. No new scope or polyphonic admission is asserted.

## Verification

The baseline `talas_single_side_band_rm_reference_tests` build passed, then its
direct/expanded comparison failed against the former DSP. After the correction,
all **8/8 focused test groups pass**:

- exact direct/expanded equality for upward, downward and unity scale;
- independent settled sine amplitude/phase and opposite-sideband projections;
- finite aliased carrier agreement in offline and prepared rendering;
- explicit right input, save/reopen and exact agreement across callbacks;
- connected frequency and scale schedules across 1/7/31-frame callbacks and
  reset, without measured allocation;
- late realtime failure/retry restoring both histories and carrier phase;
- offline failure/retry/reset restoring both histories, phase, input position
  and scheduled failure frame;
- rejection of three forged right-input buffer, binding and producer records
  by both backends.

The seven shared regression suites, including the existing PhaseShiftBy90
reference suite, pass in 70.11s. The migrated catalog, offline, realtime and CLI
example suites pass 4/4 in 127.32s. Together with the new SSB suite, all **12/12
executed test binaries pass**. Independently calculated C101/C161 model vectors
allow `1e-14` rounding error; CLI/backend, partition and reset comparisons stay
exact. The offline quarter-cycle sine oracle uses its closed form to avoid
large-angle argument rounding.

CLI and Workbench build successfully. The isolated Workbench smoke exits 0 with
`WORKBENCH_SMOKE_OK` and zero JUCE assertions; log:
`build/workbench-debug/ssb-workbench-smoke.log`. Generated catalog, capability
matrix and help freshness checks pass, as does the whole-checkout whitespace
check. Existing ignored duplicate-library linker warnings remain.
