# Project-format migrations

Talas's portable `talas.project` migration boundary accepts source versions
1 through 156. A successful load upgrades the in-memory document to version 156;
the canonical writer always saves version 156. Version 14 adds explicit MIDI
CC source authority: older channel/controller mappings migrate to visible
`legacyAnyInput` compatibility records, while new exact mappings retain the
bounded hardware input identifier. Version 15 adds one closed MIDI Clock
transport source: older documents migrate to Internal, while exact-input
authority must name one enabled bounded hardware identifier.
Version 16 adds one closed integer musical-time map inside the Timeline. Older
documents migrate to a single 120 BPM and 4/4 anchor at tick zero; current
documents require 960 PPQ plus bounded, strictly ordered tempo and meter maps.
Version 17 replaces the standalone MIDI Clock field with one closed transport
sync authority: Internal, exact-input MIDI Clock, or exact-input MTC. Version
15/16 exact MIDI Clock settings migrate to the corresponding MIDI Clock mode;
no legacy source invents MTC authority.
Version 18 adds disabled-by-default local-loopback OSC ingress and optional
exact OSC addresses on Virtual Controls. Versions 1 through 17 migrate with
OSC disabled on canonical port 9000 and no invented mappings or network I/O.
Version 19 adds disabled-by-default local-loopback OSC state-feedback egress
and optional exact output addresses on Virtual Controls. Versions 1 through 18
migrate with feedback disabled on canonical destination port 9001, no invented
output mappings, and no network I/O. Enabled ingress and feedback ports must
differ.
Version 20 upgrades portable presentation to schema 2. It stores target-keyed
Performance controls, bounded macro modulation slots, and master-output meters
separately from Structure topology. Versions 1 through 19 migrate every Event
Value presentation to one stable target/control identity, retain its position
and visibility, derive the visual kind from the Virtual Control, and invent no
optional Sound-parameter controls, modulation routes, or meters.
Version 21 adds one bounded Score inside the Timeline. Versions 1 through 20
migrate to an empty score; current documents persist closed track routing,
clips, automation lanes, and markers without inventing score content.
Version 22 adds typed partial-spectrum assets plus nondestructive edit recipes
bound to the exact source asset generation and SHA-256 hash. Versions 1 through
21 migrate with no partial-spectrum assets or edit recipes; current documents
persist only bounded, canonical edit sets and never rewrite source analysis.
Version 23 adds one required closed project expression language: `legacyV1` or
`pureV2`. Versions 1 through 22 migrate losslessly to `legacyV1` without
reparsing, rewriting, or inventing expression source. Version 24 adds the
closed `pureV3` language while continuing to accept and preserve exact
`legacyV1` and `pureV2` authority. New documents default to `pureV3`; the
canonical writer persists the exact dialect so a saved migrated project
reopens with unchanged semantics. Version 25 adds the closed `pureV4`
language and a required bounded `expressionDefinitions` library. Versions 1
through 24 migrate with an empty library and preserve their exact prior
language authority. Version 26 adds the closed `sessionV1` language for bounded
prepared-instance state operators. Format 25 remains `pureV4`; formats 1
through 25 preserve their exact prior language authority. New documents default
to `sessionV1`. Version 27 adds the closed `variablesV1` language and a required
bounded version-1 `variableLibrary`. Formats 1 through 26 preserve their exact
prior expression authority and acquire only an empty migrated variable library;
no historical project silently acquires variable references or live variable
state. New documents default to `variablesV1`.
Version 28 advances the required variable library to schema version 2 and adds
closed `midiVoice` and `workspaceSession` scalar owner identities. Format 27
libraries migrate from schema version 1 without changing their declarations;
formats 1 through 26 acquire the same empty schema-version-2 in-memory library.
Version 29 adds the required bounded version-1 `soundVariableLibrary` with
exact reusable-Sound candidate identities and project, Sound-definition,
MIDI-voice, or workspace-session ownership. Formats 1 through 28 migrate with
an empty Sound-variable library and invent no selected Sound state.
Version 30 adds the required bounded version-1 `generatorLibrary`. It stores
exact generator identity, explicit terminal declarations, canonical grammar
source, deterministic unsigned-64 seed, and fixed rewrite/output limits.
Formats 1 through 29 migrate with an empty generator library and invent no
generator program or generated output.
Version 31 advances the generator library to schema version 2 and adds one
bounded output profile per generator. Exact terminal bindings may author
Timeline Event Values or Score clips, automation points, and markers against
existing document targets. Format 30 generators migrate losslessly as
preview-only programs with empty output bindings; formats 1 through 29 remain
empty and no historical document invents generated Timeline or Score content.
Version 32 adds the closed generator `graphNode` output action. It stores only
one bounded builtin node type ID for the restricted linear reusable-Sound
constructor. Formats 1 through 31 migrate without graph actions and invent no
nodes, connections, Sound definitions, or catalog authority.
Version 33 adds an optional bounded exact root-graph Sound `sourceInstanceId`
and an optional closed Score clip render-artifact record. The source identity
must resolve to an instance of the clip's exact Sound definition and is needed
to materialize a Sound with external boundary inputs. Null remains a valid,
visibly unavailable compatibility state for such migrated clips. The artifact binds `cached` or
`frozen` mode, a canonical source fingerprint, exact integral sample
rate/frame count, and one verified mono PCM16-WAV Project Audio asset per clip
channel. Formats 1 through 32 migrate with neither source identity nor render
artifact; migration never invents root-source, cache/freeze, or asset authority.
Version 34 adds one required bounded version-1 `liveSetLibrary`. It stores
closed 8x16 compiled-grid definitions, exact Sound/Snapshot/ranged-Score/Stop
cell targets, source-aware MIDI Program Change and OSC mappings, and bounded
finite-state tool definitions over the approved command vocabulary. Formats 1
through 33 migrate with an empty library; migration never invents live cells,
mappings, tool state, or executable behavior.
Version 35 adds the frozen `variablesV2` expression language and its exact
checked binary `rem:` operator. Formats 1 through 34 preserve their selected
language and expression bytes exactly; formats 35 and later may name `variablesV2`.
New documents default to `variablesV2`, while upgrading a `variablesV1`
project remains an explicit, audited, undoable transaction.

Version 36 adds the opt-in `voiceV1` language and the exact `?VoiceNumber`
preparation token. Earlier languages and authored expression bytes retain their
meaning; new documents still default to `variablesV2`. Upgrading VariablesV2
is explicit, audited and undoable. Voice templates are serialized as source,
never as per-instance literals. The compiler binds one-based voice ordinals
before preparing each voice; reusable Sound definitions defer ownership until
expansion, while concrete root parameters require a MIDI Voice input graph.
Formats before 36 reject `voiceV1`.

