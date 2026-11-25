# ADR-0030: Plugins follow their host's tempo and transport

Status: Accepted (decided under the owner's standing approval of 2026-11-25:
"Sve odluke donosiš autonomno"). Amends [ADR-0029](0029-plugin-export-from-template-bundles.md).

## Context

An exported plugin (ADR-0029) played its project's own tempo and Timeline from
the moment the host prepared it. A DAW user expects a synced delay, an
arpeggiator or a Timeline to follow the song's tempo, its Play and Stop, its
locates and its loop.

The engine's musical transport, as the Workbench drives it:

- The transport counts frames (`RealtimeGraphHost`); a stage's prepared
  musical map (tempo and meter events, 960 ticks a quarter, from the
  Timeline's musical time) turns frames into bars, beats and ticks.
- That map places Score clips, prepared as frames with their audio rendered
  off the audio thread; resolves Live Grid launches at Next Beat or Next Bar;
  and gives `beats` units and the `!TimingClock` stand-in their tempo, fixed
  when the Sound starts.
- The MIDI clock authority (`configureMidiClockAuthority`) takes Start, Stop
  and Song Position from an external clock and publishes its musical tick,
  but the DSP still advances one frame a sample at the map's tempo. Song
  Position relocates through `relocateTransportAtNextRender`, on the audio
  thread.
- The engine's playing flag is the Workbench's Play: a stopped engine renders
  silence. An effect or an instrument in a DAW has to keep sounding while the
  song is stopped.

## Decision

### The transport's Event Values

Four built-in Event Values, all plain numbers (scalar):

| Name | Value | In a plugin | In the Workbench | Offline (`talas_cli`) |
| --- | --- | --- | --- | --- |
| `!Tempo` | quarter notes a minute | the host's BPM | the musical map's tempo at the transport's position | the tempo the map starts with |
| `!SongPosition` | quarter notes since the song's start, negative in a count-in | the host's PPQ position | the transport's position in the map | 0 |
| `!BeatPhase` | how far through its quarter note the song is, 0 to below 1 | from the host's position | from the transport's | 0 |
| `!TransportPlaying` | 1 while the transport plays, 0 while it is stopped | the host's | 1 (a Sound renders only while it plays) | 1 |

- **Names.** They say what they measure, as `!KeyDown`, `!LocalTime` and
  `!TimingClock` do. They have no "Host" in them because the Workbench gives
  them too, from its own transport, so a project behaves the same in the
  app and in a DAW.
- **A beat is a quarter note**, as in the `beats` unit and a BPM, whatever the
  meter.
- **Binding.** `prepareProjectRenderSettings` binds the ones a project reads
  (`projectReferencesEventValue`), `!Tempo` starting at the map's tempo.
  Discovery treats them as built-ins: no Virtual Control, no plugin
  parameter. A project's own Virtual Control or Timeline Event Value of the
  same name, or a binding the caller gives (`talas_cli --event`), wins. An
  older project with a `!Tempo` fader, as the Arpeggiator demo had, keeps it,
  so no project format change is needed.
- **Updates.** At each render the bridge schedules them on the engine about
  to render, at its first frame and before any other control
  (`scheduleEventValue`, as it sets `!KeyDown`), a value only when it changed.
  A field that fixes one when its Sound starts takes no live change, as for
  any Event Value. To leave a project's own control alone, each stage
  publishes which of the four it binds as built-ins: a ring of eight atomic
  words, `(stage id << 8) | bits`, which the audio thread reads when the
  stage it renders changes. A performance overlay keeps its document stage's
  bits.
- **The project's own transport.** `RealtimeAudioEngine::musicalClockAtFrame`
  reads the stage's prepared musical clock at the playhead frame: the song
  position to a fraction of a tick and the tempo there. A frame that a tick
  lands on is at that tick exactly, so a beat in a tempo whose microseconds
  a quarter were rounded (90 BPM) falls on its beat.

### Reading the host

`talas::plugin::HostTransportFollower` (JUCE-free, in
`talas_plugin_support`) reads `getPlayHead()->getPosition()` at every
`processBlock`:

- **Nothing to follow.** No playhead, no tempo, or a tempo outside 1 to 60000
  BPM (what the map can hold): the project's own transport and tempo run, as
  before.
- **Tempo** is the host's BPM at the block.
- **Position** is the host's PPQ at the block's first frame. A host that gives
  a tempo but no position plays from the song's start at its tempo.
- **Musical tick** is the position times 960, rounded, and 0 before the song's
  start.
- **Bars and beats** use the host's time signature (1 to 64 beats of a
  power-of-two note up to a 64th, else 4/4); the host's last bar start places
  the beats; bars are counted in bars of the meter from the song's start,
  since hosts do not say how many came before.
- **Start and stop** come from `getIsPlaying()`.
- **Jumps.** A block that starts more than 5 ms, at the host's tempo, from
  where the previous one ended is a locate or a loop's return; so is the
  first block followed. Hosts round their positions, so the test is not
  exact.
- A host block the engine renders in parts gives each part its position at
  the tempo; a jump belongs to the first part.

The processor passes each part's `HostMusicalTransport` to
`ProjectRuntimeBridge::followHostTransport` before rendering it. The bridge
adopts it at the render's start, after the MIDI clock and MTC drains, so the
host has the last word. Everything on this path is plain audio-thread state:
no allocation and no lock (`talas_transport_event_value_tests` counts
allocations while following).

