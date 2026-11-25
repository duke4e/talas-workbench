# Talas demo projects

Twenty-four ready-to-play projects, thirteen effects and eleven instruments, each a single
self-contained `.talas.json` file with a handful of named live controls (Event Values, shown as
`!Name`). Open one in the Workbench and move its controls, or render it with the CLI. They are
written to double as presets: every control has a label, a range and a default, and nothing
depends on files outside the project.

## In the Workbench

The demos are the **Demos** category of the Sound Library, effects first, then instruments;
typing `demo` in its search finds them too. Select one to read what it does. **Open Example**
opens an untitled copy, so saving never changes the original. An instrument's **Audition**
plays its demo phrase. An effect has no Audition there, since it plays live audio input 1 and
would be silent: open it, pick an input with **Audio Device Settings...**, and play into it.
**Open Help** names the Effects or Instruments table below.

## How they are built

- **Effects** read live audio from input channel 1 (`talas.audioInput`, mono) and write stereo.
  Offline, supply that input with `--audio-input 1=file.wav` (mono 16-bit WAV); `validate` needs
  it too.
- **Instruments** keep one voice as a reusable Sound (shown in the Sound Definitions). A
  `keys` MIDI Voice (source *Live MIDI*) plays copies of it from a MIDI keyboard; a `demo`
  MIDI Voice (source *Script*) plays the built-in phrase on its own copies when the Sound starts.
  The `!Demo` toggle sets the demo level: turn it off for keyboard-only or plugin use, or delete
  the `demoVoice` and `demo` nodes. Voices read `!KeyPitch`, `!KeyDown` and `!KeyVelocity`.
- Every project uses the `variablesV2` expression language and only built-in wavetables
  (`Saw21`, `Square21`, `Buzz`, `Triangle`, `Hann`, `ATAN20`).

## Effects

