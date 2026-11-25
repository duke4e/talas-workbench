# ADR-0008: Partial Spectra, Complex Frames, and FFT Contracts

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

The existing `PortType::Spectrum` is a Talas partial-list compatibility
representation encoded as count followed by amplitude/frequency/bandwidth
triples. It is already consumed by partial selection, oscillator-bank, formant,
and related nodes. It is not a complex FFT frame, does not preserve bin phase,
and currently represents at most one static frame per runtime buffer.

The historical `talas.fft` descriptor has Audio input/output ports plus `length`
and `inverse`. The implemented subset accepts only length one and copies audio.
Silently changing those ports to a spectral type would reinterpret saved
projects. Reusing `Spectrum` for complex bins would also make partial-track and
STFT algorithms type-compatible when their data is not compatible.

M3 needs a portable FFT with known provenance, a partition-invariant frame
clock, exact startup/tail/latency behavior, and a compatibility story for the
historical descriptor.

## Decision

### Two spectral families

Partial spectra and complex spectra are separate data families.

The persisted `Spectrum` family is named `partialSpectrum` in new public
metadata. Its version-one native frame contains:

- `formatVersion = 1`;
- `sourceTimestamp = {transportEpoch, discontinuitySequence, absoluteFrame}`,
  using the ADR-0005 transport snapshot rather than session or playhead time;
- zero or more ordered partials;
- per partial: optional stable `trackId`, finite linear amplitude, finite
  frequency in hertz, and finite non-negative bandwidth in hertz.

Compatibility decoding accepts the existing count/triples buffer as one frame
with no track IDs. A static legacy producer emits that held frame once per
prepared activation/discontinuity and stamps it with that process segment's
transport epoch, discontinuity sequence, and absolute start frame. Downstream
compatibility adapters hold the frame until a newer timestamp arrives. No
complex frame is decoded through that adapter.

Add `PortType::ComplexSpectrum`. Its version-one frame contains:

- `formatVersion = 1`;
- `analysisTimestamp` in the same
  `{transportEpoch, discontinuitySequence, absoluteFrame}` domain;
- `frameSize`, `hopSize`, and window kind;
- ordered channel index for a fixed prepared bus;
- exactly `frameSize / 2 + 1` complex bins for a real-input transform;
- the convention and scale described below.

Graph validation rejects partial-to-complex and complex-to-partial connections.
Conversions, when later needed, are explicit nodes with their own latency and
quality contracts.

### Bounded frame sequences

A prepared spectral port carries a bounded sequence of frames for the current
process interval. It is not a mutable singleton frame. Preparation derives
capacity from maximum callback frames, the smallest admitted hop, one boundary
frame, and declared synthesis-tail allowance. Every frame owns a slice of
preallocated storage; process calls update counts and contents only.

Frames are ordered lexicographically by transport epoch, discontinuity
sequence, absolute frame, then source sequence. Equal timestamps preserve
source sequence.
The half-open callback interval rule still applies: a frame whose emission
offset equals `frameCount` belongs to the next call. Callback partitioning must
not change emitted timestamps, frame contents, or ordering.

### FFT backend and convention

The first backend is an original Talas iterative radix-2 FFT authored under
this repository's Apache-2.0 license. No third-party FFT implementation is
copied or linked. Source files and the commit history are the provenance record.

The transform convention is:

- forward transform: unscaled;
- inverse transform: normalized by `1/N`;
- complex sign: forward uses the negative exponential, inverse the positive;
- supported native sizes: powers of two from 16 through 4096 inclusive;
- plans contain immutable bit-reversal indices and twiddles prepared off the
  callback.

Rectangular and periodic Hann windows are the initial window set. A frame/hop
combination is admitted only when its analysis/synthesis normalization is
validated. Rectangular supports `hop = N`. Hann initially targets `N/2` and
`N/4`; a combination remains unsupported if its COLA/WOLA test does not pass.

### Native nodes

Add separately versioned `talas.forwardFftV1` and `talas.inverseFftV1`. Forward takes
Audio and emits ComplexSpectrum; inverse takes ComplexSpectrum and emits Audio.
Parameters explicitly select frame size, hop, and window. Unsupported settings
fail during preparation.

The analysis timestamp is the first input sample in the window and copies the
transport epoch/discontinuity sequence active when that window began. A window
never spans a transport discontinuity. The first frame is emitted only after
all `N` input samples exist. A direct native
analysis/synthesis path delays its reconstructed timeline by the prepared
round-trip latency recorded in the compiled plan. Startup zeros and the final
synthesis tail are part of the contract and must be included or excluded
consistently by render/export callers.

### Historical `talas.fft`

`talas.fft` keeps its type ID, Audio ports, `length`, `inverse`, and serialization.
It uses nonoverlapping rectangular frames. For even `N > 1`, its Audio packing
contains exactly N scalar samples:

`[Re(0), Re(N/2), Re(1), Im(1), ..., Re(N/2-1), Im(N/2-1)]`.

Every adapter has a compiled session-phase origin equal to its input's known
cumulative latency relative to activation/discontinuity. Starting at that
origin, frame `q` consumes exactly the N contiguous input samples at offsets
`qN ... qN + N - 1`. Before the first complete frame, output is zero. On the
same process sample that accepts the final input value, the adapter computes
the frame and emits packed/output element zero; it emits the remaining elements
one per following sample while independently collecting the next frame. Thus
each adapter has preparation-time-fixed latency `N - 1` and pack boundaries are
independent of callback boundaries.

