# Talas Examples

These project-original examples are local, inspectable product evidence. Open
any `.talas.json` file in the Workbench, or use the core CLI from the repository
root. The M3 examples are current project format 10 and use a fixed 8192 Hz
inspection rate so their prepared timing is easy to verify.

The generated [built-in node reference](../docs/help/builtin-node-reference.md)
links these exact examples to their live-checked descriptors. It reports
declared ports, parameters, maturity, execution contexts, and limitations;
the authored prose below remains the semantic walkthrough and does not turn
placeholder, subset, or unsupported behavior into a claim of full support.

The Workbench Sound Library packages this README, the generated index and
built-in reference, every indexed project file (the demo projects in
[`demos/`](demos/README.md) and their README among them), and only their
declared repository-original resources. `Open Help` projects the exact checked heading
and expected outcome into local Context Help; it does not open a hosted page,
copy third-party material, or claim tutorial completeness.

The four M4 examples are canonical format 13. The two state-layer examples
retain an empty version-1 sample-map boundary; the sample-map instrument owns a
populated map over the repository-original PCM16-WAV fixture; and the
missing-component example owns a bounded data-only preservation capsule. They
are intentionally separate from the frozen format-10 M3 migration evidence.

Validate any M3 example normally:

```bash
./build/core-debug/talas_cli validate \
  --project examples/m3_native_fft_round_trip.talas.json
```

Inspect its deterministic prepared contract without attaching audio hardware:

```bash
./build/core-debug/talas_cli validate \
  --project examples/m3_native_fft_round_trip.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
```

## Demo projects

New to Talas? Start with the twenty-four [demo projects](demos/README.md):
thirteen effects and eleven instruments, each a ready-to-play project with a
handful of named live controls. In the Workbench they are the **Demos**
category of the Sound Library, listed ahead of the examples below. An effect
plays live audio input 1, so open it and play into that input; an
instrument's Audition plays its demo phrase, and once opened it also plays
from a MIDI keyboard. [`demos/README.md`](demos/README.md) lists every control
and the render commands.

## Bounded Analog Sequencer

[`c14_bounded_analog_sequencer.talas.json`](c14_bounded_analog_sequencer.talas.json)
routes a mono oscillator through one immutable four-step amplitude sequence to
stereo output. The sequence uses different durations, duty cycles, and
velocities, while a bounded LFO drives the connected positive `rate` control.
Offline and prepared realtime retain the same samples across callback
partitions; closing `gate`, transport reset, and restaging restart elapsed
sequence time deterministically.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c14_bounded_analog_sequencer.talas.json \
  --status --sample-rate 8000 --max-block-frames 7
./build/core-debug/talas_cli render \
  --project examples/c14_bounded_analog_sequencer.talas.json \
  --frames 960 --sample-rate 8000 --max-block-frames 7 \
  --format json
```

Prepared admission accepts at most 256 static steps, polyphony exactly one,
blank pitch/extra event lists, and only the documented live
`left`/`right`/`gate`/`step`/`rate` controls. Dynamic sequence-list mutation,
pitch events, extra-value events, MIDI, and larger polyphony remain closed.

## Stereo Routing Utilities

[`c15_stereo_routing_utilities.talas.json`](c15_stereo_routing_utilities.talas.json)
forms one audible chain through `Level`, `Channel Join`, `Channel Crosser`, and
`Channeller`. The Level's historical `noGain=true` compatibility setting still
applies its independent left/right gains. A bounded LFO drives one connected
matrix coefficient before Channeller selects the final left path; the Crosser's
right path remains independently audible.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c15_stereo_routing_utilities.talas.json \
  --status --sample-rate 48000 --max-block-frames 17
./build/core-debug/talas_cli render \
  --project examples/c15_stereo_routing_utilities.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 17 \
  --format json
```

Offline and prepared realtime share checked finite stereo arithmetic. Missing
optional Crosser right input remains exact zero; non-finite input, coefficient,
product, sum, or output fails the render transaction without publishing partial
audio. Dynamic channel-selector booleans and undeclared multichannel expansion
remain outside this package.

## Shelving and Presence Filters

[`c16_shelving_presence_filters.talas.json`](c16_shelving_presence_filters.talas.json)
forms one audible mono chain through Low Shelf, Presence, and High Shelf. A
bounded LFO drives the Presence boost/cut control sample by sample while all
three filters preserve independent, partition-invariant biquad history.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c16_shelving_presence_filters.talas.json \
  --status --sample-rate 48000 --max-block-frames 17
./build/core-debug/talas_cli render \
  --project examples/c16_shelving_presence_filters.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 17 \
  --format json
```

Offline and prepared realtime use the same checked coefficient and state
transition. Invalid frequency, bandwidth, coefficient, input, scale, product,
accumulation, output, or state arithmetic fails without committing partial
state or publishing partial callback audio. Alternate shelf slopes, smoothing,
and stereo or multichannel expansion remain outside this package.

## Checked Biquad Family

[`c17_checked_biquad_family.talas.json`](c17_checked_biquad_family.talas.json)
combines Two Formant Element's two parallel second-order sections with fixed
double zeros at +1 and the deterministic Two Formant Voice source, which still
uses two RBJ bandpass sections with sine excitation. The current TFE candidate
pole map is `r=exp(-pi*bandwidth/Fs)` and `theta=2*pi*formant/Fs`, with signed
section scales and no peak normalization. The fixed-zero topology is the
checked contract; the coefficient mapping, normalization, and live parameter
transitions are model choices. The graph then passes their
mix through Dual Parallel Two Pole Filter, Graphic EQ, and the stereo
quadrature Analysis Filter. All five C17 nodes therefore execute in one
audible stateful prepared graph.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c17_checked_biquad_family.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c17_checked_biquad_family.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared realtime share checked coefficient construction,
per-sample transitions, parallel and seven-band sums, allpass quadrature, and
oscillator phase. Pole text and seed remain preparation-time values; invalid
control, coefficient, sample, phase, output, or state arithmetic rejects the
complete callback without publishing partial audio.

## Bounded Static FIR Family

[`c18_bounded_static_fir_family.talas.json`](c18_bounded_static_fir_family.talas.json)
passes one audible oscillator through FIR Filter and RE Resonator. FIR
coefficients are frozen from text, while the RE coefficients come from the
verified immutable `resources/c18-re-kernel.txt` project asset.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c18_bounded_static_fir_family.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c18_bounded_static_fir_family.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Both causal convolutions share checked multiply-and-ordered-sum behavior,
preallocated history, exact reset/restage, and whole-callback rollback. The
prepared profile accepts 1–4096 finite taps; longer historical coefficient
sets remain explicitly offline-only.

## Checked Four-Pole TPT VCF

[`c19_checked_vcf_tpt.talas.json`](c19_checked_vcf_tpt.talas.json) routes one
oscillator through the mono four-pole TPT VCF. The audible response preserves
the historical ladder order while amplitude, cutoff, cutoff modulator,
modulation range, and resonance remain available as static, direct Event Value,
or connected prepared controls.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c19_checked_vcf_tpt.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c19_checked_vcf_tpt.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared realtime share one checked candidate-before-commit
transition over exactly four stage states. Invalid modulation, coefficient,
feedback, stage, state, or output arithmetic rejects the complete callback;
alternate pole modes, saturation, oversampling, self-oscillation guarantees,
and multichannel VCF remain outside this package.

## Bounded Harmonic Resonator

[`c20_bounded_harmonic_resonator.talas.json`](c20_bounded_harmonic_resonator.talas.json)
routes one mono oscillator through a checked bounded Private-memory comb. The
prepared resonator preserves the integer one-period delay and 60 dB decay law;
decay time, frequency, and excitation scale remain available as static, direct
Event Value, or connected controls while brightness stays preparation-time.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c20_bounded_harmonic_resonator.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c20_bounded_harmonic_resonator.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Prepared delay storage and its reverse rollback journal are bounded before the
callback. Invalid period, feedback, brightness, excitation, index, state, or
output arithmetic rejects the complete callback without publishing partial
audio or state. Named/shared memory, retained external `prezero=false` state,
dynamic brightness, and multichannel resonators remain outside this package.

## Bounded Dynamic Range Controller

[`c21_bounded_dynamic_range_controller.talas.json`](c21_bounded_dynamic_range_controller.talas.json)
uses separate mono source and sidechain oscillators to drive the checked
compressor profile. Compressor/downward-expander mode is preparation-time;
ratio, threshold, attack, release, integer delay, and output gain accept their
static, direct Event Value, or connected prepared forms.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c21_bounded_dynamic_range_controller.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c21_bounded_dynamic_range_controller.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Detector state, physical/logical delay storage, epoch-based direct-delay
clears, and the reverse rollback journal are bounded before the callback.
Invalid smoothing, level, gain, delay, index, state, or output arithmetic
rejects the complete callback. Variable delay latency is reported without
compensation; multiband processing, soft knee, alternate detector laws, stereo
linking, and sidechain filtering remain outside this package.

## Checked Quad Oscillator

[`c22_checked_quad_oscillator.talas.json`](c22_checked_quad_oscillator.talas.json)
uses independent audible envelope oscillators with one shared sine/cosine
phase, sums the two ring-modulated channels to mono, and mirrors that result
to the compatibility output channel. Frequency accepts finite signed static,
direct Event Value, or connected prepared forms strictly below Nyquist.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c22_checked_quad_oscillator.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c22_checked_quad_oscillator.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

The shared phase is session-owned, resettable, partition-stable, and restored
with the summed output when any frame or downstream checked node rejects the
callback. Quadrature envelope pairs cancel the opposite sideband; a negative
carrier selects the difference sideband. Alternate phase offsets, anti-aliasing
beyond strict-Nyquist admission, and multichannel expansion remain outside this
package.

## Bounded Overlapping Mixer

[`c23_bounded_overlapping_mixer.talas.json`](c23_bounded_overlapping_mixer.talas.json)
mixes four audible mono oscillators in the explicit `in3, in1; in4 in2` order,
then applies distinct bounded left and right gains. The zero-overlap profile is
the exact prepared-realtime subset; empty selection instead resolves every
connected port in numeric order.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c23_bounded_overlapping_mixer.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c23_bounded_overlapping_mixer.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Every selected input and partial sum is finite-checked in declared order before
the two output products commit together. Nonzero overlap scheduling, dynamic
sound collections, multichannel input objects, and latency compensation remain
outside this bounded package.

## Bounded Oscillator Bank

[`c24_bounded_oscillator_bank.talas.json`](c24_bounded_oscillator_bank.talas.json)
resynthesizes one original three-partial Spectrum through a mono Oscillator
Bank. The static count selects all three partials in declared order, `bankSize`
remains an accepted grouping hint, and the blank `wavetable` selects the exact
sine fallback without requiring a product asset.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c24_bounded_oscillator_bank.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c24_bounded_oscillator_bank.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared rendering share the same checked absolute-frame phase,
waveform product, and ordered partial accumulation. One through 4096 static
oscillators and an immutable finite in-memory cyclic table are admitted;
time-varying counts, file-backed or mutable tables, Nyquist-and-above
frequencies, and multichannel expansion remain outside this bounded package.

## Bounded Formant Bank

[`c25_bounded_formant_bank.talas.json`](c25_bounded_formant_bank.talas.json)
feeds a four-partial amplitude/frequency stream to a mono Formant Bank that
selects the first three formants. Their centers are absolute 500, 1100, and
2200 Hz; the fourth 3300 Hz component is outside the selected range. Source
updates retain their four-sample cycle. The 110 Hz `frequency` starts responses independently of those
centers. `nbrImpulses=2` allows two overlapping responses per formant,
`allowDC=false` suppresses DC components, and `bankSize=2` is retained as a processor
grouping hint without changing the summed sound. A blank `impulseResponse` selects the local 129-sample triangular
table candidate; finite mono tables can also be loaded as immutable resources.

The source sends no bandwidths, so the candidate plays its impulse table at the
native rate or the render rate when no source rate is available. One arriving
pair updates its retained component before that sample is synthesized.
The onset, interpolation and gain behavior are model choices.
Serialized bandwidth alignment remains open. The alternative packed `spectrum`
input retains its existing bandwidth/sampleRate window-rate candidate.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c25_bounded_formant_bank.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c25_bounded_formant_bank.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared rendering share checked pulse and response state, with
active response histories across block boundaries. The bounded candidate admits 1..4096
formants and overlaps individually, subject to 65536 total active impulse
cells. `allowDC` can enable DC formants; the example keeps them suppressed.
Centers remain absolute Hz as `frequency` changes. Bandwidth
scaling, impulse timing, and gain remain candidates. Serialized pairs require
matching direct outputs from one fixed-count, linear SyntheticSpectrum source
without bandwidths; count changes and intermediate audio adapters are outside
the current profile.

## Bounded Formant Bank Oscillator

[`c26_bounded_formant_bank_oscillator.talas.json`](c26_bounded_formant_bank_oscillator.talas.json)
selects formants two through four from one original four-partial Spectrum and
adds them, in source order, to a required audible mono cascade. The positive
110 Hz `frequency` is the independent pulse-train fundamental; the selected
centers are absolute 500, 1100, and 2200 Hz with 100, 150, and 200 Hz
bandwidths. `nbrImpulses=2` allows overlapping responses, and
`allowFormantAtDC=false` suppresses DC formants. A blank `formantImpulse`
selects the local 129-sample triangular table candidate; finite mono tables can
also be loaded as immutable resources.

Positive bandwidth advances normalized table position by bandwidth/sampleRate;
zero bandwidth uses the table's native rate or the render rate as a candidate
fallback. Bandwidth scaling, table interpolation, gain, and pulse onset remain
unverified, and serialized bandwidth alignment remains open.
FBO also accepts the same direct pair profile as FormantBank using the native
zero-bandwidth table convention. This extension does not supply the bandwidth
information that FBO's full profile requires.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c26_bounded_formant_bank_oscillator.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c26_bounded_formant_bank_oscillator.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared rendering preserve one-time cascade addition and checked
phase/response state. The bounded candidate admits 1..4096 selected formants
and overlaps individually, subject to 65536 total active impulse cells. A
single DC formant with `allowFormantAtDC=true` uses the table-shaped response;
the example keeps DC suppressed. Centers remain absolute Hz as pulse
fundamental changes. Timing, bandwidth mapping, and gain remain
candidates; serialized Spectrum N-sample updates remain open.

## Bounded Spectrum Modifier

[`c27_bounded_spectrum_modifier.talas.json`](c27_bounded_spectrum_modifier.talas.json)
selects tracks two through four of one original four-partial Spectrum using
inclusive track, frequency, and amplitude criteria. Selected partials receive
the exact affine frequency and amplitude transforms before a bounded
Oscillator Bank makes the result audible; unselected partials are muted.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c27_bounded_spectrum_modifier.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c27_bounded_spectrum_modifier.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared rendering share the exact source-order criteria,
deterministic seeded probability decision, affine transforms, muting, and
Spectrum triple encoding. Zero through 4096 input partials, the descriptor
default `hiTrack=10000`, static or complete-callback block-constant numeric
controls, and zero-latency ancestry are admitted. Dynamic booleans or seed,
live cross-callback hysteresis memory, non-finite transforms, and multichannel
Spectrum expansion remain outside this bounded package.

## Paired Stereo Output 4

[`c28_paired_stereo_output4.talas.json`](c28_paired_stereo_output4.talas.json)
routes two distinguishable mono oscillators to four exact consecutive stereo
destinations. Every pair remains two independent logical destinations carrying
bit-identical samples.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c28_paired_stereo_output4.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
```

## Paired Stereo Output 8

[`c28_paired_stereo_output8.talas.json`](c28_paired_stereo_output8.talas.json)
extends the same accepted paired-mono contract to four sources and eight
ordered destinations. Output ordinals are never deduplicated.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c28_paired_stereo_output8.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
```

Prepared realtime preserves the accepted offline mono-to-pair map through the
session, engine, graph host, Project bridge, sparse device destinations, and
multichannel WAV export. These terminals are root-only, stateless,
resource-free, preserve relative input latency, and allocate nothing in the
callback. True stereo Sound-object unpacking, hardware assignment, spatial
metadata, and arbitrary channel counts remain outside this bounded package.

## Bounded Synthetic Spectrum From Sounds

[`c29_bounded_synthetic_spectrum_from_sounds.talas.json`](c29_bounded_synthetic_spectrum_from_sounds.talas.json)
turns two bounded Audio producers into one callback-local four-partial
Spectrum. Magnitude stages keep every packed frequency sample non-negative;
the frequency path scales those samples into 0–880 Hz before the prepared
Oscillator Bank makes the frame audible.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c29_bounded_synthetic_spectrum_from_sounds.talas.json \
  --status --sample-rate 48000 --max-block-frames 127
./build/core-debug/talas_cli render \
  --project examples/c29_bounded_synthetic_spectrum_from_sounds.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 127 \
  --format json
```

Offline and prepared rendering consume the first four amplitude and frequency
samples of each callback and publish one exact legacy Spectrum frame only
after all samples validate. One through 4096 partials, optional alternating
bandwidth samples, static linear/log conversion, and a block-constant
connected count are admitted. Cross-callback accumulation, dynamic boolean
modes, insufficient callback blocks, non-finite or invalid frequency data,
feedback-pair coexistence, and Spectrum resources remain outside this bounded
package. An internal scheduler split before the packed window completes
publishes no audio and restores admitted DSP state, but due published legacy/
typed event queues and the Score cursor are consumed before the next callback.

## Bounded Warped Time Index

[`c30_bounded_warped_time_index.talas.json`](c30_bounded_warped_time_index.talas.json)
uses a three-point Warped Time Index to modulate an audible oscillator through
a zero-latency Level gain stage. A prepared Clock alternates morph between the
first ideal-time map and the second while the current-time map remains static.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c30_bounded_warped_time_index.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c30_bounded_warped_time_index.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime parses 2–4096 static brace-wrapped Seconds points before
rendering, then accepts finite per-frame trigger, non-negative rate, and
`0...1` morph controls without callback allocation. Positive rising edges
restart playback; a closed connected trigger does not pause an active map.
Dynamic point-array mutation, Event Value expressions inside those arrays,
negative/reverse rate, and more than 4096 points remain outside this package.

## Bounded Spectrum On Disk

[`c31_bounded_spectrum_on_disk.talas.json`](c31_bounded_spectrum_on_disk.talas.json)
loads one verified repository-original table as an immutable analysis-resource
snapshot. Spectrum On Disk scales its three amplitude/frequency/bandwidth
triples and passes the packed Spectrum frame to Oscillator Bank for audible
additive resynthesis.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c31_bounded_spectrum_on_disk.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c31_bounded_spectrum_on_disk.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits one finite table of at most 4096 triples,
`rateScale=1`, and static or callback-constant frequency, level, count, and
one-based first-partial controls. Resource lookup and copying happen during
preparation; the callback performs no disk I/O or allocation. Real analysis-
file decoding, streaming, frame scanning, metadata interpretation, mutable
resource publication, and within-callback control changes remain outside this
package.