| Demo | What it does | Live controls | Test input |
| --- | --- | --- | --- |
| [Stereo Delay](stereo_delay.talas.json) | Two feedback delays, the right one longer, with a tone control on the echoes. | `!Time` Time (s): 0.05 to 1.5, default 0.375<br>`!Feedback` Feedback: 0 to 0.9, default 0.45<br>`!Spread` Spread (R/L ratio): 1 to 2, default 1.5<br>`!Tone` Tone (Hz): 500 to 16000, default 5000<br>`!Mix` Mix: 0 to 1, default 0.35 | plucked melody (`melody.wav`) |
| [Stereo Chorus](chorus.talas.json) | Two LFO-swept delays around 12 ms spread left and right. | `!Rate` Rate (Hz): 0.05 to 5, default 0.6<br>`!Depth` Depth: 0 to 1, default 0.5<br>`!Width` Width: 0 to 1, default 1<br>`!Mix` Mix: 0 to 1, default 0.5 | chords (`chord.wav`) |
| [Flanger](flanger.talas.json) | Short swept delay with feedback against the dry signal; the channels sweep apart. | `!Rate` Rate (Hz): 0.02 to 5, default 0.25<br>`!Depth` Depth: 0 to 1, default 0.7<br>`!Feedback` Feedback: -0.85 to 0.85, default 0.6<br>`!Mix` Mix: 0 to 1, default 1 | drum loop (`drums.wav`) |
| [Phaser](phaser.talas.json) | Six swept all-pass stages with feedback, summed with the dry signal. | `!Rate` Rate (Hz): 0.02 to 4, default 0.3<br>`!Depth` Depth: 0 to 1, default 0.8<br>`!Feedback` Feedback: 0 to 0.9, default 0.5<br>`!Mix` Mix: 0 to 1, default 1 | chords (`chord.wav`) |
| [Hall Reverb](hall_reverb.talas.json) | Stereo Schroeder-Moorer hall: pre-delay, damping, four combs and two all-passes per side. | `!Decay` Decay (s): 0.3 to 10, default 3<br>`!Size` Size: 0.3 to 1, default 1<br>`!Damping` Damping (Hz): 1000 to 16000, default 6000<br>`!PreDelay` Pre-delay (s): 0 to 0.2, default 0.02<br>`!Mix` Mix: 0 to 1, default 0.4 | plucked melody (`melody.wav`) |
| [Tape Saturator](tape_saturator.talas.json) | Arctangent saturation, head bump, wow and flutter, and high roll-off. | `!Drive` Drive: 1 to 20, default 6<br>`!Tone` Tone (Hz): 1500 to 18000, default 9000<br>`!Wow` Wow & flutter: 0 to 1, default 0.3<br>`!Output` Output: 0 to 1, default 0.5 | chords (`chord.wav`) |
| [Bus Compressor](bus_compressor.talas.json) | Feed-forward compressor with 2 ms look-ahead; dB threshold and makeup. | `!Threshold` Threshold (dB): -48 to 0, default -24<br>`!Ratio` Ratio: 1 to 20, default 4<br>`!Attack` Attack (ms): 0.1 to 100, default 1<br>`!Release` Release (ms): 10 to 1000, default 60<br>`!Makeup` Makeup (dB): 0 to 12, default 6 | drum loop (`drums.wav`) |
| [Auto-Filter](auto_filter.talas.json) | Resonant LP/BP/HP filter swept by an LFO; the right channel sweeps against the left. | `!Rate` Rate (Hz): 0.05 to 8, default 0.5<br>`!Depth` Depth: 0 to 1, default 0.8<br>`!Cutoff` Cutoff (Hz): 60 to 4000, default 300<br>`!Resonance` Resonance: 0 to 0.9, default 0.6<br>`!Morph` Morph (LP-BP-HP): 0 to 1, default 0 | chords (`chord.wav`) |
| [Vocoder](vocoder.talas.json) | The input's spectral envelope on a sawtooth power chord with noise (CDP formant vocoder). | `!Pitch` Carrier pitch (note): 24 to 84, default 48<br>`!Noise` Noise: 0 to 1, default 0.15<br>`!Tone` Tone (Hz): 500 to 16000, default 6000<br>`!Mix` Mix: 0 to 1, default 1 | voice-like vowels (`voice.wav`) |
| [Granular Freeze](granular_freeze.talas.json) | Grains scattered from a 2 s looping recording of the input; Freeze holds the buffer. | `!Freeze` Freeze: toggle, default off<br>`!Density` Density (grains/s): 1 to 60, default 20<br>`!GrainSize` Grain size (ms): 20 to 400, default 120<br>`!Scatter` Scatter: 0 to 1, default 0.5<br>`!Mix` Mix: 0 to 1, default 0.6 | plucked melody (`melody.wav`) |
| [Spectral Blur](spectral_blur.talas.json) | CDP phase-vocoder blur and trace: smears transients into washes. | `!Blur` Blur (frames): 1 to 32, default 16<br>`!Trace` Trace (partials): 1 to 512, default 64<br>`!Mix` Mix: 0 to 1, default 0.8 | drum loop (`drums.wav`) |
| [Frequency Shifter](frequency_shifter.talas.json) | Single-sideband shift of every partial by Shift hertz, up or down. | `!Shift` Shift (Hz): -500 to 500, default 120<br>`!Spread` Spread (Hz): 0 to 50, default 7<br>`!Mix` Mix: 0 to 1, default 1 | chords (`chord.wav`) |
| [Ring Modulator](ring_modulator.talas.json) | Multiplies the input by a sine carrier with an optional LFO wobble. | `!Frequency` Carrier (Hz): 20 to 2000, default 440<br>`!Wobble` Wobble: 0 to 1, default 0.2<br>`!WobbleRate` Wobble rate (Hz): 0.1 to 10, default 3<br>`!Mix` Mix: 0 to 1, default 1 | voice-like vowels (`voice.wav`) |

## Instruments

