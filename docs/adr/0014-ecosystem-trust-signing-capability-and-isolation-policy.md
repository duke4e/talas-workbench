# ADR-0014: Ecosystem Trust, Signing, Capability, and Isolation Policy

Date: 2026-08-03

Status: Accepted under the owner's standing auto-approval

## Context

ADR-0012 preserves unavailable component data without executing it. ADR-0013
proves a provisional internal C ABI across three built-in kernel families and
contains controlled faults and crash probes, but deliberately provides no
dynamic loader or public SDK.

Gate 4 requires an explicit signing, trust, capability, and isolation policy
before executable user components can be designed. A loader cannot safely own
this decision: signatures, checksums, probing, and process boundaries answer
different questions and none is an interchangeable synonym for trust.

## Protected assets and threat boundary

The policy protects:

- Workbench and render-process integrity and availability;
- audio callback deadlines and prepared graph generations;
- project, package, recovery, preset, and user files;
- owner credentials, signing identities, devices, and network authority; and
- truthful registry, capability, and diagnostic state.

Future bundle bytes, manifests, signatures, paths, identifiers, diagnostics,
state migration data, dependencies, and IPC messages are attacker-controlled
until validated. A signed component may still be malicious, vulnerable,
incorrect, realtime-unsafe, or over-privileged.

## Decision

Talas separates six independent facts:

1. **Integrity:** the inspected bytes match the authenticated artifact.
2. **Signer identity:** a supported verifier identifies the signing principal.
3. **Owner authorization:** the owner approved one exact artifact and authority
   set under one policy version.
4. **Declared permissions:** the component requests only known execution
   permissions supported by its role and host profile.
5. **Probe evidence:** a bounded, versioned probe completed without a
   classified rejection, crash, timeout, or protocol error.
6. **Containment readiness:** a tested platform isolation backend can enforce
   the approved permissions and failure policy.

No subset silently stands in for another. A checksum without authenticated
signer evidence is change detection, not a trust anchor. A valid operating-
system signature or notarization establishes platform-defined identity and
integrity evidence, not Talas authorization. A clean probe is compatibility
evidence for that run, not proof of safety. A sandbox label without enforced
process and permission boundaries is not containment.

## Execution dispositions

The internal policy evaluator returns one of four closed dispositions:

- `Reject`: malformed, contradictory, unsupported, invalid, or explicitly
  denied evidence;
- `PreserveOnly`: retain bounded project/component data but do not inspect or
  execute component code;
- `EligibleForProbe`: the exact approved candidate may enter the closed
  validation process, but is not executable; or
- `EligibleForIsolatedActivation`: every required fact is current and a tested
  isolation profile exists.

Only product-built code linked into the shipped binary is eligible for
in-process execution, and it follows the existing static registry/B8 path
rather than gaining trust from this evaluator. Future user executable code is
never eligible for in-process activation. If isolated activation cannot be
enforced on the current platform, the result remains preserve-only even when
signature, approval, permissions, and probe evidence are otherwise valid.

## Exact owner approval

An owner approval binds one normalized tuple:

- bounded stable bundle identifier;
- semantic version;
- authenticated content digest;
- signer identity;
- requested permission set; and
- policy version.

The tuple matches exactly. Changed bytes, version, signer, permissions, or
policy version require a new approval. Missing, malformed, oversized, stale,
revoked, partially matching, or ambiguously normalized evidence cannot inherit
authorization. Approval is never inferred from a project, package, recovery
hint, filesystem location, display name, or prior successful load/probe.

The evaluator derives the approval match from the candidate and stored tuple;
it does not accept a caller-asserted `ExactMatch` state. `Verified` signature
evidence is likewise versioned and must carry the exact candidate content and
signer digests. Missing bindings, a different verifier profile, or evidence for
another artifact/signer rejects before probe eligibility.

This record does not freeze a persisted trust-store format. The first policy
implementation uses in-memory evidence so persistence, credential storage,
revocation refresh, and migration can receive their own bounded design.

## Permission policy v1

Execution permissions are distinct from NodeDescriptor DSP support
capabilities. They use a separate closed type and authority.

Policy v1 can describe only:

- realtime audio processing inside the isolated host contract;
- host-mediated immutable resource reads;
- host-mediated state migration; and
- declarative UI descriptions rendered by product code.

Direct filesystem, network, subprocess, device, credential, arbitrary UI-code,
host-memory, registry, and project-document authority is not grantable in v1.
Unknown bits and unsupported combinations reject. Requested permissions must be
both owner-approved and enforceable by the chosen isolation profile. Signatures
and probes cannot add permissions or turn a denial into a grant.

