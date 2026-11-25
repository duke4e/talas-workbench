# Local sanitizer and fuzzing workflow

Talas keeps fuzz harnesses opt-in and nonshipping. The default core,
Workbench, XPC, tests-off Release, install, and package configurations do not
build or link them. The checked-in quality workflow declares only the bounded
deterministic replay preset; native libFuzzer campaigns remain local.

## Deterministic sanitizer replay

The accepted AppleClang 21 toolchain does not ship
`libclang_rt.fuzzer_osx.a`. The default fuzz preset therefore compiles the
Project JSON and expression entrypoints, plus their reachable libraries, with
ASan/UBSan and drives them with a fixed-seed bounded replay mutator:

```sh
cmake --preset core-fuzz
cmake --build --preset core-fuzz
ctest --preset core-fuzz
```

The checked-in CI declaration runs the same bounded replay preset on macOS 14
and Ubuntu 24.04 with `CC=clang` and `CXX=clang++`. It also runs the five
fixed-seed property tests through the target-limited
`core-asan-ubsan-property` and `core-tsan-property` build/test presets. Those
property presets are a CI-cost boundary, not a replacement for the broader
local `core-asan-ubsan` and `core-tsan` investigation presets.

The CI jobs are blocking, reject empty labels, compile UBSan with
`-fno-sanitize-recover=undefined`, do not use sanitizer suppressions, and do
not upload corpora or reproducers. Their declaration does not claim native
libFuzzer, Windows sanitizer support, or an observed cloud runner result.

The smoke budget is 2,048 Project JSON mutations and 4,096 expression
mutations. To run a longer deterministic replay, invoke either executable from
`build/core-fuzz` with one checked-in corpus directory plus larger `-runs=`,
the retained `-seed=`, and a bounded `-max_len=` value.

## Native libFuzzer when available

A Clang distribution that actually includes its libFuzzer runtime can use the
same `LLVMFuzzerTestOneInput` entrypoints:

```sh
cmake -S . -B build/core-libfuzzer \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTALAS_BUILD_TESTS=ON \
  -DTALAS_BUILD_WORKBENCH=OFF \
  -DTALAS_BUILD_FUZZERS=ON \
  -DTALAS_FUZZ_ENGINE=LIBFUZZER
cmake --build build/core-libfuzzer \
  --target talas_project_json_fuzzer talas_expression_fuzzer
```

Configuration may succeed even when a vendor compiler omits the runtime; the
link step is the authority. Do not report native libFuzzer as available until
both executables link and run.

## Corpus and reproducer policy

Only project-original Apache-2.0 seeds may be checked in under
`tests/fuzz/corpus/`. Do not read or derive seeds from third-party reference
material, private projects, downloaded corpora, or user data. Keep a sanitizer/libFuzzer
reproducer in a dedicated retained local directory, record the exact target,
seed, maximum length, and commit, and reduce it only with the same instrumented
parser boundary. Never add a reproducer containing external or sensitive
material to Git.