| Demo | What it does | Live controls | Voices |
| --- | --- | --- | --- |
| [Sub Bass](sub_bass.talas.json) | Mono saw plus sub square through a resonant low-pass with a filter envelope. | `!Cutoff` Cutoff (Hz): 40 to 4000, default 260<br>`!Resonance` Resonance: 0 to 0.9, default 0.45<br>`!EnvAmount` Envelope amount: 0 to 1, default 0.5<br>`!Decay` Decay (s): 0.02 to 2, default 0.25<br>`!Demo` Demo phrase: toggle, default on | mono |
| [Poly Pad](poly_pad.talas.json) | Eight voices of three detuned saws and a sub, slow envelope, stereo chorus. | `!Brightness` Brightness (Hz): 200 to 12000, default 2200<br>`!Detune` Detune (cents): 0 to 40, default 12<br>`!Attack` Attack (s): 0.005 to 4, default 0.8<br>`!Release` Release (s): 0.05 to 6, default 1.5<br>`!Demo` Demo phrase: toggle, default on | 8-voice |
| [FM Electric Piano](fm_electric_piano.talas.json) | Two-operator FM tine piano with a 14:1 "bark" and stereo tremolo. | `!Brightness` Brightness: 0 to 1, default 0.5<br>`!Decay` Decay (s): 0.3 to 8, default 2.5<br>`!Bark` Bark: 0 to 1, default 0.3<br>`!Tremolo` Tremolo: 0 to 1, default 0.4<br>`!Demo` Demo phrase: toggle, default on | 10-voice |
| [Drawbar Organ](drawbar_organ.talas.json) | Six sine drawbars per key and a rotary speaker that ramps between speeds. | `!Drawbar16` 16' (sub): 0 to 1, default 0.6<br>`!Drawbar8` 8' (fundamental): 0 to 1, default 1<br>`!Drawbar4` 4' (octave): 0 to 1, default 0.7<br>`!Mixture` Mixture (2 2/3', 2', 1'): 0 to 1, default 0.4<br>`!Rotary` Rotary speed: 0 to 1, default 0<br>`!Demo` Demo phrase: toggle, default on | 10-voice |
| [Morph Lead](morph_lead.talas.json) | Mono Dual Morph Oscillator lead with fold and phase modulation, low-pass and echoes. | `!Morph` Morph: 0 to 1, default 0.4<br>`!Fold` Fold: 0 to 1, default 0.25<br>`!PhaseMod` Phase mod: 0 to 2, default 0.5<br>`!Cutoff` Cutoff (Hz): 200 to 12000, default 3000<br>`!Echo` Echo: 0 to 1, default 0.3<br>`!Demo` Demo phrase: toggle, default on | mono |
| [Granular Synth](granular_synth.talas.json) | A Grain Cloud of pulse grains at each key's pitch. | `!Density` Density (grains/s): 2 to 100, default 30<br>`!GrainSize` Grain size (ms): 10 to 300, default 80<br>`!Jitter` Pitch jitter: 0 to 1, default 0.2<br>`!Spread` Stereo spread: 0 to 1, default 0.7<br>`!Demo` Demo phrase: toggle, default on | 6-voice |
| [Plucked String](plucked_string.talas.json) | Waveguide string plucked by each key; releasing the key damps it. | `!Decay` Decay (s): 0.2 to 10, default 3<br>`!Brightness` Brightness: 0 to 0.8, default 0.6<br>`!Pick` Pick position: 0.02 to 0.5, default 0.2<br>`!Stiffness` Stiffness: 0 to 1, default 0.1<br>`!Demo` Demo phrase: toggle, default on | 8-voice |
| [Drum Kit](drum_kit.talas.json) | Synthesized kick, snare, hats and pitched toms on General MIDI-style keys. | `!Tune` Tune: 0.7 to 1.5, default 1<br>`!Decay` Decay: 0.3 to 2, default 1<br>`!Snap` Snare snap: 0 to 1, default 0.6<br>`!HatTone` Hat tone (Hz): 3000 to 12000, default 7000<br>`!Demo` Demo phrase: toggle, default on | 12-voice |
| [Arpeggiator](arpeggiator.talas.json) | Each held key arpeggiates root, fifth, octave and twelfth in sixteenths. | `!Tempo` Tempo (BPM): 60 to 200, default 120<br>`!Cutoff` Cutoff (Hz): 300 to 8000, default 2500<br>`!Resonance` Resonance: 0 to 0.9, default 0.5<br>`!Echo` Echo: 0 to 1, default 0.3<br>`!Demo` Demo phrase: toggle, default on | 4-voice |
| [Terrain Drone](terrain_drone.talas.json) | Two wave-terrain oscillators on drifting orbits with slow swells and a long reverb. | `!Motion` Motion: 0.1 to 4, default 1<br>`!Depth` Depth: 0 to 1, default 0.6<br>`!Shape` Shape (y ratio): 0.5 to 4, default 1.5<br>`!Space` Space: 0 to 1, default 0.5<br>`!Demo` Demo phrase: toggle, default on | 6-voice |
| [Modal Mallets](modal_mallets.talas.json) | Four decaying sine modes from marimba towards metal bar ratios, plus a mallet strike. | `!Decay` Decay (s): 0.2 to 8, default 1.6<br>`!Brightness` Brightness: 0 to 1, default 0.6<br>`!Metal` Metal: 0 to 1, default 0.2<br>`!Hardness` Mallet hardness: 0 to 1, default 0.5<br>`!Demo` Demo phrase: toggle, default on | 12-voice |