## Isolation and failure policy

Any future executable user component runs in a separate process behind:

- bounded, versioned IPC and host-owned shared buffers;
- explicit copied/validated metadata and diagnostics;
- no direct project, filesystem, network, credential, or device access;
- watchdog, timeout, crash quarantine, and explicit off-callback teardown;
- generation-scoped activation and failure-atomic graph replacement; and
- no allocation, lifetime transition, logging, blocking, or process control on
  the audio callback.

The isolation backend must report a supported, tested profile that covers the
requested permissions. Merely spawning another ordinary process is
insufficient. The supported macOS build now contains one closed product-fixture
backend. Its opaque adapter emits readiness only for the production service
after the exact configuration, handshake, prewarm, sandbox/packaging profile,
live connection, host-owned shared storage, watchdog, and generation checks
hold. The evidence is bound to the live session generation, shared-storage
identity, and validation nonce, and copied views revoke when that session stops
or quarantines. Callers cannot construct or override its individual claims.

Every unsupported platform and every absent, test-only, stopped, or failed
backend reports `Unavailable` with zero profile, safeguards, permissions, and
session bindings. Any mixture is contradictory evidence and rejects. This
product-fixture containment result does not identify or authorize a user
artifact. Executable use requires the separate signature, exact owner approval,
clean probe, one-use activation authority, sealed Runtime, and transient graph
chain described below; isolation evidence alone never grants it.

B8 subprocess probing remains a separate pre-activation phase. Its input stays
closed, module-controlled text never enters a shell command, and successful
probing never confers trust. Crash, timeout, protocol error, stale result, or
evidence mismatch quarantines/preserves without weakening the main process. A
clean result is bound to the complete exact candidate fingerprint, the closed
probe protocol version, a nonzero artifact generation, and a validation-session
nonce. It cannot be replayed across different bytes, signer, identity, version,
permissions, policy, generation, or validation session.

## Diagnostics and product truth

Policy outputs expose stable bounded diagnostic codes and product-authored
summaries. Module-controlled text is never promoted into an unbounded log,
command, path, accessibility label, or trust explanation.

Workbench/CLI language must distinguish preserved, rejected, probe-eligible,
and isolated-activation-eligible. It may not describe a preserved component as
installed, trusted, loaded, verified safe, or runnable. Node catalog and
capability projections remain derived from the existing registry and are not
changed by policy evidence.

`docs/governance/component-ecosystem-policy.json` is the machine-readable
state with one macOS product-fixture isolation backend. Schema v8 retains the
accepted `macos-one-shot-xpc-v1` probe boundary and active Step 8 runtime scope,
and now validates the closed Step 9 replacement orchestration contract:
`macos-stateless-realtime-audio-multiple-immutable-resources-v1`,
reachable only through the private Runtime API or the explicit owner-selected
Workbench replacement surface and the accepted
`macos-sealed-runtime-xpc-v1` loading boundary. The current machine state is
`isolated-activation-explicit-replacement`. It permits the Step 4
stateless profile plus one ordered atomic set of at most eight unique
generation-bound immutable project snapshots, at most two of which may be
bounded decoded `Audio`, within 64 MiB of aggregate canonical source bytes.
The replacement addition is
`explicit-owner-confirmed-local-replacement-v1` on an
`owner-selected-workbench-only` surface: it owns a bounded old/new
projection, one move-only session receipt, a private exact Runtime admission
seam, and one generation-scoped off-thread inspection/probe/activation
controller. The macOS Workbench exposes the root-preserved-node
chooser and accessible copyable approve/preview/confirm/cancel surface; exact
approval is session-local and grants no execution or stage authority. The controller
retains live Probe evidence after successful staging and exposes a separate
control-thread adoption point. The shipped Workbench surface admits only
same-kind resource identities; every resource kind transition remains closed
until a dedicated owner-authored asset-kind workflow exists. The private receipt
and bridge validators retain their exact cross-kind checks as a non-product
future boundary. Test-only signature/deadline/chooser seams remain
confined to `TALAS_BUILD_TESTS`; Release uses the production verifier, deadlines,
and explicit owner chooser. No surface can discover or install a component.
Writable, network, compressed, reverse, path, descriptor, enumeration, and
broker authority remain unavailable. Dynamic loading and
user-code execution are exactly
`macos-isolated-probe-and-runtime-only` and
`private-macos-isolated-runtime-only`; replacement activation is reachable only
from the explicit owner-selected Workbench flow. Discovery,
installation, trust persistence, registry promotion, project-format changes,
CLI activation, in-process user code, and a public SDK remain disabled.

