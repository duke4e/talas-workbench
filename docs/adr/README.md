# Architecture Decision Records

These records are internal implementation contracts. They do not promise a
public ABI or plugin SDK. Status `Accepted` means the owner approved the
decision, directly or under standing auto-approval.

## M1 contract tranche

1. [ADR-0001: Shared kernel lifecycle and legacy adaptation](0001-shared-kernel-lifecycle-and-legacy-adaptation.md)
2. [ADR-0002: Descriptor-derived capability authority](0002-descriptor-derived-capability-authority.md)
3. [ADR-0003: Signal, rate, channel, edge, and latency contracts](0003-signal-rate-channel-edge-and-latency-contracts.md)
4. [ADR-0004: Stable identity and state boundaries](0004-stable-identity-and-state-boundaries.md)
5. [ADR-0005: Typed events and transport time domains](0005-typed-events-and-transport-time-domains.md)

The tranche is accepted together. Implementation is conformant only after the
same contracts survive three materially different kernels:

- Oscillator: source, phase state, audio-rate modulation, and reset edges;
- Filter: input processor, coefficient changes, latency declaration, and
  persistent history;
- ADSR: control output, typed gate transitions, transport segmentation, and
  retrigger state.

Until all three pass the shared conformance harness, registry APIs remain
internal and provisional. ADR-0004's amendment of 2026-10-13 has a voice set
its Sound's constant fields up afresh, with its Event Values, as it starts,
from project format 91.

## M2 contract tranche

6. [ADR-0006: Reusable Sound interfaces and structural scopes](0006-reusable-sound-interfaces-and-scopes.md)
7. [ADR-0007: Explicit delayed feedback and SCC execution](0007-explicit-delayed-feedback-and-scc-execution.md)

The M2 tranche is accepted under the owner's standing auto-approval. It builds
on the M1 contracts: structural identity resolves to generation-scoped handles,
mutable state remains instance-owned, and delayed feedback is compiled as an
explicit prepared topology rather than an exception to ordinary graph-cycle
validation. ADR-0006's amendments of 2026-10-13 let a Sound's input bind a
Variable, so that an instance gives the Variable a Sound, or a
SoundCollectionVariable, so that it gives the collection Sounds.

## M3 contract tranche

8. [ADR-0008: Partial spectra, complex frames, and FFT contracts](0008-partial-spectrum-complex-frames-and-fft.md)
9. [ADR-0009: Immutable resource generations and streaming ownership](0009-immutable-resource-generations-and-streaming.md)
10. [ADR-0010: N-channel buses and device mapping](0010-n-channel-buses-and-device-mapping.md)
11. [ADR-0011: Cumulative latency, fan-in alignment, and reprepare](0011-cumulative-latency-alignment-and-reprepare.md)

ADR-0008's amendment of 2026-10-03 gives `talas.fft` a stereo format from
project format 62; older projects keep the packed layout.

The M3 tranche is accepted under the same standing auto-approval. The four
records close the decision portion of Gate 3 together: typed spectral families,
project-original FFT provenance, immutable resource/stream ownership,
Discrete4 routing, and prepared latency alignment are one coordinated contract.
Every defining runtime package and the original format-10 demonstrator set are
implemented and accepted locally. Final closeout evidence is core `65/65`,
coverage `22/22`, governance `10/10`, source-derived capability authority,
Gate -1 PASS, and Release Workbench `20/20`. The Step-13
review raised two P2 weaknesses in demonstrator evidence; commit `86235004`
closed both with a real JSON parser and exact Matrix4 route/signal assertions.
Re-review found no remaining P0-P2. M3 and Gate 3 are fully implemented locally.

## M4 contract tranche

12. [ADR-0012: Missing-node preservation and non-execution](0012-missing-node-preservation-and-non-execution.md)

ADR-0012 is accepted under the owner's standing auto-approval for M4 Step 6 /
F5a. It separates lossless, bounded component-data preservation from execution:
one node-owned capsule retains opaque state and frozen interfaces, structural
editing may preserve it, and runtime remains fail-closed until a real trusted
descriptor matches. Executable bundle loading remains explicitly deferred to
F5b and later Gate-4 work.

