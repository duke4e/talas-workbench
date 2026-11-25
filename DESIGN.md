---
name: "Talas Workbench"
description: "A night-signal laboratory under glass: deep ink, lit glass panels and one violet-to-aqua signal."
colors:
  deep-ink: "#0c0e16"
  backdrop-top: "#151829"
  backdrop-bottom: "#090a11"
  glass-surface: "#161a27"
  raised-surface: "#23293d"
  well: "#0d0f18"
  steel-outline: "#272d42"
  interactive-outline: "#58628a"
  enhanced-outline: "#d4e2ef"
  reading-frost: "#edeff8"
  muted-steel: "#a6acc6"
  selection-frost: "#a6d2ff"
  selected-row: "#2e3470"
  talas-violet: "#9b6bff"
  talas-aqua: "#22d8e6"
  accent-ink: "#0a0b14"
  signal-aqua: "#2ee6c9"
  control-amber: "#ffb84d"
  event-pink: "#ff5fa8"
  data-periwinkle: "#8fa3ff"
  output-blue: "#5fa8ff"
  fault-coral: "#ff5d6e"
  runtime-green: "#16805a"
  subset-ochre: "#9a6410"
  placeholder-rose: "#c2475a"
  utility-slate: "#56709a"
  grid-ink: "#222842"
  control-tile: "#1b2031"
  glass-highlight: "rgba(255, 255, 255, 0.13)"
  glass-edge: "rgba(255, 255, 255, 0.085)"
  shadow: "rgba(0, 0, 0, 0.6)"
gradients:
  signature: "linear-gradient(90deg, #9b6bff 0%, #5b9dff 50%, #22d8e6 100%)"
  level-meter: "linear-gradient(90deg, #2ee6c9 0%, #ffb84d 72%, #ff5d6e 100%)"
categories:
  sources: "#a47dff"
  filters: "#2fd3f0"
  effects-dynamics-space: "#6a8dff"
  mixing: "#3ddc97"
  envelopes-control-time: "#ffb84d"
  math-scripts-display: "#b5e853"
  spectral-cdp: "#ff5fa8"
  sampling-tracking: "#ff8a5b"
  midi-io: "#8ec5ff"
  other: "#8c95b4"
typography:
  title:
    fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
    fontSize: "13px"
    fontWeight: 700
  body:
    fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
    fontSize: "12px"
    fontWeight: 400
  label:
    fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
    fontSize: "11px"
    fontWeight: 400
  micro:
    fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
    fontSize: "10px"
    fontWeight: 400
  readout:
    fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
    fontSize: "13px"
    fontWeight: 700
    fontFeature: "tnum"
rounded:
  pill: "999px"
  field: "4px"
  control: "5px"
  tile: "6px"
  card: "7px"
  panel: "9px"
spacing:
  xxs: "4px"
  xs: "5px"
  sm: "6px"
  md: "8px"
  lg: "10px"
  xl: "12px"
  xxl: "14px"
  grid: "32px"
components:
  toolbar-button:
    backgroundColor: "{colors.raised-surface}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.control}"
    height: "32px"
  toggle-on:
    background: "{gradients.signature}"
    textColor: "{colors.accent-ink}"
    rounded: "{rounded.control}"
  text-field:
    backgroundColor: "{colors.well}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.field}"
    height: "24px"
  combo-box:
    backgroundColor: "{colors.raised-surface}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.control}"
    height: "24px"
  panel:
    backgroundColor: "{colors.glass-surface}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.panel}"
    padding: "8px"
  graph-node:
    backgroundColor: "{colors.glass-surface}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.tile}"
    padding: "7px 10px"
    width: "178px"
  virtual-control:
    backgroundColor: "{colors.control-tile}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.card}"
    padding: "6px"
    width: "200px"
    height: "64px"
  support-badge-full:
    backgroundColor: "{colors.runtime-green}"
    textColor: "#ffffff"
    rounded: "{rounded.field}"
    height: "18px"
    width: "72px"
  transport-readout:
    backgroundColor: "{colors.well}"
    textColor: "{colors.talas-aqua}"
    rounded: "{rounded.control}"