`tools/check_component_ecosystem_policy.py` accepts only complete dependency-
ordered states from probe authorization through active aggregate-budgeted
multiple-resource mediation and the dependency-ordered explicit-replacement
progression.
Mixed claims fail, and every pre-Step-9 state rejects replacement preview
contracts.
The sandbox-only state permits the third exact runtime XPC with App Sandbox but
no loader or library-validation exception. Runtime validation adds one target-
owned loader, the exact exception, private authorization/protocol seams,
ordered sealed-before-bytes transfer, and canary/deadline/quarantine tests, but
still makes no graph-activation claim. Active Step 4 additionally requires
transient stateless graph integration; active Step 5 requires the exact
host-authored Table contract, mediated-digest graph binding, and real Table
transient-graph integration target. Active Step 6 additionally requires the
closed Binary graph author/validator declarations and real Binary
transient-graph integration target; those declarations fail under authorized
or validation-only policy states. Active Step 7 additionally requires the
closed Audio grant/transport/worker/ABI-v3 declarations, exact host-authored
Audio graph contract and validator, and real Audio transient-graph integration.
Active Step 8 additionally requires the closed API-v4 set contract, bounded
XPC transfer, product Runtime helper wiring, ordered Host/graph contracts, and
real mixed-resource transient-graph integration. Every earlier state rejects
those executable set/stream/graph declarations. The gate also rejects detached
or over-budget grants, writable/network/compressed/reverse streams,
paths, descriptors, arbitrary stream brokers, loader-source reuse,
entitlement drift, caller-owned activation authority, canonical-registry
promotion, canonical-project-format coupling, public installation/export
surfaces, in-process loading, and unsupported product claims. The active
foundation permits only its exact private transient graph path plus the explicit
owner-selected Workbench replacement action. That action changes no canonical
registry or project bytes and adds no CLI, install, discovery, or SDK surface.

The loader audit covers
direct POSIX/Windows/Mach-O primitives,
JUCE DynamicLibrary, CoreFoundation/Foundation bundle loading, Swift Bundle
loading (including split construction/use), `NSAddImage`, and direct or
`find_library`-aliased dynamic-loader build linkage. In the inactive state,
every loader form is forbidden. In the active probe-only state, the single
exact POSIX loader source is the only exception; platform bundle-loader types
and every other direct loader API remain denied. Direct, directory-wide,
aliased, and absolute `libdl` build linkage spellings are rejected, including
CMake `LINKER:`/`SHELL:`, GNU `-l` exact-name, `-Wl`, and `--library` option
families.

The selected Step 3 architecture is a second, disposable sandboxed XPC service,
not the long-lived realtime helper and not an ordinary spawned child. It may
receive only previously frozen entry bytes under one fixed product protocol and
symbol. It may produce bounded probe evidence, never registry or activation
authority.

The accepted Step 4 foundation adds a third activation-only runtime XPC. Its
initial scope is
one root preserved node, one stateless audio-only instance, exact 44.1/48 kHz
and 256/512-frame profiles, static prepare parameters, one-block declared
latency, opaque single-use authorization, and transient F5a resolution without
document or canonical-registry mutation.

The active Step 5 successor adds one closed resource profile. One `Resource`
parameter binds one positive-generation project `Table` snapshot to an opaque
single-use grant. Only bounded canonical F64 little-endian bytes cross XPC;
the helper validates them into a read-only private mapping before component
load, exposes one preparation-only ABI-v2 resolver, and pins the mapping through
off-callback graph retirement. The graph contract rechecks parameter/resource
IDs, kind, generation, canonical source hash, mediated payload digest, element
count, and byte count before failure-atomic staging. It adds no general resource
registry, filesystem authority, public action, or canonical project mutation.

The active Step 6 successor extends that same one-resource profile with the
closed `Binary`/raw-octet alternative. Binary payloads contain 1 through
1,048,576 bytes, require `elementCount == byteCount`, and require the canonical
source SHA-256 to equal the mediated-payload SHA-256 because no transformation
occurs. The same one-shot grant, XPC envelope, read-only non-executable private
mapping, prepare-only ABI-v2 resolver, binding lifetime, callback-boundary
adoption, and off-callback retirement remain in force. Cross-kind replacement
requires a strictly higher generation and new content identity. Binary remains
opaque data: it is not executable code, decoded audio, a streaming source,
component state, or a general filesystem service.

