# PhaseShiftBy90 reference candidate

## Behavior

PhaseShiftBy90 is a pair of filters whose outputs differ by 90 degrees at
Frequency: one advances the input by 45 degrees and the other delays it by
45 degrees. Only the selected frequency has that relative phase. The input is
a Sound with left and right channels. QuadOscillator sums left Envelope times
sine and right Envelope times cosine; quadrature input cancels the lower
sideband. Swapping Envelope channels or using negative carrier frequency
selects the lower band. SingleSideBandRM cancellation is weaker away from
Frequency.

This candidate is a narrow-band IIR allpass pair. Broadband quadrature is not
implemented.

## Explicit candidate choices

The descriptor exposes `audioOut` for the left channel and `rightOut` for the
right channel. Optional `rightIn` supplies a separate right input; when absent,
both branches receive `audioIn`. Each branch has independent filter history.
The left branch advances and the right branch delays, choosing the orientation
that selects the upper band when connected to QuadOscillator. This
sign-to-channel assignment is a model choice.

For `t = tan(pi * Frequency / sampleRate)` and `r = sqrt(2) - 1`, the implementation
uses first-order allpasses `H(a,z) = (a + z^-1)/(1 + a*z^-1)`:

- left: `-H((t*r - 1)/(t*r + 1), z)`;
- right: `H((t/r - 1)/(t/r + 1), z)`.

At Frequency, their steady-state phases are +45 and -45 degrees. Both have unit
magnitude. Their relative phase differs from 90 degrees away from Frequency,
so an expanded PhaseShiftBy90/QuadOscillator leaves a residual opposite band.
The filters have direct feedthrough and require no fixed sample alignment.
Frequency must be finite, positive and below Nyquist, with representable stable
coefficients. Connected frequency takes precedence over the authored fallback.

These coefficients, unit gain, mono expansion and zero-initialized histories
are implementation choices, not measured against an external reference.
Nonlinear response, transients, gain and coefficients are not calibrated.

## Verification

The new `talas_phase_shift_by_90_reference_tests` first failed because the descriptor
had no right output. The previous mono -90-degree implementation could not meet
the two-output pair contract. All six new test groups now pass:

- sinusoidal phase at 64, 128 and 512 Hz, plus an off-target tone;
- separate stereo inputs, including project save/reopen;
- opposite-sideband cancellation through QuadOscillator with signed carriers;
- exact offline/prepared agreement across 1/7/31-frame callbacks, measured
  allocation-free rendering/reset and late right-lane failure/retry;
- offline restoration of both histories, input position and schedule after
  a late right-lane overflow;
- rejection of three forged right-output, input-binding and producer records
  by both backends.

C100/C160 now route both channels and retain exact backend, partition and reset
comparisons. Their independently calculated Python model vectors use a `1e-14`
absolute tolerance for transcendental and constant rounding. The initial bitwise
model comparison exposed differences no larger than `2.23e-16`; these vectors
are mathematical candidate oracles, not external recordings.

The full catalog, compile-plan, offline, prepared-scope, common-conformance,
nested-Sound, realtime, MIDI-playback, project-bridge and CLI-example suites pass
(**10/10** regression suites). The isolated Workbench smoke exits 0 with
`WORKBENCH_SMOKE_OK` and zero JUCE assertions; its final log is
`build/workbench-debug/phase-shift-workbench-smoke-final.log`. The initial launch
required creation of its output directory and exited before the smoke; that
separate launch log is preserved. Generated catalog/matrix/help freshness and
whitespace checks pass. No agreement with an external reference is claimed.