---

# Design System: Talas Workbench

## Overview

**Creative North Star: "The Night Signal Laboratory, under glass"**

Talas is a precise, nocturnal, instrument-like workspace: a deep ink field
where signal flow, state and structure become legible through colour. The
working surfaces are glass panels lit from above, floating a little over the
ink on soft shadows; one violet-to-aqua gradient is the signature that marks
what is active. Density stays intentional, because the user is operating a
serious sound instrument, and every accent still earns its place.

The look borrows the clarity of dense flat DAWs (one clear accent on a dark
body), the deep navy and glowing displays of modern soft-synths, colour-coded
modules, and LCD-like readouts, without copying any of them. Faux hardware
(wood, brushed metal, rack screws, photographic knobs) stays out.

**Key Characteristics:**

- Dark, compact, instrument-like desktop density.
- Glass panels: a tinted gradient fill under a translucent top sheen and
  highlight line, a thin light edge, a soft shadow.
- One signature gradient, violet through blue to aqua, for active state.
- Colour carries meaning: port kinds, Sound categories, severities. Text and
  shape always carry it too.
- Displays (scopes, spectra, waveforms, the transport time) are recessed wells
  with glowing traces.
- Native platform typography keeps the interface operational.

## Colors

The palette is a deep ink body, glass surfaces over it, the Talas gradient, and
a compact set of saturated signal colours. Values live in
`src/app/workbench/WorkbenchTheme.cpp`; everything paints through the tokens
and the `WorkbenchStyle` recipes in `WorkbenchTheme.h`.

### Signature

- **Talas Violet** (`#9b6bff`) to **Talas Aqua** (`#22d8e6`), through a clear
  blue (`#5b9dff`) at the middle: toggled-on buttons and workspace tabs, ticked
  boxes, slider and progress fills, rotary arcs, panel-title bars, the front
  Sound editor tab and the highlighted popup-menu item. Text drawn on the
  gradient is **Accent Ink** (`#0a0b14`), at least `5:1` on every stop.

### Signals (port kinds)

- **Signal Aqua** (`#2ee6c9`): Audio ports (`A`), audio cables, audio traces
  and waveforms.
- **Control Amber** (`#ffb84d`): Control ports (`C`), control cables,
  automation lines and spectra.
- **Event Pink** (`#ff5fa8`): Event ports (`E`), triggers and Score markers.
- **Data Periwinkle** (`#8fa3ff`): typed Data ports (`D`), spectra and info
  severities.
- **Output Blue** (`#5fa8ff`): final output meters and Master rows.

### Sound categories

Each Sound category has a colour code, shared by the cards in the signal flow
(a lit header and a gradient cap) and the Prototypes list (a dot beside each
category and a bar beside each prototype): Sources violet, Filters aqua,
effects/dynamics/space blue, Mixing mint, envelopes/control/time amber,
math/scripts/display lime, spectral and CDP pink, sampling and live tracking
coral, MIDI and I/O sky, and slate for the rest. Ports keep their kinds'
colours on every card.

### Neutral

- **Deep Ink** (`#0c0e16`) and the backdrop gradient (`#151829` to
  `#090a11`, with a faint violet glow at the top left and an aqua one at the
  bottom right): the window behind the panels.
- **Glass Surface** (`#161a27`): panels, lists and graph cards.
- **Raised Surface** (`#23293d`): buttons and combo boxes.
- **Well** (`#0d0f18`): text fields, plots, the graph canvas and readouts.
- **Control Tile** (`#1b2031`): Event Value and performance tiles, Live Grid
  cells.
- **Steel Outline** (`#272d42`) and **Interactive Outline** (`#58628a`):
  structural lines; Interactive Outline also replaces the glass edge when
  Enhanced Contrast is on.
- **Reading Frost** (`#edeff8`): primary text, about `15:1` on glass.
- **Muted Steel** (`#a6acc6`): metadata and secondary labels, above `6:1` on
  every surface.
- **Selection Frost** (`#a6d2ff`): selected node outline and keyboard focus.
- **Selected Row** (`#2e3470`): selected list rows.

