# TwoFormantElement construction audit

## Contract

TwoFormantElement is realized as a DualParallelTwoPoleFilter: two summed
second-order sections, each with fixed zeros at complex `1 % 0`, conjugate
poles and an independent signed scale. The frequency/bandwidth-to-pole
equation and any peak-gain normalization are Talas model choices.

The former TwoFormantElement candidate used an RBJ bandpass numerator, with
zeros at `+1` and `-1`. The existing DualParallelTwoPoleFilter numerator is
`[1, -2, 1]`, with both zeros at `+1`. The initial new reference test built and
failed with `TwoFormantElement must equal its documented fixed-zero dual-pole
expansion`; this establishes a reproducible construction mismatch.

## Bounded correction

Both renderers now call `makeFormantPoleCoefficients` in
`src/runtime/CheckedBiquadMath.h`. It constructs conjugate poles using the
explicit candidate map:

```
r = exp(-pi * bandwidth / sampleRate)
theta = 2 * pi * formant / sampleRate
pole = r*cos(theta) % r*sin(theta)
```

It then uses the existing fixed-pole coefficient constructor. Independent
signed scales apply to the section outputs without peak normalization. This
restores the DualParallelTwoPoleFilter section topology and enables an exact
graph expansion oracle. The radius/angle map is a model choice, not an observed
external algorithm.

Sample rate, formant frequency and bandwidth must be finite and positive, with
formant frequency below Nyquist. Arithmetic must remain finite; the computed
radius and coefficient radius squared must be strictly below one. A positive
bandwidth that rounds to a unit radius is rejected, while exponential underflow
to zero radius is admitted. Section histories, per-frame controls and public
failure/reset transactions retain their existing state model. Gain, the
bandwidth convention, extreme bounds and live transitions are not calibrated
against an external reference.

## Separate Voice gaps

TwoFormantVoiceElement is meant to use glottal-like excitation and a randomly
selected vibrato rate, and it is not tied to the TwoFormantElement
construction. Its current implementation generates a sine without vibrato and
filters it with RBJ sections. Seed is validated but unused. Excitation and
vibrato are two separate unimplemented behaviours.

The invalid regression coupling Voice output to the changed TwoFormantElement
has been replaced by a 16-sample Voice regression captured from the existing
candidate binary before this correction. Those samples are candidate output,
not an external reference. The C73 mixed example's eight expected samples were derived
with an independent Python scalar recurrence: the derivation first reproduced
the old RBJ/RBJ vector exactly, then replaced only its TwoFormantElement branch.
No gain compensation was added to the example; its new raw peak exceeds one.

## Verification

All five focused cases pass in `talas_two_formant_reference_tests`:

1. Exact explicit-pole graph equality at 8192, 16000 and 44100 Hz, including
   signed scales and a muted section.
2. Independently scaled section sums and sign inversion.
3. Settled DC rejection with a nonzero Nyquist response, distinguishing the
   specified fixed zeros from the former RBJ numerator.
4. Six connected controls changed at frames 37 and 83, an independent
   direct-form recurrence, exact offline/native-float prepared agreement in
   1/7/31-frame callbacks, allocation checks, and both backend resets.
5. A positive bandwidth that rounds the radius to one, rejected after two
   callback samples, with zero public output and exact repaired retry after
   restoring histories, controls and scheduling state.

The connected fixture retains the established offline `Subset` and prepared
`Supported` authority. Each six-control change uses two three-value API
transactions at the same frame. Earlier test-only assumptions about an offline
`Supported` tier and six-value transactions were corrected to the existing
public contract; runtime behavior was not changed to accommodate those tests.

The eight broader CTest groups pass in **168.39 seconds**: catalog, compile,
offline, prepared scope, conformance, realtime, checked-biquad family authority
and CLI examples. The first run failed closed on two exact descriptor seals
missing their generated final periods; both seals were corrected without
relaxing authority. Workbench built and passed an isolated smoke run with
`WORKBENCH_SMOKE_OK` and zero JUCE assertion failures.

Evidence under the registered disposable `build/workbench-debug` root:

- Initial pre-correction failure: `two-formant-reference-red.log`.
- Final focused build: `two-formant-reference-build-after-3.log`;
  five-case run: `two-formant-reference-tests-3.log` (0.87 seconds).
- Broad build: `two-formant-regression-build-2.log`;
  tests: `two-formant-regression-tests-2.log`.
- Smoke: `two-formant-workbench-smoke.log`, outputs in
  `two-formant-smoke-20260927`.
- Previous Voice regression capture: `two-formant-voice-baseline-render.json`;
  independent C73 calculation: `two-formant-c73-derived-golden.json`.

Generated catalog, capability matrix and node help freshness checks pass.
Tracked and new-source whitespace checks pass. Passing local tests does not
establish agreement with an external reference.
