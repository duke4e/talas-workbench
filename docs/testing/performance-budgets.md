# Local core performance budgets

Talas's first performance gate is local, opt-in, and headless. It measures
real core compile, prepared-engine construction, callback rendering, graph
adoption, prepared buffer slots, estimated prepared bytes, and four fixed
node-family scenarios. It does not measure OS/device xruns, actual JUCE frame
time, full build duration, process RSS, energy, or notarized package size.

## Build and smoke

```sh
cmake --preset core-bench-release
cmake --build --preset core-bench-release
ctest --preset core-bench-release
```

The checked-in macOS 14/Ubuntu 24.04 CI declaration runs exactly this preset's
single `benchmark-smoke` test. That smoke proves the target builds, the
provenance manifest is authored, and a small fixed campaign executes. It does
not pass `--output`, compare a platform budget, update a baseline, publish a
report, or establish a cross-platform performance acceptance result. The
five-report campaign below remains the separate retained local budget
authority.

The configure step is required before an acceptance campaign because it binds
the report to the exact current Git commit. The benchmark option is OFF in all
normal and tests-off Release configurations and the target is never installed
or packaged.

## Five-report acceptance campaign

Run one unbudgeted default campaign to prime process-independent code and data
caches, then run five default campaigns sequentially in a quiet window. Keep
the priming report and every accepted report in a dedicated retained local
directory, and pass the five accepted reports explicitly:

```sh
build/core-bench-release/talas_core_performance_benchmarks \
  --output=/private/tmp/talas-g5/prime.json

build/core-bench-release/talas_core_performance_benchmarks \
  --output=/private/tmp/talas-g5/report-1.json

python3 tools/check_performance_budget.py \
  --report /private/tmp/talas-g5/report-1.json \
  --report /private/tmp/talas-g5/report-2.json \
  --report /private/tmp/talas-g5/report-3.json \
  --report /private/tmp/talas-g5/report-4.json \
  --report /private/tmp/talas-g5/report-5.json \
  --budget benchmarks/baselines/macos-arm64-appleclang21-release.json \
  --expected-commit aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  --explain
```

Replace the example `--expected-commit` value with the exact full SHA configured
into the benchmark executable. Configuration and every benchmark-target build
fail when tracked sources are dirty or HEAD no longer matches; the report also
binds the accepted preset-equivalent contract and a SHA-256 build-manifest
fingerprint. The manifest combines a configuration-cache digest with the
normalized actual compile commands of every transitive benchmark target and
the actual executable link command. A manual configure is accepted only when
that measured contract is identical; setting the preset label alone is
insufficient. Project-specific CMake injection/rule-override variables are
rejected before the target is generated.

The checked-in campaign identity is exact: Release, 48 kHz, 256 frames, eight
warmups, 1,024 compile/prepare/adoption samples, 2,048 callback samples, and the
recorded seed. Custom options are useful for investigation but cannot pass the
accepted budget.

## Noise and budget policy

For p50/p95/p99, the checker first rejects excessive five-report spread, then
applies both baseline-relative and absolute ceilings. The p50/p95 rules remain
tight regression indicators. Because a scheduler burst can move the p99 of
microsecond operations without moving p50/p95, p99 allows at most 2x its
baseline and 100% cross-report spread; it still rejects a sustained tail beyond
double baseline. The observed `max` is scheduling-sensitive and therefore uses a
separate absolute fail-safe ceiling, not a misleading relative stability
claim. Structural counts are exact or maximum rules. Callback durations above
the 48 kHz / 256-frame period are reported as benchmark deadline misses, not OS
xruns.

The checker validates a closed JSON schema, unique 128-bit run identities,
identical report commit/campaign, exact platform/compiler/preset binding,
complete scenario and metric sets, and the required rule mode for every metric.
It does not update the baseline. A new baseline requires a
reviewed patch backed by retained raw reports from the same platform/preset.

For an optimization claim, retain before and after five-report sets, run both
through the same checked-in checker/budget, and record the exact commits. Do not
delete slow/noisy reports or choose only the fastest run.

The separate tests-off Workbench artifact envelope and observational JUCE
UI/device workflow are documented in `docs/testing/product-performance.md`.