### Status

- **Fault Coral** (`#ff5d6e`): diagnostics, error borders and badges.
- **Runtime Green** (`#16805a`), **Subset Ochre** (`#9a6410`), **Placeholder
  Rose** (`#c2475a`), **Utility Slate** (`#56709a`): the support badges, each
  dark enough for white microtype at `4.5:1`.

**The Signal Is Semantics Rule.** The port-kind colours mean a port kind and
the category colours mean a category; neither is a free decorative accent.
The gradient means "on" or "this one".

**The Never Colour Alone Rule.** Every coloured state is also written: `A`,
`C`, `E`, `D` before each port name, severity words in their pills, the state
under each Live Grid cell's name, a tick in a ticked box.

## Typography

**Display Font:** None; the Workbench has no display-type layer.
**Body Font:** Native platform sans serif (San Francisco on macOS, Segoe UI on
Windows, with the JUCE default sans fallback).
**Label/Mono Font:** Native platform sans serif; no distinct monospaced face is
currently established.

**Character:** Compact, plainspoken, and mechanically legible. Typography
supports hierarchy through size, weight, color, and placement rather than
through brand fonts or decorative treatment.

### Hierarchy

- **Title** (bold, `13px`): panel headings and graph-node names.
- **Body / Control** (regular or bold, `12px`): buttons, combo-box values,
  prototype and Sound-definition names, primary list content, and operational
  copy.
- **Label / Data** (regular or bold, `11px`): node IDs, ports, categories,
  domains, status, and tabular-numeral readouts.
- **Micro** (regular or bold, `10px`): support badges, plot readouts, and dense
  timeline annotations. No authored interface text drops below this floor.

**The Labels Stay Labels Rule.** Do not enlarge labels into decorative
headlines. Hierarchy must stay compact enough that the graph, timeline, and
analyzers retain visual priority.

## Layout

The Workbench opens at `1180 × 700px`, has a minimum usable size of
`960 × 640px`, and is resizable. A compact `46px` toolbar spans the top. The
main area uses `10px` outer insets and three working zones: a library column, a
fluid central canvas, and an inspector workspace, separated by `8px` gutters.

The current Workbench exposes Patch, Automation, Analysis, Performance and
Samples modes. Patch/Automation belong to the Structure editing context;
Structure supports primary and secondary graph views with an adaptive split,
separate layout state and a stable nested-Sound breadcrumb. Patch keeps the
library, graph and inspector relationship; Automation focuses Timeline editing.
Performance gives the main area to controls, snapshots and the bounded Live
Grid. Samples gives it to sample-map and waveform authoring. Analysis gives it
to partial-track editing and analysis workflows. These focused modes do not
need to keep a reduced graph visible merely to satisfy the old three-column
layout. Source authority is WorkbenchUiPolicy and WorkbenchLayout.

Graph nodes retain the established dimensions and semantic port/wire system.
The largest continuous area belongs to the user's active task: a graph in
Structure, performance controls in Performance, or the editor in Samples and
Analysis. Proposed Timeline, grid and layout extensions are not current UI
claims.

Spacing is dense and discrete: `4–6px` separates controls within a row, `8px`
separates sibling regions, and `10–14px` forms panel interiors and heading
insets. Three desktop width classes protect the graph and measurement surfaces:
compact below `1100px`, regular from `1100–1499px`, and expanded from `1500px`.
Compact mode starts with `180px` and `300px` side zones at the minimum width,
then interpolates toward the regular `220px / 330px` composition while moving
secondary edit commands to menus. Narrow regular windows keep those secondary
commands in menus until enough status space is available. Expanded mode grows
the supporting zones smoothly up to `240px / 360px`. Height policies similarly
interpolate Timeline, Analysis, Diagnostics, and Event Values. Below `1100px`,
the library and right workspace become mutually summonable docks so the graph
never collapses between two narrow columns. The graph supports pan, zoom, and
Command-0 zoom-to-fit; workspace changes reframe it without changing project
state.

