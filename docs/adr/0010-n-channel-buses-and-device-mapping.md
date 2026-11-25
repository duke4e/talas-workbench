# ADR-0010: N-Channel Buses and Device Mapping

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Offline plans already retain multiple terminal buffer indices and can render
Output4, Output8, Matrix4, and Matrix8 channel vectors. Prepared realtime APIs
still expose left/right spans, `RealtimeAudioEngine` owns two scratch buffers,
and every host channel above two is cleared. Matrix4 is treated as a terminal
with generated buffers and has no descriptor outputs, so the roadmap's explicit
Matrix4 -> Output4 graph cannot be represented.

Project device settings persist enabled output-channel indices, but there is no
compiled graph bus layout or one rule for fewer/more/null device channels.
Packed stereo and historical stereo-pair terminal nodes also cannot be allowed
to define the new general bus semantics accidentally.

## Decision

### Prepared bus layout

Add `AudioBusLayout`, an ordered prepared value. Initial named layouts are:

- Mono: one channel `[M]`;
- Stereo: two channels `[L, R]`;
- Discrete4: four ordered channels `[1, 2, 3, 4]`;
- custom fixed discrete layouts may exist internally, but they are unsupported
  until descriptor and execution evidence is present.

Layout is fixed for one prepared graph generation. Dynamic channel-count change
requires staged reprepare. A port or terminal descriptor states its accepted
and produced layout; a plain historical Audio port remains mono unless an
explicit adapter or bus descriptor says otherwise.

### Matrix4 and Output4

Matrix4 gains explicit mono outputs `out1...out4`. Output4 remains a terminal
with mono inputs `out1...out4` and produces a Discrete4 host bus. The compiler
can therefore represent and validate Matrix4 -> Output4 directly.

The old terminal Matrix4 form remains an offline compatibility subset: when no
explicit consumer exists and the graph uses the historical terminal shape, the
compiler may expose its four generated buffers. This does not count as the
prepared Matrix4 -> Output4 demonstrator and is labeled separately.

Matrix row text resolves at preparation into a finite 4x4 coefficient matrix.
Channel order is always numeric 1 through 4. No hidden stereo pairing or
speaker-label inference occurs.

### Runtime API

Prepared rendering uses an N-channel output view containing channel pointers,
channel count, and equal frame count. `RealtimeRenderSession` exposes the
required output layout/channel count. `RealtimeAudioEngine` preallocates one
scratch channel per prepared output channel and preserves the existing stereo
overloads as adapters.

All output channels are written or cleared on every callback. A failure clears
every supplied host channel, not just the first two.

Offline render, prepared realtime, graph host, Workbench device callback, WAV
export, and analysis meters consume the same ordered layout. No layer truncates
to stereo without recording a deterministic mapping result.

### Device mapping

`ProjectDeviceSettings.outputChannels` contains ordered, unique, one-based
physical device channel numbers. Entry `outputChannels[projectOrdinal]` is the
physical destination for that project output. The value is never used directly
as a zero-based pointer index.

JUCE's standalone callback pointer array is a compact list of enabled physical
outputs in ascending physical-channel order. For physical channel `p`, the
Workbench resolves `callbackIndex` as the zero-based rank of `p` in that active
ascending set. For example, persisted mapping `{3, 1}` enables physical
channels `{1, 3}` and maps project outputs to callback pointer indices `{1, 0}`.
The Workbench then presents a project-ordered pointer view to
`RealtimeGraphHost`; the host/engine never need to infer physical numbering.
Any future host adapter must explicitly declare compact-enabled versus physical
pointer indexing and perform the same project-ordered conversion before render.

- A valid one-based physical destination resolved to a non-null compact host
  pointer receives the project channel.
- A missing, out-of-range, disabled, or null host destination discards that
  project channel and increments bounded mapping telemetry.
- Host channels not targeted by the project are cleared.
- Duplicate host destinations are rejected during staging rather than summed.
- With no explicit saved mapping, project ordinal maps to the same compact
  callback ordinal (`0 -> 0`, `1 -> 1`, and so on) for available channels.

Mapping is prepared on the control thread and immutable for the active device
epoch. Device or enabled-channel change stages/rebuilds the mapping. The audio
callback performs only bounds-checked pointer copies/conversions and atomic
telemetry updates.

### Compatibility

Mono and stereo graphs retain their current first-channel and first-two-channel
samples. Existing `StereoInOutput4/8` behavior remains a compatibility adapter
that duplicates each mono pair input exactly as documented; it does not prove a
native stereo bus. Output8/Matrix8 remain offline subsets until separately
instrumented prepared evidence exists.

## Realtime impact

Scratch channels, pointer tables, maps, and meters are fixed during
preparation/device setup. Render does not allocate, resize, lock, wait, parse
matrix text, format, log, or touch device-management APIs. Null pointers are
handled in the prepared copy loop without dereference.

## Alternatives rejected

- Treating an Audio port as implicitly N-channel would silently reinterpret all
  existing mono nodes.
- Extending the current left/right API with special cases for four channels
  would preserve a stereo-shaped runtime and fail future layout validation.
- Silently dropping channels above the device count without telemetry would
  make routing failures inaudible and invisible.
- Summing duplicate device destinations would conceal a configuration error.
- Promoting Matrix8/Output8 from shared infrastructure alone would overstate
  evidence.

## Acceptance evidence

- explicit Matrix4 -> Output4 impulse routing with four distinguishable values;
- offline/prepared equality across irregular partitions and multiple sample
  rates;
- stable numeric channel order through graph host, Workbench mapping, and WAV
  export;
- missing/null/extra/duplicate device-channel tests;
- exact mono/stereo regression tests;
- callback instrumentation proving zero forbidden work.
