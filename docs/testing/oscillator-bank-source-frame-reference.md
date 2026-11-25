# OscillatorBank source-frame cadence verification

Verified native candidate, 2026-09-27. This corrects an existing native
candidate; it is not checked against an external reference.

## Source and correction

A Spectrum source's NbrPartials may exceed the number of oscillators receiving
the spectrum. An N-partial bandwidthless stream has
one amplitude/frequency pair per sample and repeats after N samples. The old
bank instead wrapped its arrival index at its selected oscillator count M.
With source N3 and bank M1, the selected oscillator therefore took all three
source amplitudes successively.

The new distinguishing test failed against that code at offline frame 1.
Its flat single-sample wavetable and zero frequencies isolate pair selection
from oscillator phase and interpolation. The test uses only the paired input;
an initial mistaken simultaneous packed-input connection was removed before
capturing the genuine numerical failure.

`CheckedSerializedSpectrumSource.h` identifies exact matched direct pair outputs
from Array, Sounds and SpectrumInRAM. Their authenticated sibling Spectrum
header supplies actual N, including connected Sounds counts and inferred InRAM
selection. InRAM count zero selects the remaining range; an explicit count
greater than that range still rejects. Serialized-only Sounds publishes a valid zero-filled N-partial
placeholder; an explicitly connected packed output follows its original data
path. Existing pair edges already order producer and consumer.

Both renderers retain M oscillator states, validate every incoming pair and
update retained state only for arrival indices below M. The index wraps at N;
N below M rejects. A changed N rebases the index modulo new N. Raw, crossed,
or transformed Audio pairs preserve the native implicit N=M convention because
the graph does not propagate their source-frame metadata. Existing reset and
whole-render rollback cover the retained state and index. An empty offline
render preserves a warm bank's phase.

The update-before-synthesis rule, initial zero components, absolute-time phase,
wavetable interpolation and source-count transition convention remain native
candidates. Serialized bandwidths and general metadata propagation remain open.

## Executed verification

All **seven focused groups** in
`talas_oscillator_bank_source_frame_reference_tests` pass:

1. Direct Array N3 feeding one oscillator preserves its first amplitude over
   39 samples and prepared 1/7/31 partitions. This is the original RED fixture.
2. Sounds N3 feeding two oscillators receives changing amplitudes and nonzero
   frequencies. An independent piecewise triangle oracle matches offline and
   native float input callbacks exactly. An unused fallback N1 does not override
   connected N3. Reset reproduces output; an empty offline render at source
   phase two preserves the cursor. Callbacks and reset allocate nothing.
3. Connected Sounds count changes N4 -> N2 -> N3 rebase a nonzero source phase
   and preserve selected values while unselected pairs arrive.
4. InRAM's remaining-count sentinel with firstPartial 2 -> 4 -> 2 yields actual
   N3 -> N1 -> N3 from a four-partial table. Distinct amplitudes/frequencies
   prove active-header authority, source/consumer alignment, partition equality
   and reset. An initial fixture incorrectly expected an oversized explicit
   count to clip; inspection of CheckedSpectrumPartialRange corrected the test
   to the existing zero-sentinel contract without changing InRAM behavior.
5. Downstream overflow after bank updates silences the entire callback, then
   a repaired retry matches an untouched session from an unselected phase.
   Nyquist rejection after a count change restores the previous unnormalized
   phase. Offline late invalid input followed by a valid-prefix retry restores
   source input position and retained values. All prepared attempts allocate zero.
6. Raw and crossed-source Audio pairs preserve their implicit N=M behavior in
   both renderers. Metadata is never inferred from one lane alone.
7. N<M rejects; forged pair scope, duplicate inputs, a count-output binding
   redirected to an Audio buffer and duplicate count-buffer identity reject in
   both paths.

All **nine broader CTest groups** pass in **174.76 seconds**: catalog, compile,
offline, prepared scope authority, runtime conformance, realtime, formant-bank
reference, CLI examples and Sound Library. Runtime/CLI and Workbench builds
pass. Generated catalog, help and capability freshness checks pass. Read-only runtime review
found no actionable defect in source authority, buffer lifetime, placeholder
format, phase normalization or rollback state ownership.

Logs under the registered disposable `build/workbench-debug` root:
`oscillator-source-frame-red-build.log`,
`oscillator-source-frame-red-test.log`,
`oscillator-source-frame-runtime-build.log`,
`oscillator-source-frame-regression-build.log`,
`oscillator-source-frame-regression-tests.log`,
`oscillator-source-frame-reference-build.log`,
`oscillator-source-frame-reference-tests.log`,
`oscillator-source-frame-final-build.log`,
`oscillator-source-frame-coverage-tests.log` and
`oscillator-source-frame-generated-check.log`.
