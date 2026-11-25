# Local testing and evidence index

This directory records reproducible local evidence. It does not represent a
hosted runner, remote branch-protection result, notarization, publication, or
manual certification.

## Workbench smoke assertions

`talas_workbench_app_smoke` treats any `JUCE Assertion failure` output as a failure,
including when the process exits 0 and prints `WORKBENCH_SMOKE_OK`. Keep the
Debug smoke free of assertions; a success marker alone is insufficient.

## Standalone-v1 closeout

- `standalone-v1-local-release-notes.md` states the product-visible local
  boundary and every deliberately unselected external/product decision.

The final cumulative P0-P3 review was accepted at a checkpoint before the
public history began. This historical acceptance does not cover later candidates.

## Focused contracts

- [time-frequency-scale-low-source-reference.md](time-frequency-scale-low-source-reference.md):
  original-rate source analysis, low tones/DC, source clocks, latency and rollback;
- [waveshaper-memory-writer-reference.md](waveshaper-memory-writer-reference.md):
  live named shaping functions, concurrent writes and transactional lookup;
- [formant-bank-inram-source-reference.md](formant-bank-inram-source-reference.md):
  direct InRAM/Sounds source counts, selected formants and empty-render phase;
- [oscillator-bank-source-frame-reference.md](oscillator-bank-source-frame-reference.md):
  source-count cadence, selected oscillator state and transactional pair inputs;
- [runtime-conformance.md](runtime-conformance.md): common N-channel audio,
  scheduled MIDI notes, independent oracles and remaining P1 work;
- `project-format-migrations.md`: portable format 1-35 compatibility;
- `fuzzing.md`: sanitizer and fixed-seed replay fuzz workflow;
- `performance-budgets.md` and `product-performance.md`: core and product
  performance evidence;
- `live-sets.md`, `timeline-score.md`, and
  `project-generator-output.md`: current bounded composition workflows;
- `expression-dsl-v2.md`: closed expression dialect and state contract;
- `e7-accessibility-manual-checklist.md`: locally testable accessibility
  evidence and explicitly external manual checks;
- the other `*-reference.md` and `time-frequency-scale-*.md` files: per-node
  check records with their oracles, model choices and limits.

Use the configured bundled Python interpreter when the system interpreter
lacks the checked tooling dependencies. The ordinary local admission commands
are:

```sh
ctest --test-dir build/core-debug -j1 --output-on-failure
python3 tools/check_generated_artifacts.py --cli build/core-debug/talas_cli
git diff --check
```

There are no hosted CI workflows; builds and checks run locally.
