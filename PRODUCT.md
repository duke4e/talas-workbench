# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

The primary user is a professional or advanced sound designer or composer who
builds, performs, and renders complex sounds in a studio or live-performance
setting.

## Product Purpose

Talas is a native desktop Sound Workbench and host-independent audio runtime
for constructing typed sound graphs, controlling them in real time, and
rendering them deterministically offline. It brings reusable Sound
composition, expressions with units, Event Values, MIDI and Timeline control,
analysis, project assets, and dependable project ownership together in one
original, modern application.

Success means an experienced user can move from sound construction to live
control and a reproducible render without leaving the product, while the system
remains candid about unsupported or partially supported behavior.

## Positioning

Talas is a software-only Sound Workbench. It is built around Sound-object
graphs, nested Sounds, expressive parameters, and live Event Values, with a
sample-accurate runtime, failure-safe desktop workflows, and no dependency on
dedicated hardware.

Where a behavior is only partly implemented, the product uses an explicitly
labeled functional subset or a modern equivalent.

Development direction: stabilize first — fix recorded defects and keep the
full local gate green — then broaden behavior, measured by conformance tests.
Catalog presence alone is not support, and no support label is promoted merely
because a historical identifier or neutral placeholder is available.

## Operating Context

Talas targets standalone native use on macOS and Windows. The checked
native Workbench closeout evidence is macOS-local; Windows native observation
remains a separate verification requirement. The `adaptive` classification
means a desktop product respecting its host OS, not iOS or Android support.

Users work directly in the Workbench with audio and MIDI devices, typed node
graphs, reusable Sound definitions, Event Value controls, Timeline automation,
oscilloscope and spectrum analysis, external project assets, and real-time
transport. Projects are stored as `.talas.json` documents or portable `.talaspkg`
packages. Audio can be exported to WAV, and the CLI supports validation,
catalog inspection, graph export, and deterministic offline rendering.

The first screen is the actual workbench. The product is an authoring and
performance tool, not a marketing shell.

## Capabilities and Constraints

- The implementation is original C++20. JUCE provides the standalone desktop
  shell, audio/MIDI integration, and cross-platform UI; the engine remains
  host-independent and plugin-ready. A plugin artifact is not currently part
  of the product.
- The realtime callback must remain bounded and allocation-free: no file I/O,
  expression parsing, locks, exceptions, or UI access on the audio thread.
- Offline and prepared-realtime paths must agree where the product claims the
  same behavior. Graph changes are prepared away from the audio thread and
  swapped atomically.
- Support maturity is part of the product contract. Node maturity is
  reported as `FULL`, `SUBSET`, `PLACEHOLDER`, or `UTILITY`; these
  labels are promoted only with defining implementation and tests.
- Project saves, recovery, packaging, external-change handling, export, and
  release publication must fail safely and preserve user work.
- The product is standalone-first on macOS and Windows. The default core build
  is JUCE-free and network-free; the reproducible Workbench release path may
  fetch the pinned JUCE revision.
- Third-party branding, documentation prose, icons, UI assets, source code,
  and other protected material must not be copied into the product.
- Original Talas code is Apache-2.0 licensed. `third_party/cdp/` is under the
  LGPL-2.1, and a built Workbench is also subject to JUCE's license.
  Historical identifiers are used only for compatibility identification.
- The local repository and workstation are the active development authority.
  GitHub Cloud—including remote repository mutations, hosted Actions, checks,
  pull requests, releases, and billing—is outside the active workflow and must
  not be contacted or used as a progress gate unless the owner explicitly
  reverses this decision. Local Git history, builds, tests, and recorded
  evidence remain authoritative.
- Typed reusable Sound interfaces, per-instance overrides, multiple/nested
  MIDI Voice regions, spectral frames and multichannel contracts already have
  bounded implementations. Remaining work includes broader language and
  expressive controller semantics, live/long-form Timeline execution, parallel
  grid routing, hierarchical control layouts, media and mutable-memory breadth,
  further node behavior, and independent device evidence.

## Brand Commitments

`Talas` is the product name. Product language must be technically precise,
restrained, and honest about maturity, limitations, compatibility, and
provenance. It must not imply affiliation with any other product or vendor.

## Evidence on Hand

- `README.md` documents the implemented Workbench, CLI, project lifecycle,
  realtime engine, analyzers, release path, and current support counts.
- `docs/coverage/` contains generated node-maturity and capability data and
  regeneration instructions.
- `docs/adr/` records the architecture decisions and their known limits;
  `docs/testing/` records the test strategy and conformance evidence.
- `docs/governance/` holds the machine-checked component-ecosystem, node-help,
  and project-format migration policies.
- `examples/`, the automated test suites, Workbench smoke test, CLI, offline
  renderer, and release packaging provide executable product evidence.
- There are no confirmed customer testimonials, public adoption claims, or
  benchmark claims. Future product work must not fabricate them.

## Product Principles

1. **Product truth over feature theatre.** State exactly what works, where it
   works, and what remains a subset or open decision.
2. **Realtime safety is a feature.** Determinism, bounded execution, and safe
   graph replacement outrank shortcuts that endanger live audio.
3. **Compose Sounds, not just nodes.** Reuse, hierarchy, expressive control,
   and performance context should shape the product more than raw node count.
4. **Modernize without erasing the useful model.** Keep strong workflow
   concepts while choosing modern algorithms, formats, and ergonomics.
5. **Protect the user's work and the project's provenance.** Saving, recovery,
   packaging, publication, and asset handling must be explicit and
   failure-safe.
6. **Depth before catalog breadth.** Build the shared semantic and DSP
   foundations that make whole node families work; measure progress by
   executable workflows and conformance evidence rather than node count.

## Accessibility & Inclusion

The desktop Workbench must support a practical native baseline: keyboard
operation for core workflows, readable contrast, status and error information
that does not rely on color alone, and a usable interface when windows or UI
scale change. This is a product requirement, not a claim that the current
interface has passed a formal accessibility audit. No WCAG conformance claim
is made for the JUCE desktop application.
