#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// ADR-0029: the few platform-specific steps plugin export needs, behind small
// functions. Only macOS is verified; the Windows and Linux branches keep the
// code portable.
namespace talas::plugin::platform
{
// The binary (executable, dylib, bundle or DLL) holding the address.
[[nodiscard]] std::optional<std::filesystem::path> modulePathContaining(const void* address);
[[nodiscard]] std::optional<std::filesystem::path> currentExecutablePath();

// A plugin binary's bundle's Contents/Resources: the binary sits in
// Contents/MacOS (macOS), Contents/<arch>-win (Windows) or
// Contents/<arch>-linux (Linux), so the resources are two folders up.
[[nodiscard]] std::filesystem::path bundleResourcesForBinary(const std::filesystem::path& binary);

// Whether bundles carry an Info.plist (macOS) and Audio Units exist.
[[nodiscard]] bool bundlesUseInfoPlist() noexcept;
[[nodiscard]] bool supportsAudioUnits() noexcept;

struct ProcessResult
{
    int exitCode = -1;
    std::string output;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && exitCode == 0; }
};

// Runs a program with arguments, no shell, collecting its stdout and stderr.
[[nodiscard]] ProcessResult runProcess(const std::vector<std::string>& arguments);

// macOS: codesign --force --deep --sign - <bundle>, then verifies it.
// Elsewhere nothing to do. Empty on success, else what failed.
[[nodiscard]] std::string adHocSignBundle(const std::filesystem::path& bundle);
[[nodiscard]] std::string verifyBundleSignature(const std::filesystem::path& bundle);

// The folders hosts scan for installed plugins (read only; export never
// writes there).
[[nodiscard]] std::vector<std::filesystem::path> installedPluginFolders();
}
