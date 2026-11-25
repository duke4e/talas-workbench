# ADR-0013: Internal Kernel ABI and Public SDK Gate

Date: 2026-08-03

Status: Accepted under the owner's standing auto-approval

## Context

ADR-0001 established one shared kernel lifecycle and proved it with an
Oscillator source, Filter stateful processor, and ADSR event/control envelope.
Those kernels are C++ implementation types used inside one build. Freezing
their classes, STL views, compiler enum layout, exception behavior, or allocator
ownership as a binary interface would make future toolchain and runtime changes
unsafe.

F5a now preserves unavailable component data without executing it. That makes
an internal ABI decision safe to explore without introducing a loader or
turning missing-node placeholders into executable adapters.

## Decision

Talas adopts a provisional internal ABI v1 with a strict C11-compatible
function-table boundary. It is an implementation contract, not a public SDK.

`include/talas/runtime/KernelAbi.h` is self-contained and crosses the boundary
using only:

- fixed-width integer and IEEE-754 f64 values;
- raw pointers plus explicit fixed-width counts;
- size-tagged records with version fields and reserved-zero tails;
- opaque instance/module context pointers;
- host-owned allocation, deallocation, and diagnostic callbacks; and
- C-linkage lifecycle function pointers returning fixed status codes.

No STL type, C++ class, RTTI object, exception, allocator object, compiler-sized
enum, `bool`, `size_t`, or Talas C++ header crosses the ABI.

## Compatibility and validation

The host validates a module completely before any lifecycle callback. ABI v1
requires:

- an API range containing the current API revision;
- the exact ABI revision and required record prefixes;
- compatible pointer width, endianness, `double` width/IEC-559 behavior, and
  maximum alignment;
- native-endian f64 sample support;
- only known capability/trust bits and zero reserved fields;
- bounded valid UTF-8 module/type identities and deterministic uniqueness;
- complete required lifecycle callbacks; and
- agreement with the existing NodeRegistry/kernel/capability authority.

An additive future tail is tolerated only on a standalone record passed
directly across one call, where v1 reads the proven prefix and ignores the
tail. Repeated array elements and embedded records require the exact v1 size:
v1 deliberately has no element-stride field, so accepting a larger element
would make pointer arithmetic ambiguous. A larger standalone struct is not
compatible merely because its prefix fits: unknown flags, versions, or nonzero
reserved fields still reject. The host copies validated metadata before
publication; module string views remain valid only under the immutable module
lease.

## Lifecycle and realtime rules

The ABI follows ADR-0001:

1. registration/validation;
2. create and prepare outside the callback;
3. reset at an explicit safe boundary;
4. bounded process over one non-empty frame range; and
5. retirement and destroy outside the callback.

Host allocation/deallocation and diagnostic callbacks may be used only by
registration/create/prepare/reset/destroy as their individual contracts allow.
`process` must not allocate, free, log, throw, lock, wait, perform I/O, inspect
registries, change lifetime ownership, or publish UI/document state.

A rejected process transaction changes no output or kernel state. A controlled
fatal status quarantines only that instance; future calls reject without
re-entering module code. Exceptions may not cross the C boundary.

## Retirement and code lifetime

Registration creates one immutable host-owned module record. Every prepared
instance owns a lease captured before callback visibility. Retirement has two
phases:

1. `requestRetirement` prevents new create/prepare work;
2. off-callback collection destroys retired instances and releases the module
   only after the final active lease is gone.

The process path uses immutable/raw prepared pointers and does not mutate a
reference count. Module unload is never a callback-time operation. An old
prepared graph may finish or remain active while a replacement fails, matching
the existing generation contract.

## Trust and crash boundary

B8 production registration accepts only compiled built-in modules. Test
fixtures may use an explicit owner-approved-test trust class. Untrusted or
unknown trust metadata rejects before factory invocation.

A small internal subprocess probe classifies clean exit, metadata rejection,
controlled fatal status, timeout, and abnormal termination without terminating
the parent validator. The probe uses only closed repository-original fixtures;
it does not accept arbitrary executable/module paths.