Version 37 adds the required `sharingMemoryWriters` boolean to every closed
Score track record. Formats 21 through 36 migrate with the option off, so
separate Sounds keep private recording storage unless a track explicitly opts
in. The option is part of the prepared Score routing binding. Shared offline
clip materialization replays a completed earlier recording on the same track;
overlapping reads, cross-track transfer and realtime capture remain outside
this bounded implementation.
Version 37 also admits the explicit `variablesV3` language for millisecond-clock
`ramp:` expressions. New projects still default to `variablesV2`; older
projects retain their exact language. Workbench offers a separate audited,
undoable VariablesV2-to-VariablesV3 upgrade alongside VoiceV1.

Version 38 adds MIDIMapper's persisted `voiceModel` choice. Newly authored
Mappers default to `polyphonicInput`: an accepted live MIDI note activates one
independent copy of the exclusive Input graph, up to Polyphony, with local Map
associations projected into every voice. Formats 1 through 37 acquire an
explicit `legacyPassthrough` choice in root and reusable-Sound graphs before
expression audit. This keeps their blank/local Map and gate/velocity PCM
unchanged; older formats cannot declare the new choice.
Version 38 also admits the explicit `pureV5` expression profile for bounded
`intoXArray` mapping. Earlier formats retain their selected language and reject
that syntax; importing a format-38 `pureV5` document preserves the exact
profile. Newly authored projects still default to `variablesV2`.

Version 39 gives newly authored AnalogSequencers zero-based StartIndex and
EndIndex. Formats 1 through 38 acquire an explicit `indexOrigin` choice of
`legacyOneBased` in root and reusable-Sound graphs. Their parameter expressions,
exposed defaults and instance overrides retain their original text and meaning.
Current projects default to `zeroBased`; saving a migrated project preserves
its explicit origin. Earlier formats reject the new choice instead of silently
accepting conflicting index conventions.

Version 40 admits the explicit `variablesV4` expression profile for units after
scalar expressions, such as `!Interval nn` and `(!Length * 2) ms`. Formats 1–39
retain their selected language and authored expression bytes; earlier formats
reject the new profile. The default remains `variablesV2`. The ProjectEditor
can audit and upgrade VariablesV2 or VariablesV3 as one undoable operation, and
Workbench offers the VariablesV3-to-VariablesV4 action in Inspector. Saving and
reopening preserves the exact profile and postfix expression text.

Version 41 adds the `stepZeroMode` choice to AnalogSequencer. New key-mode
sequences default to `waitForTrigger`: constant Step 0 waits for a trigger,
while constant Step 1 uses Durations and DutyCycles to generate KeyDown events.
Formats 1 through 40 acquire `legacyAutomatic` on every root and reusable-Sound
AnalogSequencer before expression audit. This keeps their prior Step-zero
automatic behavior while preserving Step expressions, exposed defaults and
instance overrides. Formats before 41 reject an authored `stepZeroMode`; saving
a migrated project keeps its explicit compatibility choice. The choice applies
to key mode; amplitude mode retains its existing local behavior.

Version 42 admits the `valV1` expression language, which reads fields
with Val message syntax and precedence. Formats before 42 reject
it; no project changes language when it is migrated.

Version 43 replaces the two-input Mixer (`talas.mixer`, in1/in2 weighted by
gain1/gain2) with the many-input Mixer (`talas.mixerV2`): one Inputs port that adds any
number of connections, and Left and Right output levels. Opening an older
project connects each input straight to Inputs when its gain is a plain unit
gain; any other gain moves to a VCA inserted on that input, together with the
gain's connection, Sound-parameter bindings and snapshot targets. A snapshot
bank scoped to the Mixer itself keeps its old targets. Readers of `audioOut`
read `leftOut`, which carries the same mono sum. Formats before 43 reject
`talas.mixerV2`. The two-input Mixer remains loadable in current projects but is
no longer offered in the prototype palette.

Version 44 measures TunableVocoder spacing in hz between linear centers, or nn (half steps) between logarithmic
ones, recorded by the new `spacingUnits` choice `hzOrNn`. Before, spacing was
a multiple of the base frequency, or octaves. Formats 1 through 43 acquire an
explicit `legacyRelative` choice, and a spacing they left at its old default
gains the explicit value 1, so they play the same samples. Formats before 44
reject `spacingUnits`.

Version 45 lets TimeControl and TimeStopper pass their Input through: Rate paces the Input's events rather than
its sample rate, and the Input keeps playing while time is stopped. Before, a
TimeControl replayed its Input at Rate from a history and a TimeStopper held
its Input's sample until Resume. Formats 1 through 44 acquire the explicit
choices `rateChanges: legacyPlaybackSpeed` and `whileStopped: legacyHold`, so
they play the same samples; formats before 45 reject both parameters.

Version 46 lets an OverlappingMixer play its Inputs one after another, each starting OverlapTime before the one
before it ends; an Input lasts as long as the SetDuration that ends it, and
one without a duration plays on, so no later Input starts. The new
`sequence` choice records it as `oneAfterAnother`. Formats 1 through 45
acquire `legacyAllAtOnce`, which keeps mixing every Input at once; formats
before 46 reject the parameter.

Version 47 changes the FrequencyTracker's and FrequencyScale's units: the tracker's estimate is a fraction of half
the sample rate, 0 until it has one, and a FrequencyScale reads its
FreqTracker that way. The new choices `outputUnits` and `trackerUnits` record
it as `fractionOfNyquist`. Formats 1 through 46 acquire `legacyHertz`, which
keeps the estimate in hz; formats before 47 reject both parameters.

Version 48 gives ArcTan the four-quadrant arctangent of its Input's right
channel over its left: `value` and the new
`rightValue` are the two channels, a mono Input feeds both, and the angle is
a fraction of a half turn, within -1 to 1. The new `angle` choice
records it as `rightOverLeft`. Formats 1 through 47 acquire `legacyRadians`,
which keeps the arctangent of `value` in radians; formats before 48 reject
the parameter and a connection to `rightValue`.

Version 49 lets a FrequencyTracker's Scale attenuate its Input instead of multiplying its estimate; the estimate no longer
depends on Scale, and a Scale of 0 leaves the tracker nothing to hear. The
new `scaleAttenuates` choice records it as `input`. Formats 1 through 48
acquire `legacyEstimate`, which keeps scaling the estimate; formats before 49
reject the parameter.

Version 50 lets a GrainCloud's grain last GrainDur plus CyclesPerGrain cycles
of its own frequency; before, it lasted GrainDur,
or the cycles when GrainDur was 0, and both together had to agree. The new
`grainDuration` choice records it as `durPlusCycles`. Formats 1 through 49
acquire `legacyDurOrCycles`; formats before 50 reject the parameter.