## Bounded GA Oscillators

[`c32_bounded_ga_oscillators.talas.json`](c32_bounded_ga_oscillators.talas.json)
loads two verified repository-original amplitude analyses as immutable table
snapshots. GA Oscillators interpolates their three partials at a static morph,
applies a bounded envelope, and renders phase-continuous audible mono output.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c32_bounded_ga_oscillators.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c32_bounded_ga_oscillators.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits one required and one optional finite amplitude table
with 1–4096 matching partials, `timeIndex=0`, and finite per-frame envelope,
positive sub-Nyquist frequency, and `0...1` morph controls. Both tables are
verified and copied during preparation; reset restarts every partial phase and
the callback performs no resource lookup or allocation. Analysis metadata,
frequency-deviation/grouping semantics, dynamic table publication, nonzero
time indices, and more than 4096 partials remain outside this package.

## Bounded Generic Source RAM Sample

[`c33_bounded_generic_source_ram_sample.talas.json`](c33_bounded_generic_source_ram_sample.talas.json)
loads one verified repository-original waveform table into an operation-owned
immutable snapshot. Generic Source plays it through both output channels with
a short attack, half-speed pitch, and continuous autoloop playback.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c33_bounded_generic_source_ram_sample.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c33_bounded_generic_source_ram_sample.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits one uniquely identified finite RAM table with
1–1,048,576 samples, static or per-frame trigger/attack/release/frequency/scale
controls, signed connected playback rate, rising-edge replay, channel toggles,
and optional autoloop. Preparation verifies and copies the table; reset returns
to pristine playback state and the callback performs no resource lookup or
allocation. Live input, disk/stream sources, mutable Memory Writer tables,
sample-header metadata, and larger tables remain outside this package.

## Bounded Multisample

[`c35_bounded_multisample.talas.json`](c35_bounded_multisample.talas.json) reuses
two verified repository-original waveform tables as one static indexed list.
Multisample selects the longer table, plays it at half speed with a short
attack, and continuously loops the normalized inner window as audible mono.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c35_bounded_multisample.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c35_bounded_multisample.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits a static comma/ASCII-whitespace list of 1–256
positions and at most 4096 source bytes. Up to 256 unique finite tables share
an operation-owned aggregate budget of 1,048,576 samples; repeated list
identities retain separate selectable positions but share one copied table.
Frequency, gate, index, scale, attack, and release may vary per frame, while a
gate start or retrigger applies the current index. Signed/default-pitch
playback, normalized looping, gate release, reset, and callback failure remain
transactional. Disk/dynamic lists, default-pitch metadata, mutable tables, and
multichannel resources remain outside this package.

## Bounded Key-Mapped Multisample

[`c36_bounded_key_mapped_multisample.talas.json`](c36_bounded_key_mapped_multisample.talas.json)
reuses one verified repository-original waveform table. The admitted
`byPitchRange` mapping accepts 110–880 Hz, plays 220 Hz at half speed with a
short attack, and continuously loops the complete table as audible mono.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c36_bounded_key_mapped_multisample.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c36_bounded_key_mapped_multisample.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits one uniquely identified finite RAM table with
1–1,048,576 samples. `byPitchRange` and the existing single-resource
`byBasePitch` behavior are supported; frequency/range/gate/scale/velocity/
attack/release controls may vary per frame. Static `noTransposition` selects
fixed 1x or 440-Hz-relative playback, zero frequency retains the accepted 1x
sentinel, and the complete-table loop exits on gate release. Preparation
copies the table; reset and callback failure remain transactional. Multi-table
key maps, `onePerHalfStep`, `equallySpaced`, disk/stream sources, mutable
tables, and multichannel resources remain outside this package.

## Bounded Multifile Disk Player

[`c37_bounded_multifile_disk_player.talas.json`](c37_bounded_multifile_disk_player.talas.json)
reuses two verified repository-original waveform tables. Index 1 selects the
longer waveform when the trigger rises, and a 1/16-rate interpolated one-shot
produces audible mono before stopping exactly at the table end.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c37_bounded_multifile_disk_player.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c37_bounded_multifile_disk_player.talas.json \
  --frames 512 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

Prepared realtime admits a static list of 1–256 positions and at most 4096
bytes, deduplicates at most 256 immutable finite RAM tables, and owns at most
1,048,576 aggregate samples. Finite integer index, finite trigger, and finite
nonzero signed rate may vary per frame; index is sampled only on a rising-edge
start or retrigger. Static gating chooses stop/retrigger or release-through-end
behavior. Playback, reset, and callback failure remain transactional. Real
disk streaming/decoding, dynamic lists, sample-rate metadata, mutable or
multichannel resources remain outside this package.

## Bounded Memory Writer Monitor

[`c38_bounded_memory_writer.talas.json`](c38_bounded_memory_writer.talas.json)
routes an audible oscillator through the existing monitor-only Memory Writer
subset. A static rising trigger starts a 5 ms preview; the monitor becomes
silent when that bounded interval ends.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c38_bounded_memory_writer.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c38_bounded_memory_writer.talas.json \
  --frames 512 --sample-rate 48000 --max-block-frames 37 \
  --format json
```

Prepared realtime admits finite mono input, static or per-frame finite trigger,
silent/audible monitoring, fractional-duration ceiling, and cyclic held-trigger
wrapping. Positive duration is bounded to 1,048,576 frames; zero duration
retains indefinite monitoring with constant state. Reset and callback failure
remain transactional. No captured table is stored or published: global
storage, exact cyclic overwrite bytes, `fromMemoryWriter` readers, EventValue
scheduling, and persistence remain outside this package.

## Bounded Raw MIDI Output

[`c39_bounded_raw_midi_output.talas.json`](c39_bounded_raw_midi_output.talas.json)
captures one device-neutral Note On packet (`90 40 7f`) through the prepared
terminal-event API. It intentionally owns no audio ports and performs no MIDI
endpoint or device I/O.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c39_bounded_raw_midi_output.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c39_bounded_raw_midi_output.talas.json \
  --frames 128 --sample-rate 48000 --max-block-frames 37 \
  --format json
```

Prepared realtime accepts blank text or 1–256 decimal, `16r` hexadecimal, or
precompiled Event Value brace-expression bytes. Changed packets carry exact
callback-relative frames, remain deduplicated across partitions, and publish
only after the complete bounded callback succeeds. Endpoint selection,
wall-clock conversion, device delivery, and cross-thread MIDI queues remain
outside this package.

## Bounded MIDI Controller Output

[`c40_bounded_midi_controller_output.talas.json`](c40_bounded_midi_controller_output.talas.json)
captures one device-neutral MIDI Control Change packet (`b2 0a 7f`) through
the same caller-owned prepared terminal-event API. It intentionally owns no
audio ports and performs no MIDI endpoint or device I/O.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c40_bounded_midi_controller_output.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c40_bounded_midi_controller_output.talas.json \
  --frames 128 --sample-rate 48000 --max-block-frames 37 \
  --format json
```

Static channel, controller number, and value publish once from pristine
state. Connected finite integer controls publish only changed three-byte
tuples at exact callback-relative frames. Reset, replacement, and restaging
restart tuple publication; failure and capacity overflow publish nothing.
Endpoint selection, timestamp conversion, device delivery, and cross-thread
MIDI queues remain outside this package.

## Bounded MIDI Note Output

[`c41_bounded_midi_note_output.talas.json`](c41_bounded_midi_note_output.talas.json)
captures one device-neutral MIDI note-on packet (`91 45 7f`) through the same
caller-owned prepared terminal-event API. It intentionally owns no audio
ports and performs no MIDI endpoint or device I/O.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c41_bounded_midi_note_output.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c41_bounded_midi_note_output.talas.json \
  --frames 128 --sample-rate 48000 --max-block-frames 37 \
  --format json
```

Positive gate edges publish note-on packets from frequency, amplitude, and
channel sampled at the edge; falling edges publish note-off using the active
note and channel captured by the matching note-on. Held gates do not
retrigger. Reset, replacement, and restaging restart ownership; failure and
capacity overflow publish nothing. Voice stealing, panic/all-notes-off,
endpoint selection, timestamp conversion, device delivery, and cross-thread
MIDI queues remain outside this package.

## Bounded MIDI File Echo

[`c42_bounded_midi_file_echo.talas.json`](c42_bounded_midi_file_echo.talas.json)
captures note-on `90 3c 40` at frame zero and note-off `80 3c 00` at frame
four when rendered at 8 Hz. The repository-original SMF resource is frozen
with the project and parsed completely before the prepared session becomes
ready; the callback only copies due messages into caller-owned storage.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c42_bounded_midi_file_echo.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c42_bounded_midi_file_echo.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

SMF formats 0, 1, and 2, PPQ tempo maps, supported SMPTE divisions, running
status, stable simultaneous channel-event order, and channel filtering share
one offline/prepared parser. Prepared schedules admit at most 4096 events and
publish atomically across partitions, reset, discontinuity, replacement, and
corrected retry. Live MIDI input echo, SysEx publication, endpoint selection,
device delivery, and cross-thread queues remain outside this package.

## Bounded Global Controller Output

[`c43_bounded_global_controller_output.talas.json`](c43_bounded_global_controller_output.talas.json)
captures `!Generated = 0.75` at frame zero. Offline and prepared realtime
publish the same immutable name, finite value, and frame through capture-only
results; neither path mutates a global controller.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c43_bounded_global_controller_output.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c43_bounded_global_controller_output.talas.json \
  --frames 128 --sample-rate 48000 --max-block-frames 37 \
  --format json
```

Static values emit once from pristine frame zero. Connected Control values
emit the first finite value and later changes beyond `1e-12`, preserving
state across callbacks. Caller event and name capacity, reset,
discontinuity, replacement, rollback, and corrected retry are atomic.
Dynamic names, Event Value mutation, VCS/grid/range mapping, device delivery,
cross-thread I/O, and global-state feedback remain outside this package.

In Workbench, a tagged controller example can use **Control** from the
Sound Library when the open Audio Sound has a prepared VCS fader with the same
Event Value name. The controller runs as an isolated source; its bounded
terminal events are applied on the control thread and the target remains
audible. **Stop** restores the prior transport state.

## Triggered Global Controller Override

[`c201_triggered_global_controller_override.talas.json`](c201_triggered_global_controller_override.talas.json)
asserts `!Generated = 0.4` when its Trigger opens. Its
`AllowLiveOverride` switch is enabled, so a matching target fader and Preset
can replace the generated value until the next source assertion. The example
can be selected in Workbench's Sound Library and started with **Control**.
A target project must expose a prepared, direct scalar VCS control
named `Generated`.

## Dynamic Control Utilities

[`c44_dynamic_control_utilities.talas.json`](c44_dynamic_control_utilities.talas.json)
routes a one-hertz oscillator through Quantizer, Gate, Logic, and VCA. Its
bounded scalar Timeline values change Quantizer step/offset, Gate thresholds,
and the unconnected Logic fallback at exact sample boundaries. Offline and
prepared rendering retain the same Schmitt state and output across irregular
callback partitions; the Logic operation itself remains a static choice.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c44_dynamic_control_utilities.talas.json \
  --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c44_dynamic_control_utilities.talas.json \
  --frames 6 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Dynamic Slew Times

[`c45_dynamic_slew_times.talas.json`](c45_dynamic_slew_times.talas.json) routes a
one-hertz Clock through Slew to control the gain of a half-hertz oscillator.
Seconds-domain Timeline values change fall time at frame 5 and rise time at
frame 9 of the 8 Hz proof render. The held Slew value remains continuous until
each zero-time assignment snaps only its matching direction at the exact sample
boundary; offline and prepared rendering share the same kernel and state.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c45_dynamic_slew_times.talas.json \
  --sample-rate 8 --max-block-frames 4
./build/core-debug/talas_cli render \
  --project examples/c45_dynamic_slew_times.talas.json \
  --frames 12 --sample-rate 8 --max-block-frames 4 \
  --format json
```

## Dynamic Clock Parameters

[`c46_dynamic_clock_parameters.talas.json`](c46_dynamic_clock_parameters.talas.json)
uses a one-hertz Clock to gate a one-hertz oscillator. At frame 3 of the 8 Hz
proof render the Clock retunes to two hertz without moving its accumulated
phase; duty cycle opens fully at frame 4 and closes at frame 6. Declared
transport discontinuities rebase from the adopted absolute frame, while
ordinary Event Value boundaries and callback partitions remain phase
continuous and exactly equal offline/prepared.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c46_dynamic_clock_parameters.talas.json \
  --sample-rate 8 --max-block-frames 4
./build/core-debug/talas_cli render \
  --project examples/c46_dynamic_clock_parameters.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 4 \
  --format json
```

## Dynamic Clock Divider Parameters

[`c47_dynamic_clock_divider_parameters.talas.json`](c47_dynamic_clock_divider_parameters.talas.json)
feeds a two-hertz Clock into Clock Divider and uses the held step to scale a
one-hertz oscillator. At frame 9 of the 8 Hz proof render, both integer moduli
shrink: partial-edge, held-step, and next-step state reduce by their new
moduli without a synthetic edge or step. At frame 17, a compatibly rounded
divisor change proves the established integer-admission tolerance. Offline and
prepared partitions remain exactly equal.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c47_dynamic_clock_divider_parameters.talas.json \
  --sample-rate 8 --max-block-frames 5
./build/core-debug/talas_cli render \
  --project examples/c47_dynamic_clock_divider_parameters.talas.json \
  --frames 20 --sample-rate 8 --max-block-frames 5 \
  --format json
```

## Dynamic Global Controller Fallback

[`c48_dynamic_global_controller_fallback.talas.json`](c48_dynamic_global_controller_fallback.talas.json)
uses a no-audio `Sound To Global Controller` terminal whose text-backed value
references `!C48Value`. The 8 Hz proof render publishes values at frames 0, 2,
and 5; the frame-4 update remains within the exact `1e-12` deduplication
threshold and emits nothing. Offline and prepared execution share the same
bounded Event Value segments, caller-owned atomic publication, and capture-only
contract.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c48_dynamic_global_controller_fallback.talas.json \
  --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c48_dynamic_global_controller_fallback.talas.json \
  --frames 7 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Live Channeller Selectors

[`c49_live_channeller_selectors.talas.json`](c49_live_channeller_selectors.talas.json)
feeds a 1 Hz left and a 2 Hz right oscillator into Channeller. Two Event
Value-backed Constant controls drive its optional selectors: strictly positive
samples check a channel, while zero and negative finite samples uncheck it.
One checked channel plays on both outputs, both checked pass the pair through, and neither is silent. The stored
Boolean parameters remain exact static fallbacks. The 8 Hz proof render checks
left at frame 2, unchecks right at frame 3, and swaps both at frame 5 without
block delay, state, interpolation, or external I/O.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c49_live_channeller_selectors.talas.json \
  --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c49_live_channeller_selectors.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Live Spectrum Modifier Selectors

[`c50_live_spectrum_modifier_selectors.talas.json`](c50_live_spectrum_modifier_selectors.talas.json)
drives Spectrum Modifier's optional `select` and `hearAll` Control inputs from
two Scalar Event Values. Strictly positive connected values override the stored
false Boolean fallbacks. `select` transforms the bounded middle partials while
`hearAll` preserves the original outer partials; Oscillator Bank resynthesizes
the resulting four-partial frame without device, resource, or external-I/O
authority. Connected selector values are finite and constant for each public
render transaction or realtime callback.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c50_live_spectrum_modifier_selectors.talas.json \
  --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c50_live_spectrum_modifier_selectors.talas.json \
  --frames 8 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Live Spectrum Modifier Seed

[`c51_live_spectrum_modifier_seed.talas.json`](c51_live_spectrum_modifier_seed.talas.json)
drives Spectrum Modifier's optional `seed` Control from a Scalar Event Value.
The connected finite value stays in `[-1, 1]`, overrides only the stored `0.5`
fallback for each complete public transaction, and feeds the existing
deterministic per-partial probability hash. Oscillator Bank resynthesizes the
accepted four-partial frame without mutable RNG state, device, resource, or
external-I/O authority.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c51_live_spectrum_modifier_seed.talas.json \
  --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c51_live_spectrum_modifier_seed.talas.json \
  --frames 8 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Live Spectrum Modifier Hysteresis

[`c52_live_spectrum_modifier_hysteresis.talas.json`](c52_live_spectrum_modifier_hysteresis.talas.json)
connects the finite `C52LoFreq` Event Value to Spectrum Modifier's low-frequency
bound and drives it through `4 → 6 → 4.5 → 3` at exact three-frame public
boundaries. The 4 Hz partial starts active, exits when the low bound becomes
6 Hz, remains silent in the 4.5 Hz deadband, and re-enters only when the
contracted lower boundary reaches 4 Hz. Oscillator Bank makes that bounded
per-source-ordinal membership audible without resources, devices, schema
changes, mutable randomness, or external I/O.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c52_live_spectrum_modifier_hysteresis.talas.json \
  --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c52_live_spectrum_modifier_hysteresis.talas.json \
  --frames 12 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Spectrum Modifier Probability Stream

[`c53_spectrum_modifier_probability_stream.talas.json`](c53_spectrum_modifier_probability_stream.talas.json)
feeds one 4 Hz partial through Spectrum Modifier with probability `0.7` and
seed `-1`. The first successfully published public transaction or callback
uses ordinal 0 and sounds; the next uses ordinal 1 and is silent. Internal
scheduler or render-block segments reuse the public frame's ordinal, so the CLI
command below renders one continuous ordinal-0 decision while a prepared host
auditions the deterministic stream across successive callbacks. The example
adds no resources, devices, schema changes, mutable global randomness, or
external I/O; spectral metadata and source-driven cadence remain outside
the bounded contract.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c53_spectrum_modifier_probability_stream.talas.json \
  --status --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c53_spectrum_modifier_probability_stream.talas.json \
  --frames 6 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Spectrum Modifier Track Probability Profile

[`c54_spectrum_modifier_track_probability_profile.talas.json`](c54_spectrum_modifier_track_probability_profile.talas.json)
feeds four ordered partials through Spectrum Modifier with the static
probability expression `TrackNumber / 4` and seed `-1`. Compilation validates
and clips one immutable entry for every admitted one-based source ordinal
1–4096. On public ordinal 0, tracks 2–4 sound while track 1 is silent, making
the per-track profile distinct from one scalar fallback. A connected
`probability` Control would remain authoritative. TrackNumber is source order,
not stable spectral metadata identity; reassociation and source-driven cadence
remain outside this bounded local profile.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c54_spectrum_modifier_track_probability_profile.talas.json \
  --status --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c54_spectrum_modifier_track_probability_profile.talas.json \
  --frames 6 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Spectrum Modifier Remainder Probability Profile

