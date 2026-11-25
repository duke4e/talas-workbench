---
name: "Talas Workbench"
description: "A precise nocturnal laboratory for building, routing, and performing sound."
colors:
  carbon-night: "#1c2026"
  instrument-graphite: "#252c35"
  steel-outline: "#3a4654"
  interactive-outline: "#607184"
  reading-frost: "#e8edf2"
  muted-steel: "#aab7c4"
  selection-frost: "#d9e7ff"
  signal-teal: "#4ec7b5"
  control-amber: "#f0b35b"
  event-magenta: "#d97dbb"
  data-periwinkle: "#8da8e8"
  output-blue: "#7fa7ff"
  fault-coral: "#f06a6a"
  runtime-green: "#287a55"
  subset-ochre: "#8c5b12"
  placeholder-rose: "#a94f58"
  utility-slate: "#57708c"
  grid-graphite: "#222933"
  selected-row: "#33465a"
  control-tile: "#202831"
  widget-surface: "#263238"
  widget-outline: "#8e989b"
  widget-text: "#ffffff"
  widget-fill: "#42a2c8"
  widget-highlight: "#181f22"
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
rounded:
  square: "0px"
  field: "3px"
  plot: "4px"
  card: "6px"
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
    backgroundColor: "{colors.widget-surface}"
    textColor: "{colors.widget-text}"
    rounded: "{rounded.card}"
    height: "32px"
  text-field:
    backgroundColor: "{colors.widget-surface}"
    textColor: "{colors.widget-text}"
    rounded: "{rounded.square}"
    height: "24px"
  combo-box:
    backgroundColor: "{colors.widget-surface}"
    textColor: "{colors.widget-text}"
    rounded: "{rounded.field}"
    height: "24px"
  panel:
    backgroundColor: "{colors.instrument-graphite}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.square}"
    padding: "8px"
  graph-node:
    backgroundColor: "{colors.instrument-graphite}"
    textColor: "{colors.reading-frost}"
    rounded: "{rounded.card}"
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
    textColor: "{colors.widget-text}"
    rounded: "{rounded.plot}"
    height: "18px"
    width: "72px"
---

# Design System: Talas Workbench

## Overview

**Creative North Star: "The Night Signal Laboratory"**

Talas is a precise, nocturnal, instrument-like workspace: a dark technical
field where signal flow, state, and structure become legible through restrained
color. The interface should feel like a working laboratory after the room
lights are lowered, not like a themed dashboard. Density is intentional because
the user is operating a serious sound instrument, but every layer must remain
scannable and every accent must earn its place.

The visual system is layered and tactile without becoming dimensional. Cool
graphite surfaces, thin steel outlines, compact native controls, and small
system typography create the body of the instrument; vivid signal colors carry
meaning across ports, cables, timelines, analyzers, and status. Retro
skeuomorphic synthesizers and glossy consumer-DAW chrome are explicit
anti-references.

**Key Characteristics:**

- Dark, compact, instrument-like desktop density.
- Color carries signal semantics before decoration.
- Tonal layering and one-pixel outlines replace shadows.
- Native platform typography keeps the interface operational.

## Colors

The palette is a cold graphite instrument body activated by a compact set of
high-legibility signal colors.

### Primary

- **Signal Teal** (`#4ec7b5`): identifies audio ports, cables, active audio
  traces, and the leading accent on audio-producing nodes.
- **Output Blue** (`#7fa7ff`): identifies final output behavior when it needs a
  distinct role from ordinary audio routing.

### Secondary

- **Control Amber** (`#f0b35b`): identifies scalar/control-rate ports, control
  connections, loop regions, and automation-oriented emphasis.
- **Event Magenta** (`#d97dbb`): identifies event and note-stream routing,
  triggers, and event markers.

### Tertiary

- **Data Periwinkle** (`#8da8e8`): identifies sample buffers, wavetables,
  spectra, and related typed data.
- **Widget Fill** (`#42a2c8`): the inherited JUCE fill for slider thumbs,
  scrollbars, and native control emphasis; it is a control-state color rather
  than a graph signal type.

### Neutral

- **Carbon Night** (`#1c2026`): application and graph-canvas ground.
- **Instrument Graphite** (`#252c35`): panels, graph nodes, lists, and primary
  working surfaces.
- **Grid Graphite** (`#222933`): the quiet 32-pixel graph grid.
- **Control Tile** (`#202831`): inset Event Value control cards.
- **Widget Surface** (`#263238`): inherited JUCE button, input, and selection
  control ground.
- **Widget Highlight** (`#181f22`): inherited pressed or highlighted widget
  ground.
- **Steel Outline** (`#3a4654`): panel, plot, and tile boundaries.
- **Interactive Outline** (`#607184`): idle graph-node boundaries, kept above
  `3:1` against the graph canvas without brightening every structural divider.
- **Widget Outline** (`#8e989b`): inherited JUCE input and control outline.
- **Reading Frost** (`#e8edf2`): primary project-authored text.
- **Widget Text** (`#ffffff`): inherited JUCE control and badge text.
- **Muted Steel** (`#aab7c4`): metadata, categories, IDs, status copy, and
  secondary labels.
- **Selection Frost** (`#d9e7ff`): selected node outline, drag preview, and
  high-confidence active emphasis.
- **Selected Row** (`#33465a`): selected list-row background.

### Status

- **Fault Coral** (`#f06a6a`): compile diagnostics, error borders, and error
  badges.
- **Runtime Green** (`#287a55`): `FULL` support badge.
- **Subset Ochre** (`#8c5b12`): `SUBSET` support badge.
- **Placeholder Rose** (`#a94f58`): `PLACEHOLDER` support badge.
- **Utility Slate** (`#57708c`): `UTILITY` support badge.

