# Node Coverage

This directory holds generated, descriptor-derived node data: the checked
`NodeRegistry` catalog snapshot (`builtin-node-catalog-v2.json`) and the
capability matrix derived from it (`capability-matrix-v2.json` and
`capability-matrix-v2.md`).

Each built-in node carries a product maturity label:

- `fullRuntime`: no subset restriction is currently tracked. This does not
  claim more than the node's documented contract.
- `functionalSubset`: useful runtime behavior exists, with documented limits.
- `placeholder`: only a narrow compatibility path or metadata shell exists;
  the node remains addable so projects can be inspected and evolved.
- `utility`: a Talas-native helper or terminal.

The labels and support summaries loaded into the built-in registry come from
`src/core/BuiltinNodeSupport.generated.inc`, which is maintained by hand.
Repository tests verify that every built-in descriptor receives support
metadata.

## Capability matrix v2

`NodeRegistry` is authoritative for orthogonal execution capabilities. Generate
its deterministic checked snapshot after building `talas_cli`, then derive the v2
matrix from that snapshot:

```bash
./build/core-debug/talas_cli catalog --format json \
  > docs/coverage/builtin-node-catalog-v2.json
python3 tools/report_capability_matrix.py
python3 tools/report_capability_matrix.py --check
```

This writes `capability-matrix-v2.json` and `capability-matrix-v2.md`. The
matrix exposes product maturity, offline and prepared-realtime support, their
explicit graph-expansion/legacy/kernel bindings, nested Sound behavior, state
ownership, reset/event/transport flags, typed per-port signal rate/layout/edge
and spectral-frame contracts, preparation-time latency rules, fan-in alignment,
resource access, terminal output layouts, and stable limitation IDs. State,
latency, and resource access stay explicitly unknown until a node-specific
contract proves them; generated projections never turn an unknown axis into a
support claim. `Spectrum` ports declare either the historical
`LegacyPartialTriples` contract or native timestamped `PartialSpectrumV1`;
the formats require an explicit bridge. `ComplexSpectrum` remains a distinct
versioned frame family. The shared-kernel contracts currently prove session
state and zero internal latency for Oscillator, Filter, ADSR, Clock, Clock
Divider, Gate, and Slew, plus stateless zero-latency processing for Quantizer
and Logic. Clock alone claims transport awareness because it consumes the
host-owned absolute frame. ADSR alone claims `typedEvents=true`: its prepared
shared-kernel path directly consumes generation-scoped gate edges, preserves
same-frame sequence, defers exact callback-end delivery, resets at the declared
discontinuity, and matches its scalar gate bridge across irregular partitions.
Connected scalar gate input retains precedence. The six native utilities
deliberately retain `typedEvents=false` while they use the documented
scalar-edge bridge.
Quantizer step/offset, Gate thresholds, unconnected Logic numeric fallbacks,
Slew rise/fall times, Clock frequency/duty cycle, and Clock Divider integer
division/steps accept the bounded segmented scalar Event Value subset;
connected Logic inputs remain
authoritative and its operation choice stays static. Clock preserves phase
across ordinary parameter boundaries and deterministically rebases at declared
transport discontinuities. Clock Divider retains representable partial-count,
held-step, and next-step state by deterministic modulo reduction; reset remains
authoritative.
`Sound To Global Controller` also accepts a bounded finite Scalar Event Value
text fallback. An optional connected Control input remains authoritative;
inactive connected or blank-name fallbacks create no live binding. Static
frame-zero behavior, exact `1e-12` de-duplication, caller-owned terminal
publication, rollback, reset/discontinuity, and the no-mutation/no-I/O boundary
remain unchanged.
`Channeller` preserves its two stored Boolean channel fallbacks and accepts
independent same-named finite Control inputs at sample rate. A connected value
enables its side exactly when strictly positive; zero and negative values
disable it. Offline and prepared routing remain stateless, zero-latency,
allocation-free, and failure-atomic, with no format or external-I/O change.
`Spectrum Modifier` preserves its stored `select` and `hearAll` Boolean
fallbacks and finite `seed` parameter, and accepts optional same-named finite
Control inputs. Connected selectors use strictly-positive truth; connected
seed remains in `[-1, 1]` and feeds the deterministic per-partial probability
hash. All three remain constant for the complete offline render transaction or
prepared callback, while callback-boundary changes remain authoritative. The
checked 4096-partial transition, fourteen numeric block controls, zero latency,
and no-I/O contract remain unchanged.
Session reset is true only where a prepared driver invokes the
descriptor-declared reset at an adopted host transport discontinuity;
stateless nodes remain false. All other nodes stay conservatively
unclassified. A C++ test compares the checked catalog snapshot
to the live registry byte for byte; Python tests validate the v2 schema, counts,
binding consistency, ordering, and generated-file drift. Dispatch-source
inspection is not a capability authority.

