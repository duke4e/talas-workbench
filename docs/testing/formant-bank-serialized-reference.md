# Formant-bank serialized input verification

Verified native candidate, 2026-09-27. This package extends the previously
checked impulse synthesis; it is not checked against external reference
recordings.

Follow-up: [plain FormantBank direct InRAM and runtime counts](formant-bank-inram-source-reference.md)
extends the plain bank beyond this package's fixed-count source profile and
stores only selected pairs. The FBO profile recorded here is unchanged.

## Source and implementation

FormantBank may resynthesize fewer partials than its Spectrum source
contains. A serialized Spectrum source carries two amplitude/frequency channels
in an N-sample frame without bandwidths. With bandwidths, only alternating
right-input values are defined; left-lane alignment and the N-versus-2N period
remain unresolved. FBO requires bandwidth information. No repeated amplitudes,
third lane or 2N contract was invented.

Both banks accept the original packed Spectrum or direct matching Audio pair
outputs from one scoped SyntheticSpectrumFromArray/FromSounds. Their static
source N sets storage and cadence independently of the selected formant count
or FBO first index. One retained pair updates before each output sample.
Unreceived components start at zero; width is zero and uses the existing
native table-rate convention. These onset rules and the FBO zero-width
extension remain native choices. Dynamic count, indirect sources, logarithmic
encoding and serialized bandwidths remain rejected.

Offline state journaling and prepared preallocated checkpoints now include the
retained pairs and cycle index. Reset clears them with all impulse histories.
The shared source resolver checks source identity, scope, port names, buffer
ownership, encoding and fixed count. Both renderers share exact ordered input
shape validation, including exclusive packed-versus-pair admission.

## Executed evidence

All **14 grouped cases** in `talas_formant_bank_reference_tests` pass (1.84 seconds).
The five added groups cover:

1. A three-pair static source feeding one selected Bank formant and non-first
   FBO selection. Independent DC/table pulse samples verify source-N ordering;
   a serialized project round-trip preserves connections and selection.
2. A four-pair FromSounds source with changing nonzero centers and amplitudes.
   Both banks follow an independent cosine-by-pulse-age oracle, exactly match
   offline output across native float 1/7/31 callbacks, and replay after reset.
   Callbacks and reset allocate nothing.
3. A late Nyquist value in an unselected component and a late FBO arithmetic
   overflow after another bank has processed its pairs. Failures publish
   silence, and repaired retries match untouched sessions with zero callback
   allocations. Offline failed render followed by a valid-prefix retry also
   preserves pair index, retained values and input position.
4. Missing/mixed lanes, ordinary audio sources, crossed producer identities,
   incomplete Array vectors, logarithmic/width encoding and connected source
   count reject in compilation or both native renderers.
5. Forged extra/duplicate bindings and a mismatched amplitude-lane source scope
   reject in both paths. The scope mutation targets the pair lane explicitly,
   including FBO whose first input is its cascade.

All **8 broader CTest groups** pass in **172.14 seconds**: catalog, compile,
offline, prepared scope authority, runtime conformance, realtime, CLI examples
and Sound Library. The existing packed graphs, impulse assets, connected
controls, resource authority and overlap boundaries retain their tests.

Workbench smoke exited successfully with `WORKBENCH_SMOKE_OK` and no JUCE
assertion failures. C25 now feeds a four-pair source into three formants; its
Sound Library audition, open, playback and explicit restage exercise the new
path. C26 retains the packed-width example. Generated catalog/help/capability
freshness checks pass. Tracked and changed new-file whitespace checks
also pass.

Two review fixes preceded final execution: adding Audio inputs requires Bank's
fan-in policy to be `Align`, and offline input-shape validation now matches the
prepared path. A dangling channel-pointer array in a new test helper was fixed
before running it. No DSP adjustment was needed to make the final oracles pass.

Logs under the registered disposable `build/workbench-debug` root:
`formant-serialized-runtime-build.log`,
`formant-serialized-regression-build-2.log`,
`formant-serialized-regression-tests.log`,
`formant-serialized-reference-build.log`,
`formant-serialized-reference-tests.log`,
`formant-serialized-workbench-smoke.log`,
`formant-serialized-coverage-tests.log` and
`formant-serialized-generated-check.log`.

FBO's full serialized bandwidth requirement remains open. Agreement between
the native renderers is not agreement with an external reference.