The repository-original format-13 preservation fixture and
`talas_m4_missing_node_workflow_tests` are the end-to-end decision evidence. They
prove byte-identical lifecycle/package retention, honest non-executable CLI
status, no partial render destination, and exact no-history repair against a
test-only trusted descriptor; they add no executable bundle or capability
claim.

## M5 contract tranche

13. [ADR-0013: Internal kernel ABI and public SDK gate](0013-internal-kernel-abi-and-public-sdk-gate.md)
14. [ADR-0014: Ecosystem trust, signing, capability, and isolation policy](0014-ecosystem-trust-signing-capability-and-isolation-policy.md)

ADR-0013 is accepted under the owner's standing auto-approval for B8. It
defines a provisional C-compatible, size-tagged internal boundary with
host-owned memory/diagnostics, three real built-in family adapters, deferred
retirement, and controlled crash-probe evidence. It does not authorize dynamic
loading, user bundles, or a public SDK; those remain gated by explicit trust,
isolation, packaging, and product decisions. B8 passes fresh Debug `92/92`,
coverage `22/22`, governance `10/10`, release configuration `7/7`, unchanged
Release Workbench smoke, byte-identical generated authority, Gate -1, and an
independent review with no remaining P0-P2.

ADR-0014 is accepted under the same standing auto-approval for the remaining
Gate 4 policy boundary. It separates integrity, signer identity, exact owner
authorization, execution permissions, probe evidence, and real containment.
Only product-built static code may run in process. F5b Step 1 has since accepted
the real macOS product-fixture isolated host, and Step 2 has accepted private
immutable bundle inspection, CMS verification, live ephemeral owner approval,
and preserve-only compatibility at reviewed code commit `b8aa9c47`. Policy
schema v3 retains the exact disposable macOS XPC probe-loader helper and the
accepted Step 4 activation-only runtime XPC. The current state is
`isolated-activation-foundation`: one private transient stateless audio graph
path is active only through the sealed runtime, opaque authorization, fixed
realtime profile, and failure gates. Discovery, installation, Workbench/CLI activation, registry
promotion, persistent trust, project-format changes, state/resources,
capability promotion, and a public SDK remain separately gated.

## C7 bounded-language decision

15. [ADR-0015: Bounded generative language and broader script-runtime deferral](0015-bounded-generative-language-and-script-runtime-deferral.md)

ADR-0015 accepts C7's bounded deterministic generator, typed output,
compile-time transformer, and restricted graph-construction boundary. It
explicitly defers arbitrary Smalltalk/Lime-style execution, keeps the three
historical script-family node identities as compatibility placeholders, and
defines the separate evidence and security gate required for any future
language runtime. Its amendment of 2026-10-03 makes the ContextFreeGrammar
a bounded expansion into Sounds when the project compiles; that of
2026-10-12 lets a Script give an Input's Variable a Sound and keeps the
LimeInterpreter a documented placeholder.

## Later decisions (from 2026-09-29)

16. [ADR-0016: Implicit audio/control signal adapters](0016-implicit-audio-control-signal-adapters.md)

ADR-0016 records the owner's decision that any signal may feed any input.
Compiling adapts audio to control and back through typed copies, so
typed node checks stay intact.

17. [ADR-0017: The Mixer and inputs that take many Sounds](0017-mixer-and-collection-inputs.md)

ADR-0017 replaces the two-input Mixer with one that has a single Inputs port
taking any number of connections, and Left and Right levels. Project format 43
migrates older Mixers without changing their sound. Its amendment of
2026-10-12 has a SoundCollectionVariable in such a field stand for all its
Sounds, from project format 90; that of 2026-10-13 lets a Script set one to
an Array of its Inputs and a MIDIVoice declare its own.

18. [ADR-0018: CDP algorithms in a separate LGPL module](0018-cdp-provider-module.md)

ADR-0018 keeps CDP8-derived code in `third_party/cdp` under its LGPL-2.1
license, built as the `talas_cdp` library that CDP nodes call. Each port is a
prepared stream processor checked against the CDP program it comes from.