### What follows the host

- **Every project**: the four Event Values, at the block where the host's
  tempo or position changes. A quarter-note delay is `(60 / !Tempo) * 1 s`.
- **A project that plays a Timeline** (Score clips, Score automation or
  Timeline Event Values): the host's Play and Stop start and stop its Sound,
  as the Workbench's do. Play, a locate and a loop's return put the
  transport at the frame of the host's musical tick in the staged map, the
  path MIDI Song Position takes, unless it is there already (within 2 ms, as
  hosts round): Play after Stop plays on, the clips under way intact. A
  count-in waits at the song's start. A move on the audio thread cannot
  rebuild the sources of a clip under way at the new position, which only a
  relocation prepared on a control thread does, so while the host is stopped
  the worker below prepares one (`requestTransportRelocation`) to where the
  host stands; Play after a locate then sounds the clips under way at once.
  A locate or loop return during playback into the middle of a clip leaves
  that clip silent until the next one starts, as MIDI Song Position does.
- **Any other project** keeps running whatever the host's transport does, and
  the host's jumps do not move its transport: a relocation resets the DSP
  that resets with the transport, such as delay lines, so a loop in the DAW
  would cut a reverb's tail.
- **The musical map.** A project whose sound depends on it (a Timeline, a Live
  Set, `!TimingClock` or `!TimingClockDuration`, a field in `beats`) is staged
  again with a one-tempo, one-meter map at the host's tempo and meter while
  the host is stopped and they differ from the staged map. A worker thread
  polls every 50 ms, reuses the assets the first stage loaded, collects the
  retired engine, and takes `stageMutex_`, which `prepareToPlay` and
  `releaseResources` also take and the audio thread never does. The next
  render the host drives puts the transport at the host's position in the
  new map, and every parameter is sent again. Preparing the plugin again
  stages it at the last host map it knew. Live Grid launches, which a plugin
  makes only through Live Tools its Event Values trigger, use the staged map,
  which is then the host's.
- **Status.** The plugin's editor shows "host at N BPM" while it follows.

A Timeline therefore follows a host tempo change the next time the host
stops: Score clips are prepared as frames with their audio already rendered,
and `beats` and `!TimingClock` are fixed when the Sound starts, so following a
change in mid-play would mean staging again, which costs a compile, renders
the clips again and resets the DSP, too much for a tempo ramp. The Event
Values follow at once.

## Alternatives considered

- **Warp the transport**, advancing it at the host's tempo over the map's: the
  Score's clip audio is rendered at the sample rate and cannot be stretched by
  the transport.
- **Relocate whenever the transport drifts** from the host's position: each
  relocation resets DSP state.
- **Stage again at every tempo change, playing or not**: a compile and clip
  render per change, and a reset each time; a tempo ramp would glitch.
- **Drive the published musical tick** (`setExternalMusicalTick`): it carries
  the MIDI clock follower's state and range checks (20 to 300 BPM), and
  nothing in a plugin reads it. A Timeline project's transport is placed at the
  host's position, so the map-derived bar and beat match the host's.
- **Synthesize MIDI timing clock** from the host for `!TimingClock`: left out;
  staging at the host's tempo covers its stand-in.
- **`!HostTempo`, `!HostPlaying`**: they would read wrong in the Workbench.
- **A project format flag** like format 122's for `!Time`: a declared control
  winning keeps older faders; only an undeclared `!Tempo` in an older project
  changes meaning, from a fader without a control to the tempo.

## Verification

- `talas_plugin_host_transport_tests`: the follower (missing fields, tempo
  changes, PPQ to tick, time signatures, start and stop, locates, loop
  returns, rounding, parts) and which projects play a Timeline or read the
  map.
- `talas_transport_event_value_tests`: bindings offline and their discovery,
  the Workbench's transport (tempo changes in the map, position, phase), a
  host's values, a project's own `Tempo` left alone, a host starting,
  stopping, locating and counting in a Timeline, a new map placing the host's
  position, Play inside a Score clip after Stop and after a prepared
  relocation, and no allocation while following.
- `talas_plugin_export_integration_tests` hosts exported VST3 and AU plugins
  with a fake `AudioPlayHead`: `!Tempo` follows a host tempo change at its
  block, Stop and Play set `!TransportPlaying`, a VST3 without a playhead
  plays the project's tempo, the Stereo Delay's Sync echoes three sixteenths
  late at 120 and 90 BPM, and a Timeline is silent while the host is stopped,
  is staged again at the host's 90 BPM, starts at the host's position, plays
  on after Stop inside its clip, sounds the clip at once after a locate into
  it while stopped, and falls silent at a jump back before it.

## Consequences

- Synced effects and instruments need only `!Tempo` and `!BeatPhase`; the
  Arpeggiator demo now steps at `!Tempo` and the Stereo Delay has Sync to
  tempo.
- The Event Values change once a render, at block rate: a phase-locked LFO
  built on `!BeatPhase` steps once a block.
- An AU asks its host for the tempo through callbacks, and JUCE's AU host
  answers 120 BPM and position 0 when it has no playhead, so through it a
  plugin always follows a tempo; through VST3 a host without a playhead leaves
  the project's tempo.
- A project's own Timeline loop still loops inside the plugin; switch it off
  to let the host's loop rule.
- Not verified: real DAWs, AU tempo and transport in a real host, and Windows
  or Linux.
