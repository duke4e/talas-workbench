# ADR-0018: CDP algorithms in a separate LGPL module

Status: Accepted (owner decisions 2026-09-28 and 2026-09-29)

## Context

The Composers Desktop Project (CDP8) offers a large set of sound
transformations. The owner chose to bring CDP processes into Talas,
keeping CDP-derived code under its own license. CDP8 is licensed under the
GNU Lesser General Public License 2.1 or later; Talas's own code is
Apache-2.0. CDP programs read and write whole sound files, which the realtime
engine must never do, and many keep global program state.

## Decision

CDP-derived code lives only in `third_party/cdp/`, which carries the CDP8
license text and a notice naming the CDP8 revision and the source functions
each port comes from. It builds as its own library, `talas_cdp`. No Apache file
contains CDP code; Talas nodes call the library through its public
headers, as they would any dependency.

Each port turns one CDP algorithm core into a prepared processor: it is
configured and allocated before rendering, then processes blocks without file
I/O, global state, allocation or locking, and both renderers call the same
processor. A port keeps CDP's arithmetic, including its quirks and its float
sample buffers, and is checked against the output of the CDP program it comes
from. Each difference a stream forces (causal control ramps, latency in place
of lookahead) is named in the node's limitations.

Nodes that use a port have `talas.cdp…` identifiers and the CDP category. They
are Talas utilities.

## Consequences

- The first ports are Lower Resolution (CDP `modify radical` mode 4) and Ring
  Modulate (mode 5). Cross modulation is Product. Later ports follow the plan:
  tremolo and pan, tap delay and echo, reverb, filters, then distortion.
- A binary that links `talas_cdp` and is distributed must meet LGPL 2.1
  section 6: ship the library's license and source, and allow relinking,
  for example by providing object files.
- Updating a port to a newer CDP8 revision updates the notice.

## Amendment (2026-10-12): multichannel tools have eight outputs

Owner plan 2026-10-12: CDP's multichannel tools become nodes with N
outputs. Taken as recommended (standing approval 2026-10-12), the simplest
form consistent with Matrix8 and the Horizontal Ambisonic Decoder
(ADR-0010): each such node has eight mono outputs, `out1` to `out8`, plays
CDP's channels on `out1` to `outN` and writes the others silent every
block, so an output past N is silence and never stale. Where CDP's output
count is a parameter it is the node's `channels` field (whole, the port's
fewest to 8); a tool that takes N channels takes them on `in1` to `inN`
(`in1`, or `in1` and `in2`, required: the port's fewest), connected without
a gap, and plays as many. A stereo input is the Input and Right Input, and
Antiphony's second file a Side Chain (and Side Chain Right). Nodes that
already played one or two channels on `leftOut` and `rightOut` (Cascade,
Phasor, Spin) keep them, silent when CDP plays more, and gain `out1` to
`out8`. The ports take up to 16 channels; the nodes stop at 8, the width of
StereoInOutput8.

The nodes are mchanpan's rotate, panspread, table pan, stepspread, crosspan
and antiphony, mchanrev, flutter, frame shift's rotate (modes 1-2), remap
(3-6 and 8) and envchans (7), and tostereo, besides cascade 4-5, phasor's
3-8 channels with surround and spin stereo's wide modes. Their fields,
preparation, live setters and chunked playing are one header,
`src/runtime/CdpMultichannelPorts.h`, which both renderers call. CDP8's
mapping bugs in antiphony and crosspan stay by default, as its output shows
them; a `corrected` field plays what CDP8 evidently intends (owner
decision). Whole-file facts CDP8 finds ahead in its breakpoint files (frame
shift's first directions) and the buffer sizes its quirks depend on are
fields read once.

## Amendment (2026-10-19): fixed fields are constant fields

Taken as recommended by analogy with the owner's 2026-10-12 decision on
constant fields (ADR-0004; standing approval 2026-10-12). A field CDP8 takes as
a fixed value rather than a breakpoint file (`parstruct.c`'s lower-case
letters) has no later value in CDP8, so given a time function or an Event
Value it compiles to its starting value, as constant fields do, and
both renderers play that value; before, both refused it. Inside a MIDIVoice
a moving one is refused by name, as the process is set up as it starts. A
starting value outside the range CDP8 checks is still refused, as CDP8
refuses such a value. The fields CDP8 reads as breakpoint files keep
following live values where CDP8 reads them. The list,
`src/runtime/CdpFixedFields.h` (322 fields; 349 by 2026-10-19), comes from a survey that moved
each CDP field without changing its value and kept the fields such a moving
value made a renderer refuse.

## Amendment (2026-10-19): CDP processes that change length or order

Taken as recommended (standing approval 2026-10-12). CDP8's spectral
processes that change a sound's length or the
order of its windows (`stretch time`, `focus hold`, `spec cut`, `grab` and
`magnify`, `combine interleave`, `blur drunk`, `shuffle` and `weave`)
cannot stream, so they are not nodes: they are an offline "process to
sample". `talas::cdp::renderOfflineSpectralProcess` analyses each input
sample whole (`pvoc anal`), runs the CDP program over the analysis with
CDP8's 20-window buffers emulated, bugs included, and plays the result
through `pvoc synth` as CDP8 writes it; `talas::audio::processSamplesThroughCdp`
reads samples (a stereo one as its mix) and writes CDP's floats as a 32-bit
float WAV. `talas_cli cdp-process` and the Workbench's Tools > CDP Process to
Sample run it; the Workbench leaves an example Sound playing the new sample.
Data and breakpoint files are given as text. `blur drunk`'s seed stands for
the Unix time CDP8's `srand(time(0))` takes. Alternatives: nodes that render
their Input whole before playing (a DiskCache-like latency of the whole
sound), which a stream cannot bound.

## Amendment (2026-10-19): channel and edit tools

Taken as recommended. CDP8's channel and edit
tools that stream join the multichannel nodes of the 2026-10-12 amendment,
playing their channels on out1 to outN: `modify space` 2 and 4 (Stereo
Space), `housekeep chans` 3, 4 and 5 (Channel Zero, Channel Mixdown on a
stereo input, Mono To Stereo), `chanphase` (Channel Phase), `mton` (Mono To
Many), `pairex` (Channel Pair), `mchstereo` (Stereo Placement, its stereo
inputs as pairs of in1 to in8), `prefix silence` (Prefix, at the input's
length) and `sfedit masks` and `insil -o` (Silence Masks and Overwrite,
with CDP8's buffers). `envel warp 6` for a stretch of 1 or more is
talas.cdpEnvelopeTimestretch. `submix mergemany` and `housekeep chans 4` on
more than two channels rescale by a whole file's peak, `insil` without -o
lengthens its sound and `envel warp 6` below 1 reads ahead without bound,
so they stay out of the nodes.