An inverse adapter requires a known input cumulative latency so it can derive
the packed stream's phase; unknown phase fails preparation rather than scanning
for a boundary. A direct forward/inverse pair reconstructs with total delay
`2N - 2`. Length one remains the existing zero-latency identity. Reset, stop,
seek, sample-rate epoch change, or graph activation clears both collection and
serialization FIFOs and establishes a new phase origin; pause and ordinary
loop wrap preserve them. An incomplete final input frame is discarded on stop,
and no implicit legacy tail is appended beyond a caller's requested render
interval. The adapter supports only admitted radix-2 sizes and rejects
malformed or non-finite packs. It does not claim compatibility with an
undocumented external binary frame format.

## Amendment (2026-10-03): stereo FFT format

Owner decision 2026-10-03. The stereo FFT
transforms two independent signals, one on the left channel and one on the
right. Each frame of Length samples holds the left signal's spectrum and then
the right's, Length / 2 bins each in frequency order from 0 hz, with the real
part on the left channel and the imaginary part on the right. `talas.fft` takes
this format from project format 62:

- `talas.fft` gains an optional `rightIn` and a `rightOut`, and a `packing`
  choice: `talasStereo` (the default) or `legacyPacked`. Format 62 migration
  gives every older `talas.fft` `legacyPacked`, which keeps the packed layout
  above on `audioOut` and sends it on `rightOut` too.
- An unconnected `rightIn` carries the left signal, as a mono Sound does.
  When the two inputs arrive with different latencies, the earlier one is
  delayed to the later (ADR-0011) and frames start from the later one.
- The forward transform divides by Length, so values stay within the range
  of audio, and the inverse does not, so the round trip is the identity.
- The format does not fix where the Nyquist bins go. Each spectrum's Nyquist bin, which
  is real, takes the imaginary place of its DC bin, which is always zero for
  a real signal. This is our choice.

Both channels share the adapter's frame clock, latency `N - 1`, and reset
rules. The prepared summary reports the format as `TalasStereoAudioV1`.

## Amendment (2026-10-19): an Audio pair's frequencies are fractions of half the sample rate

Taken as recommended (standing approval 2026-10-12). Spectral sources
send a spectrum as audio, amplitudes on the left channel and frequencies on
the right, and audio values are fractions of full scale: a frequency is a
fraction of half the sample rate. A SyntheticSpectrumFromSounds reads its
FrequenciesAndBandwidths so (a Ramp oscillator whose period is NbrPartials
samples gives the lower partials low frequencies and the higher ones high
frequencies), and a SpectrumInRAM's channels can be offset with a
ScaleAndOffset before an OscillatorBank, so the pair must carry the same
values wherever it goes. From project format 93:

- the spectral sources' `frequencyOut` (SpectrumInRAM,
  SyntheticSpectrumFromArray, and a SyntheticSpectrumFromSounds's pair, which
  passes its input on as it came) carries hertz divided by half the sample
  rate; a SpectrumTrackSelector's right output already did;
- a SyntheticSpectrumFromSounds's spectrum, and the `frequencyIn` of an
  OscillatorBank, FormantBank and FormantBankOscillator and the
  `frequencies` of a FrameSynchronizer (whose typed frames stay in hertz),
  take a pair sample times half the sample rate;
- `Spectrum` and `PartialSpectrumV1` frames keep hertz: they are ours, not
  audio.

Each of the seven node types takes a `pairFrequencies` choice,
`halfSampleRate` (the default) or `legacyHertz`; format 93 migration gives
every older one `legacyHertz`, which keeps the pair in hertz. A narrower
design, converting only the SyntheticSpectrumFromSounds's input, was
rejected: the pair would stay in hertz where any other Sound can feed or
read it (an Oscillator straight into an OscillatorBank, a ScaleAndOffset
between them), so fraction-valued pairs would still play wrongly there. A pair from a node of one
choice to a node of the other (a new node in an older project) is not
refused; the node's choice is shown with its fields.

## Compatibility and migration

- Old projects retain `talas.fft` and its ports; no automatic migration to native
  nodes occurs.
- Existing `Spectrum` connections remain partial-spectrum connections. Format
  10 may emit the clearer `partialSpectrum` token while versions 1-9 and the
  legacy `spectrum` token remain readable.
- Existing count/triples buffers are adapted explicitly until all partial nodes
  use native frame sequences.
- Capability metadata distinguishes the historical packed adapter from native
  complex-frame nodes and lists size/window/hop limits.

## Realtime impact

Plan construction, twiddle/window generation, frame storage, overlap-add
buffers, and adapter FIFOs are allocated during preparation. Process calls do
not allocate, resize, lock, wait, log, parse, format, or perform I/O. Reset,
seek/host discontinuity, sample-rate epoch change, and graph replacement reset
frame and overlap state according to the latency ADR. Pause and ordinary
playhead loop wrap preserve it.

## Alternatives rejected

- Reusing `Spectrum` for complex bins would erase the partial/complex type
  boundary and make invalid graphs compile.
- Replacing `talas.fft` ports would silently reinterpret saved projects.
- A platform-only backend such as Accelerate would make core behavior and tests
  non-portable.
- Importing an FFT dependency before license/provenance review would violate
  Gate 3.
- Performing an entire transform at callback block boundaries would make frame
  timing depend on host partitioning.
- Allocating one vector per emitted frame would violate callback safety.

## Acceptance evidence

- impulse and bin-centered sine magnitude/phase tests;
- forward/inverse maximum absolute error `<= 1e-9` for offline doubles and
  `<= 1e-6` for realtime/sample-format fixtures;
- validated window/hop COLA/WOLA tests;
- exact first-frame, tail, timestamp, packing, and latency tests;
- irregular callback partition equivalence;
- graph validation proving partial and complex ports cannot connect;
- callback instrumentation proving zero forbidden work.