The active Step 7 successor adds the third and final closed kind for this
single-resource slice: one 1–64 MiB mono/stereo PCM16-WAV `Audio` snapshot. The
Host freezes and revalidates the exact source bytes, transfers them through an
anonymous page-rounded mapping, and retains no durable path. Runtime copies the
bytes into its private read-only/non-executable mapping, owns the bounded
channel-preserving decoder/refill worker and fixed queues/cache, and exposes
only one preparation-resolved ABI-v3 token plus one nonblocking bounded read per
component callback. The graph contract rechecks the exact parameter/resource
IDs, generation, source hash, channel/rate/frame shape, encoding, and closed
stream profile without reopening or decoding the project path. Audio-to-Audio
replacement requires a higher generation and new content; every Audio↔Table or
Audio↔Binary transition remains closed. Underrun is deterministic silence,
worker failure quarantines the live graph, and retirement stops the worker and
releases mappings off callback.

The active Step 8 successor composes those closed kinds into one atomic ordered
set of at most eight unique Resource bindings, at most two Audio streams, and
64 MiB of aggregate canonical source bytes. One domain-separated set digest,
one-use authority, bounded XPC array, API-v4 preparation surface, aggregate
budget, and stage lease bind the complete set. Each Audio member retains an
independent worker, data plane, direct token, consumed bit, and authenticated
health slot. Runtime and graph admission revalidate every member, set order,
snapshot, static parameter, page-rounded transport budget, and immutable
history before a single callback-boundary adoption. Any member failure or
revocation rejects or quarantines the complete set; no partial graph or history
is published. Table/Binary cross-kind replacement still requires a higher
generation, while every Audio cross-kind transition remains closed.

## Public SDK gate

Accepting this record closes the policy-decision prerequisite only after its
pure fail-closed evaluator and tests pass. F5b Steps 1 and 2 add an internal
macOS product-fixture isolation foundation and a private non-executable bundle
inspection schema. F5b Step 3 contains the exact disposable one-shot probe
helper and its single isolated loader source. Step 4 contains the private
runtime helper, opaque activation authority, and transient graph binding. Step
5 adds the exact immutable-table mediation profile, Step 6 adds its bounded
immutable raw-Binary alternative, Step 7 adds bounded pathless decoded Audio,
and Step 8 composes those exact kinds into one bounded atomic set. Step 9 adds
one explicit owner-selected root-preserved-node Workbench action that composes
the pure preview, one-use confirmation, disposable Probe, sealed Runtime
activation, and control-thread stage admission. It adds no discovery, installer,
registry promotion, persistent trust store, CLI activation, or SDK.

The retained active Step 8 runtime boundary and its Step 9 replacement surface
require the complete sealed runtime, opaque
authorization, realtime, transient-graph, Table/Binary/Audio transfer and
mapping, raw identity, aggregate budget/set digest, bounded independent stream
workers/health slots, ABI-v4 scope,
failure-atomic replacement, packaging, failure, and test declarations together.
State migration, writable/network/compressed/reverse resources,
discovery, installation,
packaging/recovery distribution, multiple instances, dynamic controls,
declarative UI, and public SDK/versioning remain successor gates. No successful
policy evaluation ever authorizes `dlopen`,
`LoadLibrary`, or an equivalent Workbench/realtime-host in-process path for
user code.

## Rejected alternatives

- Treating a valid signature as sufficient trust confuses identity with owner
  authorization and containment.
- Trusting a checksum or known path permits artifact replacement and has no
  signer authority.
- Allowing signed user code in process exposes the Workbench to memory
  corruption and hard crashes that a C ABI cannot contain.
- Treating a clean probe as a permanent safety verdict ignores environment,
  input, timing, and later-code-path differences.
- Reusing NodeDescriptor DSP capabilities as permissions conflates product
  support claims with security authority.
- Granting broad filesystem/network/device access and relying on documentation
  is ambient authority, not enforcement.
- Persisting approvals before the exact fingerprint and migration rules are
  proven risks replay and silent privilege expansion.

## Consequences

- Gate 4 gains a deterministic, reviewable policy boundary before loader work.
- F5a preservation remains the safe fallback on every unsupported or invalid
  path.
- An isolated host is a real dependency for executable user components, not an
  optional hardening step.
- Platform verification and sandbox implementations may differ, but must
  produce the same closed evidence vocabulary and fail-closed disposition.
- Public SDK and executable-bundle delivery remain materially larger future
  packages with explicit security and product review.