`talas.sampleMapPlayer` is a Talas-native utility with defining offline and
prepared-realtime sample-map tests. Its map, region, loop, asset-generation,
package, and authoring state live in project format 13 rather than in a
historical prototype descriptor. `talas.keyMappedMultisample` remains the
historical compatibility row and keeps its existing functional-subset limits;
the native utility must not be used to upgrade that row.

The synthetic type in
`examples/m4_missing_component_preservation.talas.json` is intentionally absent
from `NodeRegistry`. It is format/lifecycle evidence for lossless missing-node
preservation, not a built-in descriptor, runtime renderer, component loader, or
historical node mapping. Therefore F5a changes none of the catalog or
capability-matrix counts.

The provisional internal kernel ABI from B8 is likewise not a capability
authority or support promotion. Its static Oscillator, Filter, and ADSR
adapters reuse the existing shared kernels and are checked against
`NodeRegistry`; they do not add descriptors, loaders, bundles, or a public SDK.
The Oscillator adapter proves the default sine/FM seam only; its production
wavetable resource path remains on the existing dispatch until a future ABI
defines resource-valued preparation and lease ownership.
Use [capability-matrix-v2.md](capability-matrix-v2.md) for current generated
counts. Execution capability and maturity are different axes and cannot be
combined into one completeness claim.

The Gate 4 policy evaluator and the later private isolated-component host are
separate from built-in descriptor promotion. A real narrow isolated activation
and resource/replacement foundation now exists; see the current
[component ecosystem boundary](../../README.md#component-ecosystem-boundary).
It does not imply a general component manager, arbitrary native execution,
public SDK or host plugin.

Fix generated prose at its generator source and regenerate; do not patch the
generated Markdown outputs by hand. A matching snapshot proves
reproducibility, not the correctness of every explanatory sentence.

C9 Live Grid and bounded Tools are likewise a system-level prepared workflow,
not node-descriptor support promotion. They reuse existing Sound, Snapshot,
Score, transport, variable, Event Value, MIDI, and OSC authorities and add no
catalog type or execution binding. Their capability boundary is documented in
`docs/help/live-grid-and-bounded-tools.md`, and their source/test authority map
is `docs/testing/live-sets.md`; generated node-capability counts remain
unchanged.

## Generated-authority gate

After building `talas_cli`, run the complete read-only generated-authority gate:

```bash
python3 tools/check_generated_artifacts.py --cli build/core-debug/talas_cli
```

On a stock Windows multi-config build, use the configured Python launcher and
the configuration-specific executable path:

```powershell
python tools/check_generated_artifacts.py --cli build/core-debug/Debug/talas_cli.exe
```

The command byte-compares the live registry catalog with
`builtin-node-catalog-v2.json` and checks that the capability matrix
JSON/Markdown and the generated node help in `docs/help/` are current. It
canonicalizes only the host text transport's CRLF to LF, rejects bare carriage
returns, and never regenerates tracked files. Test-enabled CMake configuration
requires a Python 3 interpreter so the generated gate cannot disappear from a
green suite. The configured hosted pipeline separates the generated label
with `--no-tests=error` from conformance/examples and the full core suite.
That configuration is not evidence of execution: active acceptance is local,
and hosted CI remains outside the current workflow.