[`c55_spectrum_modifier_remainder_probability_profile.talas.json`](c55_spectrum_modifier_remainder_probability_profile.talas.json)
uses the format-35 `variablesV2` language to compile the exact historical
probability expression `(TrackNumber - 1) rem: 2`. The immutable 4096-entry
profile assigns positive zero to every odd one-based source ordinal and one to
every even ordinal, so this four-partial patch sounds only tracks 2 and 4. The
expression is evaluated entirely during preparation; callbacks consume the
frozen profile, and a connected `probability` Control would still override it.
TrackNumber remains static source order rather than stable spectral metadata;
general Smalltalk selectors, live expression evaluation, source-driven cadence,
and track reassociation are not claimed.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c55_spectrum_modifier_remainder_probability_profile.talas.json \
  --status --sample-rate 32 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c55_spectrum_modifier_remainder_probability_profile.talas.json \
  --frames 6 --sample-rate 32 --max-block-frames 3 \
  --format json
```

## Spectrum On Disk Frame Scan

[`c56_spectrum_on_disk_frame_scan.talas.json`](c56_spectrum_on_disk_frame_scan.talas.json)
loads one repository-original immutable table containing eight equal-width
four-partial frames. `rateScale` advances by 0.02 logical analysis frames after
each successfully published public render transaction, so adjacent frames are
interpolated before the Oscillator Bank resynthesizes them. Set the
`C56Restart` Event Value from zero to a positive value to restart at frame zero;
leave it positive to continue forward, then return it to zero before the next
restart. The table is fully copied and validated during preparation. This
example does not stream or decode a disk analysis in the callback and does not
claim SOS compatibility, analysis-hop timing, reverse playback, or
sample-accurate mid-block trigger changes.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c56_spectrum_on_disk_frame_scan.talas.json \
  --status --sample-rate 44100 --max-block-frames 512
./build/core-debug/talas_cli render \
  --project examples/c56_spectrum_on_disk_frame_scan.talas.json \
  --frames 1024 --sample-rate 44100 --max-block-frames 512 \
  --format json
```

## Spectrum Voiced Unvoiced Transition

[`c57_spectrum_voiced_unvoiced_transition.talas.json`](c57_spectrum_voiced_unvoiced_transition.talas.json)
scans a repository-original immutable five-frame, two-partial Spectrum table.
The first frame is harmonic, the next two are non-harmonic, and the final two
are harmonic again. One successful public render transaction per logical frame
therefore produces the exact detector sequence `1, 0.5, 0, 0.5, 1` on both
channels. The half-valued frames are transactional transition markers rather
than sample ramps: internal scheduler blocks reuse one selected value, and a
failed transaction does not advance either the source scan or detector state.
Reset, transport discontinuity, replacement, and Workbench restage clear the
history so the next frame publishes its endpoint. This example preserves the
bounded harmonic-multiple classifier; it does not claim native analysis
voicing metadata, transient inference, or smoothing longer than one public
frame.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c57_spectrum_voiced_unvoiced_transition.talas.json \
  --status --sample-rate 8 --max-block-frames 1
./build/core-debug/talas_cli render \
  --project examples/c57_spectrum_voiced_unvoiced_transition.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 1 \
  --format json
```

## Spectrum In RAM Normalized TimeIndex

[`c58_spectrum_in_ram_normalized_time_index.talas.json`](c58_spectrum_in_ram_normalized_time_index.talas.json)
loads a repository-original immutable three-frame, two-partial Spectrum table
into prepared operation-owned memory. The `C58TimeIndex` Event Value feeds a
Constant connected to `SpectrumInRAM`: `-1` selects the first analysis frame,
`0` the exact middle frame, and `1` the last frame. Intermediate values linearly
interpolate adjacent fixed-width frames before the frequency and level scales
are applied. The connected control must remain constant for each public render
transaction; the callback performs no path access, decoding, resource lookup,
or allocation. This example does not claim automatic traversal, variable-width
analysis frames, analysis metadata, or resynthesis.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c58_spectrum_in_ram_normalized_time_index.talas.json \
  --status --sample-rate 1000 --max-block-frames 4
./build/core-debug/talas_cli render \
  --project examples/c58_spectrum_in_ram_normalized_time_index.talas.json \
  --frames 4 --sample-rate 1000 --max-block-frames 4 \
  --format json
```

## Spectrum In RAM Default Analysis Metadata

[`c59_spectrum_in_ram_default_analysis_metadata.talas.json`](c59_spectrum_in_ram_default_analysis_metadata.talas.json)
loads a repository-original immutable one-frame, three-partial Spectrum table.
The source intentionally omits `frequency` and `nbrPartials`: the static
`analysisFrequency = 125 Hz` metadata supplies the original pitch, while the
default count selects every partial from `firstPartial = 1` through the end of
the authenticated fixed-width frame. Track 3 therefore publishes amplitude
`0.6` on the left and frequency `375 Hz` on the right in both offline and
prepared realtime. The numeric sentinels are static fallback states; a
connected zero `Frequency` or `NbrPartials` Control is rejected. This example
does not claim file-header metadata discovery, automatic traversal,
variable-width analysis, or resynthesis.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c59_spectrum_in_ram_default_analysis_metadata.talas.json \
  --status --sample-rate 1000 --max-block-frames 4
./build/core-debug/talas_cli render \
  --project examples/c59_spectrum_in_ram_default_analysis_metadata.talas.json \
  --frames 4 --sample-rate 1000 --max-block-frames 4 \
  --format json
```

## Spectrum On Disk Default Analysis Metadata

[`c60_spectrum_on_disk_default_analysis_metadata.talas.json`](c60_spectrum_on_disk_default_analysis_metadata.talas.json)
loads a repository-original immutable two-frame, three-partial Spectrum table.
The source intentionally omits `frequency` and `nbrPartials`:
`analysisFrequency = 125 Hz` preserves the authored analysis pitch and the
numeric default count selects every partial through the authenticated frame
tail. Track 3 publishes amplitude/frequency `0.6 / 375 Hz` in the first public
transaction and `1.2 / 437.5 Hz` in the second. Offline internal blocks and
prepared scheduler segments capture one effective six-control snapshot; a
failed or late-changing transaction publishes nothing and advances no scan
state. Connected zero sentinels are rejected. This bounded example does not
decode a native analysis header, stream from disk in the callback, infer
analysis cadence, traverse in reverse, or resynthesize the Spectrum.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c60_spectrum_on_disk_default_analysis_metadata.talas.json \
  --status --sample-rate 1000 --max-block-frames 2
./build/core-debug/talas_cli render \
  --project examples/c60_spectrum_on_disk_default_analysis_metadata.talas.json \
  --frames 4 --sample-rate 1000 --max-block-frames 2 \
  --format json
```

## Sum Of Sines Default Analysis Frequencies

[`c61_sum_of_sines_default_analysis_frequencies.talas.json`](c61_sum_of_sines_default_analysis_frequencies.talas.json)
loads one repository-original immutable amplitude table and intentionally omits
both `frequency0` and `frequency1`. Their static `0 hz` defaults resolve
independently from `analysisFrequency0 = 1 hz` and
`analysisFrequency1 = 3 hz`; `pchMorph = 0.5` therefore resynthesizes one
partial at exactly `2 Hz`. At the example's 16 Hz verification rate, the first
four samples are `0`, `sqrt(2)/2`, `1`, and `sqrt(2)/2` offline and in prepared
realtime. A connected zero frequency is invalid rather than another Default
sentinel. This bounded example does not decode native analysis metadata, scan
time-varying analysis frames, or enable loops and `CtrlTime`.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c61_sum_of_sines_default_analysis_frequencies.talas.json \
  --status --sample-rate 16 --max-block-frames 4
./build/core-debug/talas_cli render \
  --project examples/c61_sum_of_sines_default_analysis_frequencies.talas.json \
  --frames 4 --sample-rate 16 --max-block-frames 4 \
  --format json
```

## Sum Of Sines Controlled-Time Frames

[`c62_sum_of_sines_controlled_time_frames.talas.json`](c62_sum_of_sines_controlled_time_frames.talas.json)
loads three one-partial amplitude snapshots and drives `timeIndex` with the
normalized per-sample sequence `-1, 0, 1, 0`. With `ctrlTime = true`, linear
selection produces amplitudes `1, 0.5, 0.25, 0.5`; `gate = 0` and
`loop = true` are deliberately inert. At 8 Hz with a 1 Hz fundamental, the
first four output samples are exactly `0`, `sqrt(2)/4`, `0.25`, and
`sqrt(2)/4` offline and prepared realtime. Automatic Gate/Loop traversal and
real SOS-file decoding remain outside this bounded example.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c62_sum_of_sines_controlled_time_frames.talas.json \
  --status --sample-rate 8 --max-block-frames 2
./build/core-debug/talas_cli render \
  --project examples/c62_sum_of_sines_controlled_time_frames.talas.json \
  --frames 4 --sample-rate 8 --max-block-frames 2 \
  --format json
```

## Sum Of Sines Automatic Gate Loop

[`c63_sum_of_sines_automatic_gate_loop.talas.json`](c63_sum_of_sines_automatic_gate_loop.talas.json)
loads three one-partial amplitude snapshots and drives `gate` with the finite
sequence `1, 1, 1, 1, 1, 1, 0, 0, 1, 1`. With `ctrlTime = false`, a one-second
full traversal at 4 Hz enters the normalized `[0.25, 0.75)` loop, releases
immediately for the two zero-Gate samples, and retriggers from the analysis
start. The ten exact output samples are `0, 0.75, 0, -0.75, 0, 0.75, 0, 0,
0, 0.75` offline and in prepared realtime. Loop points are exact static
proportions; native SOS-file decoding and stored per-partial pitch envelopes
remain outside this bounded example.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c63_sum_of_sines_automatic_gate_loop.talas.json \
  --status --sample-rate 4 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c63_sum_of_sines_automatic_gate_loop.talas.json \
  --frames 10 --sample-rate 4 --max-block-frames 3 \
  --format json
```

## Synthetic Spectrum Array Prefix

[`c64_synthetic_spectrum_array_prefix.talas.json`](c64_synthetic_spectrum_array_prefix.talas.json)
requests three synthetic partials but provides only one amplitude, two
frequencies, and three bandwidths. The common non-empty prefix therefore emits
one partial at amplitude `0.75` and frequency `1 Hz`; negative values in the
discarded frequency and bandwidth tails carry no authority. A one-oscillator
bank resynthesizes the exact 4 Hz sequence `0, 0.75, 0, -0.75`, repeating,
offline and across irregular prepared callbacks. Empty applicable arrays,
dynamic array elements, and sample-serialized envelope update cadence remain
outside this bounded example.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c64_synthetic_spectrum_array_prefix.talas.json \
  --status --sample-rate 4 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c64_synthetic_spectrum_array_prefix.talas.json \
  --frames 8 --sample-rate 4 --max-block-frames 3 \
  --format json
```

## Synthetic Spectrum Static Expressions

[`c65_synthetic_spectrum_static_expressions.talas.json`](c65_synthetic_spectrum_static_expressions.talas.json)
evaluates an immutable `{-6.020599913279624 db}` amplitude followed by the
plain literal `0.25`, plus an arithmetic `{2 * 0.5 hz}` frequency, before C64's
common-prefix selection. The requested three partials are bounded to one by the
shorter enabled-bandwidth array, so a one-oscillator
bank renders the exact 1 Hz, approximately `0.5`-amplitude sequence `0, 0.5,
0, -0.5`, repeating at a 4 Hz sample rate. The unused frequency tail includes
`{60 nn}` and the unused amplitude tail prove complete-tail parsing without
granting either second element DSP authority.
Event Value, variables, stateful/definition expressions, named-pitch literals,
and live array reevaluation remain outside this bounded example.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c65_synthetic_spectrum_static_expressions.talas.json \
  --status --sample-rate 4 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c65_synthetic_spectrum_static_expressions.talas.json \
  --frames 8 --sample-rate 4 --max-block-frames 3 \
  --format json
```

## Threshold Hysteresis Gate

[`c66_threshold_hysteresis_gate.talas.json`](c66_threshold_hysteresis_gate.talas.json)
uses a bipolar `-0.1` to `0.7` control LFO to drive Threshold at `0.5` with
`0.4` hysteresis. At 8 Hz the control reaches the exact `0.7` upper boundary,
returns through the exact `0.3` lower boundary, and then falls strictly below
it. The latch therefore opens at the upper equality, remains open at the lower
equality, and closes on the following sample. It gates a distinct `0.5 Hz`
carrier through a VCA, producing the exact eight-sample lifecycle `0`, `0`,
`sin(pi/4)`, `sin(3*pi/8)`, `1`, `0`, `0`, `0`. Offline and irregular
allocation-free prepared callbacks agree, and reset restores the inactive
latch before the first post-reset sample may reactivate it.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c66_threshold_hysteresis_gate.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c66_threshold_hysteresis_gate.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Time Control Half Speed

[`c67_time_control_half_speed.talas.json`](c67_time_control_half_speed.talas.json)
routes an original `1 Hz` oscillator through `TimeControl` at rate `0.5`.
At 8 Hz, linear reads at positions `0`, `0.5`, `1`, `1.5`, `2`, `2.5`, `3`,
and `3.5` produce the exact half-speed sequence `0`, `sin(pi/4)/2`,
`sin(pi/4)`, `(sin(pi/4)+1)/2`, `1`, `(1+sin(pi/4))/2`, `sin(pi/4)`,
and `sin(pi/4)/2`. Offline and irregular allocation-free prepared callbacks
agree, and reset/restage restore empty history plus read position zero.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c67_time_control_half_speed.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c67_time_control_half_speed.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Time Stopper Sample Hold

[`c68_time_stopper_sample_hold.talas.json`](c68_time_stopper_sample_hold.talas.json)
routes a `1 Hz` oscillator through the current mono `TimeStopper` sample-hold
subset. A scheduled scalar Event Value bridge starts closed, opens at frame one,
and recloses at frame four. At 8 Hz the exact sequence is `0`, `sin(pi/4)`,
`1`, `sin(3*pi/4)`, followed by four held copies of `sin(3*pi/4)`. Offline
and irregular allocation-free prepared callbacks agree, while reset and an
explicit transport rewind/restage restore an empty hold and the pristine
resume schedule. This example does not
claim wider processor-time-freeze semantics.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c68_time_stopper_sample_hold.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c68_time_stopper_sample_hold.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Time Offset Delay Line

[`c69_time_offset_delay_line.talas.json`](c69_time_offset_delay_line.talas.json)
routes a `1 Hz` oscillator through the current mono forward `TimeOffset`
subset with a static `0.25 s` offset. At 8 Hz the exact sequence is three
initial zeroes, `sin(pi/4)`, `1`, `sin(3*pi/4)`, `sin(pi)`, and
`-sin(pi/4)`. Offline and irregular allocation-free prepared callbacks agree;
reset and explicit transport rewind/restage restore zero-filled history and
write index zero. This example does not claim prepared Reverse/Retrograde,
automatic timeline extension, pull-ahead scheduling, or multichannel timing.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c69_time_offset_delay_line.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c69_time_offset_delay_line.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Triggered Sample And Hold Edges

[`c70_triggered_sample_and_hold_edges.talas.json`](c70_triggered_sample_and_hold_edges.talas.json)
uses a bipolar `2 Hz` LFO to trigger a changing `1 Hz` control oscillator on
two exact inactive-to-positive edges. The two distinct held values drive a
`1 Hz` carrier through a VCA, producing initial silence, first capture/hold, a
required non-positive re-arm, and second capture/hold in eight samples at 8 Hz. Offline
and irregular allocation-free prepared callbacks are bit-identical; reset and
explicit transport rewind/restage restore the uncaptured value, initialization
flag, and trigger polarity. This example does not claim general
Sound-object/event-graph scheduling, multichannel sampling, or device behavior.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c70_triggered_sample_and_hold_edges.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c70_triggered_sample_and_hold_edges.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Triggered Table Read Gate Reset

[`c71_triggered_table_read_gate_reset.talas.json`](c71_triggered_table_read_gate_reset.talas.json)
reads a repository-original four-sample immutable table while its direct
Trigger is positive. A scheduled Gate Event Value resets the cursor before
same-frame reading at frame four, so eight samples at 8 Hz advance through the table, replay
from its first value at frame four, and preserve exact partition continuity.
The table drives a 1 Hz carrier through a VCA. Offline and irregular
allocation-free prepared callbacks are bit-identical; transport reset restores
cursor zero, held sample zero, and non-positive previous-Gate state. This
example does not claim MemoryWriter tables, file I/O in the callback, stored
wavetable loop metadata, multichannel tables, or general EventSource graphs.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c71_triggered_table_read_gate_reset.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c71_triggered_table_read_gate_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Tunable Vocoder Bank Reset

[`c72_tunable_vocoder_bank_reset.talas.json`](c72_tunable_vocoder_bank_reset.talas.json)
feeds deterministic 1 Hz carrier and side-chain oscillators into one active
static Tunable Vocoder band. Independent attack and release times make its
carrier integrators, analysis integrators, and envelope history audible; only
the filtered carrier is heard. Offline and irregular allocation-
free prepared callbacks are bit-identical; transport reset clears all five
mutable values and replays the pristine prefix. The example does not claim
dynamic parameters, stereo processing, automatic gain compensation, scale-
derived layouts, or a calibrated filter topology.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c72_tunable_vocoder_bank_reset.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c72_tunable_vocoder_bank_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Two Formant Pair Reset

[`c73_two_formant_pair_reset.talas.json`](c73_two_formant_pair_reset.talas.json)
mixes a deterministic oscillator through Two Formant Element's parallel
fixed-double-zero sections with the accepted deterministic Two Formant Voice
source. Voice remains a sine excitation through RBJ bandpass sections; it does
not share TFE's section topology, and glottal-like excitation
and randomized vibrato remain unimplemented. The TFE candidate pole map is
`r=exp(-pi*bandwidth/Fs)` and `theta=2*pi*formant/Fs`, with signed section
scales and no peak normalization. The section topology is the checked
contract; the coefficient mapping, normalization, and live parameter
transitions are model choices. The graph makes both section histories and the
voice excitation phase audible; offline public transactions and allocation-
free prepared callbacks preserve exact irregular-partition identity, and
transport reset replays the pristine prefix. Multichannel behavior is not
implemented.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c73_two_formant_pair_reset.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c73_two_formant_pair_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Sound Variable Selector Reset

[`c74_sound_variable_selector_reset.talas.json`](c74_sound_variable_selector_reset.talas.json)
selects the second of two deterministic oscillator Sound definitions through a
project-owned Sound variable. Both candidates are fully prepared before
rendering; offline public transactions and allocation-free prepared callbacks
preserve exact irregular-partition identity. Transport reset preserves the
live selection while restoring both candidate phases and selector crossfade
state, so the selected branch restarts without a stale transition. The example
does not claim dynamic topology, input-bearing or multichannel candidates, or
general Sound-variable semantics.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c74_sound_variable_selector_reset.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c74_sound_variable_selector_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Fixed Vocoder Bank Reset

[`c75_fixed_vocoder_bank_reset.talas.json`](c75_fixed_vocoder_bank_reset.talas.json)
feeds deterministic 1 Hz carrier and side-chain oscillators into one active
static fixed Vocoder band. Independent attack and release times make its two
carrier integrators, two analysis integrators, and envelope history audible;
only the filtered carrier is heard. Offline public renders and irregular
allocation-free prepared callbacks are bit-identical; failed public
transactions restore the complete bank, and transport reset replays the
pristine prefix. The example does not claim pitch tracking, dynamic
parameters, logarithmic or scale layouts, stereo processing, automatic gain
compensation, or a calibrated filter topology and scheduling.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c75_fixed_vocoder_bank_reset.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c75_fixed_vocoder_bank_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Vocoder Channel Bank Reset

[`c76_vocoder_channel_bank_reset.talas.json`](c76_vocoder_channel_bank_reset.talas.json)
mixes deterministic 1 kHz and 2 kHz carrier and analysis tones through two
independently authored Spectrum maps, then adds a separate 250 Hz cascade.
The complete mapped coefficient/gain identity, readiness flag, two carrier
integrators, two analysis integrators, and envelope per band are restored after
failed public transactions. Offline renders and irregular allocation-free
prepared callbacks are bit-identical, and transport reset replays the pristine
prefix. The example retains the static contiguous 1..64-channel subset; it does
not claim within-block Spectrum mutation, noncontiguous selection, automatic
gain compensation, stereo/multichannel processing, or a calibrated filter
topology and scheduling.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c76_vocoder_channel_bank_reset.talas.json \
  --status --sample-rate 8000 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c76_vocoder_channel_bank_reset.talas.json \
  --frames 16 --sample-rate 8000 --max-block-frames 3 \
  --format json
```