Successful probing does not make arbitrary in-process code safe. Memory
corruption, hostile code, and hard process crashes cannot be contained by a C
function table. A public SDK or user-component bundle remains blocked until a
separate approved design defines signing/provenance, permissions, dependency
policy, sustained crash isolation, and whether execution remains out of
process. No B8 result silently approves `dlopen` or equivalent loading.

## Three-family evidence

The initial static ABI module adapts exactly:

- `talas.oscillator` — stateful phase source;
- `talas.filter` — stateful input processor; and
- `talas.adsr` — typed-event/control envelope.

Adapters own the existing C++ kernel objects behind opaque pointers. They do
not copy DSP algorithms or create a second capability authority. Direct and ABI
paths receive equivalent prepared values, ordered signals, transport, and
events and must agree across required sample rates, block partitions, reset,
and failure cases.

The B8 Oscillator evidence is deliberately limited to the empty-wavetable sine
path, FM/pitch behavior, interpolation metadata, reset, and inactive inputs.
ABI v1 has no resource-valued prepare parameter or host resource-lease service,
so the adapter does not expose the direct kernel's in-memory wavetable span.
Production resource-backed Oscillator dispatch remains unchanged; resource
configuration support requires a later ownership-aware ABI decision and is not
silently inferred from this seam proof.

This evidence hardens the seam without replacing the central production
dispatch with foreign lookup and without changing a support count.

## State and migration

ABI v1 defines no portable DSP-state serialization or transfer. Project state,
F5a opaque preservation bytes, prepared configuration, and session DSP state
remain separate under ADR-0004 and ADR-0012. A later state ABI requires its own
schema, version compatibility, migration callbacks, limits, and failure-atomic
tests.

## Rejected alternatives

- Exporting current C++ kernel classes would freeze toolchain/STL/allocator
  details and make compiler upgrades unsafe.
- A neutral placeholder execution adapter would violate F5a fail-closed
  behavior.
- Dynamic loading during B8 would combine ABI, trust, packaging, discovery,
  and crash policy before any one boundary is proven.
- Treating an out-of-process probe as proof of safe later in-process execution
  would be crash theatre.
- Serializing current kernel object memory would couple projects to private DSP
  layout and pointer state.

## Acceptance evidence

- One C11 and one C++20 translation unit compile the same ABI header and assert
  required fixed-width/layout properties.
- Host validation rejects every mismatch before a lifecycle callback.
- Instrumented services prove balanced host allocation and no process-time
  allocation/logging/lifetime work.
- Oscillator sine/FM, Filter, and ADSR direct/ABI conformance passes required
  sample rates, partitions, resets, event edges, and rejection atomicity.
- Retirement tests prove leased instances survive until off-callback
  collection and no new instance starts after retirement.
- Controlled fault and subprocess crash/timeout fixtures quarantine or reject
  without terminating the parent.
- Generated catalog/capability authority and project format remain unchanged.

Final local verification uses a fresh isolated Debug tree and passes every
CTest (`92/92`), coverage (`22/22`), governance (`10/10`), and release
configuration (`7/7`) check. Generated catalog and capability artifacts are
byte-identical to the checked authority at offline `0/108/49` and prepared
realtime `59/92/6`, and Gate -1 passes. A fresh Release Workbench built
against pinned JUCE revision
`91ad83ae34a81e0833b1a2b0866f54846370ae53` passes its native smoke test on an
unchanged binary (SHA-1 `db16d1b90211c688e4771f0b90488a4c6dea4b2c`, SHA-256
`93715864422ca5fffd29ef73ec242f1d4b13b008dbb99a8f103d97fbb301cecf`).

Independent latest-state review is PASS with no remaining P0-P2. The accepted
residual boundaries are explicit: callers must quiesce raw instance access
before retirement, wavetable-resource ownership remains outside ABI v1, and
the Windows crash-probe branch is source-reviewed but was not executable on
the accepting macOS host. None of these boundaries authorize dynamic loading
or a public SDK.

## Optional offline checkpoint extension — 2026-09-05

`TalasKernelAbiCheckpointExtensionV1` is a separate internal registration argument,
not a tail on the existing module or repeated descriptor. Its exact versioned
record identifies the module/version and a nonempty subset of kernel types.
Each exact-size checkpoint descriptor supplies capture, restore and payload
destruction callbacks. Registration validates all identities, bounds, reserved
fields and callbacks before publication, then copies callback records under the
existing module lease. Legacy registration never implies checkpoint support.
The V1 layouts, API/capability bits and public SDK gate are unchanged.