Version 51 lets a TimeOffset with Reverse or Retrograde, but not both, put
its silence after the Input, so the Input plays
at once; with both, the silence comes first again. Before, Reverse played the
Input backwards in windows of the offset. The new `reverseMeans` choice
records it as `silenceFollows`. Formats 1 through 50 acquire
`legacyReverseWindows`; formats before 51 reject the parameter.

Version 52 lets a FrequencyScale keep its Input's formants where they are,
as grains of the Input do; before, the
formants moved with the pitch. The new `formants` choice records it as
`stay`. Formats 1 through 51 acquire `legacyMove`; formats before 52 reject
the parameter.

Version 53 lets a SpectrumTrackSelector give its track's frequency as a
fraction of half the sample rate, as spectra carry it; before, it gave hz. The new `freqUnits` choice
records it as `fractionOfNyquist`. Formats 1 through 52 acquire
`legacyHertz`; formats before 53 reject the parameter.

Version 54 lets an AnalysisFilter average its Input, demodulated by the
harmonic, over one period of the Fundamental:
every other harmonic cancels, and the filters at every harmonic add up to the
Input. Before, it was a bandpass that passed a good part of the neighbouring
harmonics. The new `design` choice records it as `harmonicWindow`. Formats 1
through 53 acquire `legacyBandpass`; formats before 54 reject the parameter.

Version 55 lets a SpectralShape read its Wavetable as the shape of the
spectrum from 0 hz up to half the sample rate:
each partial is as loud as the table at its frequency, and a partial at or
above half the sample rate is silent. Before, partial n took the table's nth
sample. The new `envelope` choice records it as `frequency`. Formats 1
through 54 acquire `legacyPartial`; formats before 55 reject the parameter.

Version 56 lets a DiskPlayer's Duration be the Sound's whole life, within
which a Trigger restarts the file and plays it to its end; an OverlappingMixer knows a DiskPlayer's length from it.
Before, Duration bounded each triggered playback. The new `durationBounds`
choice records it as `sound`. Formats 1 through 55 acquire
`legacyPlayback`; formats before 56 reject the parameter.

Version 57 lets a LiveSpectralAnalysis give one track per band up to half
the sample rate, at most 512 of them: a track
follows the peak in its band and is silent otherwise, and LowFreq, HighFreq
and InitFreq no longer bound it. Before, it kept the 64 strongest peaks
between LowFreq and HighFreq (2000 hz by default). The new `tracks` choice
records it as `bands`. Formats 1 through 56 acquire `legacyPeaks`; formats
before 57 reject the parameter.

Version 58 makes the Bandwidth of a FilterBank and a FilterBank-Element
a 0 to 1 control: each filter is that
fraction of its spacing from the nearer neighbouring partial wide, and 0
passes nothing. Before, it was in hertz, 0 taking the Spectrum's own
bandwidths. The new `bandwidthUnits` choice records it as `spacing`.
Formats 1 through 57 acquire `legacyHertz`; formats before 58 reject the
parameter.

Version 59 lets a CloudBank and a CloudBank-Element start a grain whenever
the Trigger is above 0 and one of their grains is free, on the next track
in ascending order for Seed 0 or drawn from the Seed otherwise, reading
FreqMod, GrainDurMod and Pan as it starts;
the CloudBank keeps NbrTracks times GrainsPerTrack grains, the Element
MaxGrains. Before, every track repeated grains regularly, GrainsPerTrack of
them overlapping, the controls changed sounding grains, and BankSize
limited the tracks. The new `grainStarts` choice records it as `trigger`.
Formats 1 through 58 acquire `legacyRegular`; formats before 59 reject the
parameter.

Version 60 lets a TwoFormantVoiceElement sing a glottal pulse with a
vibrato whose rate its Seed chooses, through the TwoFormantElement's two
formant sections. Before, a sine went
through two bandpass sections. The new `excitation` choice records it as
`glottalPulse`. Formats 1 through 59 acquire `legacySine`; formats before
60 reject the parameter.

Version 61 makes a SpectrumModifier's FreqOffset without units a fraction
of half the sample rate, as spectra measure frequency: 0.01 adds 240 hz at 48 kHz. Before, it added that many
hertz. A FreqOffset with units still adds them. The new `freqOffsetUnits`
choice records it as `fractionOfNyquist`. Formats 1 through 60 acquire
`legacyHertz`; formats before 61 reject the parameter.

Version 62 lets an FFT take a stereo format (ADR-0008 amendment): it transforms a left and a right signal, and
each frame holds the left signal's spectrum and then the right's, with real
parts on the left channel and imaginary parts on the right. The new
`packing` choice records it as `talasStereo`. Formats 1 through 61 acquire
`legacyPacked`; formats before 62 reject the parameter.

Version 63 makes an REResonator the time-varying all-pole lattice filter its
RE table describes, stepping through the table's coefficient sets as
TimeIndex goes from -1 to 1 (ADR-0022). Before, it was a static FIR filter.
The new `filterForm` choice records it as `allPoleLattice`. Formats 1
through 62 acquire `legacyFir`; formats before 63 reject the parameter.

Version 64 lets a TimeOffset with a fixed SilentTime start its Input late
(ADR-0023), so a live Input plays live once
started. Before, it delayed the Input's samples. The new `offsetMeans`
choice records it as `startsInputLate`. Formats 1 through 63 acquire
`legacyDelay`; formats before 64 reject the parameter.

Version 65 makes a ContextFreeGrammar a rewrite-rule grammar, expanded
when the project compiles (ADR-0015 amendment), and drops the placeholder's text Inputs field, which held
nothing the grammar reads. Its Inputs are now the Sounds connected to it.

Version 66 lets a SetDuration with a fixed, positive StartTime start its
Input late (ADR-0023 amendment), as a
TimeOffset does. Before, it delayed the Input's samples. The new
`startTimeMeans` choice records it as `startsInputLate`. Formats 1 through
65 acquire `legacyDelay`; formats before 66 reject the parameter.

Version 67 lets a MIDIOutputController send its Value as an amplitude, a
7-bit controller value at most once a millisecond,
and a MIDIOutputEvent send a Frequency between keys as the nearest key and a
pitch bend for a one-octave bend range. Before, the Value was the
controller value itself and the Frequency went out as the nearest key alone.
The new `valueMeans` and `offKeyPitch` choices record them as `amplitude`
and `pitchBend`. Formats 1 through 66 acquire `legacyControllerValue` and
`legacyNearestNote`; formats before 67 reject the parameters.

Version 68 records the project's timecode frame rate,
`timecodeFramesPerSecond` (24, 25 or 30), which converts Val times
written hh:mm:ss.ff SMPTE to seconds, written
only when it is not 30. Formats 1 through 67 acquire 30; formats before 68
reject the field.

