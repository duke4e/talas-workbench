#!/bin/bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/notarize_macos_release.sh [options]

Build, sign, notarize, staple, and verify a macOS Talas release.

Options:
  --build-dir PATH          CMake build directory (default: build/workbench-release)
  --output-dir PATH         New output directory (default: timestamped under the build)
  --identity NAME           Developer ID Application identity (default: unique identity)
  --keychain-profile NAME   notarytool keychain profile (default: talas-notary)
  --juce-path PATH          use this JUCE checkout instead of fetching the pinned one
  --cmake PATH              cmake executable (default: CMAKE_BIN or PATH)
  --ctest PATH              ctest executable (default: CTEST_BIN or beside cmake)
  -h, --help                Show this help

The keychain profile must already exist. This script never accepts or stores an
Apple ID password, app-specific password, API private key, or certificate.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/workbench-release"
OUTPUT_DIR=""
IDENTITY=""
NOTARY_PROFILE="talas-notary"
JUCE_PATH=""
CMAKE_EXE="${CMAKE_BIN:-}"
CTEST_EXE="${CTEST_BIN:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a value"
            BUILD_DIR="$2"
            shift 2
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || die "--output-dir requires a value"
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --identity)
            [[ $# -ge 2 ]] || die "--identity requires a value"
            IDENTITY="$2"
            shift 2
            ;;
        --keychain-profile)
            [[ $# -ge 2 ]] || die "--keychain-profile requires a value"
            NOTARY_PROFILE="$2"
            shift 2
            ;;
        --juce-path)
            [[ $# -ge 2 ]] || die "--juce-path requires a value"
            JUCE_PATH="$2"
            shift 2
            ;;
        --cmake)
            [[ $# -ge 2 ]] || die "--cmake requires a value"
            CMAKE_EXE="$2"
            shift 2
            ;;
        --ctest)
            [[ $# -ge 2 ]] || die "--ctest requires a value"
            CTEST_EXE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || die "macOS is required"

if [[ -z "$CMAKE_EXE" ]]; then
    CMAKE_EXE="$(command -v cmake || true)"
fi
[[ -n "$CMAKE_EXE" && -x "$CMAKE_EXE" ]] || die "cmake was not found; use --cmake"

if [[ -z "$CTEST_EXE" ]]; then
    CTEST_EXE="$(dirname "$CMAKE_EXE")/ctest"
    if [[ ! -x "$CTEST_EXE" ]]; then
        CTEST_EXE="$(command -v ctest || true)"
    fi
fi
[[ -n "$CTEST_EXE" && -x "$CTEST_EXE" ]] || die "ctest was not found; use --ctest"

for tool in codesign ditto hdiutil plutil security shasum spctl xcrun; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool was not found"
done

if [[ -z "$IDENTITY" ]]; then
    IDENTITY="$(
        security find-identity -v -p codesigning |
            awk -F'"' '/"Developer ID Application:/ { print $2 }'
    )"
    IDENTITY_COUNT="$(printf '%s\n' "$IDENTITY" | awk 'NF { count++ } END { print count + 0 }')"
    [[ "$IDENTITY_COUNT" -eq 1 ]] ||
        die "expected exactly one valid Developer ID Application identity; use --identity"
else
    security find-identity -v -p codesigning |
        awk -F'"' -v expected="$IDENTITY" '$2 == expected { found = 1 } END { exit !found }' ||
        die "the requested signing identity is not valid in the keychain"
fi

xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null ||
    die "notarytool could not use keychain profile '$NOTARY_PROFILE'"

VERSION="$(
    sed -nE 's/^[[:space:]]*project\(Talas VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
        "$REPO_ROOT/CMakeLists.txt"
)"
[[ -n "$VERSION" ]] || die "could not read the project version"
ARCH="$(uname -m)"

if [[ -z "$OUTPUT_DIR" ]]; then
    RELEASE_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
    OUTPUT_DIR="$BUILD_DIR/notarized/$RELEASE_STAMP"
fi
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac
case "$OUTPUT_DIR" in
    /*) ;;
    *) OUTPUT_DIR="$REPO_ROOT/$OUTPUT_DIR" ;;
esac
[[ ! -e "$OUTPUT_DIR" ]] || die "output path already exists: $OUTPUT_DIR"

ENTITLEMENTS="$REPO_ROOT/cmake/macos/TalasWorkbench.entitlements"
[[ -f "$ENTITLEMENTS" ]] || die "missing entitlements: $ENTITLEMENTS"
plutil -lint "$ENTITLEMENTS" >/dev/null
HELPER_ENTITLEMENTS="$REPO_ROOT/cmake/macos/TalasComponentHost.entitlements"
[[ -f "$HELPER_ENTITLEMENTS" ]] ||
    die "missing helper entitlements: $HELPER_ENTITLEMENTS"
plutil -lint "$HELPER_ENTITLEMENTS" >/dev/null
PROBE_HELPER_ENTITLEMENTS="$REPO_ROOT/cmake/macos/TalasComponentProbe.entitlements"
[[ -f "$PROBE_HELPER_ENTITLEMENTS" ]] ||
    die "missing probe helper entitlements: $PROBE_HELPER_ENTITLEMENTS"
plutil -lint "$PROBE_HELPER_ENTITLEMENTS" >/dev/null
RUNTIME_HELPER_ENTITLEMENTS="$REPO_ROOT/cmake/macos/TalasComponentRuntime.entitlements"
[[ -f "$RUNTIME_HELPER_ENTITLEMENTS" ]] ||
    die "missing runtime helper entitlements: $RUNTIME_HELPER_ENTITLEMENTS"
plutil -lint "$RUNTIME_HELPER_ENTITLEMENTS" >/dev/null

STAGE_ROOT="$OUTPUT_DIR/staging/Talas-$VERSION"
APP="$STAGE_ROOT/Talas Workbench.app"
CLI="$STAGE_ROOT/bin/talas_cli"
HELPER="$APP/Contents/XPCServices/TalasComponentHost.xpc"
PROBE_HELPER="$APP/Contents/XPCServices/TalasComponentProbe.xpc"
RUNTIME_HELPER="$APP/Contents/XPCServices/TalasComponentRuntime.xpc"
PLUGIN_TEMPLATES="$APP/Contents/Resources/PluginTemplates"
HELPER_ENTITLEMENTS_ACTUAL="$OUTPUT_DIR/helper-entitlements.plist"
HELPER_ENTITLEMENTS_REMAINDER="$OUTPUT_DIR/helper-entitlements-remainder.plist"
PROBE_HELPER_ENTITLEMENTS_ACTUAL="$OUTPUT_DIR/probe-helper-entitlements.plist"
PROBE_HELPER_ENTITLEMENTS_REMAINDER="$OUTPUT_DIR/probe-helper-entitlements-remainder.plist"
RUNTIME_HELPER_ENTITLEMENTS_ACTUAL="$OUTPUT_DIR/runtime-helper-entitlements.plist"
RUNTIME_HELPER_ENTITLEMENTS_REMAINDER="$OUTPUT_DIR/runtime-helper-entitlements-remainder.plist"
APP_NOTARY_ZIP="$OUTPUT_DIR/Talas-$VERSION-app-notary.zip"
PORTABLE_ZIP="$OUTPUT_DIR/Talas-$VERSION-Darwin-$ARCH-notarized.zip"
DMG="$OUTPUT_DIR/Talas-$VERSION-Darwin-$ARCH-notarized.dmg"
APP_RESULT="$OUTPUT_DIR/app-notarization.json"
DMG_RESULT="$OUTPUT_DIR/dmg-notarization.json"

json_value() {
    plutil -extract "$2" raw "$1" 2>/dev/null || true
}

mkdir -p "$STAGE_ROOT"

printf 'Configuring and building release...\n'
JUCE_ARGUMENTS=()
if [[ -n "$JUCE_PATH" ]]; then
    [[ -d "$JUCE_PATH" ]] || die "JUCE path not found: $JUCE_PATH"
    JUCE_ARGUMENTS=(-DTALAS_FETCH_JUCE=OFF "-DTALAS_JUCE_PATH=$JUCE_PATH")
fi
"$CMAKE_EXE" --preset workbench-release -S "$REPO_ROOT" -B "$BUILD_DIR" \
    "${JUCE_ARGUMENTS[@]+"${JUCE_ARGUMENTS[@]}"}"
"$CMAKE_EXE" --build "$BUILD_DIR" --config Release
"$CTEST_EXE" \
    --test-dir "$BUILD_DIR" \
    --no-tests=error \
    --output-on-failure \
    -C Release \
    -R '^talas_workbench_app_smoke$'
"$CMAKE_EXE" --install "$BUILD_DIR" --prefix "$STAGE_ROOT" --config Release

[[ -d "$APP" ]] || die "installed app was not found: $APP"
[[ -x "$CLI" ]] || die "installed CLI was not found: $CLI"
[[ -d "$HELPER" ]] || die "embedded component helper was not found: $HELPER"
[[ -d "$PROBE_HELPER" ]] ||
    die "embedded component probe helper was not found: $PROBE_HELPER"
[[ -d "$RUNTIME_HELPER" ]] ||
    die "embedded component runtime helper was not found: $RUNTIME_HELPER"
[[ -d "$PLUGIN_TEMPLATES" ]] ||
    die "embedded plugin templates were not found: $PLUGIN_TEMPLATES"

printf 'Signing CLI, nested helpers, and application...\n'
codesign \
    --force \
    --options runtime \
    --timestamp \
    --sign "$IDENTITY" \
    "$CLI"
codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$HELPER_ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$HELPER"
codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$PROBE_HELPER_ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$PROBE_HELPER"
codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$RUNTIME_HELPER_ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$RUNTIME_HELPER"
# The plugin templates are code inside Resources, which signing the app does
# not reach; each is signed on its own, before the app seals its resources.
PLUGIN_TEMPLATE_COUNT=0
for template in "$PLUGIN_TEMPLATES"/*.vst3 "$PLUGIN_TEMPLATES"/*.component; do
    [[ -d "$template" ]] || continue
    codesign \
        --force \
        --options runtime \
        --timestamp \
        --sign "$IDENTITY" \
        "$template"
    codesign --verify --strict --verbose=4 "$template"
    PLUGIN_TEMPLATE_COUNT=$((PLUGIN_TEMPLATE_COUNT + 1))
done
[[ $PLUGIN_TEMPLATE_COUNT -gt 0 ]] || die "no plugin templates were signed"
codesign \
    --force \
    --options runtime \
    --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$APP"

codesign --verify --strict --verbose=4 "$CLI"
codesign --verify --strict --verbose=4 "$HELPER"
codesign --verify --strict --verbose=4 "$PROBE_HELPER"
codesign --verify --strict --verbose=4 "$RUNTIME_HELPER"
codesign --verify --deep --strict --verbose=4 "$APP"
codesign -d --xml --entitlements "$HELPER_ENTITLEMENTS_ACTUAL" "$HELPER"
plutil -lint "$HELPER_ENTITLEMENTS_ACTUAL" >/dev/null
[[ "$(plutil -extract 'com\.apple\.security\.app-sandbox' raw \
    "$HELPER_ENTITLEMENTS_ACTUAL")" == "true" ]] ||
    die "embedded component helper is missing App Sandbox entitlement"
cp "$HELPER_ENTITLEMENTS_ACTUAL" "$HELPER_ENTITLEMENTS_REMAINDER"
plutil -remove 'com\.apple\.security\.app-sandbox' \
    "$HELPER_ENTITLEMENTS_REMAINDER"
[[ "$(plutil -convert json -o - "$HELPER_ENTITLEMENTS_REMAINDER")" == "{}" ]] ||
    die "embedded component helper has unexpected entitlements"
codesign -d --xml --entitlements "$PROBE_HELPER_ENTITLEMENTS_ACTUAL" \
    "$PROBE_HELPER"
plutil -lint "$PROBE_HELPER_ENTITLEMENTS_ACTUAL" >/dev/null
[[ "$(plutil -extract 'com\.apple\.security\.app-sandbox' raw \
    "$PROBE_HELPER_ENTITLEMENTS_ACTUAL")" == "true" ]] ||
    die "embedded component probe helper is missing App Sandbox entitlement"
[[ "$(plutil -extract 'com\.apple\.security\.cs\.disable-library-validation' raw \
    "$PROBE_HELPER_ENTITLEMENTS_ACTUAL")" == "true" ]] ||
    die "embedded component probe helper is missing disabled library validation"
cp "$PROBE_HELPER_ENTITLEMENTS_ACTUAL" \
    "$PROBE_HELPER_ENTITLEMENTS_REMAINDER"
plutil -remove 'com\.apple\.security\.app-sandbox' \
    "$PROBE_HELPER_ENTITLEMENTS_REMAINDER"
plutil -remove 'com\.apple\.security\.cs\.disable-library-validation' \
    "$PROBE_HELPER_ENTITLEMENTS_REMAINDER"
[[ "$(plutil -convert json -o - \
    "$PROBE_HELPER_ENTITLEMENTS_REMAINDER")" == "{}" ]] ||
    die "embedded component probe helper has unexpected entitlements"
codesign -d --xml --entitlements "$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL" \
    "$RUNTIME_HELPER"
plutil -lint "$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL" >/dev/null
[[ "$(plutil -extract 'com\.apple\.security\.app-sandbox' raw \
    "$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL")" == "true" ]] ||
    die "embedded component runtime helper is missing App Sandbox entitlement"
[[ "$(plutil -extract 'com\.apple\.security\.cs\.disable-library-validation' raw \
    "$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL")" == "true" ]] ||
    die "embedded component runtime helper is missing disabled library validation"
cp "$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL" \
    "$RUNTIME_HELPER_ENTITLEMENTS_REMAINDER"
plutil -remove 'com\.apple\.security\.app-sandbox' \
    "$RUNTIME_HELPER_ENTITLEMENTS_REMAINDER"
plutil -remove 'com\.apple\.security\.cs\.disable-library-validation' \
    "$RUNTIME_HELPER_ENTITLEMENTS_REMAINDER"
[[ "$(plutil -convert json -o - \
    "$RUNTIME_HELPER_ENTITLEMENTS_REMAINDER")" == "{}" ]] ||
    die "embedded component runtime helper has unexpected entitlements"

SMOKE_DIR="$OUTPUT_DIR/smoke-test"
mkdir -p "$SMOKE_DIR"
"$APP/Contents/MacOS/Talas Workbench" --smoke-test "$SMOKE_DIR"

printf 'Submitting application to Apple notary service...\n'
ditto --rsrc --extattr -c -k --keepParent "$APP" "$APP_NOTARY_ZIP"
if ! xcrun notarytool submit \
    "$APP_NOTARY_ZIP" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait \
    --output-format json >"$APP_RESULT"; then
    die "application notarization request failed; see $APP_RESULT"
fi
APP_STATUS="$(json_value "$APP_RESULT" status)"
APP_SUBMISSION_ID="$(json_value "$APP_RESULT" id)"
if [[ "$APP_STATUS" != "Accepted" ]]; then
    if [[ -n "$APP_SUBMISSION_ID" ]]; then
        xcrun notarytool log \
            "$APP_SUBMISSION_ID" \
            --keychain-profile "$NOTARY_PROFILE" \
            "$OUTPUT_DIR/app-notarization-log.json" || true
    fi
    die "Apple did not accept the application (status: ${APP_STATUS:-unknown})"
fi

xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose=4 "$APP"

printf 'Creating and signing distribution archives...\n'
ditto --rsrc --extattr -c -k --keepParent "$STAGE_ROOT" "$PORTABLE_ZIP"
hdiutil create \
    -volname "Talas $VERSION" \
    -srcfolder "$STAGE_ROOT" \
    -ov \
    -format UDZO \
    "$DMG"
codesign --force --timestamp --sign "$IDENTITY" "$DMG"
codesign --verify --strict --verbose=4 "$DMG"

printf 'Submitting disk image to Apple notary service...\n'
if ! xcrun notarytool submit \
    "$DMG" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait \
    --output-format json >"$DMG_RESULT"; then
    die "disk-image notarization request failed; see $DMG_RESULT"
fi
DMG_STATUS="$(json_value "$DMG_RESULT" status)"
DMG_SUBMISSION_ID="$(json_value "$DMG_RESULT" id)"
if [[ "$DMG_STATUS" != "Accepted" ]]; then
    if [[ -n "$DMG_SUBMISSION_ID" ]]; then
        xcrun notarytool log \
            "$DMG_SUBMISSION_ID" \
            --keychain-profile "$NOTARY_PROFILE" \
            "$OUTPUT_DIR/dmg-notarization-log.json" || true
    fi
    die "Apple did not accept the disk image (status: ${DMG_STATUS:-unknown})"
fi

xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
spctl \
    --assess \
    --type open \
    --context context:primary-signature \
    --verbose=4 \
    "$DMG"

(
    cd "$OUTPUT_DIR"
    shasum \
        -a 256 \
        "$(basename "$DMG")" \
        "$(basename "$PORTABLE_ZIP")" >SHA256SUMS
)

printf '\nNotarized release is ready:\n'
printf '  DMG: %s\n' "$DMG"
printf '  ZIP: %s\n' "$PORTABLE_ZIP"
printf '  App submission: %s\n' "$APP_SUBMISSION_ID"
printf '  DMG submission: %s\n' "$DMG_SUBMISSION_ID"
