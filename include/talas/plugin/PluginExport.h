#pragma once

#include "talas/plugin/PluginIdentity.h"
#include "talas/plugin/PluginParameters.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ADR-0029: turning a project into a VST3 or AU plugin without a compiler.
// The exporter copies a prebuilt template bundle, embeds the project and the
// plugin's identity, rewrites the bundle's Info.plist and VST3
// moduleinfo.json, and ad-hoc signs it on macOS. JUCE-free, so talas_cli
// and the Workbench share it.
namespace talas::plugin
{
enum class PluginFormat
{
    Vst3,
    AudioUnit,
};

inline constexpr std::string_view pluginTemplateDirectoryName = "PluginTemplates";
// A list of template folders (':'-separated, ';' on Windows) searched first.
inline constexpr std::string_view pluginTemplateEnvironmentVariable = "TALAS_PLUGIN_TEMPLATES";
inline constexpr std::string_view vst3SdkVersionString = "VST 3.8.0";

[[nodiscard]] std::string_view pluginFormatName(PluginFormat format) noexcept;
[[nodiscard]] std::string_view pluginFormatExtension(PluginFormat format) noexcept;
// "vst3" or "au", in any case.
[[nodiscard]] std::optional<PluginFormat> parsePluginFormat(std::string_view text) noexcept;
// The template bundles' base names: "Talas Instrument" and "Talas Effect".
[[nodiscard]] std::string_view templateBundleBaseName(PluginKind kind) noexcept;

// The AU's name as hosts list it, "Manufacturer: Name", and its component
// type: 'aumu' for an instrument, 'aufx' for an effect.
[[nodiscard]] std::string auComponentName(const PluginIdentity& identity);
[[nodiscard]] std::string_view auComponentType(PluginKind kind) noexcept;

struct InfoPlistRewrite
{
    std::string plist;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// A template's Info.plist with the identity's bundle name, identifier,
// executable and version and, for an AU, its AudioComponents entry; the
// factory function and every other key stay as the template has them.
[[nodiscard]] InfoPlistRewrite rewritePluginInfoPlist(
    std::string_view templatePlist,
    const PluginIdentity& identity,
    PluginFormat format,
    std::string_view executableName);

// The VST3 Contents/Resources/moduleinfo.json for the identity, as JUCE's
// VST3 manifest helper writes one: the factory, then the component and
// edit controller classes with the IDs JUCE gives them at load time.
[[nodiscard]] std::string vst3ModuleInfoJson(const PluginIdentity& identity);

// The Workbench's app bundle, whose Resources hold the templates.
inline constexpr std::string_view workbenchAppBundleName = "Talas Workbench.app";

// The template folders for a program at this path: PluginTemplates next to
// it, in its app bundle's Resources, in ../share/talas, and in the Workbench
// app beside its folder (a release's bin/talas_cli next to the app); on macOS
// then the Workbench installed in /Applications and ~/Applications.
[[nodiscard]] std::vector<std::filesystem::path> pluginTemplateDirectoriesFor(
    const std::filesystem::path& executable);
// The template folders searched when a request names none: the
// TALAS_PLUGIN_TEMPLATES folders, then those for the running program.
[[nodiscard]] std::vector<std::filesystem::path> defaultPluginTemplateDirectories();
[[nodiscard]] std::optional<std::filesystem::path> findPluginTemplate(
    std::span<const std::filesystem::path> directories,
    PluginKind kind,
    PluginFormat format);

struct PluginExportRequest
{
    // The project to export: a .talas.json file or .talaspkg package.
    std::optional<std::filesystem::path> projectPath;
    // Or a document in memory (the Workbench's); projectPath, when given,
    // is still where its relative asset paths resolve.
    std::optional<talas::project::ProjectDocument> document;
    std::string name;
    std::string manufacturer{defaultPluginManufacturer};
    PluginKind kind = PluginKind::Effect;
    std::vector<PluginFormat> formats;
    std::filesystem::path outputDirectory;
    // Searched in order; empty: defaultPluginTemplateDirectories().
    std::vector<std::filesystem::path> templateDirectories;
    // Empty: derived from the names (see PluginIdentity.h).
    std::string manufacturerCode;
    std::string pluginCode;
    std::string version{defaultPluginVersion};
    // macOS: ad-hoc sign each bundle (needed for it to load on Apple silicon).
    bool sign = true;
};

struct PluginExportedBundle
{
    PluginFormat format = PluginFormat::Vst3;
    std::filesystem::path path;
};

struct PluginExportResult
{
    std::string error;
    std::vector<std::string> warnings;
    std::optional<PluginIdentity> identity;
    std::vector<PluginExportedBundle> bundles;
    std::vector<PluginParameterSpec> parameters;
    std::size_t latencyFrames = 0;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Stages the project once from inside the first bundle (its assets loading
// from there) before writing anything to outputDirectory; an export that
// would not play fails instead. Replaces an earlier Talas export of the same
// file name, never anything else.
[[nodiscard]] PluginExportResult exportPlugin(const PluginExportRequest& request);

// What an exported plugin is licensed under, for the CLI and the Workbench.
[[nodiscard]] std::string_view pluginExportLicenseNotice() noexcept;
}
