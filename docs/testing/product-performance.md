# Local product performance envelope

Talas's product performance workflow keeps deterministic shipping artifact
budgets separate from noisy product timing observations. Version 1 hard-gates
only the tests-off Workbench app and executable byte sizes. Full-build time,
JUCE GraphCanvas paint duration, real-device callback timing, and package size
are recorded as `observational` and cannot be promoted to blocking rules
without a future schema revision.

The workflow is local and never invokes notarization. A signed local app is not
a notarized app. An existing package may be ingested as `unsigned-local`,
`signed-local`, or `notarized-ingested`, but the collector only records its
exact path name, bytes, and SHA-256.

## Product probe

Configure and build the opt-in Release probe from a clean exact commit:

```sh
cmake --preset product-bench-release \
  -DTALAS_FETCH_JUCE=OFF \
  -DTALAS_JUCE_PATH=/absolute/path/to/pinned-juce
cmake --build --preset product-bench-release -j8
ctest --test-dir build/product-bench-release \
  -R '^talas_product_performance_probe_smoke$' --output-on-failure
```

The accepted UI campaign uses the real JUCE GraphCanvas, a deterministic,
product-validated 1,000-node/2,000-connection synthetic presentation graph, a
1440x900 logical surface at 1x scale, 16 warmups, and 512 measured paints. The
probe records the 16-run structural-cache rebuild sample count and p50/p95/max
in the provenance-bound UI report (and mirrors them to stderr), verifies viewport
culling and a fixed spatial-membership envelope, and rejects any paint that
rebuilds structural or accessibility caches. Setup, image allocation,
percentile work, and JSON stay outside the measured interval. Every frame is
cleared first and the report carries the resulting pixel digest.

```sh
build/product-bench-release/talas_product_performance_probe_artefacts/Release/\
talas_product_performance_probe \
  --warmups=16 --samples=512 \
  --output=/private/tmp/talas-product-probe-1.json
```

Run five reports sequentially in a quiet window. Paths must be new; the probe
will not overwrite a report. Each report carries the full commit, the
compiler-launcher-backed transitive build fingerprint, and the SHA-256 of the
actual running executable. The generated manifest and fingerprint files remain
beside the retained product-probe build.

To request a real default-device observation through the normal Workbench
hardware initialization, project staging, Play, callback, and G3 metrics path:

```sh
build/product-bench-release/talas_product_performance_probe_artefacts/Release/\
talas_product_performance_probe \
  --warmups=16 --samples=512 \
  --device-duration-ms=2000 --require-device \
  --output=/private/tmp/talas-product-device-1.json
```

The device section records the exact device identity/configuration, callback
count, configured callback period, maximum callback duration, and deadline
overruns. It does not report OS or audible xruns. `--require-device` returns a
distinct nonzero status when a coherent device result is unavailable. CoreAudio
initialization is protected by a benchmark-process watchdog; an uninterruptible
open records `device-initialisation-timeout` and terminates only the probe.

## Fresh Workbench artifact reports

Use a previously absent build directory and report path for every run. The
collector requires clean tracked sources, a full expected SHA, the exact
tests-off `workbench-release` cache contract, deep/strict signing, and exactly
the Host, Probe, and Runtime XPC services. It removes compiler/linker/Make
environment overrides and fixes `SOURCE_DATE_EPOCH=0`; this prevents JUCE's
compilation date/time and the derived Mach-O UUID/signature from changing an
otherwise identical artifact. The collector also resolves the active macOS SDK
through `xcrun` and explicitly records that canonical SDK path in the CMake
cache fingerprint instead of trusting an inherited `SDKROOT` override.

```sh
python3 tools/collect_product_performance.py \
  --source-root "$PWD" \
  --build-dir /private/tmp/talas-product-build-1 \
  --report /private/tmp/talas-product-report-1.json \
  --expected-commit aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  --juce-source /absolute/path/to/pinned-juce \
  --jobs 8 \
  --probe-report /private/tmp/talas-product-probe-1.json \
  --probe-manifest build/product-bench-release/talas-product-probe-build-manifest.json \
  --probe-executable build/product-bench-release/talas_product_performance_probe_artefacts/Release/talas_product_performance_probe
```

Replace the example SHA with `git rev-parse HEAD`. Probe report, manifest, and
executable are one optional all-or-none tuple; their commit, manifest
fingerprint, and executable SHA-256 must agree. The collector accepts only an
explicit clean local JUCE checkout at the pinned exact revision, forces
`TALAS_FETCH_JUCE=OFF` and `FETCHCONTENT_FULLY_DISCONNECTED=ON`, and therefore
cannot fetch from GitHub. Build duration spans configure plus the requested
Workbench target build but remains observational. The bundle aggregate counts
regular files once by canonical lexical relative path; symlinks are metadata
only and must stay inside the bundle. Report paths must be new and outside both
the source and build roots.

After three fresh reports, run the artifact budget:

```sh
python3 tools/check_product_performance_budget.py \
  --report /private/tmp/talas-product-report-1.json \
  --report /private/tmp/talas-product-report-2.json \
  --report /private/tmp/talas-product-report-3.json \
  --budget benchmarks/baselines/macos-arm64-workbench-release.json \
  --expected-commit aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  --explain
```

The reports must have unique run IDs, one identical environment and commit,
and byte-for-byte identical artifact records. Budget updates are manual review
changes backed by retained raw reports. The tools never delete retained builds
or reports, reuse a build as a fresh run, update the baseline, contact GitHub,
or contact a notary service.