An exclusive control-thread call may capture one baseline per prepared instance.
The instance owns the opaque payload; restore or commit consumes it. Retirement
and host destruction release it before destroying the instance and unloading the
module. Callers still quiesce raw instance access before retirement. The payload
never crosses the process boundary and must not be treated as serialized state.

Capture failure leaves DSP unchanged and destroys any partially returned payload.
`INTERNAL_ERROR` from capture can represent an allocation failure and is retryable.
Unknown statuses, success without a payload, thrown exceptions and fatal capture
quarantine. Restore and destruction must not allocate or throw; a violation
quarantines the instance. An already quarantined instance discards its payload
without entering the restore callback. Processing authority is never part of
restored history. Callbacks must not re-enter host lifecycle/checkpoint methods.

The builtin extension snapshots typed Oscillator, Filter and ADSR kernel values.
Fixed parameters and scratch ownership stay on the live ABI instance; scratch
contents are overwritten before processing uses them. The current ABI oscillator
has no external wavetable binding. Conformance includes scalar and typed-gate ADSR,
reset during provisional processing, repeated abort/retry, allocation failure,
explicit phase/attack expectations and direct kernel continuation. This internal
extension initially accepted the local host and builtin kernels only. The loaded
kernel continuation below adds discovery and lifecycle; host/helper and resource
participants remain separate acceptance work.

### Loaded kernel discovery and bounded checkpoint ownership

The authenticated macOS loader resolves the optional metadata-only
`talas_component_checkpoint_entry_v1` after validating image/module metadata and
the selected authorized descriptor, before create/prepare. Missing entry retains
legacy behavior. A present invalid or throwing entry rejects admission. Shared
validation with the local host prevents identity, table and callback rules from
drifting; the loader copies the selected callbacks before use. Admission failure
closes the image before any unaccepted factory or unload callback runs.

One exclusive kernel baseline belongs to the loaded instance. It is destroyed
before resource shutdown, instance destruction, module unload and image closure.
Successful prepare transfers lifecycle ownership even if allocating the final
public wrapper fails. This specifically closes a reproduced publication leak;
it does not redefine failed untrusted entry/create/prepare cleanup policy.

The host retains its cumulative lifetime allocator limits and adds a separate
checkpoint domain with fixed slots, 64 calls and 1 MiB per capture. Capture and
destruction may free only checkpoint allocations; restore cannot allocate or
free through host services. Wrong-domain frees, budget violations and leaked
checkpoint allocations invalidate the host. The domain drains completely before
reuse. A physical allocation failure within the valid checkpoint budget can
reject capture without quarantining the instance. Fatal state remains monotonic.

These methods explicitly capture kernel state only. The real prepared binding
still has no whole-call checkpoint capability: mediated resource consumers,
pending audio, authenticated helper sequencing and deferred health require
their own coordinated acceptance. Frozen V1 layouts and the public SDK gate
remain unchanged.

### Coordinated loaded kernel and resources

The loader additionally exposes a distinct exclusive checkpoint that composes
the validated kernel baseline with each mediated audio-stream baseline. It
captures streams before the kernel and unwinds earlier participants on capture
failure. Kernel-only and coordinated ownership cannot interleave. A terminal
resource or failed kernel restore prevents continued use of the loaded instance;
remaining snapshots are discarded without restoring authority. Shutdown consumes
all checkpoint ownership before destroying the instance and mappings.

A control-only host inspection mutex holds aggregate resource-health publication
at the captured boundary across sequential participant capture, unwind, restore
and commit. Live processing-health inspection bypasses stream deferral when
checking for failure, so a bad slot cannot be hidden by the aggregate. Process
callbacks acquire no group inspection mutex. Source mappings, resource-set
bindings, stream tokens, prepared configuration and outer audio sequence identity
remain live. Restore/commit allocate no memory.

This is still an in-helper loaded-instance primitive. Authenticated checkpoint
commands, audio-plane quiescence, pending response ownership and real prepared
binding integration require separate acceptance; this API does not imply them.

### Private host pending output and actual worker boundary