Version 69 lets a ReverbSection's SmoothDelayChanges glide its delay toward
a new value, as a DelayWithFeedback's does (ADR-0025). Before, it had no effect. Formats 1 through 68 acquire
`smoothDelayChanges` off.

Version 70 lets a HarmonicResonator without Prezero remember its state over
a restart, as a physical resonator would.
Before, a restart always cleared it. Formats 1 through 69 acquire `prezero`
on.

Version 71 lets a Sample loop at the loop stored in its sample file's header
while Gate is positive, as SetLoop's does.
Before, it ignored that loop. Formats 1 through 70 acquire `headerLoop`
`legacyIgnore`. A Triggered Table Read now loops at its table's header loop
unless IgnoreLoops is checked, and otherwise plays its table once;
before, it looped its whole table unless IgnoreLoops was checked. Formats 1
through 70 acquire its `headerLoop` `legacyWholeTable`. A Graphic EQ's lowest
band now shelves from DC up to about 4 f and its highest from about 9 f up to
half the sample rate; before, both were octave bandpasses at their
centers. Formats 1 through 70 acquire `outerBands` `legacyBandPass`.

Version 72 lets a Harmonic Resonator resonate at its Frequency and harmonics, reading its delay line between samples through an allpass. Before,
its period was rounded to whole samples. Formats 1 through 71 acquire
`tuning` `legacyRounded`.

Version 73 lets a Multisegment Envelope repeat its loop only while Gate is on
and then play on through its last segments; before, the loop repeated
until the next trigger. Formats 1 through 72 acquire `loopRelease`
`legacyNever`. A Multisample and a Key Mapped Multisample now loop at the loop
in the file's header, and acquire `headerLoop` `legacyIgnore`.
A Delay With Feedback's Mix now defaults to 1, so a Comb is heard only after
the first delay; one from formats 1 through 72 that leaves Mix out
acquires `mix` 0.5.

Version 74 lets a Grain Cloud's and a Sample Cloud's grains keep the
frequency, start point, duration and pan they start with; before, a grain followed them every frame. Formats 1 through 73
acquire `grainShapes` `legacyEveryFrame`.

Version 75 lets an Analog Sequencer without Pitches or KeyDowns step through
its stages each time Step becomes positive, after the stage before has lasted
its Duration, with a constant Step of 1 leaving the Durations to time them; before, Step numbered the stage to hold, and 0 followed the
Durations. Formats 1 through 74 acquire `stepMode` `legacyStageNumber`. A
Waveshaper without interpolation now reads the entry at the integer part of
its index and older ones acquire `integerIndex` `legacyRound`. A Live
Spectral Analysis now takes the power of two nearest the sample rate over
LowestAnalyzedFreq, so 1 F through 5 F give 512, 256, 128, 64 and 32 partials; older ones acquire `bandWidth` `legacyNextPower`, the next larger
power of two and a partial fewer.

Version 76 lets an Analog Sequencer stage with a DutyCycle of 1 keep its key
down into the next stage, whose note takes over its voice without a new attack;
a rest, the Gate or the end of a sequence that does not loop lets it go.
Before, every stage released its key and the next one retriggered it. Formats
1 through 75 acquire `dutyCycleOne` `legacyRetriggers`.

Version 77 lets a Multisample whose Event Value Gate falls play on through
its file and stop at its end, as one with a connected Gate does and as a
Sample does. Before, it stopped when the Gate fell. Formats 1
through 76 acquire `gateRelease` `legacyStops`.

Version 78 lets an OverlappingMixer play any number of Inputs, one after
another in the order they connect to its new many-connection `inputs` port. Formats 1 through 77 keep their Inputs on `in1` to `in4`, which
still play first, and their list of those ports, the text field `inputs`,
becomes `legacyInputs` so that the port may take the name.

Version 79 lets a Vocoder's LoCutoff, HiCutoff and Rolloff shape its Input as
a weak tone control: each carrier filter is weighted by edges at the
two cutoffs, as steep as Rolloff says, 1 precipitous, and a LoCutoff of 0 hz
cuts nothing. A TunableVocoder's Rolloff now sets how far its Tone tilts the
filters, 0 weighting them alike. Before, both tilted the filters by
Rolloff whatever the cutoffs. Formats 1 through 78 acquire `inputShaping` and
`rolloffShape` `legacyTilt`.

Version 80 makes a Filter's Order its number of poles: a
LowPass or HighPass is Order one-pole sections, which Q leaves alone, and an
AllPass pairs its poles into two-pole sections with Q, so that it shifts the
phase by -90 * Order degrees at Frequency. Scale is the gain on the Input,
and Feedback goes around the whole cascade, subtracted for a LowPass, where
a Feedback close to 1 rings. Before, each Order was a two-pole section with
Q, Scale was on the output and Feedback came a sample late. Formats 1
through 79 acquire `poles` `legacyTwoPerOrder`.

Version 81 keeps a MIDIVoice's or MIDIMapper's notes its own unless Shared
is checked: of the unshared Sounds on a channel and pitch range,
the first to take a new note keeps it, while a shared one takes every note
in its range. Before, every matching Sound took each note, and Shared was
refused. Formats 1 through 80 acquire `unsharedNotes` `legacyToEach`.

Version 82 maps every sample file in a KeyMappedMultisample's FirstSample
folder, in the order of their names, by OnePerHalfStep,
EquallySpaced, ByBasePitch (Velocity choosing among samples of one pitch) or
ByPitchRange (the header's key range, a later name first). Before,
FirstSample played alone, ByPitchRange only between LoFreq and HiFreq.
Formats 1 through 81 acquire `samples` `legacyFirstSampleOnly`.

Version 83 has a FrequencyScale granulate its Input: grains two
periods of the FreqTracker's frequency long, shaped by the Window (Hann when
none is named), are laid a period over FrequencyScale apart, from 1/64
through 64, and the Input is late by the Delay alone. Before, a phase
vocoder shifted it from 0.5 through 2, 1023 frames later than the Delay.
Formats 1 through 82 acquire `scaling` `legacyVocoder`.

WorkspaceSession remains a separate local format. Its schema version 13 adds
definition-fingerprint-bound current Live Tool state and Live Grid selection
presentation. Project migration never reads or writes that file. Reopen and
recovery reconcile compatible local Tool state but clear active and queued
cell identity, so migrating or opening a project cannot auto-start execution.

Version 84 has an Oscillator read only the first 4096 entries of a longer
wavetable; a shorter table stays one cycle. Before, the whole
table was one cycle. Formats 1 through 83 acquire `tableLength`
`legacyWholeTable`.