## Wait Until Buffered Resume

[`c77_wait_until_buffered_resume.talas.json`](c77_wait_until_buffered_resume.talas.json)
connects a scheduled scalar Resume bridge to a one-hertz oscillator through
`WaitUntil`. The first two frames remain silent and enter the bounded history;
opening then drains that exact prefix while current input remains delayed.
Offline public transactions and allocation-free prepared callbacks restore all
history bytes, read/write/count indices, the resumed flag, and predecessor state
after rejection. Transport reset returns to the closed, empty gate. The example
does not claim Resume-as-Sound end detection, EventValue process ownership,
unbounded prepared waiting, a general process lifecycle, or multichannel
timelines.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c77_wait_until_buffered_resume.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c77_wait_until_buffered_resume.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Warped Time Index Restart

[`c78_warped_time_index_restart.talas.json`](c78_warped_time_index_restart.talas.json)
drives `WarpedTimeIndex` from three scheduled scalar Control bridges. Two
initial closed frames expose the inactive index, the first rising edge starts
playback, a closed interval changes Rate and Morph without pausing the active
index, and the second rising edge restarts from the exact beginning. The index
amplitude-modulates a deterministic one-hertz carrier, making continuation and
restart audible. Offline and allocation-free prepared public transactions
restore playback time, active and trigger-edge state after rejection; reset,
replacement, and restage replay the pristine schedule. The example retains
static bounded point arrays and does not claim dynamic arrays, negative rates,
alternative normalization, or a general process lifecycle.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c78_warped_time_index_restart.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c78_warped_time_index_restart.talas.json \
  --frames 12 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Waveshaper Polynomial Curve

[`c79_waveshaper_polynomial_curve.talas.json`](c79_waveshaper_polynomial_curve.talas.json)
routes a deterministic one-hertz oscillator through the finite static
polynomial `0.5x + 0.5x^3`. The curve preserves zero and the bipolar endpoints
while reshaping every finite intermediate sample. Offline and allocation-free
prepared execution are bit-identical across arbitrary partitions; transport
reset and session replacement replay the same stateless result. The example
does not claim MemoryWriter publication, dynamic coefficient lists, saturation,
oversampling, antialiasing, or multichannel waveshaping.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c79_waveshaper_polynomial_curve.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c79_waveshaper_polynomial_curve.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Iterated Waveshaper Feedback

[`c80_iterated_waveshaper_feedback.talas.json`](c80_iterated_waveshaper_feedback.talas.json)
routes a deterministic one-hertz oscillator through an original finite
five-point shaping table three times. The first pass indexes the unscaled
input; each later pass indexes the previous shaped value after exact `0.75`
attenuation. Offline and allocation-free prepared execution share immutable
table bytes and remain bit-identical across irregular partitions, reset, and
replacement. The example does not claim file-backed or MemoryWriter tables,
live mutation, wavetable-domain metadata, interpolation selection, or more than
4096 passes.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c80_iterated_waveshaper_feedback.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c80_iterated_waveshaper_feedback.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Multiplying Waveshaper Product

[`c81_multiplying_waveshaper_product.talas.json`](c81_multiplying_waveshaper_product.talas.json)
uses independent one-hertz index and two-hertz carrier oscillators, an original
finite three-point table, and a connected constant `0.75` scale. The node
samples the table with clamped normalized linear interpolation and publishes
the ordered finite product `audioIn * tableValue * scale`. Offline and
allocation-free prepared execution share immutable table bytes and remain
bit-identical across irregular partitions, reset, and replacement. The example
does not claim editor/detector/delay variants, file-backed or MemoryWriter
tables, live mutation, wavetable-domain metadata, or interpolation selection.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c81_multiplying_waveshaper_product.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c81_multiplying_waveshaper_product.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Input Output Characteristic Curve

[`c82_input_output_characteristic_curve.talas.json`](c82_input_output_characteristic_curve.talas.json)
maps a one-hertz oscillator through five static finite input/output points with
`0.75` smoothing and bounded endpoint slopes. The example exercises the
existing blend between linear and Hermite interpolation while offline and
allocation-free prepared execution retain immutable breakpoint storage and
bit-exact reset/replacement replay. It does not claim Smalltalk-generated or
Event Value arrays, live point mutation, or multichannel curves.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c82_input_output_characteristic_curve.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c82_input_output_characteristic_curve.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Scale and Offset Affine Control

[`c83_scale_and_offset_affine_control.talas.json`](c83_scale_and_offset_affine_control.talas.json)
connects independent one-second Linear Ramps to `value`, `scale`, and `offset`
while retaining deliberately different static scale/offset fallbacks. The
connected values therefore prove sample-accurate precedence across negative
and positive bounds before the checked stateless affine result modulates a
half-amplitude carrier. Offline and allocation-free prepared execution retain
bit-exact irregular-partition, reset, and replacement output. The example does
not claim prepared right-lane or packed-stereo behavior, multichannel affine
processing, unbounded controls, or reconstruction of state already consumed by
an invalid upstream producer.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c83_scale_and_offset_affine_control.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c83_scale_and_offset_affine_control.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Interpolate Dual Lane Control

[`c84_interpolate_dual_lane_control.talas.json`](c84_interpolate_dual_lane_control.talas.json)
connects independent one-second Linear Ramps to `amount` and `rightAmount`
while retaining deliberately different static fallbacks. Independent finite
endpoint pairs exercise both `value` and `rightValue`; their sum modulates a
quarter-amplitude carrier and proves connected-control precedence, checked
convex interpolation, dual-lane publication, and exact prepared reset and
replacement replay. The example does not claim packed-stereo Sound-object
behavior, multichannel interpolation, dynamic-language expressions, or
recovery of state already consumed by an invalid upstream producer.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c84_interpolate_dual_lane_control.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c84_interpolate_dual_lane_control.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Running Max Reset Control

[`c85_running_max_reset_control.talas.json`](c85_running_max_reset_control.talas.json)
feeds a bipolar LFO into `Running Max`. A resource-free Linear Ramp and Equality
pair produce one exact connected reset pulse halfway through the eight-frame
example while the node retains a deliberately nonzero static fallback. The
result demonstrates connected-reset precedence, reset-before-current-sample
ordering, session accumulation, exact irregular prepared partitions, transport
reset, and replacement replay. It does not claim Event Value reset scheduling,
edge-triggered reset semantics, packed stereo, multichannel Sound-object
behavior, or dynamic-language expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c85_running_max_reset_control.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c85_running_max_reset_control.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Running Min Reset Baseline

[`c86_running_min_reset_baseline.talas.json`](c86_running_min_reset_baseline.talas.json)
feeds a bipolar LFO into `Running Min`. A second finite Linear Ramp supplies a
changing connected `maximumAmplitude`, while a resource-free Linear Ramp and
Equality pair produce one exact connected reset pulse at the third frame. The
reset first installs the selected `0.5` baseline and only then compares the
current `1.0` input, visibly restarting the previous zero minimum. Deliberately
different static fallbacks prove connected-control precedence. The result also
demonstrates exact irregular prepared partitions, transport reset, and
replacement replay. It does not claim Event Value reset scheduling,
edge-triggered reset semantics, packed stereo, multichannel Sound-object
behavior, or dynamic-language expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c86_running_min_reset_baseline.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c86_running_min_reset_baseline.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## RMS Squared Envelope

[`c87_rms_squared_envelope.talas.json`](c87_rms_squared_envelope.talas.json)
feeds a finite mono oscillator into `RMS Squared`. Resource-free connected
Linear Ramps select a changing positive `timeConstant` and non-negative
`scale`, deliberately overriding different static fallbacks. The squared
envelope modulates a second oscillator and demonstrates the existing
60-percent exponential calibration, exact irregular prepared partitions,
zero-state transport reset, and replacement replay. It does not claim a square
root/true RMS output, arbitrary windows, multichannel analysis, scheduled
Event Value controls, resources, or dynamic-language expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c87_rms_squared_envelope.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c87_rms_squared_envelope.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Amplitude Follower Envelope

[`c88_amplitude_follower_envelope.talas.json`](c88_amplitude_follower_envelope.talas.json)
feeds a finite mono oscillator into `Amplitude Follower`. Resource-free
connected Linear Ramps select a changing positive `timeConstant` and
non-negative `scale`, deliberately overriding different static fallbacks. The
absolute-amplitude envelope modulates a second oscillator and demonstrates the
existing 60-percent exponential calibration, exact irregular prepared
partitions, zero-state transport reset, and replacement replay. It does not
claim independent attack/release times, multichannel analysis, scheduled Event
Value controls, resources, or dynamic-language expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c88_amplitude_follower_envelope.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c88_amplitude_follower_envelope.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Peak Detector Envelope

[`c89_peak_detector_envelope.talas.json`](c89_peak_detector_envelope.talas.json)
feeds a finite mono oscillator into `Peak Detector`. Resource-free connected
Linear Ramps select changing positive attack and release times plus a
non-negative scale, overriding different static fallbacks. The asymmetric
envelope modulates a second oscillator and demonstrates strict rising attack,
falling/equal release, the existing 60-percent exponential calibration, exact
irregular prepared partitions, zero-state transport reset, and replacement
replay. It does not claim arbitrary detector windows, multichannel analysis,
scheduled Event Value controls, resources, or dynamic-language expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c89_peak_detector_envelope.talas.json \
  --status --sample-rate 8 --max-block-frames 3
./build/core-debug/talas_cli render \
  --project examples/c89_peak_detector_envelope.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 \
  --format json
```

## Frequency Tracker History

[`c90_frequency_tracker_history.talas.json`](c90_frequency_tracker_history.talas.json)
feeds a finite 3.75 Hz mono oscillator into the bounded monophonic
`Frequency Tracker`. Connected minimum and maximum bounds, a descending
confidence control, and a rising scale override deliberately invalid inert
static fallbacks. At 64 Hz the tracker stays silent until it has heard two
periods of its 1 Hz minimum (frame 127); that first estimate stands whatever
its confidence, and the output holds it while the near-certain
Confidence rejects every later one. Estimates fall on a fixed grid of Input
frames, so offline and prepared execution match for any windows, and transport
reset or replacement replays exact empty-history acquisition. The example is
resource-free and schedule-free. It does not claim polyphony, non-default
detector banks, or dynamic pre-emphasis.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c90_frequency_tracker_history.talas.json \
  --status --sample-rate 64 --max-block-frames 64
./build/core-debug/talas_cli render \
  --project examples/c90_frequency_tracker_history.talas.json \
  --frames 256 --sample-rate 64 --max-block-frames 64 \
  --format json
```

## AR Envelope State

[c91_ar_envelope_state.talas.json](c91_ar_envelope_state.talas.json) drives the
resource-free finite linear `AR` envelope with four bounded local Event Value
gate steps. A short attack, held peak, longer release, and the next rising-edge
retrigger modulate a mono carrier through `VCA`. Offline and prepared execution
match exactly for the same event partitions, while transport reset and
replacement replay positive-zero empty-envelope state. It does not claim
exponential segments, connected time/scale inputs, or unrestricted dynamic
expressions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c91_ar_envelope_state.talas.json \
  --status --sample-rate 16 --max-block-frames 16
./build/core-debug/talas_cli render \
  --project examples/c91_ar_envelope_state.talas.json \
  --frames 32 --sample-rate 16 --max-block-frames 16 \
  --format json
```

## ADSR Gate State

[c92_adsr_typed_gate_state.talas.json](c92_adsr_typed_gate_state.talas.json)
drives the resource-free checked linear `ADSR` kernel with twelve bounded local
Event Value steps. Static positive attack, decay, and release times combine
with scheduled sustain, signed scale, direct gate, and legato changes to expose
attack, decay, sustain, falling-edge release, a non-legato zero restart, a live
current-value legato reattack, and a finite negative-scale phase inversion.
Offline and allocation-free prepared execution match bit-exactly across
irregular partitions; transport reset and replacement replay empty envelope,
gate, and Event Value state. Native conformance tests separately prove ordered
targeted typed gate events and connected-gate precedence because format 35 does
not persist direct typed-event authoring. The example does not claim
exponential segments, polyphony, or connected
envelope-time/level/scale inputs.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c92_adsr_typed_gate_state.talas.json \
  --status --sample-rate 16 --max-block-frames 16
./build/core-debug/talas_cli render \
  --project examples/c92_adsr_typed_gate_state.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 \
  --format json
```

## Pulse Train Phase State

[c93_pulse_train_phase_state.talas.json](c93_pulse_train_phase_state.talas.json)
contrasts two resource-free checked `Pulse Train` lanes under fourteen bounded
local Event Value steps. The first lane preserves one-sample pulses while an
intentionally out-of-range duty input proves that duty is inert when
`variableDutyCycle=false`; the second lane exposes fractional duty plus exact
always-low and always-high boundaries. Scheduled period shortening, gate close,
and gate reopen make absolute elapsed-time continuity, exact phase reset, and
the immediate reopening pulse audible through two simple carrier/VCA paths.
Offline and allocation-free prepared execution match bit-exactly across
irregular partitions, and transport reset/replacement replay the initial phase
and timeline. The example does not claim band-limited edges, sync/phase-offset
inputs, or typed trigger events.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c93_pulse_train_phase_state.talas.json \
  --status --sample-rate 16 --max-block-frames 16
./build/core-debug/talas_cli render \
  --project examples/c93_pulse_train_phase_state.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 \
  --format json
```

## Pulse Generator Phase State

[c94_pulse_generator_phase_state.talas.json](c94_pulse_generator_phase_state.talas.json)
mixes three resource-free checked `Pulse Generator` lanes under twenty bounded
local Event Value steps. Linear PolyBLEP and hard-edge lanes preserve their
distinct zero-DC shapes while live frequency and duty changes cross block
boundaries. A silent linear interval applies a negative nonzero reset before
the current sample and then reactivates from exact phase zero; the hard lane
uses the corresponding positive reset. The third lane gives a connected
frequency control and bounded audio-rate oscillator to `modulation=frequency`.
Offline and allocation-free prepared execution match bit-exactly across
irregular partitions, and transport reset/replacement replay the initial phase
and timeline. The example does not claim interpolation
beyond `linear` and `none`, phase/sync inputs, or typed trigger semantics.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c94_pulse_generator_phase_state.talas.json \
  --status --sample-rate 16 --max-block-frames 16
./build/core-debug/talas_cli render \
  --project examples/c94_pulse_generator_phase_state.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 \
  --format json
```

## Oscillator Phase State

[c95_oscillator_phase_state.talas.json](c95_oscillator_phase_state.talas.json)
mixes a sine fallback lane with linear and nearest-sample views of one small,
immutable cyclic wavetable. Twenty bounded local Event Value steps expose live
frequency, amplitude, envelope, pitch bend, positive reset-before-current-
sample, and a connected bounded audio-rate FM lane across callback boundaries.
Offline and allocation-free prepared execution match bit-exactly across
irregular partitions, while transport reset/replacement replay the initial
phase and timeline. The example does not claim negative
frequency, additional interpolation modes, phase/sync inputs, or typed trigger
semantics.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c95_oscillator_phase_state.talas.json \
  --status --sample-rate 16 --max-block-frames 16
./build/core-debug/talas_cli render \
  --project examples/c95_oscillator_phase_state.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 \
  --format json
```

## Bounded Disk Recorder

[`c13_bounded_disk_recorder.talas.json`](c13_bounded_disk_recorder.talas.json)
is one terminal mono WAV/16 profile. Offline render and prepared realtime expose
the same triggered 20 ms preview; neither operation opens the persisted
`fileName`. In the Workbench, the adopted profile changes the Analysis action
to `Record Disk Recorder...`. Recording begins only after choosing an explicit
local `.wav` destination, and publication uses the existing bounded queue and
failure-atomic take owner.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c13_bounded_disk_recorder.talas.json \
  --status --sample-rate 8000 --max-block-frames 7
./build/core-debug/talas_cli render \
  --project examples/c13_bounded_disk_recorder.talas.json \
  --frames 200 --sample-rate 8000 --max-block-frames 7 \
  --format json
```

The status line reports `owner-action=required` and `path-authority=none`.
The resource value is sanitized only into a chooser filename suggestion; it
cannot grant a directory, replace the project/package/source assets, or enable
automatic trigger-started writing. Stereo, AIFF, 24-bit, compressed, arbitrary
path, and cloud publication profiles remain unsupported.

## Portable processor placement

[`c12_portable_processor_placement.talas.json`](c12_portable_processor_placement.talas.json)
wraps one oscillator in nested logical processor 3 and processor 7 boundaries.
Prepared status publishes the oscillator plus inner boundary in domain 3, the
outer boundary in domain 7, the Audio output in host domain 0, and exact 3→7
and 7→0 transfers. Offline and prepared realtime audio remains the same
zero-latency serial passthrough.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c12_portable_processor_placement.talas.json \
  --status --sample-rate 48000 --max-block-frames 128
./build/core-debug/talas_cli render \
  --project examples/c12_portable_processor_placement.talas.json \
  --frames 512 --sample-rate 48000 --format json
```

Logical IDs are portable static placement authority, not OS thread affinity or
an expansion-card claim. Execution stays serial on the host callback.

## Native complex STFT round-trip

[`m3_native_fft_round_trip.talas.json`](m3_native_fft_round_trip.talas.json) routes
an oscillator through `talas.forwardFftV1` and `talas.inverseFftV1`. Status reports
two `ComplexSpectrumV1` records with `N=64`, `H=16`, periodic Hann, and one
63-frame round-trip latency. Select either FFT node in the Workbench to inspect
the same format, cadence, latency, and limitation IDs.

```bash
./build/core-debug/talas_cli render \
  --project examples/m3_native_fft_round_trip.talas.json \
  --frames 512 --sample-rate 8192 --format json
