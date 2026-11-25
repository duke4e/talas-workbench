# FormantBank direct InRAM and runtime source count

Verified native candidate, 2026-09-27. It extends the existing FormantBank
behaviour; agreement with an external reference is not verified.

## Source and implementation

Plain FormantBank takes amplitude/center inputs, and SpectrumInRAM emits
per-sample amplitude/frequency outputs. The former fixed-count Array/Sounds resolver rejected that direct
source. The new distinguishing fixture first failed at this admission boundary.

The shared `CheckedSerializedSpectrumSource.h` authenticates matched direct
Array/Sounds/InRAM pair identity, scope, output ownership and sibling Spectrum
count metadata. Plain FormantBank reads actual N, including connected counts
and inferred InRAM selections. It retains only M selected pairs, validates every
arrival and wraps at N; N below M rejects. A changed count normalizes phase
modulo N. OscillatorBank's existing raw-pair behavior and FBO's fixed-count
zero-bandwidth extension remain intact.

An empty offline render exposed a separate producer defect. After two samples
at connected N3, validation had empty control buffers and resolved fallback
N1. The old InRAM producer silently changed serializer phase two to zero.
The consumer's subsequent pulse was .75 instead of the independent .375
expectation. InRAM now skips serializer normalization and commit when no Audio
samples are emitted; packed validation still runs. A source-only stream check
isolates this correction from the formant consumer.

InRAM zero count continues to mean the remaining selection. Explicit oversized
counts reject. Same-sample updates, initial zero pairs, phase-modulo-N count
transitions and impulse timing remain declared native conventions. Serialized
bandwidth alignment and period and indirect metadata are not modelled. FBO's required bandwidth contract is not fulfilled by its native pair
extension.

## Focused verification

All **17 FormantBank groups** and **7 OscillatorBank groups** pass in **2.77s**.
The three new FormantBank groups cover:

1. Direct InRAM connected N3 over fallback N1 feeding M2. A one-sample DC
   impulse table at Fs16 and fundamental4 gives .125 at frame zero, .375 at
   later multiples of four and zero elsewhere. Offline and prepared 1/7/31
   partitions match this independent pulse oracle; callbacks and reset allocate
   nothing. Split offline render2/render0/render37 agrees. A source-only graph
   retains phase two through the empty render and emits .5/.125/.25/.5 next.
2. Inferred InRAM count with firstPartial2/4/2 changes N3/1/3 for M1. Separate
   M2 sessions prove a failed N1 callback restores both producer and consumer
   histories. The corrected firstPartial3/N2 retry from a nonzero source phase
   matches an untouched session and emits .625 at the next pulse. All prepared
   attempts allocate zero.
3. Connected Sounds N4 over fallback N1, M2 and a separate connected fundamental
   use changing float amplitudes and zero centers. The pulse oracle uses source
   indices directly: after onset, each fourth sample sums A[t] and A[t-3].
   Float callbacks at 1/7/31, reset and N4/3/4 transitions match independent
   expected values without allocation.

Existing groups retain nonzero-center/cosine, static Array, FBO range, immutable
asset, unselected Nyquist, downstream failure/retry, forged scope and exact
input-shape coverage. The shared source helper's refactor passes the existing
OscillatorBank count-metadata and raw/crossed-pair cases.

## Broader gates

Runtime/CLI and Workbench rebuilds pass. The linker reports only the existing
duplicate-library warnings. All **eight broader CTest groups pass in 173.15s**:
catalog, compile, offline, prepared scope authority, runtime conformance,
realtime, CLI examples and Sound Library. A stale coverage-text assertion
failed on the newly explicit count-transition caveat; it was updated and passes.
Generated catalog/help/capability freshness checks pass. Tracked
and new-source whitespace checks pass. No UI code changed in this package.

All logs are under the registered disposable `build/workbench-debug` root:
`formant-inram-source-red-build.log`, `formant-inram-source-red-test.log`,
`formant-inram-source-runtime-build.log`,
`formant-inram-source-reference-tests.log`,
`formant-inram-source-regression-build.log`,
`formant-inram-source-regression-tests.log`,
`formant-inram-source-coverage-tests.log` and
`formant-inram-source-generated-check.log`.