Version 85 has a FrequencyScale read its Delay once, at the start:
an Event Value or a function of time in it keeps its starting value, in
both renderers. Before, offline rendering followed a Delay that changed
and realtime refused it. Formats 1 through 84 acquire `delayRead`
`legacyFollows`.

Version 86 gives table fields the system wavetables
as defaults, by kind: Sine for the waveforms of OscillatorBank,
SOSOscillators, GrainCloud, CloudBank, CloudBankElement and
TriggeredTableRead; Hann for the envelopes and windows of GrainCloud,
CloudBank, CloudBankElement, SampleCloud, Chopper, FunctionGenerator and
SpectralShape; LinearEnvelope for FormantBank's and FormantBankOscillator's
impulses; FullRamp for the shaping functions of Waveshaper,
IteratedWaveshaper and MultiplyingWaveshaper. Before, they were empty.
Formats 1 through 85 that leave such a field out acquire an empty table.

Version 87 has a TimeOffset or SetDuration start its Input late in a
flat MIDIVoice too, counted from the MIDIVoice's start,
and read a SilentTime or StartTime fed by a Sound or that moves once, as
the step starts (ADR-0023, 2026-10-10); a moving Rate or a TimeStopper
paces them (ADR-0019). Before, such a TimeOffset or SetDuration delayed
its Input's samples. Formats 64 (TimeOffset) or 66 (SetDuration) through
86 acquire `offsetMeans` or `startTimeMeans` `legacyFixedStartsLate`.

Version 88 has an ADSR or AR whose Gate is negative, as !KeyDown is for
5 ms at a note-on, run its release in at most 2 ms, so it
decays rapidly to zero and attacks from zero at the +1. Before, it
released over ReleaseTime. A MultisegmentEnvelope or GraphicalEnvelope,
which has no release, falls from where it is to zero in 2 ms and starts
again from its first breakpoint at the +1; before, it played on. Formats
1 through 87 acquire `negativeGate` `legacyReleases`.

Version 89 has a TimeOffset or SetDuration in a Sound with nested MIDIVoices
start its Input late too, counted from where its own MIDIVoice starts
(ADR-0023), lets a TimeControl or TimeStopper around a MIDIVoice pace the
timing in its voices, and divides a SilentTime or StartTime read from a
Sound by the fixed Rates of the TimeControls around it, as compiling
divides one written as an expression (ADR-0019). Before, such steps
delayed their Input's samples, counted real time and read a Sound in
real time. Formats 87 and 88 acquire
`offsetMeans` or `startTimeMeans` `legacyFlatVoicesOnly`.

Version 90 has a SoundCollectionVariable in a field that takes more than
one Sound, such as a Mixer's Inputs, stand for all the Sounds of its
collection, and refuses it
anywhere else; before, it played the one Sound its declaration selected.
Formats 1 through 89 acquire `members` `legacySelected`.

Version 91 reads a constant field of a Sound inside a MIDIVoice, MIDIMapper
or AnalogSequencer as each voice starts, with that voice's Event Values, the
voice's node set up afresh (owner decision 2026-10-12; ADR-0004 amendment);
before, it was read once, as the Sound compiled. Formats 1 through 90 acquire
`constantFieldsRead` `legacyWhenCompiled` on their MIDIVoices, MIDIMappers
and AnalogSequencers.

Version 92 changes five behaviours
(owner decision 2026-10-18, as recommended): HotPink noise's largest
changes come once a period of Frequency, so its spectrum peaks there and
falls above it; a Filter's negative Feedback is the positive one 180
degrees out of phase; a SpectrumFrequencyScale moves the pitch and
leaves the formants, each partial taking the spectrum's amplitude at its new
frequency; a TimeFrequencyScale refuses a fixed Rate above 1 and
plays a moving one at 1; and a VocoderChannelBank scales its
filters by the spectra's NbrPartials, so a cascade plays as one bank. Formats 1 through 91 acquire Noise `hotPink`
`legacySmoothedSteps`, Filter `negativeFeedback` `legacyTurnsSignOver`,
SpectrumFrequencyScale `formants` `legacyMove`, TimeFrequencyScale
`rateAboveOne` `legacyCompresses` and VocoderChannelBank `gain`
`legacyCount`, which keep what they played.

Version 93 carries an Audio pair's frequencies as fractions of half the
sample rate, as audio values are: a SpectrumInRAM's and SyntheticSpectrumFromArray's
`frequencyOut`, a SyntheticSpectrumFromSounds's FrequenciesAndBandwidths and
the paired frequency inputs of OscillatorBank, FormantBank,
FormantBankOscillator and FrameSynchronizer. Formats 1 through 92 acquire
`pairFrequencies` `legacyHertz` on those seven node types, which keeps hertz.

Version 94 makes a ScaleVocoder's Rolloff the width of its Tone control, 0 weighting the bands alike, as a
TunableVocoder's has been since version 79. Formats 1 through 93 acquire
ScaleVocoder `rolloffShape` `legacyTilt`, which keeps tilting the bands by
Tone and again by Rolloff.

Version 95 plays a stereo file's left channel on `audioOut` and its right
channel on a new `rightOut` in a Sample, Multisample, KeyMappedMultisample,
MultifileDiskPlayer and DiskPlayer. Formats 1 through 94 acquire `channels` `legacyMono` on
those five node types, which keeps what they played: a stereo table refused
where a field names it, its left channel where a list does, a streamed file's
channels averaged.

Version 96 paces a SetDuration's Duration fed by a Sound by the fixed Rates
of the TimeControls around it, each frame's value divided by them as
compiling divides a Duration expression, as its StartTime has been since
version 89 (ADR-0019 amendment 2026-10-19). Formats 1 through 95 whose
SetDuration starts its Input late (`startTimeMeans`
`startsInputLate`) acquire `legacyDurationUnpaced`, which starts it alike and
keeps the Duration in real time; the earlier choices already read a Sound in
real time.

Version 97 stores a Virtual Control's widget options from the VCS Editor: a `widget` object of `style` (`default`,
`fader`, `horizontalFader`, `smallFader` or `potentiometer`), `showNumber`,
`label`, `onColour` and `offColour` (`#rrggbb` or empty), `grid`, `taper`
(`linear` or `log`) and `tab`, written only when some option differs from
its default. Earlier formats refuse the object; their controls load with the
default options and play as before.

