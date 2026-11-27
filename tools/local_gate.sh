#!/usr/bin/env bash
# Local quality gate: run before every commit (owner decision 2026-09-28).
#
#   tools/local_gate.sh                      # core Release build + full ctest
#   tools/local_gate.sh --asan               # also the ASan/UBSan property suite
#   tools/local_gate.sh --tsan               # also the threaded realtime suite under TSan
#   tools/local_gate.sh --workbench <JUCE>   # also build and test the Workbench,
#                                            # and build it once more without
#                                            # tests, as a release does
#
# Compiler warnings are errors, except SDK deprecations that the pinned JUCE
# sources compiled inside the Workbench targets still trigger.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build/local-gate"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
jobs=$(( jobs > 2 ? jobs / 2 : 1 ))
run_asan=0
run_tsan=0
juce_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --asan) run_asan=1; shift ;;
        --tsan) run_tsan=1; shift ;;
        --workbench) juce_path="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "local_gate: unknown argument: $1" >&2; exit 2 ;;
    esac
done

configure_args=(
    -S "${root}" -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DTALAS_BUILD_TESTS=ON
    -DTALAS_FETCH_JUCE=OFF
    -DTALAS_WARNINGS_AS_ERRORS=ON
)
if [[ -n "${juce_path}" ]]; then
    configure_args+=(-DTALAS_BUILD_WORKBENCH=ON "-DTALAS_JUCE_PATH=${juce_path}")
else
    configure_args+=(-DTALAS_BUILD_WORKBENCH=OFF)
fi

# Stage headings carry the wall-clock time they start at, so a gate log shows
# how long each stage took.
stage() {
    echo "== $* ($(date '+%Y-%m-%d %H:%M:%S'))"
}

stage "configure (${build_dir})"
cmake "${configure_args[@]}"
stage "build (-j ${jobs})"
cmake --build "${build_dir}" -j "${jobs}"
stage "test"
ctest --test-dir "${build_dir}" -j "${jobs}" --output-on-failure -LE '^perf$'
# Wall-clock budget checks (label perf) run alone, after the parallel tests,
# so the load of other tests cannot fail them.
stage "test, serial timing budgets (label perf)"
ctest --test-dir "${build_dir}" --output-on-failure -L '^perf$' --no-tests=error

# A release builds without the tests' support code, which the build above
# always has; build the Workbench that way too and run its smoke.
if [[ -n "${juce_path}" ]]; then
    release_dir="${build_dir}-release"
    stage "Workbench release build without tests (${release_dir})"
    cmake -S "${root}" -B "${release_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTALAS_BUILD_TESTS=OFF \
        -DTALAS_BUILD_WORKBENCH=ON \
        -DTALAS_FETCH_JUCE=OFF \
        "-DTALAS_JUCE_PATH=${juce_path}" \
        -DTALAS_WARNINGS_AS_ERRORS=ON
    cmake --build "${release_dir}" -j "${jobs}"
    ctest --test-dir "${release_dir}" --no-tests=error --output-on-failure \
        -R '^talas_workbench_app_smoke$'
fi

if [[ "${run_asan}" -eq 1 ]]; then
    stage "ASan/UBSan property suite"
    (cd "${root}" \
        && cmake --preset core-asan-ubsan \
        && cmake --build --preset core-asan-ubsan-property -j "${jobs}" \
        && ctest --preset core-asan-ubsan-property)
fi

if [[ "${run_tsan}" -eq 1 ]]; then
    stage "TSan threaded realtime suite"
    (cd "${root}" \
        && cmake --preset core-tsan \
        && cmake --build --preset core-tsan-threaded -j "${jobs}" \
        && ctest --preset core-tsan-threaded -j "${jobs}")
fi

stage "local gate passed"