## Rendering

Run the commands from the repository root. Effects need a mono input file; this script writes
the four used below (6 s each, 48 kHz, peaks at -6 dBFS) into a scratch directory:

```bash
IN=/tmp/talas-demos && mkdir -p $IN && (cd $IN && python3 - <<'PY'
import math, random, struct, sys, wave

SR, N = 48000, 6 * 48000
random.seed(7)


def save(name, x, peak=0.5):
    g = peak / max(abs(v) for v in x)
    with wave.open(name, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b''.join(struct.pack('<h', int(v * g * 32767)) for v in x))


def tone(nn, t, partials=1):
    f = 440 * 2 ** ((nn - 69) / 12)
    return sum(math.sin(2 * math.pi * f * k * t) / k for k in range(1, partials + 1))


drums = [0.0] * N  # 120 BPM: kick, snare, eighth-note hats
for b in range(12):
    t0 = b * SR // 2
    for i in range(int(0.3 * SR)):
        t = i / SR
        if b % 2 == 0:
            drums[t0 + i] += math.sin(2 * math.pi * (50 * t + 3.6 * (1 - math.exp(-t / 0.03)))) * math.exp(-t / 0.12)
        elif t < 0.2:
            drums[t0 + i] += 0.6 * (random.uniform(-1, 1) * math.exp(-t / 0.05) + 0.5 * math.sin(2 * math.pi * 190 * t) * math.exp(-t / 0.03))
        for h in (0, SR // 4):
            if i >= h and i - h < 0.05 * SR:
                drums[t0 + i] += 0.25 * random.uniform(-1, 1) * math.exp(-(i - h) / (0.008 * SR))
save('drums.wav', drums)

chord = [0.0] * N  # C, Am, F, G for 1.5 s each
for c, notes in enumerate([(60, 64, 67), (57, 60, 64), (53, 57, 60), (55, 59, 62)]):
    for i in range(int(1.5 * SR)):
        t = i / SR
        env = min(1, t / 0.02, (1.5 - t) / 0.05)
        chord[int(c * 1.5 * SR) + i] = env * sum(tone(n, t, 7) for n in notes)
save('chord.wav', chord)

melody = [0.0] * N  # eight plucked notes, then 2 s of silence for tails
for k, n in enumerate([64, 67, 71, 72, 71, 67, 64, 62]):
    for i in range(int(0.35 * SR)):
        t = i / SR
        melody[k * SR // 2 + i] = math.exp(-t / 0.08) * (tone(n, t) + 0.3 * tone(n + 12, t))
save('melody.wav', melody)

voice, ph, y = [0.0] * N, 0.0, [[0.0, 0.0], [0.0, 0.0]]  # buzz through moving vowel formants
for i in range(N):
    ph = (ph + 110 * (1 + 0.03 * math.sin(2 * math.pi * 5 * i / SR)) / SR) % 1
    x = 1 - 2 * ph
    f1, f2 = [(800, 1150), (400, 1700), (300, 2300), (450, 800), (350, 600)][min(4, i * 5 // N)]
    out = 0.0
    for j, (f, bw, g) in enumerate([(f1, 80, 1.0), (f2, 120, 0.6)]):
        r = math.exp(-math.pi * bw / SR)
        v = (1 - r) * x + 2 * r * math.cos(2 * math.pi * f / SR) * y[j][0] - r * r * y[j][1]
        y[j] = [v, y[j][0]]
        out += g * v
    voice[i] = out * math.sin(math.pi * ((i % (SR * 2 // 5)) / (SR * 2 / 5))) ** 0.5
save('voice.wav', voice)
PY
)
```

