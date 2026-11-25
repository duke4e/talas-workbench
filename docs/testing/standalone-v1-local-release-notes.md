# Standalone v1 local closeout notes

Status: complete and independently accepted locally at a reviewed checkpoint
before the public history began. The cumulative exact
P0-P3 review reports no remaining findings. This is not a public release
announcement.

These notes describe the recorded standalone-v1 checkpoint. They do not
describe later work.

## Product boundary

The standalone product is a C++20 JUCE-free core/CLI plus an optional macOS
JUCE Workbench. The checked local inventory accepts 70 selected Gates,
milestones, and lane packages spanning:

- reusable nested Sounds, prepared scopes, deterministic typed events,
  transport/musical time, feedback, buses, latency alignment, immutable
  resources, internal kernels, and a private internal ABI;
- generated capability truth, spectral/FFT/phase-vocoder/vocoder/granular/
  spatial and advanced component families, with subset and placeholder limits
  retained rather than promoted optimistically;
- Project format 34 and separate WorkspaceSession format 13, Score, Snapshots,
  variables/state, restricted generators, Live Sets and bounded Tools;
- graph, Performance, Analysis, accessibility, Sound Library/help, partial
  spectrum, Timeline, asset/sample-map/missing-node workflows;
- private owner-selected isolated macOS component inspection, activation,
  immutable resources, and explicit replacement;
- local MIDI/OSC control, MIDI Clock/MTC sync, recording/output takes,
  packaging, recovery, sanitizers, fuzzing, performance, generated docs, and
  governance.

At the standalone-v1 checkpoint, the generated authority reported 171 catalog nodes and 15 examples,
runtime prototype coverage `32/110/0/0`, offline capability `0/109/62`, and
prepared-realtime capability `42/123/6`. Product maturity is 32 full-runtime,
107 functional-subset, 3 restricted-script placeholder, and 29 utility nodes.
“Full runtime” and “supported” remain
contract-specific terms; neither claims more than the documented contract.

## Local artifact evidence

A clean `TALAS_BUILD_TESTS=OFF` Release build produced the CLI, Workbench app, and
exact Host/Probe/Runtime XPC service set. CLI validation/render/status,
generated catalog/help, native Workbench smoke, strict/deep ad-hoc signing,
the exact 17-file resource allowlist, and the 44-file install tree passed. The
installed app and CLI were byte-identical to the verified build products. No
test-support compile definition or test-only runtime seam was present in the
shipping targets.

The cumulative exact implementation checkpoint also passed a serial 310-test
Debug suite, focused ASan/UBSan and TSan matrices, fixed-seed replay fuzz,
optimized product-valid maximum fixtures, 200 repeated concurrency/lifecycle
executions, and local generated/coverage/rights/governance/release gates.

## Compatibility and migration

After the later format-35 continuation, the portable reader accepts Project
formats 1 through 35 and writes canonical
format 35. Every supported source-version fixture migrates without filesystem,
package, asset, network, plugin, or reference access, reopens, and reaches
stable second-serialization bytes. WorkspaceSession is separate and never
participates in Project migration. Keep backups before migrating real projects;
the compatibility matrix is not a promise to repair arbitrary malformed or
private historical files.

## Explicitly not selected

None of the following is part of local standalone-v1 completion:

- AU/VST/CLAP plugin artifacts;
- a public component SDK or general component manager;
- hosted CI/GitHub Actions execution;
- Developer ID signing, notarization, upload, or public distribution;
- Windows/Linux native Workbench observation;
- WCAG/VPAT or assistive-technology certification.

These items are unselected and contribute zero to the local completion
percentage. This historical closeout file does not select the next package.