**The Active Task Owns the Center Rule.** Side panels may be dense, but the
active graph, performance surface or editor remains the largest continuous
work area. Supporting controls must not reduce it to a narrow strip.

## Elevation & Depth

Depth comes from glass over ink. A glass panel is painted in four layers
(`WorkbenchStyle::paintPanel`): a vertical gradient around Glass Surface, a
translucent white sheen fading out over the top `56px`, a one-pixel highlight
line along the top edge that fades out toward the corners, and a one-pixel
light edge (`rgba(255,255,255,0.085)`). Its parent paints a soft shadow
beneath it (`18px` blur, `6px` down, black at `0.6`); the Workbench draws the
backdrop and all panel shadows once per layout into a cached image, so panels
repaint over a blit. Wells sink the other way: a darker fill with an inner
shadow along their top. Raised controls and tiles get a gentler version of the
glass gradient and highlight.

Glow is reserved for signal: cables, scope and spectrum traces, waveform and
automation lines, the time cursor, the selected node and lit Live Grid cells
carry a soft halo in their own colour. Surfaces themselves never glow.

**The Enhanced Contrast Rule.** With Enhanced Contrast on, sheen, glows,
backdrop glows and translucency are dropped: panels become solid with a
`1.5px` Interactive Outline, field and control outlines turn to Enhanced
Outline (`#d4e2ef`), and focus outlines grow to at least `3px`.

## Shapes

The form language is softened rectangles. Panels and the graph canvas use a
`9px` radius, Event Value and performance tiles `7px`, graph cards and Live
Grid cells `6px`, buttons and combo boxes `5px`, fields `4px`; the view-status
and context badges, severity tags and scrollbar thumbs are pills.

Borders stay thin. Graph cards use a faint one-pixel edge at rest; selected and
diagnostic cards use a `2px` Selection Frost or Fault Coral outline with a soft
halo. Ports keep their closed `8px` family, now ringed in Well so they read
against cables and cards: circle for Audio, square for Control, diamond for
Event/Note, hexagon for Data. Diagnostic badges remain `14px` circles.

## Components

Components should feel compact and tactile: native controls seated in lit
glass.

### Buttons

- **Shape:** `5px` radius, a raised gradient around Raised Surface, a top
  highlight line and a faint light edge.
- **On:** a toggled-on button (a workspace tab, a mode button) fills with the
  signature gradient and writes its label in Accent Ink.
- **Hover / Press:** hover lifts the fill and edge; press darkens. Disabled
  controls keep their shape at about half opacity.
