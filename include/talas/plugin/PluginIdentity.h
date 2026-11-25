#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// ADR-0029: the identity an exported plugin reads from its own bundle at load
// time, and how the exporter derives it. JUCE-free, so the CLI and the tests
// share it with the plugin templates.
namespace talas::plugin
{
enum class PluginKind
{
    Instrument,
    Effect,
};

inline constexpr std::string_view pluginIdentityFileName = "talas-plugin.json";
inline constexpr std::string_view pluginProjectFileName = "project.talas.json";
inline constexpr std::string_view pluginIdentityFormat = "talas.plugin-identity";
inline constexpr int pluginIdentityFormatVersion = 1;
// The manufacturer an export takes when none is given.
inline constexpr std::string_view defaultPluginManufacturer = "Talas";
inline constexpr std::string_view defaultPluginVersion = "1.0.0";
inline constexpr std::size_t maximumPluginNameBytes = 63;

struct PluginIdentity
{
    std::string name;
    std::string manufacturer;
    // Four printable ASCII characters each; see derive*Code below.
    std::string manufacturerCode;
    std::string pluginCode;
    PluginKind kind = PluginKind::Effect;
    // "major.minor.patch", each part 0 to 255.
    std::string version{defaultPluginVersion};
    std::string bundleIdentifier;
    std::string description;
    // The FNV-1a hash of the embedded project.talas.json, as hex; empty for
    // the raw template.
    std::string projectHash;

    bool operator==(const PluginIdentity&) const = default;
};

[[nodiscard]] std::string_view pluginKindName(PluginKind kind) noexcept;
[[nodiscard]] std::optional<PluginKind> parsePluginKind(std::string_view text) noexcept;

// The identity of a raw template, which a template keeps when its bundle holds
// no talas-plugin.json.
[[nodiscard]] PluginIdentity templatePluginIdentity(PluginKind kind);

// 'Abcd' packed big-endian, as JUCE's JucePlugin_*Code macros and the AU
// component description hold it. Zero for anything but four characters.
[[nodiscard]] std::uint32_t fourCharCode(std::string_view code) noexcept;
[[nodiscard]] std::string fourCharCodeText(std::uint32_t code);

// Four printable ASCII characters other than quotes, backslash and space,
// at least one of them an upper-case letter (Apple reserves all-lower-case
// manufacturer codes).
[[nodiscard]] bool isValidManufacturerCode(std::string_view code) noexcept;
// Four letters or digits, the first an upper-case letter and the rest not
// upper-case, as JUCE asks for AU compatibility (GarageBand rejects others).
[[nodiscard]] bool isValidPluginCode(std::string_view code) noexcept;

// The manufacturer code a manufacturer name gives: its first four letters or
// digits, the first upper-cased and the rest lower-cased, padded from a hash
// of the name when it has fewer.
[[nodiscard]] std::string deriveManufacturerCode(std::string_view manufacturer);
// The plugin code a manufacturer and plugin name give: a stable hash of both
// spelt as [A-Z][a-z0-9]{3}. A non-zero salt picks another code for the same
// names, which the exporter uses when a different plugin already holds one.
[[nodiscard]] std::string derivePluginCode(
    std::string_view manufacturer,
    std::string_view name,
    std::uint32_t salt = 0);

// 64-bit FNV-1a of the bytes, as 16 lower-case hex digits.
[[nodiscard]] std::string stableHashHex(std::string_view bytes);
[[nodiscard]] std::uint64_t stableHash(std::string_view bytes) noexcept;

// (major << 16) | (minor << 8) | patch, or nullopt for a malformed version.
[[nodiscard]] std::optional<std::uint32_t> pluginVersionCode(std::string_view version) noexcept;

// A bundle identifier for the names: com.talas.plugin.<manufacturer>.<name>,
// each part reduced to letters, digits and hyphens.
[[nodiscard]] std::string derivePluginBundleIdentifier(
    std::string_view manufacturer,
    std::string_view name);
// The name made safe as a file name on every platform: no path separators,
// reserved characters or trailing dots and spaces; never empty.
[[nodiscard]] std::string pluginFileSystemName(std::string_view name);

// Empty when the identity is complete and well formed; otherwise what is
// wrong with it.
[[nodiscard]] std::string validatePluginIdentity(const PluginIdentity& identity);

[[nodiscard]] std::string serializePluginIdentity(const PluginIdentity& identity);
[[nodiscard]] std::optional<PluginIdentity> parsePluginIdentity(
    std::string_view json,
    std::string* error = nullptr);

// The VST3 class ID JUCE gives the component (or edit controller) of a plugin
// with these codes, as the 32 upper-case hex digits moduleinfo.json holds.
// It is JUCE's VST3Interface::jucePluginId spelt as VST3::UID::toString
// prints it, the same on every platform.
[[nodiscard]] std::string vst3ClassIdText(
    std::uint32_t manufacturerCode,
    std::uint32_t pluginCode,
    bool controller);
}