**The Signal Is Semantics Rule.** Signal Teal, Control Amber, Event Magenta,
Data Periwinkle, and Output Blue indicate type or routing; they are never free
decorative accents.

**The Frost Means Active Rule.** Selection Frost is reserved for selection,
drag targeting, or another unambiguous active state; ordinary borders remain
steel.

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

The system uses no application-level shadows. Depth comes from tonal steps
between Carbon Night, Instrument Graphite, Control Tile, and Widget Surface,
then from one-pixel Steel or Widget outlines. Selected or invalid graph nodes
increase their outline to `2px`; nested active/error states can add a second
inset outline. The native operating-system window frame may cast its own shadow,
but that is not part of the Workbench surface vocabulary.

**The Tonal Depth Rule.** Surfaces separate by tone and boundary, never by
floating cards, ambient shadows, blur, or glow.

## Shapes

The form language is rectilinear with small, functional softening. Panels and
text fields are square (`0px`). Combo boxes use a tight `3px` radius. Plots,
badges, and selected drag previews use `4px`. Buttons, graph nodes, and Event
Value control tiles use the system's gentlest curve (`6px`). Scrollbar thumbs
also use `4px` rounding.

Borders are thin and structural. Graph nodes and control tiles use a `1px`
outline at rest; selected and diagnostic nodes use `2px`. Node type is further
encoded by a flat `4px` signal-colored cap across the top edge. Ports use a
closed `8px` semantic family—circle for Audio, square for Control, diamond for
Event/Note, and hexagon for typed Data—while diagnostic badges remain `14px`
circles. This provides controlled geometric contrast inside the mostly
rectangular system without making colour the only signal authority.

## Components

Components should feel compact and tactile: native controls seated inside a
purpose-built instrument chassis.

### Buttons

- **Shape:** gently curved native rectangle (`6px` radius) with a `1px` Widget
  Outline.
- **Primary:** Widget Surface with Widget Text; toolbar buttons are `32px` high
  and vary in width to fit explicit action labels.
- **Hover / Focus:** JUCE raises contrast slightly on hover or press; keyboard
  focus adds a `2px` Selection Frost outline. Disabled controls retain their
  shape at approximately half opacity.
- **Secondary:** there is no decorative hierarchy of filled versus ghost
  buttons; importance comes from placement, availability, and precise labels.

### Chips

- **Style:** support-level badges are `72 × 18px`, white bold microtype on a
  semantic status fill, with a `4px` radius.
- **Compact state:** the badge grows continuously from a `22px` one-letter
  capsule to the full `72px` label as the library widens, so prototype names
  keep usable measure; the full maturity label remains in the accessible row
  name at every width.
- **State:** badge color communicates verified maturity (`FULL`, `SUBSET`,
  `PLACEHOLDER`, or `UTILITY`); it is not an interactive filter state.

### Cards / Containers

- **Corner Style:** main panels are square; graph nodes and virtual controls use
  the `6px` card radius.
- **Background:** Instrument Graphite for panels and nodes, Control Tile for
  inset Event Value controls.
- **Shadow Strategy:** none; follow the Tonal Depth Rule.
- **Border:** `1px` Steel Outline for containers and Interactive Outline for
  graph nodes, strengthened only by explicit state.
- **Internal Padding:** normally `8–14px`; graph-node content uses `7px 10px`.

### Inputs / Fields

- **Style:** Widget Surface, Widget Text, and a `1px` Widget Outline; text fields
  are square and combo boxes use a `3px` radius.
- **Focus:** editable fields strengthen the same outline to `2px`. Do not add a
  detached glow.
- **Error / Disabled:** disabled controls lower opacity; operational errors are
  reported through Fault Coral diagnostics and explicit status text rather than
  silently recoloring arbitrary fields.

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

Graph nodes are the signature component: fixed `178px` cards on a `32px` grid,
with a signal-colored `4px` top cap, bold title, muted stable ID, typed geometric
ports, and color-matched curved connections. Connection pattern communicates
signal rate, the destination terminal repeats the source type geometry, and
Gate/Trigger edges add square/triangle markers. Selected-incident connections
gain a neutral outer stroke while unrelated context dims. Selection and
diagnostics modify the node border without changing the node's base surface.

### Timeline and Analysis Plots

Plots are dark inset rectangles with a `4px` radius and a `1px` Steel Outline.
Timeline lanes use labeled rows and fields, restrained gridlines, Control Amber
for loop regions and automation, and Event Magenta for event markers. Analyzer
traces reuse signal colors so measurement remains visually connected to
routing; the cursor measurement remains visible text and has an equivalent
keyboard and assistive-technology path.

## Do's and Don'ts

### Do:

- **Do** keep the active graph, performance surface or editor dominant and
  use the compact, regular and expanded work-zone policies rather than
  inventing one-off fixed widths.
- **Do** use signal colors only for their established data types, routing, and
  related analysis states.
- **Do** create depth with graphite tonal steps, `1px` structural outlines, and
  `2px` active/error outlines.
- **Do** retain explicit text labels and status messages alongside color-coded
  states.
- **Do** keep controls compact, native, and usable as part of a professional
  sound instrument.

### Don't:

- **Don't** introduce glossy DAW chrome, faux hardware panels, wood, brushed
  metal, rack screws, knobs-as-decoration, or other retro-skeuomorphic synth
  cues.
- **Don't** add card shadows, bloom, glass blur, gradients, or ambient glows to
  surfaces that are currently separated by tone and outline.
- **Don't** reuse Signal Teal, Control Amber, Event Magenta, Data Periwinkle, or
  Output Blue merely to make a region feel more colorful.
- **Don't** replace dense operational labels with unlabeled icon-only controls.
- **Don't** invent a display typeface, oversized headline tier, or marketing
  composition inside the Workbench.