The host now owns fixed-shape logical delayed output independently of physical
request/response identity. It saves validated pending samples or initial silence;
restore validates current pending completion and never rewrites shared slots.
Missing control-boundary publication permits retry, while ordinary callback
missing-response quarantine remains unchanged. Move transfers sole ownership.

The actual worker's exclusive checkpoint control joins the same worker and
requires an exact completed physical sequence with no acquired, queued or
being-published next request. Capture composes loaded kernel/resources and saves
previous logical transport. Restore/commit allocate nothing and leave the worker
stopped until explicit restart. Failed boundary checks retain the baseline;
terminal authority/restoration failure discards it and prohibits restart. Worker
destruction consumes held ownership before loaded-instance retirement. The loaded
instance must outlive its worker, and direct loaded lifecycle/checkpoint operations
must not interleave with worker-owned capture.

These accepted private participants preserve existing protocol records, physical
sequence counters and health publication counters. They do not yet implement
parent health deferral, authenticated helper checkpoint commands or the real
prepared-binding opt-in. The frozen V1 layouts and public SDK gate stay unchanged.


### Authenticated private helper checkpoint control

Separate 176-byte commands and 184-byte replies now carry authenticated
Query/Begin/Restore/Commit operations without changing existing V1 layouts.
Exact operation, control sequence, completed physical audio sequence and held
checkpoint identity bind a reply to one request. The helper checks its exact next
sequence and held ownership before invoking the same prepared worker; Reset
cannot replace a held checkpoint. The parent coordinates logical pending output,
drains an in-flight heartbeat and keeps the complete operation under the existing
helper deadline. Crash, timeout and terminal restore failure revoke authority
before exposing the terminal checkpoint result. Capture failure can unwind and
retry; physical audio/control identity and live trust never roll back.

The API is private asynchronous exclusive control: process, stage and health calls
must not overlap scheduling through completion. This acceptance supersedes the
older missing-helper-command boundary above. Complete parent public-health
deferral and real prepared-binding capability remain unfinished; frozen ABI
layouts and the public SDK gate are unchanged. Final verification covered 60
native/sanitizer groups; one initial revocation-timing outlier remains
unresolved.


### Ready heartbeat before activation

A scheduled load now owns its XPC message and materialization while a previously
sent sealed Ready heartbeat completes. That heartbeat retains its original token,
exact reply validation and deadline. Only after the acknowledgment advances the
control sequence does the parent revalidate live authorization and exact peer,
then send the queued load under the existing activation deadline. Another load
cannot overwrite this ownership. Timeout, shutdown, revocation and destruction
release retained control objects and materialization; no revoked queued load may
send component bytes. This repairs the reproduced prewarm/activation sequence
race and preserves frozen records, audio callbacks and the public SDK gate.


### Checkpoint health acknowledgment and idle quarantine

Successful worker capture/restore/commit publishes one fresh authenticated health
record per stream before returning to helper control. Publication failure is
terminal, consumes held checkpoint ownership and prevents worker restart; partial
set publication cannot preserve audio authority. Existing process-path health
validation is shared by control publication, and physical publication counters
never rewind. No ABI or wire-record layout changes are introduced.

The parent heartbeat also observes live shared audio-plane quarantine directly.
A helper can fail between callbacks, so the absence of a parent callback failure
latch must not keep runtime evidence active. This closes the reproduced idle
revocation gap using the existing failure path and deadlines. Full parent health
inspection deferral and real prepared-binding whole-call opt-in remain open.


### Parent checkpoint health publication boundary

Parent public-health deferral is now implemented: one short inspection mutex
serializes consumers/caches and keeps the previous complete public snapshot
through pending control and held provisional processing. Exact successful
acknowledgments publish the complete new boundary. Fresh shared-health validation
and live quarantine remain authoritative; neither XPC waiting nor stage-gate
revocation occurs under the inspection mutex. Audio callbacks remain unchanged.
The actual worker publishes all resource slots once per round, so local accepted
sequence comparison prevents a public aggregate from mixing different rounds.
This changes no frozen ABI/wire records and never rewinds consumer identities.

