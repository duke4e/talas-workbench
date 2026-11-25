# ADR-0012: Missing-node preservation and non-execution

Date: 2026-08-03

Status: Accepted under the owner's standing auto-approval

## Context

The current project reader can parse a node whose `typeId` is not registered,
but `ProjectEditor::replaceDocument` rejects it and the Workbench therefore
cannot open the project. Removing the node would also remove its connections,
portable presentation, parameter overrides, snapshot targets, and any opaque
component state. Treating an unknown node as a neutral runtime placeholder
would be worse: it would silently change the signal graph while appearing to
support a component that is not present.

F5a must preserve missing component data before F5b introduces executable
user-component bundles. Preservation therefore cannot depend on discovering,
loading, linking, or executing code from a project or package.

## Decision

### One node authority with an attached preservation capsule

`ProjectNode` remains the only node and graph identity. Project format 13 adds
an optional schema-version-1 `preservedComponent` capsule to that node. The
existing node continues to own:

- `instanceId` and original `typeId`;
- typed project parameter overrides;
- graph connections through the existing `ProjectGraph`;
- root or Sound-definition scope through the containing graph; and
- portable coordinates through `ProjectPresentationState`.

The capsule owns only data that cannot be reconstructed when the descriptor is
absent:

- exact component type version and state schema version;
- canonical base64 opaque state bytes;
- last-known display name and optional stable bundle identifier;
- frozen input/output port contracts;
- frozen parameter identity/kind/domain contracts; and
- a non-executable recovery hint.

The capsule does not duplicate `instanceId`, `typeId`, connections, coordinates,
or typed parameter values. Deleting or undoing one node therefore cannot leave
an orphaned second record.

Format 13 limits capsule and aggregate payload sizes, port/parameter counts,
identifier lengths, and decoded state bytes before publication. Base64 must be
canonical and padded; decoding is validation-only and never invokes component
code. Versions 1 through 12 migrate in memory with no capsule and no filesystem
access.

### Derived availability, never a persisted execution claim

Availability is derived from the supplied `NodeRegistry`:

- `Native`: a node without a capsule resolves through its ordinary descriptor;
- `Missing`: a capsule exists but its `typeId` has no registered descriptor;
- `IncompatibleVersion`: type identity exists but exact type/state version is
  not admitted;
- `IncompatibleInterface`: version matches but frozen port or parameter
  identity contracts do not;
- `Resolved`: a capsule and registered descriptor match the frozen contract.

The project never serializes `Missing` or `Resolved`; those are environment
observations. Exact-match repair retains the same node, connections,
presentation, parameters, snapshots, and opaque bytes. F5a defines no state
migration callback. A version or interface mismatch fails clearly and remains
preserved for a future explicitly reviewed migration path.

`NodeTypeDescriptor` gains an internal versioned state/interface contract.
Built-in descriptors default to the reviewed built-in version and remain
behavior-compatible. F5b may later populate the same contract from a verified
bundle, but may not reinterpret format-13 bytes.

### Preservation validation and execution validation are separate

Document/editor validation may use a frozen non-executable descriptor derived
from a valid capsule. This validates the existing topology and lets unrelated
edits, movement, disconnection, deletion, Save, Save As, autosave, recovery,
and packaging proceed without deleting data.

Execution validation never substitutes that frozen descriptor for an
executable implementation. A `Missing`, `IncompatibleVersion`, or
`IncompatibleInterface` node produces a structural, scoped diagnostic before
Sound expansion, resource preparation, or runtime allocation. A failed stage
leaves the active graph unchanged. A `Resolved` capsule may use an already
registered trusted descriptor; no code is obtained from project/package data.

The editor disables parameter/state mutation and new connections on an
unresolved node, but permits move, disconnect, and explicit delete. Delete
removes the node, its connections, presentation entry, and capsule as one
undoable edit. Exact repair is identity-preserving and does not rewrite opaque
bytes.

### Package and recovery security boundary

F5a packages only canonical project JSON and the assets already reachable
through existing authorities. A bundle identifier and recovery hint are inert
text. Package validation rejects executable libraries, loader directives,
absolute executable paths, traversal, symlink escape, or any extra component
payload location introduced through the capsule.

Open, Save As, autosave, crash recovery, and external-conflict handling preserve
capsule bytes exactly. None scans global install locations or the network.

### Workbench representation

Structure renders an unresolved node as a real selectable placeholder using
its frozen port layout, stable node ID, last-known display name, and a textual
Missing or Incompatible badge. It is never omitted from the canvas. Inspector
shows type/version, preservation size, recovery hint, and compatibility reason;
ordinary parameter controls are disabled. Text, iconography, focus, and
accessibility names carry status independently of color.

The project may be edited and saved while unresolved. Play/audition/export is
blocked with a scoped diagnostic naming the node and reason. If the same
Workbench process later receives a matching trusted registry, refresh/repair
reclassifies the existing node without moving or recreating it.

## Compatibility and migration

- Formats 1 through 12 read as format 13 with no preservation capsules.
- Ordinary built-in and reusable Sound nodes retain existing JSON meaning and
  runtime behavior.
- Unknown legacy nodes without a capsule still fail closed because their
  interface and state cannot be proven.
- Format-13 capsules round-trip canonically even when no matching descriptor is
  installed.
- Downgrading to format 12 is not automatic because it would discard opaque
  component data.

## Alternatives rejected

- A top-level missing-node table duplicates scope and node identity and can
  orphan records after graph edits.
- Removing unknown nodes destroys user data.
- Registering a neutral placeholder makes an unsupported graph appear
  executable and can produce misleading audio.
- Preserving only typed parameters loses arbitrary component state.
- Parsing arbitrary opaque JSON couples the host to an unknown schema; a
  bounded base64 byte envelope preserves it without interpretation.
- Loading code from a project package before F5b violates the dependency gate
  and expands the attack surface.

## Acceptance evidence

- format-13 canonical round-trip and versions 1-through-12 migration;
- malformed, duplicate, oversized, non-canonical, and orphan capsule rejection;
- unknown capsule open/edit/save/recovery/package continuity with byte-exact
  opaque state;
- frozen-port connection and nested-Sound preservation;
- exact-match repair retaining identities, topology, presentation, snapshots,
  typed parameters, and opaque bytes;
- clear version/interface mismatch diagnostics;
- compile/stage/export refusal with unchanged active generation;
- package tests proving no executable discovery, loading, or execution; and
- selectable accessible Workbench placeholders in regular and compact layouts.