19. [ADR-0019: TimeControl and TimeStopper pace their Input's events](0019-time-control-and-time-stopper-pace-events.md)

ADR-0019 lets both pass their Input through: Rate and Resume
concern the Input's events, not its samples. Project format 45 keeps the old
replay and hold for older projects. Its amendments pace timing under a
moving Rate or a TimeStopper's Resume, from 2026-10-10 in flat
MIDIVoices and timing read as it starts, and from 2026-10-12 in nested
MIDIVoices and a value read from a Sound under a fixed Rate.

20. [ADR-0020: Sounds in parameter fields](0020-sounds-in-parameter-fields.md)

ADR-0020 lets a field name a pasted Sound with L, R or M and do arithmetic
with it: compiling adds a node that evaluates the field each
millisecond and drives the parameter's control input. Its amendment keeps
Event Values, variables and functions beside the Sound live, in up to eight
parts. That of 2026-10-18 takes a Sound named without its L at the audio
rate, keeps a live part's own unit and holds a field its Sounds leave
undefined.

21. [ADR-0021: Event Values generated by a SoundToGlobalController](0021-generated-event-values.md)

ADR-0021 lets a SoundToGlobalController beside another terminal generate its
Event Value: compiling substitutes the controller's Value, or a pasted Sound
reference to the Sound fed to it, wherever another field reads that Event
Value, mapped by the Virtual Control Surface's range.

22. [ADR-0022: RE resonator tables and analysis](0022-re-resonator-tables-and-analysis.md)

ADR-0022 makes the REResonator the time-varying all-pole lattice filter that
our own RE table describes, stepping through its coefficient sets as its
TimeIndex goes from -1 to 1, and adds an RE analysis that writes the table
and the excitation that recreates the sample. Project format 63 keeps the
static FIR reading for older projects.

23. [ADR-0023: TimeOffset starts its Input late](0023-time-offset-starts-its-input-late.md)

ADR-0023 makes a TimeOffset with a fixed SilentTime start its Input at that
time, as a WaitUntil does, instead of delaying its samples, so live Inputs
play live once started. Project format 64 keeps the delay line for older
projects. Its amendment of 2026-10-04 starts a SetDuration's Input at its
StartTime the same way, from project format 66; that of 2026-10-10 starts
them late in flat MIDIVoices too and reads a field fed by a Sound or that
moves once, as the step starts, from project format 87; that of 2026-10-12
in nested MIDIVoices too, from project format 89.

24. [ADR-0024: Default MIDI map names](0024-default-midi-map-names.md)

ADR-0024 lets the Default map's documented names, such as `!Volume`,
`!PitchBend` and `!DamperPedal`, follow MIDI controllers and pitch bend on
the default channel, live and from MIDI files, unless a MIDIMapper's Map or
a Virtual Control with its own mapping names them.

25. [ADR-0025: DelayWithFeedback's Wavetable, Prezero and SmoothDelayChanges](0025-delay-with-feedback-wavetable-prezero-and-smoothing.md)

ADR-0025 lets DelayWithFeedbacks that name one Wavetable share a delay line,
each reading its own delay and adding its write, keeps a line over a
transport restart when Prezero is off, and glides a changed delay at half a
frame per frame with SmoothDelayChanges. Its amendments share the lines
with ReverbSections, giving them Prezero too, and with HarmonicResonators.

27. [ADR-0027: GA analyses are tables of our own layout](0027-ga-analysis-tables.md)
28. [ADR-0028: A Tool is a state machine the project runs](0028-tools-as-state-machines.md)

ADR-0027 keeps a GA analysis's parts (complex waveforms of 4096 samples, an
amplitude envelope each, one frequency deviation envelope) in a table with a small header, read by GAOscillators with
TimeIndex and Morph, and made from a sample by `talas_cli ga-analysis` and the
Workbench's GA Analysis tool.

ADR-0028 runs a Tool as a headless state
machine: triggered responses tested and run in order, global and local
variables, Event Values written and read, a step interval, and a Smalltalk
session keeping its objects between steps.
