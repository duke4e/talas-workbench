# Formant-bank impulse construction audit

## Contract and corrected model

FormantBank and FormantBankOscillator perform impulse-response synthesis.
Frequency determines how often responses start; Spectrum frequencies determine
their independent centers. ImpulseResponse/FormantImpulse supplies the shape,
NbrImpulses limits overlap, and the DC option admits zero-frequency responses.
FormantBankOscillator also selects a one-based range and adds CascadeInput.
A single enabled DC formant reproduces the stored pulse shape. FormantBank
also has the overlap and DC controls that were missing from the previous
descriptor.

The prior implementation added sines at `Spectrum.frequency * Frequency`,
ignored bandwidth, rejected loaded impulse tables, and required a single
impulse. The initial test failed with `FormantBankOscillator runtime subset
does not load formant impulses`. It used a DC formant and an asymmetric table,
so the old zero-frequency sine could not satisfy the shape oracle either.

Both native renderers now share `CheckedFormantImpulseMath.h`. FormantBank has
`nbrImpulses` and `allowDC`, and both descriptors expose immutable resource
reads. The existing mono, selection, cascade, zero-latency, producer, input,
scope and parameter checks remain in place. Support stays `Subset`.

## Explicit candidate conventions

The model makes these explicit choices:

- A shared clock launches at sample zero and subsequent phase crossings.
  Every response starts a cosine carrier at its absolute formant center.
- The finite mono table is treated as a baseband window, read with linear
  interpolation and zero extension after its final sample. It is multiplied
  by the carrier and the current Spectrum amplitude without normalization.
- Positive bandwidth advances normalized window position by `bandwidth/Fs`.
  Zero bandwidth uses the table's source sample rate, or render rate if absent.
  Blank resources use a native 129-point triangular window.
- Each formant has a bounded ring of response slots; a new launch replaces the
  oldest slot when full. DC-disabled formants are suppressed while their
  histories continue advancing. CascadeInput is accumulated exactly once.
- Formants and overlap counts are each limited to 4096, with at most 65536
  cells. Missing, empty, duplicate, nonfinite or stereo tables are rejected.
  FormantBank's new defaults are one overlap and DC disabled.

These window, bandwidth, zero-bandwidth, gain, phase, interpolation, retirement
and default conventions are native candidates, not calibrated against an
external reference. Whether a table is already carrier-shaped is not
determined by the model. BankSize is retained as a processor grouping
hint without changing the summed signal.

The original package used one packed frame per block. The subsequent
[serialized input extension](formant-bank-serialized-reference.md) adds bounded
bandwidthless pairs with source-N cadence. Serialized bandwidth alignment and
transition timing are not modelled.

## Verification of the original impulse package

All nine grouped cases in `talas_formant_bank_reference_tests` pass:

1. The loaded table `[1, .5, -.25, 0]`, DC center, 1 Hz fundamental and 2 Hz
   bandwidth at 16 Hz produces independently specified pulses. Offline and
   prepared 1/7/31-frame partitions agree exactly and replay after reset.
2. At a 4 Hz fundamental, one slot retires the old response while two slots
   preserve its tail. After the first pulse the two-slot period is
   `[.75, .625, .5, .125]`, including later periods.
3. A 3 Hz center has the same first window at 1 and 2 Hz fundamentals. The
   second 2 Hz pulse starts a fresh carrier at sample eight; bandwidth limits
   the window duration independently of center frequency.
4. DC suppression and one-time cascade addition, including native float input
   in 1/7/31-frame callbacks. Rendering and reset allocate nothing.
5. FormantBank and FormantBankOscillator agree on the loaded DC impulse and
   the declared zero-bandwidth/native-rate convention.
6. A three-control scheduled transaction changes pulse timing. A valid
   connected frequency replaces an unused above-Nyquist fallback in both
   renderers; the effective input is checked per sample.
7. Late arithmetic failure restores carrier/window histories, pulse clock,
   slot and input position. Prepared repaired retry and offline valid-prefix
   retry match untouched sessions. Existing broader tests also cover failures
   in downstream nodes.
8. Invalid resources and overlap counts reject in both paths when compilation
   admits the graph. The exact 65536-cell boundary prepares and renders with
   zero callback allocation; larger products are rejected.
9. Both nodes save and reopen a native project with a relative PCM16 WAV Table
   asset. The 8 kHz source rate survives loading into a 16 kHz renderer. The
   immutable snapshot retains its sound after the file changes, and offline
   and prepared output match an independent half-sample interpolation oracle.

The asset fixture explicitly uses the existing Table WAV codec's 32767 scale;
it does not silently assume the separate audio-stream PCM conversion. Initial
test-only errors in overlap repetition, carrier restart and an incomplete
expected array were corrected against the declared model. The runtime was not
changed to preserve those incorrect expectations.

The review found and corrected a real admission mismatch: prepared rendering
had checked Nyquist on an unused frequency fallback even when a connected
input replaced it. Both paths now validate the effective frequency. Offline
diagnostics identify the frequency input port when connected, or the parameter
when using its fallback.

All nine affected CTest groups pass: catalog, compile, offline, prepared
scope, conformance, realtime, focused reference, CLI examples and Sound Library.
The first final run took 173.28 seconds and passed eight groups; the catalog's
old four-parameter fixture failed. After adding the two new controls to
that exact-schema expectation, its isolated rerun passed in 1.20 seconds. No
already-passing group was needlessly rerun. The earlier realtime/conformance
run also passed in 86.17 seconds.

Workbench built and passed an isolated smoke with `WORKBENCH_SMOKE_OK` and no
JUCE assertions. The subsequent offline-only diagnostic-context refinement is
covered by the passing final offline suite; it does not change sample output.
Generated catalog/help/capability and whitespace checks pass. Source checks
include the new untracked files.

Evidence is retained under the registered disposable `build/workbench-debug`
root: `formant-bank-reference-red.log`, `formant-bank-reference-tests-4.log`,
`formant-bank-realtime-tests.log`, `formant-bank-regression-tests.log`,
`formant-bank-catalog-repair-tests.log`, `formant-bank-workbench-smoke.log`
and `formant-bank-generated-check.log`.
