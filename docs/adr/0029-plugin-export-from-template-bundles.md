# ADR-0029: Plugins are exported by copying prebuilt template bundles

Status: Accepted (decided under the owner's standing approval of 2026-11-25:
"Sve odluke donosiš autonomno")

## Context

A Talas user should be able to turn a project (a `.talas.json` Sound graph)
into a VST3 or AU instrument or effect that runs in a DAW, the way SynthEdit
or SynthMaker export a patch: without a compiler. A JUCE plugin, however,
carries its identity in compile-time macros (`JucePlugin_Name`,
`JucePlugin_ManufacturerCode`, `JucePlugin_PluginCode` and the rest, which
JUCE's CMake defines as literals); its VST3 class IDs, its VST3 factory
description and the AU's component description are derived from them. Two
exports with the same compiled identity would be the same plugin to every
host.

## Decision

**Template plugins.** CMake builds two plugins once with `juce_add_plugin`,
formats VST3 and (on macOS) AU: `talas_plugin_instrument` ("Talas
Instrument", IS_SYNTH, MIDI in, AU type `aumu`, VST3 Instrument|Synth) and
`talas_plugin_effect` ("Talas Effect", stereo in and out, AU type `aufx`,
VST3 Fx). The code is in `src/plugin/`. They build with the Workbench
(`TALAS_BUILD_PLUGINS`, on by default when `TALAS_BUILD_WORKBENCH` is) and
gather in `<build>/PluginTemplates`, which the Workbench copies into
`Talas Workbench.app/Contents/Resources/PluginTemplates` before it is
signed. `COPY_PLUGIN_AFTER_BUILD` is off: nothing is installed.

**Embedded files.** At load time a template reads its own bundle's
`Contents/Resources/`:

- `talas-plugin.json` (`talas.plugin-identity` version 1): name,
  manufacturer, four-character manufacturer and plugin codes, kind, version,
  bundle identifier, description and the embedded project's hash.
- `project.talas.json`: the Sound, with any assets it references copied to
  `Resources/assets/` and its asset paths pointing there.

A raw template, with neither, keeps its own identity and plays a built-in
project: the effect passes its stereo input through, the instrument plays
eight saw voices with an envelope from live MIDI (`!Level`, `!Release`).

**Identity at load time, without patching JUCE.** JUCE's
`juce_TargetPlatform.h`, which every JUCE translation unit includes first,
includes the header named by `JUCE_APP_CONFIG_HEADER`, JUCE's own hook for a
global settings header. The templates set it to
`src/plugin/TalasPluginJuceConfig.h`, which `#undef`s and redefines
`JucePlugin_Name`, `JucePlugin_Desc`, `JucePlugin_Manufacturer`,
`JucePlugin_VersionString`, `JucePlugin_VersionCode`,
`JucePlugin_ManufacturerCode` and `JucePlugin_PluginCode` as calls to
functions (`talas::plugin::runtime_identity`) that return the embedded
identity, read once into a function-local static. `JucePlugin_AUSubType`,
`JucePlugin_AUManufacturerCode` and `JucePlugin_VSTUniqueID` are defined in
terms of the two codes and follow them.

This works because of every use in JUCE 8.0.15's VST3 and AU wrappers:

- `VST3/juce_VST3ModuleInfo.h`: `getVST3InterfaceId` passes the codes to
  `VST3Interface::jucePluginId`, an ordinary inline function; the factory's
  `ClassEntry` and `PFactoryInfo` take the name, manufacturer and version as
  `const char*` constructor arguments.
- `juce_audio_plugin_client_VST3.cpp`: the class IDs are `inline static const
  FUID` members initialised from `getVST3InterfaceId` during static
  initialisation, and the VST2-compatible state writes `JucePlugin_VSTUniqueID`
  and `JucePlugin_VersionCode` at run time.
- `juce_audio_plugin_client_AU_1.mm`: only the Cocoa view's description uses
  `JucePlugin_Name`, through `nsStringLiteral (const char*)`; the AU's own
  identity comes from the bundle's `Info.plist`.
- `detail/juce_CheckSettingMacros.h` only tests the macros with `#ifndef`.
- Elsewhere (`juce_AccessibilityHelpers.cpp`) the name becomes a `String`.

No use needs a literal: none is in `#if`, string-literal concatenation,
`sizeof` or a constant expression. (The LV2, AAX, VST2 and Unity wrappers do
concatenate or test them, and are not built.) The VST3 class IDs are computed
during static initialisation, so the identity loader uses the standard
library alone, not JUCE: it finds its own binary with `dladdr` (or
`GetModuleHandleEx` on Windows), which is what
`juce::File::currentExecutableFile` returns inside a plugin, and reads
`../Resources/talas-plugin.json` from it. JUCE's VST3 manifest helper is
turned off (`VST3_AUTO_MANIFEST FALSE`): it is compiled from the template's
definitions and would bake the template's identity into `moduleinfo.json`.

What stays compile-time, and so differs between the two templates: the kind
(`JucePlugin_IsSynth`), MIDI input, the AU main type and export prefix
(whose factory function, `TalasInstrumentAUFactory` or
`TalasEffectAUFactory`, the `Info.plist` names) and the VST3 category.

**The exporter** (`talas::plugin::exportPlugin`, JUCE-free in
`talas_plugin_support`) copies a template bundle to a staging folder beside
the destination and:

- renames the binary after the plugin (`Contents/MacOS/<Name>`; on Windows
  and Linux the VST3 layout requires `Contents/<arch>/<Name>.vst3|.so`);
- writes `talas-plugin.json` and `project.talas.json` (byte for byte when the
  project has no assets; reserialized when asset paths are rewritten);
- rewrites `Info.plist` with a small XML property-list model:
  `CFBundleExecutable`, `CFBundleIdentifier`
  (`com.talas.plugin.<manufacturer>.<name>`), `CFBundleName`, the versions
  and, for an AU, the `AudioComponents` entry (name "Manufacturer: Name",
  type `aumu` or `aufx`, subtype, manufacturer, version), keeping the
  factory function and the rest;
- writes the VST3 `Contents/Resources/moduleinfo.json` in the format JUCE's
  helper writes, with the class IDs `VST3Interface::jucePluginId` gives the
  new codes (`ABCDEF01 9182FAEB <manufacturer> <plugin>` for the component,
  `ABCDEF01 1234ABCD …` for the controller, printed the same on every
  platform);
- stages the embedded project once from inside the bundle, its assets
  loading from there, exactly as the plugin will, and fails instead of
  writing a plugin that would not play;
- on macOS ad-hoc signs each bundle (`codesign --force --deep --sign -`) and
  verifies it, then moves it in place. It replaces an earlier Talas export
  of the same file name, never anything else.

**Codes.** The manufacturer code is the manufacturer's first four letters or
digits, the first upper-cased (Apple reserves all-lower-case codes), padded
from a hash when shorter: "Talas" gives `Tala`. The plugin code is a 64-bit
FNV-1a hash of the manufacturer and name spelt `[A-Z][a-z0-9]{3}` (one
upper-case letter, first, as GarageBand requires). Re-exporting the same
names gives the same codes, so a host sees an update, not a new plugin.
Before using a derived code the exporter reads the `talas-plugin.json` of
every Talas export in the output folder and the standard plugin folders
(read only) and, if a different plugin holds the same pair, salts the hash
until it is free; both codes can also be given.

**Exposed twice.** `talas_cli export-plugin --project <p> --name <n> --kind
instrument|effect --output <dir> [--format vst3,au] [--manufacturer <m>]
[--manufacturer-code] [--plugin-code] [--version] [--template-dir]
[--no-sign]`, and the Workbench's File > Export Plugin..., a small dialog
that exports the open project. Templates are found in `--template-dir`
folders, then `TALAS_PLUGIN_TEMPLATES`, then `PluginTemplates` next to the
running program, in its app bundle's `Resources`, or in `../share/talas`
(the build tree puts `talas_cli` beside `PluginTemplates`).

**Audio.** `processBlock` runs the embedded project through the Workbench's
own prepared realtime engine, a `ProjectRuntimeBridge`. `prepareToPlay`
stages the project at the host's rate with a prepared block of the host's
maximum clamped to 32–2048 frames and reports the graph's terminal output
latency with `setLatencySamples`. A Sound starts from its persisted Virtual
Controls' values, which an `EventValueBinding` does not override, so the
plugin writes its parameters' values into the copy of the document it stages
(values a Sound fixes when it starts take them too), and sends every value
again at the first block. `processBlock` neither allocates nor locks: host blocks
larger than the prepared size are rendered in parts; an effect's input is
copied to preallocated scratch first (the host buffer is also the output),
host channels 1 and 2 becoming project inputs 1 and 2, summed into input 1
when the project reads only that one (as the demo effects do). An
instrument's MIDI goes to the bridge's `enqueueMidi*` calls, the path the
Workbench's device MIDI takes, so MIDI Voice projects play from the host.
Sample accuracy comes from the bridge's arrival-time mapping: the plugin
passes a callback clock that advances by each part's length, and stamps an
event `n` frames into a part with the previous part's time plus `n` frames,
which the bridge maps to exactly frame `n`.

**Parameters.** The project's Virtual Controls, in order, then the Event
Values its graph reads without one (discovered with
`discoverProjectEventValueControls`, ranges from their names or the global
map), at most 128. A fader becomes a float parameter with its range, default,
log taper and grid; a toggle or gate a switch; a label's trailing "(Hz)",
"(s)", "(dB)" and the like becomes the unit, else the domain's. An
instrument leaves out the MIDI-driven names (`!ccNN`, `!MIDIPitchBend`…) its
host sets through MIDI. Parameter IDs are the Event Value names (hashed by JUCE, so a host's
automation survives reordering the controls; AU hosts therefore list
parameters in hash order, and the plugin's editor keeps the project's).
JUCE's VST3 wrapper adds hidden "MIDI CC" parameters to an instrument,
which carry controllers, pitch bend and pressure to it. Host changes
reach the engine once per block, at its first frame, through
`enqueueEventValue`: JUCE's `AudioProcessor` gives a plugin no
sample-accurate automation. The state is `talas.plugin-state` JSON: the
parameters by name and the project's hash, so a state saved with another
version of the project still restores the parameters that match.

**Editor.** A compact editor built from `WorkbenchTheme` (compiled into the
templates, not copied): the backdrop, a glass header with the plugin's name
in the violet-blue-aqua gradient, and a knob or switch per parameter, scrolled
past three rows.

**Portability.** The platform-specific steps sit behind
`talas::plugin::platform` (module path, executable path, process launching,
signing, plug-in folders); `Info.plist` rewriting and signing are macOS-only.
On Linux the templates are PIC and link with `--exclude-libs,ALL` so two
Talas plugins in one host do not bind to each other's copies of the Talas
libraries. Only macOS is built and verified.

## Verification

`talas_plugin_export_integration_tests` exports the Sub Bass and Ring
Modulator demos with the real templates and loads them with JUCE's
`VST3PluginFormat`, once through `moduleinfo.json` and once, from a copy
without it, through the binary's own factory (same name, manufacturer and
class ID). It plays the instrument from MIDI and its Demo switch, checks the
effect changes its input and that Mix takes it back to dry, round-trips the
state, opens the editor and passes the raw effect template through. On macOS
it also registers each AU in the test process alone with
`AudioComponentRegister` and the bundle's factory function, so nothing is
installed, and plays it through JUCE's `AudioUnitPluginFormat`. No DAW is
part of the tests.

## Alternatives considered

- **Compile each export.** Generating a JUCE project per export gives true
  compile-time identity but needs a compiler and minutes per export, which
  the task rules out.
- **Patch JUCE** to call identity functions, or binary-patch the codes in a
  template: both are fragile across JUCE updates, and patching a signed
  binary still needs re-signing.
- **One template for both kinds.** `JucePlugin_IsSynth`,
  `JucePlugin_WantsMidiInput` and the AU main type are tested with `#if`,
  so the kind cannot be read at load time.

## Consequences

- Exporting takes seconds and needs no toolchain, only a template built for
  the platform. A template rebuild changes every later export's code, never
  its identity.
- Every exported plugin carries the whole Talas runtime (tens of
  megabytes) and links JUCE (AGPLv3 unless commercially licensed) and the
  LGPL-2.1 CDP library in `third_party/cdp`, which the runtime always links;
  distributing a plugin means meeting those licences. The original Talas
  code is Apache-2.0.
- Parameter changes are block-accurate. Event Values that a Sound fixes when
  it starts (the README of the demos lists some) take a new value only when
  the host prepares the plugin again.
- The host's tempo and transport are followed as
  [ADR-0030](0030-plugins-follow-the-host-transport.md) decides: the
  `!Tempo`, `!SongPosition`, `!BeatPhase` and `!TransportPlaying` Event Values
  at every block, and a Timeline started, stopped and placed by the host.
- AU export, signing and plist rewriting exist only on macOS; the Windows and
  Linux branches compile in principle but are unverified.