```

## Historical packed-Audio FFT round-trip

[`m3_packed_fft_round_trip.talas.json`](m3_packed_fft_round_trip.talas.json) keeps
the historical Audio-port `talas.fft` boundary explicit. Status reports two
`PackedRealAudioV1` records, never native complex frames, with independent
63-frame transform latencies and a 126-frame terminal path.

```bash
./build/core-debug/talas_cli render \
  --project examples/m3_packed_fft_round_trip.talas.json \
  --frames 512 --sample-rate 8192 --format json
```

## Immutable DiskPlayer and DiskCache streaming

[`m3_streaming_health.talas.json`](m3_streaming_health.talas.json) routes one
verified `Audio/M3Stream` generation through `DiskPlayer`, `DiskCache`, and the
Audio output. The original mono PCM16-WAV fixture is
[`resources/m3-stream-source.wav`](resources/m3-stream-source.wav): 2048 frames
at 8192 Hz, generation 1, SHA-256
`db335f904b35a742ab63a0fd1cb0a81a79c169d3855ef0623e54c731eff00d89`.
Status reports the verified resource plus playback and cache-prewarm contracts.
While playing in the Workbench, select the player or cache to inspect fill,
request pressure, underrun/missing frames, stale blocks, worker state, and
decode failures.

```bash
./build/core-debug/talas_cli render \
  --project examples/m3_streaming_health.talas.json \
  --frames 512 --sample-rate 8192 --format json
```

The asset was generated once from the project-original oscillator example:

```bash
./build/core-debug/talas_cli render \
  --project examples/oscillator.talas.json \
  --frames 2048 --sample-rate 8192 \
  --output examples/resources/m3-stream-source.wav
```

Regeneration is deliberate, not automatic. Different bytes require a reviewed
manifest replacement and generation increment.

## Bounded Disk Cache capture and playback

[`c11_disk_cache_capture.talas.json`](c11_disk_cache_capture.talas.json) contains the
result of the public `capture-disk-cache` operation over the enabled
`[0.002, 0.006)` second Timeline loop at 8000 Hz. The operation rendered the
upstream 1000 Hz oscillator off the audio callback, published generation 1 of
[`DiskCache/c11-disk-cache-loop.wav`](DiskCache/c11-disk-cache-loop.wav), and
committed the exact `talas.diskCache.capture.v1` source fingerprint. The original
mono PCM16-WAV is 32 frames / 108 bytes with SHA-256
`2b0ecfcfa9be0a0376fdc990c429f56e79ad793671c287bb248d52a9fe7caa5b`.

Validate and render the immutable captured generation:

```bash
./build/core-debug/talas_cli validate \
  --project examples/c11_disk_cache_capture.talas.json \
  --status --sample-rate 8000 --max-block-frames 7
./build/core-debug/talas_cli render \
  --project examples/c11_disk_cache_capture.talas.json \
  --frames 40 --sample-rate 8000 --max-block-frames 7 --format json
```

Playback begins at captured frame zero and emits zero after frame 32. To make a
new generation, set the cache node's `record` parameter to `true`, retain the
enabled bounded loop, save the project, and use the selected-node Workbench
action or the explicit CLI command. Ordinary `render` never writes a file.

M4 Step 3 also reuses this project-original WAV as missing-asset recovery
evidence. Its integration fixture moves one exact payload behind two stable
Audio asset identities, finds both from one explicitly selected local folder,
applies both path changes as one Undo step, and proves Save As, reopen,
recovery, package portability/integrity, and ordinary CLI validate/render/export
without distributing any third-party material. A same-hash move preserves
generation 1; different bytes still require explicit replacement confirmation.

## Bounded MIDI Mapper gate and velocity modes

[`c10_midi_mapper_modes.talas.json`](c10_midi_mapper_modes.talas.json) routes two
oscillators through independent `talas.midiMapper` nodes. `gate` opens its branch
fully for any accepted note, while `velocity` scales its branch by the maximum
active note velocity divided by 127. Both use channel `0` (any channel), the
full `0...127` note range, and four deterministic voice slots. With no MIDI
input both mapped branches are closed, so ordinary unscheduled rendering is
intentionally silent.

Prepared status exposes both exact mapper contracts and the deferred
`source`, `midiFile`, `script`, and `shared` fields:

```bash
./build/core-debug/talas_cli validate \
  --project examples/c10_midi_mapper_modes.talas.json \
  --status --sample-rate 8192 --max-block-frames 128
```

The checked example regression schedules Note On at frame 4 and Note Off at
frame 12, proves full-gate plus normalized-velocity samples, and compares the
offline and prepared-realtime traces sample for sample. Live Workbench playback
uses the same stage-bound MIDI ingress; no MIDI file or script path is implied.

## Polyphonic MIDI Mapper Input

[`c200_polyphonic_midi_mapper_input.talas.json`](c200_polyphonic_midi_mapper_input.talas.json)
uses the format-38 `polyphonicInput` Mapper mode. It prepares two independent
copies of the Input oscillator for overlapping notes. With no scheduled MIDI
notes, offline rendering produces silence. Prepared status lists the two voice
scopes; the example can be opened in the Workbench to send live MIDI notes.

```bash
./build/core-debug/talas_cli validate \
  --project examples/c200_polyphonic_midi_mapper_input.talas.json --status
```

## Matrix4 to Output4

[`m3_matrix4_output4.talas.json`](m3_matrix4_output4.talas.json) sends four distinct
oscillators through an identity `Matrix4` to `Output4`. Status reports
`discrete (4)` and logical ordinals 1–4 with `device=not-attached`; it does not
invent physical CLI routes. JSON render names exactly `ch1` through `ch4`.

```bash
./build/core-debug/talas_cli render \
  --project examples/m3_matrix4_output4.talas.json \
  --frames 128 --sample-rate 8192 --format json
```

## LiveSpectralAnalysis partial frames

[`m3_live_spectral_analysis.talas.json`](m3_live_spectral_analysis.talas.json) keeps
an audible oscillator branch while routing the same signal through
`LiveSpectralAnalysis`, the explicit legacy hold, and a track selector. Status
distinguishes native `PartialSpectrumV1` analysis from
`LegacyPartialTriples`: `N=16`, `H=4`, periodic Hann, and 15 frames of analysis
latency at 8192 Hz. Select the analysis or legacy-hold node in the Workbench to
inspect the boundary directly.

```bash
./build/core-debug/talas_cli render \
  --project examples/m3_live_spectral_analysis.talas.json \
  --frames 128 --sample-rate 8192 --format json
```

## Format-13 state layers

[`m4_state_layers.talas.json`](m4_state_layers.talas.json) is the original current
format-13 fixture. It separates root and reusable-Sound topology from their
portable Structure layouts, includes one portable Performance control layout,
and spells out the empty version-1 snapshot library. It contains no pane split,
pan/zoom, selection, focus, or bookmark fields; those live only in the local
WorkspaceSession codec and cannot dirty the project.

```bash
./build/core-debug/talas_cli validate \
  --project examples/m4_state_layers.talas.json
```

## Format-13 snapshot performance

[`m4_snapshot_performance.talas.json`](m4_snapshot_performance.talas.json) is the
original populated F2 fixture. Its node-scope `Voice Pitch` bank targets the
stable exposed `snapshotVoice/frequency` Sound parameter, whose prepared
binding is the scoped `SnapshotPitch:hertz` Event Value. It stores 220 Hz
`Soft` and 440 Hz `Full` snapshots, uses 0.125 seconds of smoothing, and
declares one unlocked `110...880 Hz` random range. The graph is a small audible
reusable Sound, so Performance / Snapshots recall, morph, and seeded
randomization change the oscillator while it plays. The integration test
proves canonical format-11 bytes, exact semantic resolution, one direct
transaction for recall, the exact 330 Hz midpoint, bounded seeded output, and
unchanged authoring bytes for all live actions.

```bash
./build/core-debug/talas_cli validate \
  --project examples/m4_snapshot_performance.talas.json
```

## Format-13 sample-map instrument

[`m4_sample_map_instrument.talas.json`](m4_sample_map_instrument.talas.json) is the
original populated F4 fixture. Its project-native `talas.sampleMapPlayer` reaches
one accepted Audio generation only through the `originalInstrument` map. The
map contains a forward layer with exact sustain and alternate release loops,
plus an overlapping high-velocity reverse layer with an alternate sustain
loop. Both reference the original
[`resources/m3-stream-source.wav`](resources/m3-stream-source.wav) bytes; no
external material is required.

```bash
./build/core-debug/talas_cli validate \
  --project examples/m4_sample_map_instrument.talas.json
./build/core-debug/talas_cli render \
  --project examples/m4_sample_map_instrument.talas.json \
  --frames 256 --sample-rate 8192 --max-block-frames 128 \
  --format json
```

The `talas_m4_sample_map_workflow_tests` integration gate uses this exact example
to prove canonical Open/Save/Save As, checked-save external-conflict rejection,
autosave/recovery, map-only asset watching, package integrity, and ordinary CLI
validate/render for both the source project and the portable package.

## Format-13 missing-component preservation

[`m4_missing_component_preservation.talas.json`](m4_missing_component_preservation.talas.json)
is the original F5a data-preservation fixture. Its deliberately unavailable
synthetic `org.talas.example.spectral-colour` node retains two inputs, two
outputs, five typed frozen parameters, seven opaque state bytes including
non-UTF8 values, both incident graph connections, one snapshot target, and its
portable canvas position. It contains no executable field, path, payload, or
copied reference data.

```bash
./build/core-debug/talas_cli validate \
  --project examples/m4_missing_component_preservation.talas.json
```

The command exits `1` by design. Its stdout is exactly:

```text
Portable document preserved: yes
Executable in this registry: no
```

The nonzero status is the safety contract, not a broken example: the project
can be inspected and saved, but Play/export/render cannot bypass the missing
component. `talas_m4_missing_node_workflow_tests` proves canonical Open/Save/Save
As, checked-save conflict rejection, autosave/recovery, a package containing
only `Manifest.json` and `Project.talas.json`, failed render without destination
publication, and byte-identical repair after an exact test-only trusted
descriptor is supplied.

## LFO phase and transport

[`c96_lfo_phase_transport.talas.json`](c96_lfo_phase_transport.talas.json) routes a
sine-only `talas.lfo` into an audible VCA. Ten bounded local Event Value steps
demonstrate selected frequency and range endpoints, a zero-frequency phase
hold, sample-accurate updates, continuity across blocks, and exact
positive-zero transport replay without external resources.

```bash
./build/core-debug/talas_cli render \
  --project examples/c96_lfo_phase_transport.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 --format json
```

## Ramp trigger and transport

[`c97_ramp_trigger_transport.talas.json`](c97_ramp_trigger_transport.talas.json)
combines `ramp`, `fullRamp`, `repeatingRamp`, and `repeatingFullRamp` through
one audible VCA. Fifteen bounded local Event Value steps demonstrate positive
trigger edges, close/reopen behavior, edge-captured duration changes,
continuity across irregular callback partitions, and exact transport replay
without external resources.

```bash
./build/core-debug/talas_cli render \
  --project examples/c97_ramp_trigger_transport.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 --format json
```

## Sample And Hold clock and transport

[`c98_sample_and_hold_clock_transport.talas.json`](c98_sample_and_hold_clock_transport.talas.json)
combines direct Event Value and connected hold-time authority over two periodic
`talas.sampleAndHold` clocks. Eight bounded local steps demonstrate static rounded
offsets, live per-frame modulo periods, held values across irregular callback
partitions, connected fallback hiding, and exact transport replay through one
audible resource-free carrier.

```bash
./build/core-debug/talas_cli render \
  --project examples/c98_sample_and_hold_clock_transport.talas.json \
  --frames 48 --sample-rate 16 --max-block-frames 16 --format json
```

## Samples From Disk Single Step transport

[`c99_samples_from_disk_single_step_transport.talas.json`](c99_samples_from_disk_single_step_transport.talas.json)
reads one repository-original finite immutable table through connected
file-position, trigger, and gate controls. Eight bounded local Event Value
steps demonstrate a nonzero initial position, one-sample advance, trigger hold
and resume, gate re-arm and rising-edge restart from an updated position,
connected fallback hiding, continuity across irregular callback partitions,
and exact transport replay. The admitted subset performs no callback file I/O;
disk streaming, decoding, multichannel data, rate conversion, fractional or
reverse positions, and looping remain unsupported.

```bash
./build/core-debug/talas_cli render \
  --project examples/c99_samples_from_disk_single_step_transport.talas.json \
  --frames 12 --sample-rate 8 --max-block-frames 3 --format json
```

## Phase Shift By 90 allpass transport

[`c100_phase_shift_by_90_allpass_transport.talas.json`](c100_phase_shift_by_90_allpass_transport.talas.json)
mixes one static and one connected `talas.phaseShiftBy90` path over a shared
resource-free oscillator. At its selected frequency, each phase node sends a
+45-degree shift from `audioOut` and a -45-degree shift from `rightOut` to separate stereo mixes; both
use the mono `audioIn` because `rightIn` is unconnected. Four bounded local
Event Value steps demonstrate strictly sub-Nyquist tuned-frequency changes,
connected fallback hiding, independent two-branch allpass state across
irregular callback partitions, and exact transport replay. The branch filters
have unit gain. Coefficients, gain, transients, nonlinearity, and
broadband Hilbert behavior remain unverified.

```bash
./build/core-debug/talas_cli render \
  --project examples/c100_phase_shift_by_90_allpass_transport.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

## Single Side Band RM quadrature transport

[`c101_single_side_band_rm_quadrature_transport.talas.json`](c101_single_side_band_rm_quadrature_transport.talas.json)
mixes one static and one connected `talas.singleSideBandRm` path over a shared
resource-free oscillator. Each node uses the stereo +45/-45 degree Phase Shift
By 90 pair and a Quad Oscillator sum, while the unconnected optional `rightIn`
duplicates the mono `audioIn`; the final output is mono. The connected
frequency and scale own invalid inert fallbacks while bounded Event Value
steps demonstrate positive, zero, and negative derived shifts with positive
scale values across irregular callback partitions and exact transport replay.
The allpass branches use unit gain. Coefficients, gain transients,
nonlinearity, broadband behavior, and polyphony semantics remain unverified.

```bash
./build/core-debug/talas_cli render \
  --project examples/c101_single_side_band_rm_quadrature_transport.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

## SOS Oscillators selected wavetable transport

[`c102_sos_oscillators_selected_wavetable_transport.talas.json`](c102_sos_oscillators_selected_wavetable_transport.talas.json)
mixes two `talas.sosOscillators` paths over one static three-partial Spectrum.
The first selects partials one and two with the sine fallback; the second
selects partials two and three with an immutable finite four-sample cyclic
wavetable. Separate cascade oscillators make the cascade-first ordered sum
audible. This is the admitted mono static-Spectrum additive-resynthesis
subset, not time-varying SOSAnalysis reconstruction, file streaming, a DSP
bank, or stereo/polyphonic synthesis.

```bash
./build/core-debug/talas_cli render \
  --project examples/c102_sos_oscillators_selected_wavetable_transport.talas.json \
  --frames 16 --sample-rate 16 --max-block-frames 5 --format json
```

## Low-Pass Gate control-state transport

[`c103_low_pass_gate_control_state_transport.talas.json`](c103_low_pass_gate_control_state_transport.talas.json)
drives a mono oscillator through `talas.lowPassGate` with a connected deterministic
Pulse Train. The connection owns an intentionally invalid inert direct fallback,
while separate attack and release constants make retained control and filter
state audible across irregular callback partitions and exact transport replay.
This is Talas's original finite one-pole Low-Pass Gate utility, not an analog
vactrol or circuit model, resonant or oversampled filter, or stereo/polyphonic
processor.

```bash
./build/core-debug/talas_cli render \
  --project examples/c103_low_pass_gate_control_state_transport.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

## Reverb Section fractional-delay transport

[`c104_reverb_section_fractional_delay_transport.talas.json`](c104_reverb_section_fractional_delay_transport.talas.json)
drives a mono oscillator through Talas's bounded
`talas.reverbSection` allpass implementation. A connected deterministic Linear
Ramp owns an intentionally invalid inert `delayScale` fallback and makes
fractional delay selection, the 60 dB feedback tail, fixed-capacity private
memory, callback partitions, and exact transport replay audible. The supported
contract always uses private zero-initialized memory; `prezero=false` external
reuse, smoothed delay changes, and stereo/multichannel/polyphonic behavior are
not implemented.

```bash
./build/core-debug/talas_cli render \
  --project examples/c104_reverb_section_fractional_delay_transport.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

## Monotonizer known-pitch transport

[`c105_monotonizer_known_pitch_transport.talas.json`](c105_monotonizer_known_pitch_transport.talas.json)
drives a deterministic bin-aligned 468.75 Hz mono oscillator through the
historical `talas.monotonizer` known-pitch subset toward a connected block-constant 703.125 Hz
target. The active profile exposes the exact 1023-frame latency, retained
phase-vocoder history, failure-atomic processing, and exact transport replay.
Pitch detection, non-singleton source ranges, scale or note quantization,
sample-rate target modulation, and stereo/multichannel/polyphonic behavior are
not implemented.

```bash
./build/core-debug/talas_cli render \
  --project examples/c105_monotonizer_known_pitch_transport.talas.json \
  --frames 1536 --sample-rate 48000 --max-block-frames 256 --format json
```

## Multifile Disk Player selection transport

[`c106_multifile_disk_player_selection_transport.talas.json`](c106_multifile_disk_player_selection_transport.talas.json)
reuses the two unchanged C37 finite tables. Connected Event Values own inert
`1 / 0` fallbacks, begin reverse half-rate playback from the first table, close
the trigger without truncating its release-through-end tail, then select and
retrigger the longer table. Offline and prepared execution share the same
checked selection/cursor transition, complete failure rollback, zero-frame
no-op, compiler-owned plan authority, and exact reset/replacement replay.

```bash
./build/core-debug/talas_cli render \
  --project examples/c106_multifile_disk_player_selection_transport.talas.json \
  --frames 12 --sample-rate 8 --max-block-frames 3 --format json
```

The bounded in-memory mono subset is unchanged. Real disk streaming/decoding,
sample-rate metadata/resampling, dynamic lists, loop regions, crossfades,
mutable tables, and stereo/multichannel playback remain outside this package.

## Sample Cloud dynamic controls

[`c110_sample_cloud_dynamic_controls.talas.json`](c110_sample_cloud_dynamic_controls.talas.json)
reuses the unchanged C33 waveform and C71 finite table as immutable sample and
grain-envelope authority. All nine optional controls are connected to Event
Values, so the deliberately unevaluable `1 / 0` direct fallbacks remain inert.
The static seed drives deterministic stereo grains while density, pan, and the
prepared `1..64` maximum-grain bound change during the render.

```bash
./build/core-debug/talas_cli render \
  --project examples/c110_sample_cloud_dynamic_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share checked stereo mixing, exact scheduler