Version 98 stores the project's own global map as a top-level `globalMap` string of
`!Name is: `Source` associations, written only when the project has one;
without it the Default map applies, as before. Earlier formats refuse the
member.

Version 99 stores the Class Editor's fields of a Sound definition: a definition's `description`, and a parameter's
`minimum` and `maximum` (the range of its constant values), `field` (its
place, `left`, `top`, `right` and `bottom` from 0 to 1) and `comment`, each
written only when set. Earlier formats refuse them.

Version 100 changes two behaviours. A
DynamicRangeController's Ratio is in decibels; its new
`ratioLaw` choice is `decibels`, and older projects migrate to
`legacyLinear`, which keeps dividing the level past the Threshold by the
Ratio. Val's ramp, ramp:, fullRamp and fullRamp: rest at 1 until
their receiver first becomes positive; older projects migrate to
the document's `rampsStartAtFirstValue: true`, which keeps starting them at
their first value. Earlier formats refuse both.

Version 101 changes two more. A MIDIVoice's or MIDIMapper's
script note without a frequency plays its LowPitch; the new
`scriptNotePitch` choice is `lowPitch`, and older projects migrate to
`legacyNoteZero`, which keeps playing note 0. A Mixer lasts its longest
Input for the Sounds that read their Inputs' durations (the
OverlappingMixer, the cellular automata, RandomSelection, Preset and
ContextFreeGrammar); an older project holding a Mixer and one of those
migrates to the document's `mixerDurationsUnknown: true`, which keeps the
Mixer's duration unknown there. Earlier formats refuse both.

Version 102 lets the many-input Mixer (`talas.mixerV2`), as the
Two-Input Mixer at version 101, last its longest Input for the
Sounds that read their Inputs' durations; an older project holding one and
one of those Sounds migrates to the document's
`manyInputMixerDurationsUnknown: true`, which keeps its duration unknown
there. Earlier formats refuse the field.

Version 103 lets a ramp, ramp:, fullRamp or fullRamp: run on to its end
once triggered, when its receiver falls (so a six-second ramp still runs
its full six seconds though its trigger falls at once); an
older project that names a ramp migrates to the document's
`rampsHoldUntriggered: true`, which keeps holding it while its receiver
is off. Earlier formats refuse the field.

Version 104 has a GraphicalEnvelope whose Gate falls inside its loop
jump to the loop's end (the left arrow) and go on from there; an older project with a looped GraphicalEnvelope migrates to the
document's `graphicalEnvelopeLoopsPlayOut: true`, which plays the loop's
pass out first. A loopStart alone, a release point the envelope jumps to
when Gate falls, was refused before and needs no migration. Earlier
formats refuse the field.

Version 105 adds a Sound definition's Expand box,
`expandsBeforeTransformation: false`, written only when unchecked: a
ParameterTransformer then transforms an instance's own parameters before
it expands. Every older definition expands first, as before, and needs no
migration; earlier formats refuse the field.

Version 106 refuses a Val number without its leading zero, `.5` for
`0.5`, which older formats read as `0.5`. An older
Val project's parameter values and class defaults gain the zero as they
load (`.5` becomes `0.5`), leaving comments, strings and field programs as
they are; the other expression languages keep reading `.5`.

Version 107 adds a SyntheticSpectrumFromSounds's `logFrequencies` choice:
with LogScale it reads its FrequenciesAndBandwidths as pitches, 1 half the
sample rate and each 1/15 below it an octave lower,
`pitch`. Older projects migrate to `legacyLinear`, reading frequencies and
taking their pitch as before; earlier formats refuse the field.

Version 108 lets an OscillatorBank play a log spectrum's pitches, playing
a LogScale spectral source's directly,
where it read their note numbers as hertz. Older projects with a LogScale
SyntheticSpectrumFromSounds or SyntheticSpectrumFromArray, or a
SpectrumLinearToLog, migrate to the document's
`banksReadLogSpectraAsHertz`, reading them as before; earlier formats
refuse the field. The Sounds that expect a linear spectrum still need a
SpectrumLogToLinear.

Version 109 adds a Sound definition parameter's `variable` binding,
`{"target": "variable", "variableName": "freq"}`: the class's fields
holding `?freq` (or `@freq`) take the instance's value as the class expands. Older formats have no such binding and refuse it.

Version 110 gives a frequency written in bpm its beats per minute when
removeUnits follows it, `4 s bpm removeUnits` being 15, where it gave the frequency's hertz, 0.25. A Val expression
of an older project gains `hz` after each such `bpm`, `120 bpm hz
removeUnits`, keeping the number it had.

Version 111 reads a d written right after a number as the mark of a double,
as Smalltalk does: `8.1758d hz` is 8.1758 hertz,
where it was the pitch d of octave 8.1758. A Val expression of an older
project gains a space before each such d, `4 d`, keeping the pitch it had.
It also gives a frequency written in bpm and multiplied or divided by a
number its beats per minute when removeUnits follows, `(2 s bpm * 4)
removeUnits` being 120, where it gave the hertz, 2; an older
project's expression gains `hz` after such a `bpm`.

Version 112 makes each beat of a Val `bpm:` or `bpm:dutyCycle:` a
`!KeyDown`-like trigger: -1 for its first five milliseconds, stealing its
voice, then 1, where it went straight to 1.
An older project's expression puts each such message in parentheses and
takes its `abs`, `(1 bpm: 60) abs`, keeping the beats it had.

Version 113 makes a Val project's Event Value names case insensitive,
`!SnailsburyTails + !snaiLsBuryTailS` being one Event Value named as it was
first seen; it refuses two Virtual Controls whose
names differ only in case. An older project keeps names differing only in
case apart, each after the first gaining a number: `!a` and `!A` become
`!a` and `!A_2`.

Version 114 gives each Val field's random messages numbers of their
own, two fields reading `1 s random abs` drawing apart, where they drew the same numbers. An older project with a random
message keeps drawing alike through its `randomExpressionsShareSeeds`
choice.

Version 115 has a script's `Random new` give another sequence each time the
program runs, where it gave the same one. An older
project's fields write `(Random newForTalasWithSeed: 0)` for each `Random new`,
keeping the stream they had.

Version 116 has a Val `!KeyPitch` follow the pitch wheel a half step
either way, as `!KeyNumber nn + !PitchBend nn`, and `!KeyPitch12` an octave, where `!KeyPitch` was the key's number alone.
An older Val project that reads `!KeyPitch` keeps the key alone through
its `keyPitchIgnoresPitchBend` choice.

Version 117 has a Val `!Random` and `!Random1000` draw a number from -1
to 1 every millisecond, as the signal processor's sources, where each was an Event Value of its own, a fader. An older Val
project that reads either keeps its faders through its
`randomEventSourcesAreFaders` choice.

Version 118 has a Val `neg:zero:pos:` start at 0 until its receiver first
changes, as `neg:zero:pos:initially: 0` does, where it selected at once. An older Val project that
holds one keeps selecting at once through its `signSelectionsSelectAtOnce`
choice.

Version 119 has a Val time function in a branch not taken stop, carrying
on from where it left off when the branch is next taken, where it ran on. An older Val project with an expression holding
both a conditional and state keeps running on through its
`timeRunsOnInSkippedBranches` choice.

Version 120 has a MIDIVoice without a Polyphony play two voices, where it
played four. An older
project's MIDIVoice without one gains a Polyphony of 4.

Version 121 keeps a Smalltalk program's integers and ratios of them exact,
1000 factorial with all its digits and 1024/3024 as 64/189, where every number was floating point. An older project that holds
Smalltalk (a Script or file interpreter, a ParameterTransformer, a MIDI
script or a field's program) keeps floating point through its
`smalltalkNumbersAreFloats` choice.

Version 122 has every function of time count from the start of the Sound
holding it, as !LocalTime does, and makes !RealTime and !Time in Val the
seconds since the whole Sound started, where functions of time counted from the start of the whole Sound and
the two were Event Values of their own. An older project holding a function of
time keeps counting from the whole Sound's start through its
`timeFunctionsCountFromTheWholeSound` choice, and an older Val project
reading !RealTime or !Time keeps them faders through its
`timeEventSourcesAreFaders` choice.

Version 123 gives a Timeline automation lane its state: Automated, where the lane drives its
parameter; Live, where the parameter follows its fader or MIDI controller
and the lane waits; or Record, where it is armed for the next recording. A
lane's `state` is written only when it is not Automated, so an older
project's lanes, all automated, load unchanged.

Version 124 adds the Timeline's markers and anchors: a clip's `anchorOffset`, a time within it marking an event,
written only when the anchor is not at the clip's start; a marker's
`programNumber`, the MIDI program change from 1 to 128 that jumps the
cursor to it, written only when it is not the marker's number; and the
Timeline's `markerProgramChannel`, the MIDI channel whose program changes
jump to markers rather than choosing presets, written only when one is
set. An older project's clips, markers and Timeline load unchanged.

Version 125 adds the Virtual Control Surface's arrangement: a widget's `bounds`, its left, top, width and height
in pixels once the surface has been unlocked or cleaned up, and the
project's `virtualControlLayout`, `fixed` for widgets at those places or
`fit` for those places scaled to the window, written only when the
surface is no longer laid out automatically. An older project's surface
stays automatic.

Version 126 adds the layouts made in the Virtual Control Surface Editor: `virtualControlLayoutPages`, each with its
name and the Event Values it shows, written only when there are some. The
top layout and each Sound's layout are not saved; they follow the graph.

Version 127 adds a Sound track's audio input:
`inputTrackId` on a score track, written only when the track has one, names
a submix whose tracks' audio the track's Sounds take as their input. Its own
output must not come back through it. An older track has no input.

Version 128 adds a Sound track's sends:
`sendTrackIds` on a score track, written only when it has some, names the
submixes it sends to besides its output, each distinct, channel-compatible
and off its output's route. A track lane named `To_` and the submix's ID
scales what is sent there. An older track sends nowhere.

Version 129 adds an automation lane's takes:
`takeName`, written when the take driving the lane is not `take1`, and
`takes`, written when the lane has others, each with its distinct name and
its points, timed as the lane's are. An older lane has its one take, take1.

Version 130 adds slaving: `masterLaneId` on an
automation lane, written only when it is slaved, names another control of
the same Sound, a control of its track or a master control, itself not
slaved, whose function and state drive the lane's parameter.

Version 131 adds hard-wired track outputs:
`outputChannels` on a score track, written only when it has some, lists the
distinct Master channels, from 1, its audio goes to instead of its pan
controls, its channel k to the (k mod n)th of them. An older track pans.

Version 132 adds a track's MIDI input channel: `midiChannel` on a score track, from 1 to 16 and written only when set,
is the default MIDI channel of the Sounds in the track, the channel they
expect live MIDI on. An older track takes the project's default channel.

Version 133 adds a hardware input as a track's input: `inputChannel` on a Sound track, from 1 to 8 and written only when
set, is the audio input channel its Sounds take in place of their
replaceable input, instead of a submix. An older track takes a submix or
none.

Version 134 adds No output: `noOutput: true` on a
Sound track, written only when set, keeps its audio off its output route
and the Master, so it is heard only through the submixes it sends to. An
older track has an output.

Version 135 adds the Timeline's time code offset:
`timeCodeOffsetSeconds` on the timeline, written only when set, is the
starting time code of the source the Timeline follows, so incoming time
code at it is the Timeline's start. An older Timeline starts at time code
zero.

Version 136 adds a control's transformations: `transforms` on a score automation lane, written only when it has
some, lists the transformation buttons pressed on it in order, each with
its kind, value, secondary value and seed; the lane plays its points
through them without changing them. An older lane has none.

Version 137 adds the Virtual Control Surface's texts and boxes: `virtualControlDecorations`, written only
when there are some, lists each one's kind (`text` or `box`), its text
and its left, top, width and height; and a widget's `buttonFill` style, a
momentary button filled with its On colour while held. An older surface
has none.

Version 138 adds a Sound's replaceable input: an input boundary may carry `replaces: true`, naming one of the
Sound's own Sounds (not another Sound's instance or a Variable, and not
the Sound's output) by an Audio output. Left unconnected, that Sound
plays; a Sound connected to an instance's replaceable input plays in its
place wherever it fed. An older Sound has none.

Version 139 adds a Sound class's icon: a Sound definition's `icon`, written only when it has
one, names another kind of Sound whose icon it shows (`sameAs`, a
prototype's type or a Sound class), its drawn dots (`large`, 32 rows of 16
hexadecimal digits, and `small`, 16 rows of 8) and the PNG icon box
(`png`). An older Sound has none.

Version 140 adds Sound collections: `soundCollections`, written only when there are some,
lists each collection's id, name, Sounds and collections, in order, a
Sound or collection held by at most one. An older Sound file has none.

Version 141 adds a Sound class's input and output types and Nonaudio
side effects box: a class icon's
`inputType` and `outputType` (none, audio, subaudio, spectrum,
transparent, constructor, variable or environment) and `sideEffects`,
written only when chosen, shape the icon's outline. An older icon has
none.

Version 142 adds the Virtual Control Surface's dormant widgets: `dormantVirtualControls`, written only when there are
some, holds widgets pasted into the surface for Event Values it has no
control of, in the form of `virtualControls` (at most 256, none named as
a control); a control of that name added later takes its widget and
range. An older project has none.

Version 143 adds a Sound class's numbers of input and output channels: a class icon's `inputChannels` and
`outputChannels`, 1 or 2, written only when chosen, the lines drawn into
and out of the class's Sounds. An older icon has none.

Version 144 takes a Threshold's Hysteresis in full on either side of it: the Input switches it on above Threshold plus
Hysteresis and off below Threshold minus Hysteresis. An older Threshold
gains `hysteresisBand` `legacyHalf`, half of it each way as before.

Version 145 adds the kind of field a Sound class's parameter has: an exposed parameter's
`fieldKind`, `value` or `hotValue` for an expression, `string` or
`codeString` for a text, `symbolList`, `radioButtons` or
`radioButtonsTwoColumns` for a choice, written only when chosen. An older
parameter has none, its kind's default field.

Version 146 adds the Timeline's own time code frames a second, `timeCodeFramesPerSecond` 24, 25 or 30, which override
the project's in the Timeline, and a clip's `colour`, 0xRRGGBB,
each written only when set. An older Timeline takes the project's frames
and its clips their tracks' colour.

Version 147 adds three kinds of field for a class's text parameter: `complex`, a number
or a complex number `real % imaginary`; `polesAndZeroes`, a list of them;
and `grammar`, a ContextFreeGrammar's production rules, each checked as it
is entered. An older file names none of them; a file before 147 that does
is refused.

Version 148 makes a Gain clip at full scale, as fixed-point signals do: its `overflow` choice is `clip`. An older Gain
migrates to `unclipped`, boosting past 1 as before; a file before 148
naming the choice is refused.

Version 149 gives a LimeInterpreter Inputs, Sounds connected to its
`inputs` port that its Script schedules by name as a Script does, in place of a text field the runtime only
accepted blank. An older LimeInterpreter drops that field.

Version 150 makes a PhaseShiftBy90's filters saturate at full scale, as
fixed-point filters do, very sensitive to Input amplitude: its `overflow` choice is `clip`. An older
PhaseShiftBy90 migrates to `unclipped`; a file before 150 naming the
choice is refused.

Version 151 ends an ADSR's release with its Sound when its Gate is a constant 1 in a
Sound of finite duration: its sustain is the duration less the attack,
decay and release. An older ADSR migrates its
`constantGate` choice to `legacyHolds`, holding the Gate to the end; a
file before 151 naming the choice is refused.

Version 152 makes a MIDIOutputController as the prototype comes send its Amp on MIDI
channel 1 as continuous controller 7: its
ControllerNumber defaults to 7 and its Value to `!Amp`. An older one
that leaves them out keeps controller 1 and an empty Value.

Version 153 has !Time and !LocalTime count the time a TimeControl of a
fixed Rate, or a TimeStopper whose fixed Resume holds time, lets pass in
its Input. An older
project holding either keeps them at real time
(`timeEventsIgnoreTimeControls`).

Version 154 adds the Timeline's `cacheWordSize`, the word size of its disk caches and frozen clips, 16 or 24 bits, each channel a mono WAV at the current sample rate. It is written when it is 16; a project of an earlier format loads with 16, the word size its caches were written in, and a new Timeline caches in 24.

Version 155 adds `virtualControlLayoutSettings`, what each layout of the Virtual Control Surface keeps of its own: the layout's name, the snapshot bank of its presets (`presetBankId`), its arrangement (`automatic`, `fixed` or `fit`) and its widgets, each an Event Value's kind, minimum, maximum and widget options. A layout keeping none takes the surface's shared presets and widgets, so projects before format 155, which have none, show every layout as before; the member is written only once a layout keeps something of its own.

Version 155 also refuses a Script whose event puts its value against its keyword's colon, as freq:500 hz, which needs a space after the colon (a space before it was refused already): the Script's `keywordSpacing` choice is `spaceAfterColon`. An older Script whose events touch a colon migrates to `legacyTouching` and plays as before; a file before 155 naming the choice is refused.

Version 156 adds three settings of how a project's fields read, each written only when it is not the default: `roundingRule` `manualTable` takes an alternative table, where rounded adds 0.5 and truncates toward negative infinity and truncated rounds toward negative infinity (Smalltalk's rule stays the default); `fieldBeatTempoBpm`, from 1 to 1000, is the tempo a beat written in a field lasts at, apart from the Timeline's tempo map (a new project's is 60, a quarter note at 60; none, as in older projects, keeps the tempo map's); and `unitlessSOSPitch` `linear` reads a number without units given to inSOSPitch as a fraction of 22050 hz, so 11025.0 inSOSPitch is 0.5 (hertz stays the default). Projects before 156 have none of them and play as before; a file before 156 naming one is refused.

Version 156 also adds a Timeline's sharingMemoryWritersAcrossTracks, written only when on: every Sound track then shares one pool of MemoryWriter recordings, and overlapping clips that write or read one render together, writers first, so a reader and its writer on separate tracks with one start time play as in a Mixer. Older projects load with it off and play as before.

Version 156 also adds three project choices, each off by default so that a project plays as it did, and written only when on: `pastedSoundsSaturateAtUnity`, a Sound pasted into a field and read each millisecond reads within -1 and 1, as fixed-point signals do; `pastedSoundsReadThePreviousMillisecond`, a field reads each pasted Sound as it stood just before the millisecond's tick, so that a Constant pasted into a Constant pasted into another lags a millisecond more each time; and `inverseOfZeroIsInfinite`, a number other than 0 divided by 0 is infinite and a field that changes as it plays takes it as its largest or smallest legal value, a field without one, or one that does not change, refusing it, so that no infinity reaches a Sound. A file before 156 naming any of them is refused.

The checked-in machine policy is
`docs/governance/project-format-migration-policy.json`. The dedicated
compatibility test synthesizes one minimal, project-original document for each
accepted source version. Every row must parse without source-file, package,
asset, network, plugin, or reference access; preserve empty/default state;
serialize as canonical v95; reopen; and produce the same bytes on a second
serialization.

Run the focused gate locally after configuring `core-debug`:

```sh
cmake --build --preset core-debug --parallel
ctest --preset core-debug --no-tests=error --output-on-failure -L "^migration$"
```

The gate also rejects versions 0 and 157, missing/duplicate/mathematically
fractional version fields (including binary64 rounding traps), and the v11
presentation field when injected into a v10 source. The local release gate runs
this label separately before the complete core suite.

This is a compatibility contract for the explicit portable JSON boundary and
its minimal structural rows. It is not a claim that every malformed legacy
file can be recovered, nor does it cover private historical/reference files,
workspace state, package containers, component bundle schemas, external
assets, or future versions. Retain backups of source projects before migration.

For the standalone-v1 local closeout, the then-current format-1-through-34 matrix passed in
the fresh serial Debug suite and in the focused ASan/UBSan persistence matrix
against implementation checkpoint
`060faa90c8952bb6a872f3aa4c0cfeb9e4755cc1`. C55 extends that local matrix
through format 35 without changing the separate authority of arbitrary
private/reference documents, external assets, packages, WorkspaceSession,
hosted runners, or future formats.
