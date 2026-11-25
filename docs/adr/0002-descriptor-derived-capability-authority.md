# ADR-0002: Descriptor-Derived Capability Authority

Date: 2026-08-01

Status: Accepted under the owner's standing auto-approval

## Context

Node shape is declared in `NodeTypeDescriptor`, maturity is generated into a
support include, runtime coverage is inferred by a reporting script, and
prepared-realtime support is discoverable only from central dispatch source.
Those views can disagree and cannot support honest support decisions.

## Decision

`NodeRegistry` becomes the single in-memory authority for node shape,
implementation binding, and capability metadata. Every built-in descriptor
contains a `CapabilityDescriptor` with orthogonal axes:

- product maturity: the existing `FullRuntime`, `FunctionalSubset`,
  `Placeholder`, or `Utility` label;
- offline execution: `Unsupported`, `Subset`, or `Supported`;
- prepared-realtime execution: `Unsupported`, `Subset`, or `Supported`;
- nested-Sound use: `Unsupported`, `Expanded`, or `Native`;
- state behavior: unknown during migration, stateless, or session-stateful,
  plus reset support;
- timing: latency declaration and typed-event/transport support;
- limitations: stable machine-readable IDs with concise user-facing text.

The four maturity labels and their current values do not change as part of the
schema migration. Adding an execution axis is not a maturity promotion.

The descriptor stores a kernel binding only for migrated nodes. Legacy runtime
bindings remain explicit registry metadata rather than being inferred from a
source-code scan. Registry construction validates that a claimed execution path
has an exact registered binding; the legacy runtime guard consumes that same
binding registry, and a kernel binding never claims unsupported.
An unclassified state or latency axis remains explicitly `Unknown` or
`VariableOrUnknown`; a schema default must never masquerade as measured node
behavior.

## Derived consumers

The same ordered descriptor snapshot feeds:

- generated Markdown/JSON coverage;
- CLI catalog JSON;
- Workbench badges, tooltips, and prototype-browser filtering;
- graph validation and unsupported-context diagnostics;
- conformance test enumeration.

Generated artifacts are projections, never editable authorities. Regeneration
must be deterministic, and a check mode fails on unexplained drift. User-facing
summaries may combine axis and limitation text but must retain limitation IDs in
machine-readable output.

## Promotion rules

A capability promotion requires all of:

1. a registered implementation binding for the claimed context;
2. defining behavior and failure tests;
3. differential/conformance evidence when offline and realtime both claim the
   behavior;
4. zero-allocation realtime evidence where applicable;
5. updated original provenance and limitation records.

Catalog presence, identifier compatibility, compilation success, or a neutral
placeholder is never sufficient. A descriptor may honestly report offline
subset plus realtime unsupported while retaining one maturity label.

## Compatibility and schema evolution

- `typeId`, port IDs, parameter IDs, and the four maturity names remain stable.
- New JSON fields are additive in capability-matrix v2.
- Ordering remains deterministic by `typeId` and then stable limitation ID.
- Unknown future capability values must fail validation rather than silently
  becoming supported.
- No runtime lookup depends on generated documentation files.

## Rejected alternatives

- Keeping Python as the authority would leave runtime/UI validation indirect.
- Deriving support from dispatch source would keep coverage coupled to file
  layout and could not express limitations or partial axes.
- Replacing maturity with one numeric score would hide context and encourage
  feature theatre.

## Acceptance evidence

- Existing 147 descriptors retain their current maturity labels and summaries.
- Matrix v2 reports offline and prepared-realtime independently for every type.
- CLI, generated coverage, Workbench policy, and validation tests compare
  against one descriptor snapshot.
- A deliberately inconsistent binding/capability fixture fails registry
  validation.