rollback, partition-stable repaired retry, compiler-owned ordered immutable
resources, and fresh-equivalent reset/replacement/transport state. Offline
retains its scalable grain policy while prepared callbacks retain fixed
allocation-free storage for at most 64 starts. Memory Writer, disk/stream
sources, mutable or dynamically selected resources, connected seed, and
multichannel placement beyond stereo remain outside this package.

## Cloud Bank checked Spectrum

[`c111_cloud_bank_checked_spectrum.talas.json`](c111_cloud_bank_checked_spectrum.talas.json)
reuses the unchanged C33 waveform and C71 finite table as compiler-owned grain
wave and envelope authority. A deterministic packed two-partial Spectrum and
all four required connected Event Value controls drive bounded aggregate
synthesis with live pan, frequency modulation, and duration modulation.

```bash
./build/core-debug/talas_cli render \
  --project examples/c111_cloud_bank_checked_spectrum.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the same checked per-frame transition,
exact-or-reject frame and analytical grain-index arithmetic, complete
failure-atomic retry, zero-frame identity, and compiler-owned ordered Spectrum
and immutable-table plan authority. Offline retains up to 4096 tracks/grains;
prepared callbacks retain the fixed `1..64` track/grain and 4096-candidate
work ceiling. `CloudBankElement`, dynamic or streamed tables, stochastic
allocation, native `PartialSpectrumV1`, and multichannel placement beyond
stereo remain outside this package.

## Grain Cloud Dynamic Controls

[`c113_grain_cloud_dynamic_controls.talas.json`](c113_grain_cloud_dynamic_controls.talas.json)
reuses the unchanged C33 waveform and C71 finite table as compiler-owned
waveform and grain-envelope authority. All nine optional controls are connected
to a deterministic Event Value schedule, including live frequency, duration,
density, maximum-grain, pan, amplitude, and three jitter streams.

```bash
./build/core-debug/talas_cli render \
  --project examples/c113_grain_cloud_dynamic_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share checked per-grain interpolation, duration,
frequency, jitter, pan, accumulation, and stereo publication arithmetic plus
partition-stable scheduling, complete failure rollback, zero-frame identity,
immutable compiler-owned plan/resource authority, and exact
reset/replacement/transport replay. Offline retains its scalable static-grain
policy; connected prepared `maxGrains` uses fixed allocation-free capacity 64.
Mutable or disk/stream tables, connected seed, unbounded prepared grains, and
multichannel placement beyond stereo remain outside this package.

## Chopper Dynamic Durations

[`c114_chopper_dynamic_durations.talas.json`](c114_chopper_dynamic_durations.talas.json)
reuses the unchanged C71 finite table as compiler-owned envelope authority. An
oscillator passes through the mono Chopper while two connected Event Values
change grain duration and inter-grain silence at exact scheduled frames; the
deliberately invalid direct fallbacks prove that connected controls exclusively
own the live values.

```bash
./build/core-debug/talas_cli render \
  --project examples/c114_chopper_dynamic_durations.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the same checked candidate-only cycle,
wrap, normalized linear interpolation, multiplication, state-step, and
publication arithmetic. Irregular partitions, complete failure rollback,
zero-frame identity, reset/replacement/transport replay, and exact immutable
plan/resource authority are deterministic and allocation-free in prepared
callbacks. File/stream or Memory Writer tables, overlapping or randomized
grains, and stereo/multichannel placement remain outside this package.

## Function Generator Trigger Duration

[`c115_function_generator_trigger_duration.talas.json`](c115_function_generator_trigger_duration.talas.json)
reuses the unchanged C71 finite table as compiler-owned envelope authority. A
connected Event Value trigger starts and retriggers the one-shot at exact
scheduled frames while a second connected Event Value changes its live
duration. The direct trigger is closed and the direct duration is deliberately
non-positive, proving that connected controls exclusively own the live values.

```bash
./build/core-debug/talas_cli render \
  --project examples/c115_function_generator_trigger_duration.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the same checked candidate-only trigger,
duration, normalized linear interpolation, output, state-step, and publication
arithmetic. Irregular partitions, complete failure rollback, zero-frame
identity, reset/replacement/transport replay, and exact immutable plan/resource
authority are deterministic and allocation-free in prepared callbacks.
MemoryWriter, file/stream or mutable tables, and direct parameter Event Value
scheduling remain outside this package.

## Multislope Connected Controls

[`c116_multislope_connected_controls.talas.json`](c116_multislope_connected_controls.talas.json)
uses three connected Event Values to drive live level, rising-edge gate, and
positive rate controls around a nontrivial constant four-point envelope. All
three direct fallbacks deliberately divide by zero, proving that connected
controls exclusively own the evaluated values.

```bash
./build/core-debug/talas_cli render \
  --project examples/c116_multislope_connected_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked candidate-only envelope
preparation and frame transition, including derived breakpoints, gate edges,
segment arithmetic, output scaling, elapsed stepping, complete rollback,
zero-frame identity, reset/replacement replay, and exact compiler-owned plan,
settings, binding, and envelope authority. Hot lists, loops, graphical editing,
nonlinear segments, and dynamic process lifecycle remain outside this package.

## Cloud Bank Element Range Cascade Delay

[`c112_cloud_bank_element_range_cascade_delay.talas.json`](c112_cloud_bank_element_range_cascade_delay.talas.json)
reuses the unchanged C33 waveform and C71 finite table as compiler-owned grain
wave and envelope authority. It selects packed Spectrum partials 2 and 3,
preserves a 1.5-frame initial delay at 8192 Hz, adds one finite mono oscillator
cascade exactly once to both outputs, and drives all four required controls
from a deterministic Event Value schedule.

```bash
./build/core-debug/talas_cli render \
  --project examples/c112_cloud_bank_element_range_cascade_delay.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the C111 checked transition, selected-only
packed-Spectrum validation, exact-or-reject frame and analytical grain-index
arithmetic, fractional-delay scheduling, complete failure rollback, zero-frame
identity, immutable compiler-owned plan/resource authority, and exact
reset/replacement/transport replay. Offline retains its scalable 4096 bounds;
prepared callbacks retain `first/count/maxGrains` in `1..64` and the
4096-candidate work ceiling. Dynamic or streamed tables, stochastic allocation,
connected seed semantics, native `PartialSpectrumV1`, and multichannel
placement beyond stereo remain outside this package.

## Multisegment Loop Connected Controls

[`c117_multisegment_loop_connected_controls.talas.json`](c117_multisegment_loop_connected_controls.talas.json)
uses connected Event Values to drive live level and rising-edge gate controls
around a three-segment static envelope whose second-through-third segment loop
retains endpoint drift. Both direct fallbacks deliberately produce the exact
non-finite result accepted by connected precedence, proving that connected
controls exclusively own the evaluated values without hiding other errors.

```bash
./build/core-debug/talas_cli render \
  --project examples/c117_multisegment_loop_connected_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked candidate-only envelope and
loop-window preparation plus one frame transition, including cumulative time,
loop indices, interpolation, completed-loop drift, gate edges, output scaling,
elapsed stepping, complete rollback, zero-frame identity, reset/replacement
replay, and exact compiler-owned plan, settings, binding, and static-loop
authority. Dynamic lists or loops, exact gate-release lifecycle, nonlinear
segments, and arbitrary process lifecycle remain outside this package.

## Noise Modes Connected Controls

[`c118_noise_modes_connected_controls.talas.json`](c118_noise_modes_connected_controls.talas.json)
mixes deterministic pink and hot-pink generators while connected Event Values
drive frequency, center, and scale through four audible control phases. All six
direct fallbacks deliberately divide by zero, proving that successfully
connected controls exclusively own their prepared values.

```bash
./build/core-debug/talas_cli render \
  --project examples/c118_noise_modes_connected_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked candidate-only seed, PRNG,
pink-filter, hot-pink phase/smoothing, scale, center, and output transition.
The example retains exact rollback, zero-frame identity, irregular-partition
stability, reset/replacement replay, and compiler-owned plan, settings,
binding, buffer, scope, latency, and resource authority. The generator is an
original algorithm.

## Absolute Value Connected Magnitude

[`c119_absolute_value_connected_magnitude.talas.json`](c119_absolute_value_connected_magnitude.talas.json)
routes a scheduled negative Event Value through Absolute Value before it drives
an audible VCA. The finite zero fallback remains inert while the connected
control moves from magnitude 0.25 to 0.75 halfway through the example.

```bash
./build/core-debug/talas_cli render \
  --project examples/c119_absolute_value_connected_magnitude.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite-binary64 magnitude
transition and publish a block only after every frame validates. Positive and
negative zero, subnormals, ordinary values, and maximum finite values retain
their frozen bit semantics; non-finite input, complex/vector magnitude,
audio-bus expansion, and scripts remain outside this bounded package.

## ArcTan Connected Curve

[`c120_arctan_connected_curve.talas.json`](c120_arctan_connected_curve.talas.json)
routes a scheduled negative Event Value through ArcTan before it drives an
audible VCA. The finite zero fallback remains inert while the connected
control moves from -0.25 to -2.0 halfway through the example; bypassing ArcTan
therefore produces an observably different gain curve.

```bash
./build/core-debug/talas_cli render \
  --project examples/c120_arctan_connected_curve.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the platform binary64 `atan` operation
behind one checked finite transition and publish a block only after every
frame validates. Both signed zeros, subnormals, ordinary values, and maximum
finite values retain frozen platform-result bits. Non-finite input,
complex/vector arctangent, `atan2`, degree conversion, audio-bus expansion,
alternate libm implementations, and scripts remain outside this bounded
package.

## Equality Connected Comparison

[`c121_equality_connected_comparison.talas.json`](c121_equality_connected_comparison.talas.json)
routes two scheduled finite Event Values through Equality before the exact
Boolean result drives an audible VCA. The controls move through equal,
unequal, and equal phases; bypassing the comparator, either input, or either
timeline change therefore produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c121_equality_connected_comparison.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite exact-binary64
comparison and publish a block only after both complete input lanes validate.
All signed-zero pairs compare equal and results are canonical positive `1.0`
or `0.0`; adjacent values and unequal subnormals remain unequal. Non-finite,
fuzzy, ordered, complex, vector, string, audio-bus, and script comparison
remain outside this bounded package.

## Set Range Connected Affine Mapping

[`c122_set_range_connected_affine_mapping.talas.json`](c122_set_range_connected_affine_mapping.talas.json)
routes five scheduled finite Event Values through Set Range before the mapped
Control drives an audible VCA. Four timeline phases exercise ordinary,
reversed-output, bit-distinguishing extrapolated, and reversed-old-range
mapping. Disconnecting any authoritative input, bypassing Set Range, or moving
a scheduled change therefore produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c122_set_range_connected_affine_mapping.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the ordered binary64 expression
`newMin + ((value - oldMin) / (oldMax - oldMin)) * (newMax - newMin)`
and publish a block only after all five complete selected lanes and every
named arithmetic intermediate validate. Connected bounds own validation over
their inert finite fallbacks. Non-finite or saturating arithmetic, epsilon
range comparison, clamping, range sorting, alternate expression order, and
vector/complex/audio-bus/script mapping remain outside this bounded package.

## Product Connected Finite Multiplication

[`c123_product_connected_finite_multiplication.talas.json`](c123_product_connected_finite_multiplication.talas.json)
routes two scheduled finite Event Values through Product before the exact
binary64 result drives an audible VCA. Four timeline phases exercise positive,
negative, signed-zero, and fractional multiplication. Disconnecting either
required input, replacing multiplication with addition, bypassing Product, or
moving a scheduled change therefore produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c123_product_connected_finite_multiplication.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 multiply and
publish a block only after both complete input lanes and every candidate
result validate. Signed-zero propagation, subnormal operands and results,
gradual underflow, and maximum finite products retain established bits.
Non-finite or saturating arithmetic and complex/vector/matrix/unit/audio-bus/
script multiplication remain outside this bounded package.

## Difference Connected Finite Subtraction

[`c124_difference_connected_finite_subtraction.talas.json`](c124_difference_connected_finite_subtraction.talas.json)
routes two scheduled finite Event Values through ordered Difference before the
exact binary64 result drives an audible VCA. Four timeline phases exercise
positive, negative, exact cancellation, and fractional subtraction. Swapping
the operands, disconnecting either required input, replacing subtraction with
addition, bypassing Difference, or moving a scheduled change therefore
produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c124_difference_connected_finite_subtraction.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 `left -
right` transition and publish a block only after both complete input lanes and
every candidate result validate. Operand order, signed-zero rules, exact
cancellation, subnormal operands and results, adjacent representable
differences, and maximum finite results retain established bits. Non-finite or
saturating arithmetic, epsilon comparison, and complex/vector/matrix/unit/
audio-bus/script subtraction remain outside this bounded package.

## Sqrt Magnitude Connected Saturated Norm

[`c125_sqrt_magnitude_connected_saturated_norm.talas.json`](c125_sqrt_magnitude_connected_saturated_norm.talas.json)
routes two same-frequency audio oscillators through Sqrt Magnitude directly to
the mono output. Independently scheduled amplitudes create mono-like `0.5/0`,
ordinary `0.3/0.4`, exact-boundary `0.6/0.8`, and saturated `0.8/0.8` phases.
Disconnecting either lane, duplicating one producer, bypassing Sqrt Magnitude,
replacing the norm with either raw lane, or moving a scheduled change therefore
produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c125_sqrt_magnitude_connected_saturated_norm.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 `std::hypot`
transition, saturate the magnitude at exact `1.0`, and publish only after every
present input lane and complete candidate block validate. Positive zero,
subnormal results, exact below-boundary norms, the saturation boundary, and
finite over-range inputs retain established behavior. Alternate libm identity
and complex/vector/matrix/multichannel/control/script magnitude remain outside
this bounded package.

## VCA Connected Finite Gain

[`c126_vca_connected_finite_gain.talas.json`](c126_vca_connected_finite_gain.talas.json)
routes a mono oscillator through VCA while one connected Control schedules
positive `0.5`, negative `-0.75`, zero, and unity gain phases. The stored
static fallback is deliberately different at `0.25`; disconnecting the gain,
bypassing VCA, replacing multiplication with passthrough, or moving a scheduled
change therefore produces observably different output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c126_vca_connected_finite_gain.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 multiply and
publish only after the complete audio, selected gain, and candidate block
validate. Connected gain owns selection over the inert static fallback;
signed-zero, subnormal results, gradual underflow, and all finite product bits
retain established behavior. Non-finite or saturating arithmetic, stereo,
multichannel, and complex/vector/matrix/unit/script gain remain outside this
bounded package.

## Constant Finite Event Value Broadcast

[`c127_constant_finite_event_value_broadcast.talas.json`](c127_constant_finite_event_value_broadcast.talas.json)
uses one scheduled Constant as the exact shared Control source for two VCA
consumers with distinct oscillator carriers. The stream steps through positive
`0.5`, negative `-0.75`, zero, and unity values; the two stored VCA fallbacks
are deliberately distinct, so disconnecting either fan-out edge is observable.

```bash
./build/core-debug/talas_cli render \
  --project examples/c127_constant_finite_event_value_broadcast.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite binary64 broadcast for
each scheduler-selected segment. Every destination frame receives the exact
selected bits, including signed zero and subnormal values, and non-finite
scalars reject before publication. Callback rollback restores scheduled Event
Value cursors, producer state, frame position, and output before repeated or
repaired retry. Audio-rate, unit-polymorphic, vector, matrix, complex, string,
resource, script, and random semantics remain outside this bounded package.

## Add Finite Event Value Sum

[`c128_add_finite_event_value_sum.talas.json`](c128_add_finite_event_value_sum.talas.json)
connects a scheduled finite Constant and a static finite Constant to one Add.
The exact sum fans out as the gain for two distinct oscillator/VCA branches,
then the checked branches meet in one audible mix. Timeline steps cover
positive, negative, unity, and exact-zero sums, so operand, schedule, fan-out,
and bypass mutations remain observable.

```bash
./build/core-debug/talas_cli render \
  --project examples/c128_add_finite_event_value_sum.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 addition per
frame. Finite signed-zero, subnormal, adjacent, cancellation, and maximum
finite results retain their exact bits; non-finite operands and overflow reject
before any block publication. Callback rollback restores scheduled Event Value
cursors, producer and neighboring state, frame position, and output before
repeated or repaired retry. Variadic, compensated, saturating, unit-aware,
audio-bus, vector, matrix, complex, string, resource, script, and coercion
semantics remain outside this bounded package.

## Mixer Checked Finite Weighted Sum

[`c129_mixer_checked_finite_weighted_sum.talas.json`](c129_mixer_checked_finite_weighted_sum.talas.json)
weights two distinct oscillator streams with VCAs whose gains come from two
Constant control lanes, and adds them in a Mixer. A scheduled Event Value
changes the first connected gain while the second remains static. The mixed
result fans out through a checked VCA monitor branch and, weighted again, into
a second Mixer before reaching the mono output, so connected gain precedence,
branch order, fan-out, and downstream transaction behavior remain audible and
mutation-sensitive. Project format 43 gave the example this shape when it
replaced the two-input Mixer.

```bash
./build/core-debug/talas_cli render \
  --project examples/c129_mixer_checked_finite_weighted_sum.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked ordered native binary64
transition per frame: each VCA rounds its product, and the Mixer adds the
products in connection order. Finite signed-zero, subnormal, cancellation, rounding, and
maximum-finite results retain exact bits; non-finite operands, product overflow,
and final-sum overflow reject before block publication. Callback rollback
restores scheduled Event Value cursors, producer state, frame position, and
output before repeated or repaired retry. Variadic, normalized, saturating,
multichannel, pan-law, unit-aware, vector, matrix, complex, resource, script,
and coercion semantics remain outside this bounded package.

## Level Checked Finite Stereo Gain

[`c130_level_checked_finite_stereo_gain.talas.json`](c130_level_checked_finite_stereo_gain.talas.json)
connects one oscillator to a checked Level. A scheduled Event Value drives the
connected left Constant gain while a second Constant supplies the independent
right gain; both override deliberately different finite expression fallbacks.
The Level outputs pass through separate checked VCA monitor branches before
reaching distinct stereo terminal channels, keeping gain precedence, channel
identity, and downstream transaction behavior audible and mutation-sensitive.

```bash
./build/core-debug/talas_cli render \
  --project examples/c130_level_checked_finite_stereo_gain.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 mono-to-stereo
transition per frame: `audioIn * left` followed by `audioIn * right`. Both
products are separately rounded and neither channel nor a valid block prefix
is published unless every input, gain, and product is finite. Signed zeros,
subnormals, polarity, mute, and maximum-finite boundaries preserve exact bits;
callback rollback restores scheduled Event Value cursors, producer state,
frame position, and both outputs before repeated or repaired retry. Linked,
normalized, logarithmic, dB, pan-law, stereo-input, multichannel, unit-aware,
vector, matrix, complex, resource, and script semantics remain outside this
bounded package.

