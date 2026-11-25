# ADR-0016: Implicit audio/control signal adapters

Status: Accepted (owner decision 2026-09-29)

## Context

Any Sound should be able to feed any input: Product multiplies two audio
signals to ring modulate them, and an LFO can be heard directly. Talas types
ports as Audio or Control (ADR-0003), and graph validation rejected every
connection between the two. Math prototypes took only control signals, so
audio uses of Product, Add, AbsoluteValue and the other math nodes could not
be built.

Audio and control buffers already hold one value per rendered frame, and a
node's checks, latency and realtime preparation depend on the type of the
buffers bound to its inputs.

## Decision

`portDescriptorsCompatible` accepts a connection from an Audio output to a
Control input and the reverse. When compiling binds such an input, it
appends a copy of the source buffer with the input's type and records a
`SignalAdapterPlan`: target step, input binding, source buffer and adapted
buffer. Both renderers fill the adapted buffer by copying the source before
the target step runs, ahead of any latency compensation for that input.

The adapted buffer keeps the source's node, port and scope, like a latency
compensation copy. Output accounting counts it as a copy, not as a second
output of the source node, once its structure validates.

## Consequences

- Nodes keep their typed checks: each sees a buffer of the type its port
  declares.
- Latency follows the destination port: a control input carries none, and an
  audio input fed by a control source takes that source's latency.
- An adapter costs one block copy.
- Spectrum and event ports stay strictly typed, except as amended below.

## Amendment (owner decision 2026-10-02): timestamped spectra into legacy Spectrum inputs

A LiveSpectralAnalysis should feed an OscillatorBank's Spectrum directly,
but it emitted timestamped
`partialSpectrumV1` frames that only an explicit Partial Spectrum Legacy
Hold could turn into the legacy partial triples an OscillatorBank, a
SpectrumTrackSelector or a FilterBank reads.

`portDescriptorsCompatible` now accepts a `partialSpectrumV1` Spectrum
output into a `legacyPartialTriples` Spectrum input. Before expanding
Sounds, compiling inserts one `talas.partialSpectrumLegacyHoldV1` node for each
such output, in the project graph and in every Sound definition, and routes
its connections through it; the project document keeps the direct
connection. The Hold keeps its contract: a reader sees the latest frame of
each callback, as through an explicit Hold. Legacy triples into a
`partialSpectrumV1` input stay rejected.

## Amendment (owner decision 2026-10-10): the Hold sees every frame

A LiveSpectralAnalysis emits a frame every hop, a quarter of its frame
(16 to 256 samples), but a Hold kept only the newest frame of each callback,
so a legacy Spectrum reader dropped the rest and offline and realtime, which
divide a render into different blocks, could sound different. Each partial
should change once a frame, whatever the block size.

Both renderers now split their blocks wherever a Hold, implicit or
explicit, reads a LiveSpectralAnalysis: at the sample that completes each of
its frames. A block completes at most one frame, at its first sample, so the
Hold presents each frame from the sample that completes it until the next,
and every legacy Spectrum reader sounds alike whatever the block size. This
replaces "a reader sees the latest frame of each callback" above, and the
Hold's `legacy-hold-collapses-process-interval` limitation is retired.
Before the analysis's first frame the Hold holds a spectrum of no partials:
a SpectrumFundamental gives 0 for it and a SpectrumVoicedUnvoiced
classifies nothing yet, where both refused it. A SpectrumVoicedUnvoiced
still classifies once a callback, and offline blocks keep the callbacks'
boundaries for it. The split costs sub-blocks of one hop only in Sounds
that read an analysis through a Hold.
