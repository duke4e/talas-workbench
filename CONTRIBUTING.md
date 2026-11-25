# Contributing

Talas is developed locally; there is no hosted CI. These rules keep the
tree buildable and the history honest.

## Before every commit

Run the local gate from the repository root:

```bash
tools/local_gate.sh
```

It builds the core in Release with warnings as errors and runs the complete
test suite. Add `--workbench <JUCE path>` when a change touches the Workbench,
`--asan` for the sanitizer property suite, and `--tsan` when a change touches
realtime threading. Commit only a green suite.

## Changes

- Fix a defect with a regression test that fails before the fix.
- Keep commits small, one concern each. Write an imperative subject line and a
  body that explains why the change is needed.
- Regenerate derived documentation with the tools under `tools/` (catalog,
  capability matrix, node help);
  `talas_generated_artifact_checks` fails when it is stale.
- The audio thread must not allocate, lock, sleep, or touch files.
  `talas_realtime_safety_tests` checks every example for this.
- Workbench coordinator files stay under 4500 lines; put new smoke checks in
  their own `Workbench*Smoke.cpp`.

## Style

C++20, four-space indentation, braces on their own lines for functions,
classes, and control statements, and lines of about 80 columns. The code
predates a formatter configuration, so there is no `.clang-format`: match the
surrounding code instead of reformatting it. Text files use LF line endings
(see `.gitattributes`).

## Licensing

Original Talas code is licensed under Apache-2.0. Do not commit, package, or
quote third-party manuals or other rights-restricted material. LGPL code, such
as CDP-derived DSP, belongs only in a separate module under its own license and
is never copied into Apache-2.0 files.