## Channel Crosser Checked Finite Matrix

[`c131_channel_crosser_checked_finite_matrix.talas.json`](c131_channel_crosser_checked_finite_matrix.talas.json)
feeds two independent oscillators into a checked Channel Crosser. Four connected
Constant controls override deliberately different matrix fallbacks; the first
coefficient follows a four-segment Event Value schedule. Separate checked VCA
monitor branches retain both matrix outputs before the stereo terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c131_channel_crosser_checked_finite_matrix.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 2x2
transition. Both inputs and all coefficients are validated first, four products
and then both sums are evaluated in fixed separately rounded order, and neither
channel nor a block prefix is published unless the complete block succeeds.
Missing `rightIn` supplies exact positive zero. Fused, reassociated, normalized,
pan-law, larger matrix, multichannel, vector, complex, and script semantics
remain outside this bounded package.

## Channel Join Checked Finite Dual Lane

[`c132_channel_join_checked_finite_dual_lane.talas.json`](c132_channel_join_checked_finite_dual_lane.talas.json)
feeds two independent oscillators into a checked Channel Join. The left carrier
follows a two-segment Event Value amplitude schedule; separate checked VCA
monitor branches retain both joined lanes before the stereo terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c132_channel_join_checked_finite_dual_lane.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked native binary64 dual-lane
transition. The required left input is validated before the required right
input, both values are copied bit-exactly, and neither channel nor a block
prefix is published unless the complete block succeeds. Optional or missing
inputs, stereo buses, channel conversion, swap, crossfeed, matrix routing,
gain, pan, mix, vector, complex, resource, and script semantics remain outside
this bounded package.

## Channeller Checked Finite Live Selectors

[`c133_channeller_checked_finite_live_selectors.talas.json`](c133_channeller_checked_finite_live_selectors.talas.json)
feeds two audible oscillators into Channeller's left and right inputs while two
independent Event Value controls schedule its left and right selector inputs
through all four routing masks before the stereo terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c133_channeller_checked_finite_live_selectors.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite whole-block gate.
Connected selectors override Boolean fallbacks and check a channel exactly when
strictly positive. A checked channel's binary64 bits play on both outputs, both
checked pass the pair through, and neither checked publishes canonical positive
zero. Input is validated before the left and right selectors, and neither
output nor a prefix is published on failure. Thresholds,
hysteresis, smoothing, crossfeed, swaps, matrices, gain, pan, resources, and
scripts remain outside this bounded package.

## Pan Checked Finite Live Controls

[`c134_pan_checked_finite_live_controls.talas.json`](c134_pan_checked_finite_live_controls.talas.json)
feeds one audible oscillator into Pan while two independent Event Value
controls schedule its position and scale through four sample-exact stereo
states before the terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c134_pan_checked_finite_live_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite whole-block
transition. Connected sample-rate controls override scalar fallbacks. Stereo
power and linear laws plus equal-power discrete4 and discrete8 ring layouts
validate input, pan, scale, scaled input, gains, and products in a fixed order;
no output lane or block prefix is published unless the complete active layout
succeeds. Position is bounded to `[0, 1]`, scale is finite and non-negative,
and non-finite or overflowing arithmetic fails closed. Smoothing, extrapolated
positions, negative scale, ambisonics, arbitrary speaker maps, resources, and
scripts remain outside this bounded package.

## Crossfade Checked Finite Live Controls

[`c135_crossfade_checked_finite_live_controls.talas.json`](c135_crossfade_checked_finite_live_controls.talas.json)
feeds two audible oscillators into Crossfade while three independent Event
Value controls schedule pan, scale, and fade through four sample-exact stereo
states before the terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c135_crossfade_checked_finite_live_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite whole-block
transition. Connected sample-rate controls override scalar fallbacks. Linear
and equal-power input crossfades are evaluated in ordered dual-input form,
then scale and equal-power stereo pan are applied; input samples, controls,
gains, products, sums, and outputs are validated in a fixed order. No output
lane or block prefix is published unless both outputs succeed. Pan and fade
are bounded to `[0, 1]`, scale is finite and non-negative, and non-finite or
overflowing arithmetic fails closed. Smoothing, limiting, stereo inputs,
additional inputs, alternate laws, resources, and scripts remain outside this
bounded package.

## Stereo Mix 2 Checked Live Controls

[`c136_stereo_mix2_checked_live_controls.talas.json`](c136_stereo_mix2_checked_live_controls.talas.json)
feeds two audible oscillators into Stereo Mix 2 while six independent Event
Value controls schedule bipolar left/right output levels and each input's pan
and scale through four sample-exact stereo states before the terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c136_stereo_mix2_checked_live_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite whole-block
transition. Connected sample-rate controls override scalar fallbacks. Each
input is scaled, equal-power panned, and accumulated in exact input/lane order
before bipolar output levels are applied. Inputs, controls, gains, products,
contributions, sums, and outputs are validated in a fixed order, and neither
lane nor any block prefix is published unless the complete stereo block
succeeds. Output levels are bounded to `[-1, 1]`; pans and scales are bounded
to `[0, 1]`. Stereo Mix 4, alternate laws, limiting, smoothing, stereo inputs,
additional inputs, resources, and scripts remain outside this bounded
package.

## Stereo Mix 4 Checked Live Controls

[`c137_stereo_mix4_checked_live_controls.talas.json`](c137_stereo_mix4_checked_live_controls.talas.json)
feeds four audible oscillators into Stereo Mix 4 while ten independent Event
Value controls schedule bipolar left/right output levels and every input's pan
and scale through four sample-exact stereo states before the terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c137_stereo_mix4_checked_live_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked finite whole-block
transition. Connected sample-rate controls override scalar fallbacks. All four
inputs are scaled, equal-power panned, and accumulated in exact input/lane
order before bipolar output levels are applied. Inputs, controls, gains,
products, contributions, sums, and outputs are validated in a fixed order,
and neither lane nor any block prefix is published unless the complete stereo
block succeeds. Output levels are bounded to `[-1, 1]`; pans and scales are
bounded to `[0, 1]`. Alternate laws, limiting, smoothing, stereo inputs,
additional inputs, resources, and scripts remain outside this bounded
package.

## Audio Output Exact Stereo Terminal

[`c138_audio_output_exact_stereo_terminal.talas.json`](c138_audio_output_exact_stereo_terminal.talas.json)
routes two independently scheduled audible oscillators directly into the
ordered left and right inputs of the Audio Output render terminal.

```bash
./build/core-debug/talas_cli render \
  --project examples/c138_audio_output_exact_stereo_terminal.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution preserve the two producer buffers without gain,
mixing, duplication, conversion, or reordering. The node owns no DSP state,
resources, reset, transport behavior, or physical-device selection; those
remain producer or host responsibilities.

## Averaging Low Pass Filter Dynamic Window

[`c139_averaging_low_pass_filter_dynamic_window.talas.json`](c139_averaging_low_pass_filter_dynamic_window.talas.json)
passes an audible oscillator through Averaging Low Pass Filter while a positive
connected ramp changes the fractional moving-average window sample by sample.

```bash
./build/core-debug/talas_cli render \
  --project examples/c139_averaging_low_pass_filter_dynamic_window.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share one checked fixed-capacity history ring.
The current scaled input and preceding whole samples are accumulated
newest-to-oldest, the next oldest sample receives the exact fractional weight,
and the result is divided by the unrounded cutoff period. Connected cutoff
owns its inert static fallback, callback partitions never resize or clear
history, and finite arithmetic failure publishes no prefix or state. The
`Private` resource declaration is an opaque compatibility label for node-local
scratch memory; no table payload is read. External or shared wavetable memory,
alternate windows, and multichannel filtering are outside this bounded
package.

## Delay With Feedback Dynamic Controls

[`c140_delay_with_feedback_dynamic_controls.talas.json`](c140_delay_with_feedback_dynamic_controls.talas.json)
passes an audible oscillator through Delay With Feedback while three connected
ramps change delay time, bounded feedback, and affine dry/wet mix sample by
sample.

```bash
./build/core-debug/talas_cli render \
  --project examples/c140_delay_with_feedback_dynamic_controls.talas.json \
  --frames 1024 --sample-rate 8192 --max-block-frames 127 --format json
```

Offline and prepared execution share the same checked integer-frame transition
and a fixed connected-history ring. Delay uses nearest-frame rounding,
feedback magnitude remains below one, finite mix may extrapolate beyond the
ordinary dry/wet interval, and every arithmetic intermediate is checked before
state commits. Callback partitions never resize history; zero-delay frames
retain dry input for later positive delay, transport reset clears the complete
line, and rejected callbacks publish no prefix or state. Fractional
interpolation, smoothing, multichannel delay, event scheduling, and saturation
are outside this bounded package.

## Set Duration Bounded Window

[`c141_set_duration_bounded_window.talas.json`](c141_set_duration_bounded_window.talas.json)
passes a `1 Hz` oscillator through Set Duration with a `0.25 s` start time and
`0.5 s` duration. At 8 Hz the exact eight-frame output is three zeroes,
`sin(pi/4)`, `1`, `sin(3*pi/4)`, then two zeroes: the two-frame delay replays
the source only inside the half-open frame window `[2, 6)`.

```bash
./build/core-debug/talas_cli render \
  --project examples/c141_set_duration_bounded_window.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share one checked elapsed-window and fixed
delay-ring transition. Static, direct EventValue, or connected timing is
finite and representable, connected Controls own their fallbacks, callback
partitions never resize history, reset clears elapsed time and ring bytes, and
rejected work publishes no prefix or state. Full Sound event lifecycle,
automatic source extension or termination, and multichannel timelines remain
outside this bounded package.

## Warped Time Index Scheduled Morph

[`c142_warped_time_index_scheduled_morph.talas.json`](c142_warped_time_index_scheduled_morph.talas.json)
drives Warped Time Index with direct scheduled trigger, rate, and morph Event
Values while its checked mapped index controls an audible VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c142_warped_time_index_scheduled_morph.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share one checked bounded map and playback-state
transition. Equal immutable arrays contain between 2 and 4096 strictly
ascending finite second values, morphing is convex, direct live trigger closure
continues active playback, and a rising edge restarts it exactly. Scheduled
controls are sample exact, callback partitions never allocate map storage, reset
restores pristine state, and rejected work publishes no prefix or state. Dynamic
time-point arrays, reverse playback, and pause-on-close trigger semantics
remain outside this bounded package.

## Amplitude Follower Scheduled Controls

[`c143_amplitude_follower_scheduled_controls.talas.json`](c143_amplitude_follower_scheduled_controls.talas.json)
feeds an audible oscillator into Amplitude Follower while direct scheduled
Seconds and Scalar Event Values change its time constant and scale at exact
sample positions; the resulting envelope controls a second audible oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c143_amplitude_follower_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution preserve the established
`alpha = pow(0.4, 1 / (timeConstant * sampleRate))` calibration and checked
absolute-amplitude recurrence. Direct schedules are sample exact, connected
Controls own their fallbacks, callback partitions allocate nothing, reset
restores positive zero, and rejected work publishes no prefix or state.
Multichannel analysis, independent attack/release controls, and unrestricted
dynamic-language evaluation remain outside this bounded package.

## Peak Detector Scheduled Controls

[`c144_peak_detector_scheduled_controls.talas.json`](c144_peak_detector_scheduled_controls.talas.json)
feeds an audible oscillator into Peak Detector while direct scheduled Seconds
and Scalar Event Values change its attack time, release time, and scale at
exact sample positions; the asymmetric envelope controls a second oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c144_peak_detector_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution preserve strict
`scaledAmplitude > previousEnvelope` attack selection, release on equality,
the established `alpha = pow(0.4, 1 / (selectedTime * sampleRate))`
calibration, and checked candidate-state publication. Direct schedules are
sample exact, connected Controls own their fallbacks, callback partitions
allocate nothing, reset restores positive zero, and rejected work publishes no
prefix or state. Multichannel analysis, arbitrary detector windows, and
unrestricted dynamic-language evaluation remain outside this bounded package.

## RMS Squared Scheduled Controls

[`c145_rms_squared_scheduled_controls.talas.json`](c145_rms_squared_scheduled_controls.talas.json)
feeds an audible oscillator into RMS Squared while direct scheduled Seconds
and Scalar Event Values change its time constant and scale at exact sample
positions; the squared envelope controls a second oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c145_rms_squared_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution preserve the established
`alpha = pow(0.4, 1 / (timeConstant * sampleRate))` calibration and checked
scaled-amplitude squaring recurrence. Direct schedules are sample exact,
connected Controls own their fallbacks, callback partitions allocate nothing,
reset restores positive zero, and rejected work publishes no prefix or state.
True RMS square-root output, multichannel or arbitrary-window analysis, and
unrestricted dynamic-language evaluation remain outside this bounded package.

## Running Max Scheduled Reset

[`c146_running_max_scheduled_reset.talas.json`](c146_running_max_scheduled_reset.talas.json)
feeds a bipolar LFO into Running Max while a direct scheduled Scalar Event
Value resets the stored maximum from exact positive zero before the current
sample; the resulting Control modulates an audible oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c146_running_max_scheduled_reset.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C85 transition. Any finite
nonzero reset establishes positive zero before the current value, connected
Controls own their fallbacks, equal signed-zero operands preserve the stored
operand, callback partitions allocate nothing, and rejected work publishes no
prefix or state. Multichannel Sound-object behavior and unrestricted
dynamic-language evaluation remain outside this bounded package.

## Running Min Scheduled Reset Baseline

[`c147_running_min_scheduled_reset_baseline.talas.json`](c147_running_min_scheduled_reset_baseline.talas.json)
feeds a bipolar LFO into Running Min while direct scheduled Scalar Event
Values select both reset and maximum-amplitude baseline at exact sample
positions; the resulting Control modulates an audible oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c147_running_min_scheduled_reset_baseline.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C86 transition. Any finite
nonzero reset installs the exact selected maximum amplitude before the current
value, either connected Control independently owns its fallback, equal
signed-zero operands preserve the stored operand, callback partitions allocate
nothing, and rejected work publishes no prefix or state. Multichannel
Sound-object behavior and unrestricted dynamic-language evaluation remain
outside this bounded package.

## Triggered Sample And Hold Scheduled Edges

[`c148_triggered_sample_and_hold_scheduled_edges.talas.json`](c148_triggered_sample_and_hold_scheduled_edges.talas.json)
feeds a Linear Ramp into Triggered Sample And Hold while a direct scheduled
Scalar Event Value supplies exact inactive-to-positive edges; the captured
Control modulates an audible oscillator.

```bash
./build/core-debug/talas_cli render \
  --project examples/c148_triggered_sample_and_hold_scheduled_edges.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C153 transition. A finite
value is captured only on each exact rising edge, connected Control owns its
direct fallback, signed zero remains inactive, irregular callback partitions
allocate nothing, and rejected work publishes no prefix or state. Multichannel
Sound-object behavior and unrestricted dynamic-language evaluation remain
outside this bounded package.

## Triggered Table Read Paired Schedules

[`c149_triggered_table_read_paired_schedules.talas.json`](c149_triggered_table_read_paired_schedules.talas.json)
uses one repository-owned immutable finite table while independent direct
Scalar Event Value schedules control Triggered Table Read trigger and gate.
The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c149_triggered_table_read_paired_schedules.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C154 cursor transition. The
timeline exposes a trigger hold, a gate reset before a same-frame read, and a
later loop wrap; connected Control independently owns its direct fallback,
irregular callback partitions allocate nothing, and rejected work publishes no
prefix or state. MemoryWriter/file-backed tables, loop metadata, multichannel
tables, and unrestricted dynamic-language evaluation remain outside this
bounded package.

## Function Generator Scheduled Controls

[`c150_function_generator_scheduled_controls.talas.json`](c150_function_generator_scheduled_controls.talas.json)
uses one repository-owned immutable finite envelope while direct Scalar
trigger and Seconds duration Event Value schedules control Function Generator.
The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c150_function_generator_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C155 one-shot transition.
The timeline exposes falling-edge continuation, duration shrink and retained
resume, a same-frame duration update plus retrigger, and exact completion;
connected Control independently owns its direct fallback, irregular callback
partitions allocate nothing, and rejected work publishes no prefix or state.
MemoryWriter/file-backed or metadata-bearing tables, multichannel controls,
alternate interpolation, and unrestricted dynamic-language evaluation remain
outside this bounded package.

## Multislope Scheduled Controls

[`c151_multislope_scheduled_controls.talas.json`](c151_multislope_scheduled_controls.talas.json)
uses a resource-free static piecewise-linear envelope while direct Scalar
level, gate, and rate Event Value schedules control Multislope Function
Generator. The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c151_multislope_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C156 one-shot transition.
The timeline exposes falling-edge continuation, a later level update, a
same-frame level update plus rising-edge retrigger, and a same-frame falling
edge plus rate update; direct controls are applied in level/gate/rate order,
irregular callback partitions allocate nothing, and rejected work publishes no
prefix or state. Hot or metadata-bearing lists, nonlinear segments,
multichannel controls, and unrestricted dynamic-language evaluation remain
outside this bounded package.

## Multisegment Scheduled Loop Controls

[`c152_multisegment_scheduled_loop_controls.talas.json`](c152_multisegment_scheduled_loop_controls.talas.json)
uses a resource-free static piecewise-linear envelope and static segment loop
while direct Scalar level and gate Event Value schedules control Multisegment
Envelope. The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c152_multisegment_scheduled_loop_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C157 transition. The timeline
exposes falling-edge continuation, checked slope-preserving loop drift, a later
level update, and a same-frame level update plus rising-edge retrigger; direct
controls are applied in level/gate order, irregular callback partitions
allocate nothing, and rejected work publishes no prefix or state. Dynamic
lists or loop indices, loop-gate release lifecycle, multichannel
controls, and unrestricted dynamic-language evaluation remain outside this
bounded package.

## Graphical Envelope Scheduled Rate Controls

[`c153_graphical_envelope_scheduled_rate_controls.talas.json`](c153_graphical_envelope_scheduled_rate_controls.talas.json)
uses a resource-free static piecewise-linear one-shot envelope while direct
Scalar level, gate, and positive rate Event Value schedules control Graphical
Envelope. The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c153_graphical_envelope_scheduled_rate_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C158 transition. The timeline
exposes falling-edge continuation, a later level update, a same-frame level
update plus rising-edge retrigger, and same-frame gate/rate ordering; direct
controls are applied in level/gate/rate order, irregular callback partitions
allocate nothing, and rejected work publishes no prefix or state. Graphical
editor metadata, loops, hot shape expressions, nonlinear segments,
multichannel controls, and unrestricted dynamic-language evaluation remain
outside this bounded package.

## ADSR Scheduled Typed Gate Controls