Instruments need no input: the `demo` voice plays the phrase. Use `--event-change 0:Name=value`
to start from another control setting (a persisted control keeps its saved value under
`--event`), for example `--event-change 0:Demo=0` to silence the phrase.

```bash
T=./build/talas-release/talas_cli
# Effects
$T render --project examples/demos/stereo_delay.talas.json --duration 8 --sample-rate 48000 --audio-input 1=$IN/melody.wav --output $IN/stereo_delay.wav
$T render --project examples/demos/chorus.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/chord.wav --output $IN/chorus.wav
$T render --project examples/demos/flanger.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/drums.wav --output $IN/flanger.wav
$T render --project examples/demos/phaser.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/chord.wav --output $IN/phaser.wav
$T render --project examples/demos/hall_reverb.talas.json --duration 8 --sample-rate 48000 --audio-input 1=$IN/melody.wav --output $IN/hall_reverb.wav
$T render --project examples/demos/tape_saturator.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/chord.wav --output $IN/tape_saturator.wav
$T render --project examples/demos/bus_compressor.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/drums.wav --output $IN/bus_compressor.wav
$T render --project examples/demos/auto_filter.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/chord.wav --output $IN/auto_filter.wav
$T render --project examples/demos/vocoder.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/voice.wav --output $IN/vocoder.wav
$T render --project examples/demos/granular_freeze.talas.json --duration 8 --sample-rate 48000 --audio-input 1=$IN/melody.wav --event-change 168000:Freeze=1 --output $IN/granular_freeze.wav
$T render --project examples/demos/spectral_blur.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/drums.wav --output $IN/spectral_blur.wav
$T render --project examples/demos/frequency_shifter.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/chord.wav --output $IN/frequency_shifter.wav
$T render --project examples/demos/ring_modulator.talas.json --duration 6 --sample-rate 48000 --audio-input 1=$IN/voice.wav --output $IN/ring_modulator.wav
# Instruments
$T render --project examples/demos/sub_bass.talas.json --duration 8 --sample-rate 48000 --output $IN/sub_bass.wav
$T render --project examples/demos/poly_pad.talas.json --duration 8 --sample-rate 48000 --output $IN/poly_pad.wav
$T render --project examples/demos/fm_electric_piano.talas.json --duration 8 --sample-rate 48000 --output $IN/fm_electric_piano.wav
$T render --project examples/demos/drawbar_organ.talas.json --duration 7 --sample-rate 48000 --output $IN/drawbar_organ.wav
$T render --project examples/demos/morph_lead.talas.json --duration 8 --sample-rate 48000 --output $IN/morph_lead.wav
$T render --project examples/demos/granular_synth.talas.json --duration 8 --sample-rate 48000 --output $IN/granular_synth.wav
$T render --project examples/demos/plucked_string.talas.json --duration 8 --sample-rate 48000 --output $IN/plucked_string.wav
$T render --project examples/demos/drum_kit.talas.json --duration 8 --sample-rate 48000 --output $IN/drum_kit.wav
$T render --project examples/demos/arpeggiator.talas.json --duration 8 --sample-rate 48000 --output $IN/arpeggiator.wav
$T render --project examples/demos/terrain_drone.talas.json --duration 8 --sample-rate 48000 --output $IN/terrain_drone.wav
$T render --project examples/demos/modal_mallets.talas.json --duration 7 --sample-rate 48000 --output $IN/modal_mallets.wav
```

In the Granular Freeze command the event change turns Freeze on at 3.5 s, so the cloud keeps
playing what it captured after the input ends.

Check a project and its prepared contract with `validate`, adding the same `--audio-input` for
an effect:

```bash
$T validate --status --sample-rate 48000 --project examples/demos/hall_reverb.talas.json --audio-input 1=$IN/melody.wav
```

## Notes

- The CDP processes add latency, which the compiler compensates on the dry path: Spectral Blur
  5248 frames (109 ms at 48 kHz) and Vocoder 1152 frames (24 ms).
- `validate --status` lists "uncompensated-unknown" latency warnings for the instruments and the
  compressor because MIDI Voice and the Dynamic Range Controller declare variable latency.
- The CDP reverb in Terrain Drone, the CDP formant vocoder bands and the Bus Compressor's
  detector type are fixed when the Sound starts; the listed controls are the live ones.