- **Focus:** keyboard focus adds a Selection Frost outline of at least `2px`.
- **Own colours:** a button that sets its own colour (a support badge, the
  Timeline's Cache button) keeps it, with the same gradient and edge.

### Chips

- **Support badges:** `72 × 18px`, white bold microtype on the status fill,
  `4px` radius, growing from a one-letter capsule as the library widens.
- **Severity pills:** Diagnostics rows tag `INFO`, `WARNING` and `ERROR` in
  pills tinted with Data Periwinkle, Control Amber and Fault Coral.

### Cards / Containers

- **Panels:** glass, `9px` radius, soft shadow; titles in bold Reading Frost
  after a short vertical signature-gradient bar.
- **Graph canvas:** a rounded well, lit slightly toward its middle, with a dot
  grid (a larger dot every fourth point).
- **Tiles:** Event Value, meter and Live Grid tiles use Control Tile with a
  gradient cap in their kind's colour; selected tiles add the focus outline
  and halo.
- **Lists:** selected rows are rounded Selected Row fills.

### Inputs / Fields

- **Style:** text fields are recessed wells with a faint edge; combo boxes are
  raised with a chevron and turn their edge violet while open.
- **Focus:** an editable field draws a Selection Frost outline (at least
  `2px`) with a soft halo; with Enhanced Contrast, the outline alone.
- **Sliders:** a recessed track, its filled part in the gradient, and a bright
  thumb with a violet ring and soft glow. Rotary controls draw a recessed ring
  and a gradient value arc.
- **Popup menus:** a dark glass list; the highlighted item fills with the
  gradient and Accent Ink text.
- **Fly-by help:** stays a yellow box with dark text, now rounded and lit.

### Navigation

The toolbar is a compact horizontal rail for transport, frequent edits, Save,
workspace modes, persistent project/compile/transport state, and overflow.
Native File, Edit, Transport, View, and Help menus carry the complete command
set and standard shortcuts; compact mode moves edit actions out of the rail
without removing them. Patch, Automation, Analysis, Performance and Samples
provide task-level navigation, while View summons supporting surfaces.
Structure preserves its graph split and per-view presentation. Reusable Sounds
carry a stable breadcrumb (`Project / Root` or the selected definition) and a
compact `Root` action. Navigation remains textual and native.

### Workspace and performance state

Project presentation owns authored structure layouts, performance control
positions, meter placement and visibility. WorkspaceSession separately owns
local views, split positions, selections, window/session preferences and
acknowledged live presentation state. Merely navigating or changing focus must
not rewrite musical definitions. Structural edits and explicit snapshot/capture
actions use the project transaction boundary.

Performance controls, snapshot banks and Live Grid cells expose active,
queued, unavailable and stale states in text as well as color. The current
Live Grid launches one prepared program at a time; a parallel multi-grid
surface is future work. Samples and Analysis show source,
selection, pending job, cancellation and invalidated-derivative state without
silently replacing the last-good result.

Keyboard, menu and pointer actions must use the same target and availability
model. After switching workspace or closing a transient panel, restore focus
to a valid visible control. Keep transport/Stop accessible in compact layouts.
The existing flat performance presentation does not provide hierarchical
control layouts; that extension is future work.

### Graph Nodes

Graph nodes are the signature component: fixed `178px` cards on a `32px`
grid, each a small glass card casting a short shadow, its header lit with its
category's colour under a gradient cap, then a bold title, the muted stable ID
and typed geometric ports labelled `A`/`C`/`E`/`D`. Cables keep their port
kind's colour with a soft glow under the core stroke; their dash pattern still
communicates signal rate, the destination terminal repeats the source type
geometry, and Gate/Trigger edges add square/triangle markers. Selected-incident
connections gain a neutral outer stroke while unrelated context dims. The
Sound whose parameters are open below keeps its gray diagonal pattern.

### Timeline and Analysis Plots

Plots are recessed wells with a fine display grid. Timeline rows alternate
over the well, each led by a gradient stripe in its kind's colour; clips are
tinted glass in their own colour (Signal Aqua by default) with a lit top band,
fades and repeat marks; automation lines glow in Control Amber over a faint
fill; the yellow time cursor carries a soft halo. Analyzer traces, scopes and
spectra glow in their signal colours, the spectrum filled beneath its line;
the Partial Tracks editor keeps its black plot and amplitude colours. Sample
maps tint each region in a cycling gradient colour over faint black-key
columns, and waveforms glow in Signal Aqua. The transport time is an LCD-like
readout in Talas Aqua.

## Do's and Don'ts

### Do:

- **Do** keep the active graph, performance surface or editor dominant and
  use the compact, regular and expanded work-zone policies rather than
  inventing one-off fixed widths.
- **Do** paint through the tokens and `WorkbenchStyle` recipes: glass for
  panels, wells for displays and fields, tiles for cards.
- **Do** keep the signature gradient for what is on or chosen, and the signal
  and category colours for their meanings.
- **Do** retain explicit text labels and status messages alongside
  colour-coded states.
- **Do** keep text at `4.5:1` or better, including on the gradient and on tinted
  tiles.
- **Do** check every change with Enhanced Contrast on.

### Don't:

- **Don't** introduce faux hardware panels, wood, brushed metal, rack screws,
  photographic knobs, or other retro-skeuomorphic synth cues.
- **Don't** make surfaces glow, or add blur that hides content; glow belongs
  to signals.
- **Don't** use the gradient or a signal colour merely to make a region feel
  more colourful.
- **Don't** replace dense operational labels with unlabeled icon-only controls.
- **Don't** invent a display typeface, oversized headline tier, or marketing
  composition inside the Workbench.