The focused evidence is 66/66 serial groups across Debug, Release, ASan/UBSan core
and TSan core, including native held-reply/concurrent/split-slot XPC tests and two
rejected guard-removal mutations. This supersedes the open deferral
note above. Real prepared-binding whole-call opt-in remains open, and the public
SDK gate and Supported profile counts are unchanged.


### Checked whole-call participant finalization

The private C++ isolated checkpoint and enclosing transaction now report commit
success explicitly. The combined-input offline adapter rejects an assembled
result on failure and advances public cursors only after successful finalization.
Participant control finishes before publication stage leases are acquired; those
leases are released before any failure quarantine. Failed or partly committed
sets quarantine every captured binding, preserve deferred publication and native
undo, and never become a retryable distributed transaction. Repeated terminal
transaction commit remains false. Kernel C ABI and wire records are unchanged.

The original successful-audio-on-terminal-commit regression is closed. Evidence
includes explicit commit failure, revoked or denied final publication authority,
retained prefix analysis, zero-allocation finalization/cleanup and 26/26 native
and sanitizer groups. The later real-integration section
records production whole-call opt-in and the real final-admission allocation fix.


### Accepted real prepared-binding integration

The real prepared binding now opts into whole-call checkpoints on the existing
combined-input offline path after an authenticated helper capability query.
Storage for the RAII checkpoint is allocated before Begin. Restore/commit run
outside the control queue and publication/inspection gates, using the existing
control watchdogs. Only processing inside an exclusive offline checkpoint waits
for exact helper completion through the production audio-boundary validator.
Ordinary realtime processing preserves its nonwaiting deadline behavior.

Nine actual XPC apps cover single Audio and Table/Binary/two-Audio participants,
ordinary capture failure and exact retry, unsupported legacy capability,
terminal restore/commit loss, capture/restore timeouts, and ordinary realtime
deadline quarantine. After a committed prefix, single/set cases inject caller
allocation failure after real provisional blocks, fail twice after provisional
EOF, then compare 8192 retry samples with independent source PCM, recursive kernel
history, float pipeline and scheduled gain. Prefix analysis remains unchanged on
failure; successful commit exposes the final pending EOF health. Concurrent
public inspection checks live, coherent resource health through those calls.

Two additional defects were reproduced and fixed before acceptance. Preparation
transfer now rejects pending or held raw-host checkpoints without moving their
owner; the original host can restore and retry transfer. Independent review
found allocation in final publication admission after the helper had committed.
The private stage lease now stores authority and four shared locks inline in
their existing order, with moves retaining authority until the old locks release.
A real public-render allocation trap starts immediately after helper commit and
remains armed through transaction cleanup; the entire finish allocates zero
caller C++ objects. Direct restore/commit cleanup and authorization admission/move
tests also prove that boundary. This does not claim C/libdispatch/XPC bookkeeping
is allocation-free or relabel focused XPC faults as a complete allocation-site sweep.

Evidence includes initial missing-capability RED, a Restore-to-Commit mutation
that fails provisional-health rollback, pending/held transfer RED/GREEN (2/2,
2.46 s), and stage-allocation RED (both authorization and actual public render)
followed by focused GREEN (3/3, 17.62 s).
Independent final review found the stage-allocation issue above; targeted
re-review confirms the fix with no new findings.

Fresh final verification after the fixes: **Debug 37/37 (77.61 s),
Release 37/37 (54.69 s), ASan/UBSan 9/9 (78.25 s), TSan 9/9 (154.82 s)** — 92
passing groups in total. Native groups include
all nine whole-call apps, the prior 26 stream/control/health/teardown groups,
the core transient graph and authorization. Sanitizer groups cover the relevant
nine core/protocol/authorization tests; the macOS host is also built with both
sanitizers, while actual XPC execution is verified natively. Builds finish before
serial runtime tests; TSan runs last. The earlier accepted 26-group core commit
verification remains separate; its large unchanged realtime sanitizer suite was
not rerun for this host/authorization increment.

This supersedes historical statements that the real binding has no checkpoint
capability. Kernel C ABI and authenticated wire layouts remain unchanged. Legacy
bindings without the optional extension retain their existing admission.
Terminal participant loss rejects output and analysis and cannot become a reusable
retry. This accepts the described real integration, not universal offline graph
admission or full P2. Supported profiles remain **55.0% realtime / 50.3% offline**.