[`c154_adsr_scheduled_typed_gate_controls.talas.json`](c154_adsr_scheduled_typed_gate_controls.talas.json)
uses direct scheduled Seconds attack, decay, and release plus Scalar sustain,
scale, gate, and legato Event Values to drive the resource-free shared checked
linear ADSR kernel. The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c154_adsr_scheduled_typed_gate_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C159 kernel transition. The
timeline exposes live attack/decay/sustain shaping, falling-edge release, a
same-frame attack/scale/legato update plus rising-edge reattack, and a later
release-time update. Native C159 conformance tests separately prove exact
ordered targeted typed-gate edges, same-frame current attack/scale/legato use,
and connected-gate precedence because format 35 intentionally does not persist
project-authored targeted events. Irregular callback partitions allocate
nothing, and rejected work publishes no output prefix or state. Exponential or
arbitrary curves, polyphony, connected envelope controls, and project-authored
targeted events remain outside this bounded package.

## Oscillator Scheduled Phase Controls

[`c155_oscillator_scheduled_phase_controls.talas.json`](c155_oscillator_scheduled_phase_controls.talas.json)
uses direct scheduled finite frequency, amplitude, envelope, pitch-bend, and
reset Event Values to drive a sine carrier with bounded connected audio-rate
frequency modulation.

```bash
./build/core-debug/talas_cli render \
  --project examples/c155_oscillator_scheduled_phase_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C160 Oscillator kernel. The
timeline exposes live frequency and gain changes, envelope scaling, pitch bend,
positive reset before the current sample, phase continuity after reset falls,
and later restoration of the initial control profile. The connected modulator
retains precedence over its inactive scalar fallback, irregular callback
partitions allocate nothing, and rejected work publishes no output prefix or
state. Existing C95 product coverage separately demonstrates immutable cyclic
wavetables with linear and nearest interpolation. Band limiting,
negative frequency, file-backed or metadata wavetables, additional
interpolation, phase sync, typed triggers, and multichannel/polyphonic
expansion remain outside this bounded package.

## Chopper Scheduled Cycle Controls

[`c156_chopper_scheduled_cycle_controls.talas.json`](c156_chopper_scheduled_cycle_controls.talas.json)
uses direct scheduled positive Seconds grain-duration and inter-grain-delay
Event Values to chop a deterministic sine carrier with one immutable finite
plain-table envelope.

```bash
./build/core-debug/talas_cli render \
  --project examples/c156_chopper_scheduled_cycle_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C114 frame transition. The
timeline performs a same-frame cycle shrink, then grows the grain and delay
independently, proving pre-sample shrink/wrap ordering and exact irregular
partition identity. Prepared callbacks allocate nothing, reset returns to the
fresh state, and rejected work publishes no output prefix, schedule cursor, or
elapsed state. Overlapping or randomized grains, density/spatial scheduling,
file-backed or mutable tables, alternate interpolation, and multichannel output
remain outside this bounded package.

## Analog Sequencer Scheduled Controls

[`c157_analog_sequencer_scheduled_controls.talas.json`](c157_analog_sequencer_scheduled_controls.talas.json)
drives the bounded mono-input/stereo-output Analog Sequencer with direct
scheduled left, right, gate, manual-step, and rate Event Values over one
three-step static amplitude sequence.

```bash
./build/core-debug/talas_cli render \
  --project examples/c157_analog_sequencer_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C14 frame transition. The
timeline changes stereo levels together, closes gate while changing step and
rate at the same sample, returns to timed playback, and proves exact block-3,
block-7, irregular-partition, and single-frame identity. Prepared callbacks
allocate nothing, reset returns to fresh state, and rejected work publishes no
output prefix, schedule cursor, or elapsed state. Pitch and extra-value events,
polyphony above one, dynamic step lists, and multichannel input remain outside
this bounded package.

## AR Scheduled Controls

[`c158_ar_scheduled_controls.talas.json`](c158_ar_scheduled_controls.talas.json)
uses direct scheduled positive Seconds attack/release times plus finite Scalar
scale and gate Event Values to drive the resource-free checked linear AR
envelope. The result drives an audible oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c158_ar_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked C91 transition. The timeline
changes attack time and signed scale together, starts release, shortens release
while active, then applies a same-frame attack/scale update plus rising-edge
current-value retrigger before a final falling edge. Irregular callback
partitions allocate nothing, reset returns to fresh state, and rejected work
publishes no output prefix, schedule cursor, or envelope state. Exponential
segments, connected attack/release/scale controls, targeted gate events, and
polyphony remain outside this bounded package.

## Analysis Filter Scheduled Controls

[`c159_analysis_filter_scheduled_controls.talas.json`](c159_analysis_filter_scheduled_controls.talas.json)
drives the bounded mono-input/stereo-output Analysis Filter with direct scheduled
positive fundamental and harmonic Event Values.

The node also accepts harmonic `0` to extract a DC component on the left output,
with zero sine output on the right. The filter shape and complete-bank
unit-amplitude normalization remain open; this example keeps positive harmonics.

```bash
./build/core-debug/talas_cli render \
  --project examples/c159_analysis_filter_scheduled_controls.talas.json \
  --frames 8 --sample-rate 8 --max-block-frames 3 --format json
```

Offline and prepared execution share checked bandpass and first-order allpass
sample transitions. Same-frame control updates change both the fundamental
bandwidth and multiplied center before the current sample. Irregular callback
partitions allocate nothing, reset returns to fresh stereo state, and rejected
work publishes no output prefix, schedule cursor, or filter state. Exact
quadrature, multi-band analysis, live fundamental tracking, arbitrary dynamic
expressions, and polyphony remain outside this bounded package.

## Phase Shift By 90 Scheduled Frequency

[`c160_phase_shift_by_90_scheduled_frequency.talas.json`](c160_phase_shift_by_90_scheduled_frequency.talas.json)
drives both Phase Shift By 90 output branches with a direct scheduled positive
frequency Event Value. `audioOut` advances by 45 degrees and `rightOut` delays by
45 degrees at each scheduled target frequency; the unconnected `rightIn`
duplicates the mono carrier. The first-order branches use unit gain.
Coefficients, gain, transients, nonlinearity, and broadband Hilbert
behavior remain unverified.

```bash
./build/core-debug/talas_cli render \
  --project examples/c160_phase_shift_by_90_scheduled_frequency.talas.json \
  --frames 8 --sample-rate 1024 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked first-order allpass sample
transition. Per-frame frequency updates take effect before the current sample;
irregular callback partitions allocate nothing, reset returns to fresh state,
and rejected work publishes no output prefix, schedule cursor, or filter state.
Broadband Hilbert and multistage behavior, arbitrary dynamic
expressions, smoothing, multichannel layouts, polyphony, and sideband semantics
remain outside this bounded package.

## Single Side Band RM Scheduled Quadrature

[`c161_single_side_band_rm_scheduled_quadrature.talas.json`](c161_single_side_band_rm_scheduled_quadrature.talas.json)
drives the mono-output Single Side Band RM with independent direct frequency
and frequency-scale Event Value schedules. The node uses the stereo +45/-45
degree Phase Shift By 90 pair and Quad Oscillator sum; its unconnected optional
`rightIn` duplicates `audioIn`.

```bash
./build/core-debug/talas_cli render \
  --project examples/c161_single_side_band_rm_scheduled_quadrature.talas.json \
  --frames 8 --sample-rate 1024 --max-block-frames 3 --format json
```

The schedules exercise negative, zero, and positive derived shifts while
keeping frequency scale positive. Offline and prepared execution share the
checked two-filter and carrier state; per-frame updates precede the current
sample, irregular callback partitions allocate nothing, reset restores fresh
state, and rejected updates publish no partial output or state. The allpass
branches use unit gain. Coefficients, gain transients, nonlinearity,
broadband behavior, frequency-scale range, aliasing, smoothing, and polyphony
semantics remain unverified.

## SOS Oscillators Static Spectrum Sine Bank

[`c162_sos_oscillators_static_spectrum_sine_bank.talas.json`](c162_sos_oscillators_static_spectrum_sine_bank.talas.json)
feeds a finite four-partial static Spectrum into the bounded mono SOS
Oscillators bank, selects partials two and three, and adds a nonzero cascade
before using the compiler-owned sine fallback.

```bash
./build/core-debug/talas_cli render \
  --project examples/c162_sos_oscillators_static_spectrum_sine_bank.talas.json \
  --frames 8 --sample-rate 16 --max-block-frames 3 --format json
```

Offline and prepared execution share the checked cascade-first additive
transition, retained phase, exact range selection, failure-atomic state, and
transport reset. The project is intentionally resource free; the immutable
table-backed profile remains covered by C102. Time-varying Spectrum sources,
SOSAnalysis, arbitrary dynamic selection, and multichannel layouts remain
outside this bounded package.

## Low-Pass Gate Scheduled Control

[`c163_low_pass_gate_scheduled_control.talas.json`](c163_low_pass_gate_scheduled_control.talas.json)
drives the checked mono Low-Pass Gate with one direct sample-accurate Event
Value control schedule and immutable cutoff, smoothing, curve, and scale
configuration.

```bash
./build/core-debug/talas_cli render \
  --project examples/c163_low_pass_gate_scheduled_control.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

The control opens at frame four and partially closes at frame eight. Offline
and prepared execution preserve identical smoothing and one-pole state across
callback partitions, exact reset, allocation-free callbacks, and
failure-atomic schedule retry. Dynamic filter configuration, analog vactrol
nonlinearities, resonance, oversampling, and multichannel layouts remain
outside this bounded package.

## Morph Filter Scheduled Controls

[`c164_morph_filter_scheduled_controls.talas.json`](c164_morph_filter_scheduled_controls.talas.json)
drives the checked mono state-variable Morph Filter with five direct,
sample-accurate Event Value schedules for cutoff, resonance, morph, drive, and
scale.

```bash
./build/core-debug/talas_cli render \
  --project examples/c164_morph_filter_scheduled_controls.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

The schedules change together at frames four and eight, exercise the low,
band, and high morph endpoints, and retain bit-identical output across callback
partitions. Alternate filter types, smoothing, oversampling, and multichannel
layouts remain outside this bounded package.

## Quad Function Generator Scheduled Controls

[`c165_quad_function_generator_scheduled_controls.talas.json`](c165_quad_function_generator_scheduled_controls.talas.json)
drives all six bounded controls of the Quad Function Generator with direct,
sample-accurate Event Value schedules and routes its ordered quarter-phase
functions to a discrete four-channel output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c165_quad_function_generator_scheduled_controls.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

The schedules change together at frames four and eight and preserve exact
four-channel output across callback partitions and transport reset. Alternate
function families, independent phase relationships, bipolar or unbounded
scales, stereo remapping, and polyphony remain outside this bounded package.

## Probability Chaos Generator Scheduled Controls

[`c166_probability_chaos_generator_scheduled_controls.talas.json`](c166_probability_chaos_generator_scheduled_controls.talas.json)
drives all five bounded controls of the deterministic Probability Chaos
Generator with direct, sample-accurate Event Value schedules and routes its
retained value and accepted-event gate to ordered stereo output.

```bash
./build/core-debug/talas_cli render \
  --project examples/c166_probability_chaos_generator_scheduled_controls.talas.json \
  --frames 16 --sample-rate 8 --max-block-frames 3 --format json
```

The schedules change together at frames four and eight, exercise
probability and chaos endpoints, exponential slew, deterministic seed replay,
and exact stereo output across callback partitions. Alternate random streams,
chaos maps, mutable seeds, gate widths, and polyphony remain outside this
bounded package.

## Stereo Dispersive Resonator Scheduled Controls

[`c167_stereo_dispersive_resonator_scheduled_controls.talas.json`](c167_stereo_dispersive_resonator_scheduled_controls.talas.json)
drives all eight bounded controls of the fractional-delay stereo
resonator with direct, sample-accurate Event Value schedules after a sine
source.

```bash
./build/core-debug/talas_cli render \
  --project examples/c167_stereo_dispersive_resonator_scheduled_controls.talas.json \
  --frames 16 --sample-rate 8000 --max-block-frames 3 --format json
```

The schedules change together at frames four and eight, exercising delay,
decay, detune, damping, dispersion, width, wet mix, and input gain while
preserving exact stereo output across callback partitions and transport reset.
Alternate interpolation, nonlinear or modal waveguides, multichannel layouts,
and polyphony remain outside this bounded package.

## Plucked Waveguide Scheduled Controls

[`c168_plucked_waveguide_scheduled_controls.talas.json`](c168_plucked_waveguide_scheduled_controls.talas.json)
drives all seven bounded controls of the deterministic monophonic
Plucked Waveguide with direct, sample-accurate Event Value schedules and an
immutable seed.

```bash
./build/core-debug/talas_cli render \
  --project examples/c168_plucked_waveguide_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

Explicit low/high trigger edges begin two reproducible plucks while frequency,
decay, brightness, pick position, stiffness, and level change at exact frames.
Polyphony, MIDI note allocation, audio excitation, body or coupled-string
models, and named instruments remain outside this bounded package.

## Dynamic Breakpoint Oscillator Scheduled Controls

[`c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json`](c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json)
drives all seven bounded controls of the deterministic monophonic
Dynamic Breakpoint Oscillator with direct, sample-accurate Event Value
schedules and immutable point count and seed.

```bash
./build/core-debug/talas_cli render \
  --project examples/c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

Explicit low/high reset edges reproduce the seeded sequence while frequency,
volatility, correlation, timing jitter, curve, and level change at exact
frames across multiple breakpoint segments. Live point-count changes, authored
breakpoint tables, multichannel output, and alternate stochastic/interpolation
laws remain outside this bounded package.

## Wave Terrain Oscillator Scheduled Controls

[`c170_wave_terrain_oscillator_scheduled_controls.talas.json`](c170_wave_terrain_oscillator_scheduled_controls.talas.json)
drives all eight bounded controls of the deterministic monophonic
Wave Terrain Oscillator with direct, sample-accurate Event Value schedules and
one immutable repository-original 8-by-8 row-major terrain.

```bash
./build/core-debug/talas_cli render \
  --project examples/c170_wave_terrain_oscillator_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

Explicit low/high reset edges restart the phase while frequency, both radii,
y ratio, rotation, phase offset, and level select several elliptical and
Lissajous trajectories at exact frames. Live dimensions or terrain changes,
higher-order interpolation, antialiasing, and multichannel output remain
outside this bounded package.

## Scanned Mass Oscillator Scheduled Controls

[`c171_scanned_mass_oscillator_scheduled_controls.talas.json`](c171_scanned_mass_oscillator_scheduled_controls.talas.json)
drives all eleven bounded controls of the deterministic monophonic
Scanned Mass Oscillator with direct, sample-accurate Event Value schedules and
immutable mass count and initial shape.

```bash
./build/core-debug/talas_cli render \
  --project examples/c171_scanned_mass_oscillator_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

Explicit low/high reset and excitation edges reconstruct or perturb the fixed
circular model while scan rate, model rate, stable coupling/restoring tuples,
damping, excitation position and amount, scan offset, and level change at exact
frames. Live topology, arbitrary adjacency, external force input, multiple scan
heads, and multichannel output remain outside this bounded package.

## Diffuse Spectral Stretch Scheduled Controls

[`c172_diffuse_spectral_stretch_scheduled_controls.talas.json`](c172_diffuse_spectral_stretch_scheduled_controls.talas.json)
drives the reset, gate, and level controls of the deterministic
monophonic Diffuse Spectral Stretch with direct, sample-accurate Event Value
schedules and one immutable repository-original 8192-sample harmonic table.

```bash
./build/core-debug/talas_cli render \
  --project examples/c172_diffuse_spectral_stretch_scheduled_controls.talas.json \
  --frames 12000 --sample-rate 48000 --max-block-frames 7 --format json
```

The first 4095 frames expose the fixed startup latency; later level changes,
gate restart, deterministic seed, smear, and diffusion demonstrate stable
partition-independent spectral output. Disk streaming, live input, dynamic
spectral configuration, multichannel output, and alternate FFT profiles remain
outside this bounded package.

## Dual Morph Oscillator Scheduled Controls

[`c173_dual_morph_oscillator_scheduled_controls.talas.json`](c173_dual_morph_oscillator_scheduled_controls.talas.json)
routes both outputs of the deterministic Dual Morph Oscillator to the
terminal stereo pair while six direct, sample-accurate Event Value schedules
drive frequency, ratio, phase modulation, morph, fold, and amplitude.

```bash
./build/core-debug/talas_cli render \
  --project examples/c173_dual_morph_oscillator_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

The compact timeline crosses sine/triangle morph, fold, phase-modulation,
ratio, amplitude, and frequency boundaries while preserving two-output
partition identity. Band limiting, oversampling, hard sync, wavetables, unison,
and polyphony remain outside this bounded package.

## Logic Scheduled Fallbacks

[`c174_logic_scheduled_fallbacks.talas.json`](c174_logic_scheduled_fallbacks.talas.json)
uses static XOR with two direct, sample-accurate Event Value fallbacks. Its
exact binary gate controls a deterministic oscillator through a VCA.

```bash
./build/core-debug/talas_cli render \
  --project examples/c174_logic_scheduled_fallbacks.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

The compact schedule crosses all XOR truth combinations and yields identical
output for callback blocks 1, 7, and 64. Dynamic operation choice, alternate
truth thresholds, coercion, and edge-triggered state remain outside this
bounded package.

## Annotation Pass-Through

[`c175_annotation_passthrough.talas.json`](c175_annotation_passthrough.talas.json)
routes a deterministic mono oscillator through the exact built-in Annotation
node. Annotation copies its admitted audio input unchanged, introduces no state
or latency, and creates no realtime analysis capture.

```bash
./build/core-debug/talas_cli render \
  --project examples/c175_annotation_passthrough.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

The output is byte-identical for callback blocks 1, 7, and 64. Annotation text,
rich formatting, virtual-control-surface layout, interaction, and multichannel
audio remain outside this bounded package.

## Filter Scheduled Controls

[`c191_filter_scheduled_controls.talas.json`](c191_filter_scheduled_controls.talas.json)
routes a deterministic oscillator through the exact mono, stateful Filter
kernel. Sample-accurate Event Values schedule frequency, Q, scale, and feedback
while the static low-pass topology retains four cascaded stages.

```bash
./build/core-debug/talas_cli render \
  --project examples/c191_filter_scheduled_controls.talas.json \
  --frames 64 --sample-rate 48000 --max-block-frames 7 --format json
```

The output is byte-identical for callback blocks 1, 7, and 64. Dynamic filter
type or order, stateful or variable-backed controls, multichannel filtering,
and oversampling remain outside this bounded package.

## Other focused examples

- [`oscillator.talas.json`](oscillator.talas.json) is the smallest renderable graph.
- [`sample_text_table.talas.json`](sample_text_table.talas.json) demonstrates an
  explicit numeric table binding.
- [`reusable_feedback_help.talas.json`](reusable_feedback_help.talas.json) shows two
  reusable Sound instances with exposed overrides and scope-local named
  delayed feedback. It is an older format-9 migration fixture by design.

All M3 and M4 graph coordinates, parameters, prose, and fixture bytes are
original Talas work. No third-party content is redistributed through these
examples.
