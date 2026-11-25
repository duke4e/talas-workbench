# AnalysisFilter reference audit

## Contract

AnalysisFilter is a harmonic-isolating bandpass with center
`Fundamental * Harmonic`, cosine output on the left and sine output on the right.
A bank of AnalysisFilters includes harmonic zero, extends to the harmonic
nearest half the sample rate and is intended to sum to unit amplitude.

The contract does not fix coefficients, per-filter gain, zero-bin filter shape,
startup state, live transitions or an exact Nyquist endpoint rule. The existing
positive-harmonic bandpass/allpass model is consistent with quadrature at the selected frequency,
but that alone does not establish the bank's normalization.

## Zero-harmonic candidate

The initial `talas_analysis_filter_reference_tests` compiled successfully and failed
because harmonic zero was rejected. Static, direct live and connected harmonic
admission now permits finite nonnegative values; Fundamental remains positive,
and the center must remain below Nyquist. Negative harmonics are still rejected.

The zero bin uses the continuous-center limit of the existing RBJ bandpass at
fixed bandwidth `Fundamental`. For `alpha = pi * Fundamental / sampleRate`,

`H(z) = alpha * (1 + z^-1) / ((1 + alpha) - (1 - alpha) * z^-1)`.

Its real output has unity DC gain. The imaginary/sine output is zero. The
bandpass history is retained across bin changes; the absent sine history is
cleared in the zero bin. Both operations are checked before state publication,
and existing block/whole-call rollback and reset include the histories.
The positive-harmonic arithmetic remains unchanged.

This lowpass shape and transition policy are explicit implementation choices,
not measured against an external reference. Representable stable coefficients and finite sample
transitions are required. The code does not claim that admitting harmonic zero
also implements the complete bank normalization.

## Complete-bank measurement

The CLI probe sums each output channel across harmonic indices 0 through 4.
At sample rate 8192 Hz and Fundamental 1000 Hz, the nearest harmonic to Nyquist
is 4000 Hz, safely below Nyquist. This avoids the unresolved endpoint question.
Each independent input sine has amplitude 0.1. Each render has 8192 samples;
sine/cosine projection over the final 4096 samples measures the settled amplitude.
Twenty per-bin CLI renders produced these normalized gains:

| Input frequency | Summed left gain | Summed right gain |
| --- | ---: | ---: |
| 1000 Hz | 1.5513417264 | 0.8493436305 |
| 2000 Hz | 1.4843750128 | 0.8102651832 |
| 3500 Hz | 0.4910010219 | 0.4265509135 |
| 4000 Hz | 1.0042591871 | 0.9403442038 |

Under the direct interpretation of summing the filters' corresponding output
channels, the current candidate does not meet unit amplitude. For the 1000 Hz
input, the summed left amplitude is 55.1% above unity. A single output gain
correction cannot fix these frequency-dependent results. No normalization
algorithm or per-bin gain correction has been inferred from these measurements.
The full-bank requirement remains open and separate from zero-bin admission.
This is an internal counterexample to the bank-level unit-amplitude contract,
not a comparison with an external reference.

The contract does not disambiguate a sum of transfer functions, a sum of their
magnitudes or a particular reconstruction from the two output lanes. A
phase-aligned partition of unity or a telescoping bank of matched cumulative
lowpasses could meet a coherent reconstruction contract, but either would
define a new filter design. Normalizing magnitudes alone does not align phase.
A useful reference capture would measure per-bin complex gains at harmonic
centers, midpoints, off-bin tones, DC and Nyquist, then compare complex sums,
magnitude sums and each lane separately. Integer and fractional ratios of
Nyquist to Fundamental distinguish endpoint policies. These alternatives are
analysis of possible constructions, not a description of an existing algorithm.

The reproducible script, twenty exact input projects and full-precision report
are retained under registered `build/workbench-debug` as
`analysis-bank-probe.py`, `analysis-bank-probe-<frequency>hz-h<harmonic>.talas.json`
and `analysis-bank-probe-report.json`. The initial probe failed project parsing
because a required presentation layout was omitted; the corrected input layouts
preserve all three nodes and the final run exits 0. The initial and final logs
are separate.

## Verification status

The new reference suite passes all five focused cases:

- Settled unity DC, zero sine output and attenuation of a 2048 Hz tone.
- Direct EventValue and connected control schedules changing both Fundamental
  and Harmonic through zero/positive/zero spans. Offline and allocation-free
  1/7/31-frame realtime output agree exactly, including after reset.
- Realtime downstream overflow after positive-bin warmup, with both histories,
  controls and schedule restored for a successful retry.
- Offline downstream overflow, identical failed retry and exact reset replay.

The original baseline failed specifically at zero-harmonic admission. The first
expanded run exposed a fixture domain error: Constant control outputs need
scalar EventValues, while direct Fundamental expressions use Hertz. Corrected
fixtures pass in 0.73s (`analysis-reference-expanded-tests-2.log`). The test
preserves the promoted support assertion with positive out-of-range hidden
fallbacks, avoiding the unrelated unary-expression profile restriction.

Seven catalog, compile-plan, offline, prepared-scope, conformance, realtime and
CLI regression suites pass, 7/7 in 167.96s. The final connected-fallback fixture
rerun passes the realtime suite in 60.34s. CLI and Workbench build; isolated
Workbench smoke reports `WORKBENCH_SMOKE_OK` with zero JUCE assertions.

The separate permanent counterexample is executable with:

```sh
build/workbench-debug/talas_analysis_filter_reference_tests --check-bank-unity
```

It currently exits 1: expected summed left gain 1.000000, measured 1.551342.
Its log is `build/workbench-debug/analysis-bank-unity-oracle.log`. It is not a
passing default CTest gate; the complete-bank requirement remains unmet. The
zero-harmonic candidate and the complete-bank requirement are tracked
separately.

## 48 kHz measurement packet

`tools/analysis_filter_bank_probe.py` now produces reproducible native projects,
five input stimuli and both actual rendering paths. The prepared callbacks use
1/7/31-frame partitions with allocation counting. A separate graph renders the
five filters and unity-gain stereo summation together, in addition to isolated
member captures. The native runtime implementation is unchanged by this packet.

At 48 kHz, Fundamental 5625 Hz and harmonics 0..4, a coherent multitone input
separates possible interpretations of the bank sum. Standard PCM16 measurements
of the offline candidate include:

| Input Hz | Complex left sum magnitude | Sum of left magnitudes | Positive analytic gain | Negative analytic gain |
| --- | ---: | ---: | ---: | ---: |
| 2812.5 | 1.108831 | 1.567585 | 0.597186 | 0.669920 |
| 5625 | 1.554870 | 2.018635 | 1.176194 | 0.426637 |
| 11250 | 1.495464 | 2.048590 | 1.148402 | 0.347279 |
| 16875 | 1.293052 | 1.707356 | 1.022824 | 0.270240 |
| 22500 | 1.026445 | 1.186920 | 0.940502 | 0.104155 |

The DC left/right gains are 1/0; the Nyquist input gives 0/0. Analytic gains
are `(left ± i*right)/2` and are omitted at those real endpoints. These figures
show that the open gap survives several distinct interpretations, including at
the assumed Fundamental itself. They do not establish which metric is intended.
Midpoint, off-bin and endpoint probes remain diagnostic rather than additional
invented acceptance requirements. Quantization and actual candidate input
conversion remain part of these measurements.

The separate pure 5625 Hz input meets the assumed-Fundamental setup directly.
Its isolated left sum is 1.554960 and its directly rendered bank is 1.554945;
the full graph therefore retains the same normalization failure. The difference
between a direct PCM bank and the sum of independently quantized member WAVs
is at most two PCM16 integer steps across all five stimuli.

All 696 complex gains agree with an independent NumPy FFT to `4.71e-13` maximum
absolute error. The maximum raw offline/realtime sample difference is
`4.76413e-9`, consistent with their double/float input paths. All six generated
native projects validate, and CLI renders of the multitone and pure-tone bank
match the corresponding native offline WAVs byte for byte. Ten measurement and
packet-integrity tests pass; the five default native reference cases still pass.
The opt-in floating-point bank counterexample still exits 1 at gain 1.551342.

The final packet contains 60 candidate WAVs and is retained under
`build/workbench-debug/analysis-bank-capture-20260927-final`. Exact commands, source,
runner and file fingerprints are in its manifest. External PCM passed to the
tool's `analyze` command is reported as unreviewed and is not treated as
reference evidence.
