#include "talas/plugin/PluginIdentity.h"

#include "project/JsonValue.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace talas::plugin
{
namespace
{
namespace json = talas::project::json;

[[nodiscard]] bool isAsciiUpper(char character) noexcept
{
    return character >= 'A' && character <= 'Z';
}

[[nodiscard]] bool isAsciiLower(char character) noexcept
{
    return character >= 'a' && character <= 'z';
}

[[nodiscard]] bool isAsciiDigit(char character) noexcept
{
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool isAsciiAlphanumeric(char character) noexcept
{
    return isAsciiUpper(character) || isAsciiLower(character) || isAsciiDigit(character);
}

[[nodiscard]] char asciiLower(char character) noexcept
{
    return isAsciiUpper(character) ? static_cast<char>(character - 'A' + 'a') : character;
}

[[nodiscard]] char asciiUpper(char character) noexcept
{
    return isAsciiLower(character) ? static_cast<char>(character - 'a' + 'A') : character;
}

// The bundle identifier part for a name: letters, digits and hyphens.
[[nodiscard]] std::string identifierPart(std::string_view text, std::string_view fallback)
{
    std::string part;
    bool pendingHyphen = false;
    for (const auto character : text)
    {
        if (isAsciiAlphanumeric(character))
        {
            if (pendingHyphen && !part.empty())
                part.push_back('-');
            pendingHyphen = false;
            part.push_back(asciiLower(character));
        }
        else
        {
            pendingHyphen = true;
        }
    }
    return part.empty() ? std::string{fallback} : part;
}

[[nodiscard]] const json::Value* member(const json::Value& object, std::string_view key)
{
    return json::findObjectMember(object, key);
}

[[nodiscard]] bool readString(
    const json::Value& object,
    std::string_view key,
    std::string& out,
    bool required,
    std::string* error)
{
    const auto* value = member(object, key);
    if (value == nullptr)
    {
        if (required && error != nullptr)
            *error = "talas-plugin.json lacks \"" + std::string{key} + "\"";
        return !required;
    }
    if (!value->isString())
    {
        if (error != nullptr)
            *error = "talas-plugin.json \"" + std::string{key} + "\" must be a string";
        return false;
    }
    out = value->string();
    return true;
}
}

std::string_view pluginKindName(PluginKind kind) noexcept
{
    return kind == PluginKind::Instrument ? "instrument" : "effect";
}

std::optional<PluginKind> parsePluginKind(std::string_view text) noexcept
{
    if (text == "instrument")
        return PluginKind::Instrument;
    if (text == "effect")
        return PluginKind::Effect;
    return std::nullopt;
}

PluginIdentity templatePluginIdentity(PluginKind kind)
{
    PluginIdentity identity;
    identity.kind = kind;
    identity.manufacturer = std::string{defaultPluginManufacturer};
    identity.manufacturerCode = "Tals";
    if (kind == PluginKind::Instrument)
    {
        identity.name = "Talas Instrument";
        identity.pluginCode = "Tins";
        identity.bundleIdentifier = "com.talas.plugin.template.instrument";
        identity.description = "A Talas Sound played as an instrument";
    }
    else
    {
        identity.name = "Talas Effect";
        identity.pluginCode = "Tfxs";
        identity.bundleIdentifier = "com.talas.plugin.template.effect";
        identity.description = "A Talas Sound used as an effect";
    }
    identity.version = std::string{defaultPluginVersion};
    return identity;
}

std::uint32_t fourCharCode(std::string_view code) noexcept
{
    if (code.size() != 4)
        return 0;
    std::uint32_t value = 0;
    for (const auto character : code)
        value = (value << 8U) | static_cast<std::uint8_t>(character);
    return value;
}

std::string fourCharCodeText(std::uint32_t code)
{
    std::string text(4, ' ');
    for (std::size_t index = 0; index < 4; ++index)
        text[index] = static_cast<char>((code >> (8U * (3U - index))) & 0xFFU);
    return text;
}

bool isValidManufacturerCode(std::string_view code) noexcept
{
    if (code.size() != 4)
        return false;
    bool hasUpper = false;
    for (const auto character : code)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte >= 0x7FU || character == '"' || character == '\\' || character == '\'')
            return false;
        hasUpper = hasUpper || isAsciiUpper(character);
    }
    return hasUpper;
}

bool isValidPluginCode(std::string_view code) noexcept
{
    if (code.size() != 4 || !isAsciiUpper(code[0]))
        return false;
    return std::all_of(code.begin() + 1, code.end(), [](char character) {
        return isAsciiLower(character) || isAsciiDigit(character);
    });
}

std::uint64_t stableHash(std::string_view bytes) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto character : bytes)
    {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string stableHashHex(std::string_view bytes)
{
    char text[17] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(stableHash(bytes)));
    return text;
}

std::string deriveManufacturerCode(std::string_view manufacturer)
{
    std::string code;
    for (const auto character : manufacturer)
    {
        if (!isAsciiAlphanumeric(character))
            continue;
        code.push_back(code.empty() ? asciiUpper(character) : asciiLower(character));
        if (code.size() == 4)
            break;
    }
    if (code.empty() || !isAsciiUpper(code.front()))
        code.insert(code.begin(), 'T');
    constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    auto hash = stableHash(manufacturer);
    while (code.size() < 4)
    {
        code.push_back(alphabet[hash % alphabet.size()]);
        hash /= alphabet.size();
    }
    code.resize(4);
    return code;
}

std::string derivePluginCode(std::string_view manufacturer, std::string_view name, std::uint32_t salt)
{
    std::string key;
    key.reserve(manufacturer.size() + name.size() + 16);
    key += "talas-plugin-code\x1f";
    key += manufacturer;
    key += '\x1f';
    key += name;
    if (salt != 0)
    {
        key += '\x1f';
        key += std::to_string(salt);
    }
    auto hash = stableHash(key);
    constexpr std::string_view upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::string_view rest = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string code;
    code.push_back(upper[hash % upper.size()]);
    hash /= upper.size();
    for (int index = 0; index < 3; ++index)
    {
        code.push_back(rest[hash % rest.size()]);
        hash /= rest.size();
    }
    return code;
}

std::optional<std::uint32_t> pluginVersionCode(std::string_view version) noexcept
{
    std::array<std::uint32_t, 3> parts{};
    std::size_t partIndex = 0;
    const char* cursor = version.data();
    const char* end = version.data() + version.size();
    if (version.empty())
        return std::nullopt;
    while (true)
    {
        if (partIndex >= parts.size())
            return std::nullopt;
        std::uint32_t value = 0;
        const auto [next, status] = std::from_chars(cursor, end, value);
        if (status != std::errc{} || next == cursor || value > 255U)
            return std::nullopt;
        parts[partIndex++] = value;
        cursor = next;
        if (cursor == end)
            break;
        if (*cursor != '.')
            return std::nullopt;
        ++cursor;
    }
    if (partIndex != parts.size())
        return std::nullopt;
    return (parts[0] << 16U) | (parts[1] << 8U) | parts[2];
}

std::string derivePluginBundleIdentifier(std::string_view manufacturer, std::string_view name)
{
    return "com.talas.plugin." + identifierPart(manufacturer, "manufacturer") + "."
        + identifierPart(name, "plugin");
}

std::string pluginFileSystemName(std::string_view name)
{
    std::string safe;
    for (const auto character : name)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || character == '/' || character == '\\' || character == ':' || character == '*'
            || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
        {
            safe.push_back('-');
            continue;
        }
        safe.push_back(character);
    }
    while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
        safe.pop_back();
    while (!safe.empty() && (safe.front() == '.' || safe.front() == ' '))
        safe.erase(safe.begin());
    return safe.empty() ? std::string{"Talas Plugin"} : safe;
}

std::string validatePluginIdentity(const PluginIdentity& identity)
{
    const auto printable = [](std::string_view text) {
        return std::all_of(text.begin(), text.end(), [](char character) {
            return static_cast<unsigned char>(character) >= 0x20U && character != 0x7F;
        });
    };
    if (identity.name.empty())
        return "The plugin needs a name";
    if (identity.name.size() > maximumPluginNameBytes)
        return "The plugin name is longer than " + std::to_string(maximumPluginNameBytes) + " bytes";
    if (!printable(identity.name))
        return "The plugin name holds control characters";
    if (!json::isValidUtf8(identity.name) || !json::isValidUtf8(identity.manufacturer)
        || !json::isValidUtf8(identity.description))
        return "The plugin's texts must be UTF-8";
    if (identity.manufacturer.empty())
        return "The plugin needs a manufacturer";
    if (identity.manufacturer.size() > maximumPluginNameBytes)
        return "The manufacturer name is longer than " + std::to_string(maximumPluginNameBytes) + " bytes";
    if (!printable(identity.manufacturer))
        return "The manufacturer name holds control characters";
    if (!isValidManufacturerCode(identity.manufacturerCode))
        return "The manufacturer code '" + identity.manufacturerCode
            + "' must be four printable characters with at least one upper-case letter";
    if (!isValidPluginCode(identity.pluginCode))
        return "The plugin code '" + identity.pluginCode
            + "' must be an upper-case letter followed by three lower-case letters or digits";
    if (!pluginVersionCode(identity.version).has_value())
        return "The version '" + identity.version + "' must be major.minor.patch, each 0 to 255";
    if (identity.bundleIdentifier.empty())
        return "The plugin needs a bundle identifier";
    for (const auto character : identity.bundleIdentifier)
    {
        if (!isAsciiAlphanumeric(character) && character != '.' && character != '-')
            return "The bundle identifier may hold only letters, digits, dots and hyphens";
    }
    return {};
}

std::string serializePluginIdentity(const PluginIdentity& identity)
{
    std::string text = "{\n";
    const auto field = [&text](std::string_view key, std::string_view value, bool last = false) {
        text += "  ";
        json::appendEscapedString(text, key);
        text += ": ";
        json::appendEscapedString(text, value);
        text += last ? "\n" : ",\n";
    };
    field("format", pluginIdentityFormat);
    text += "  \"version\": " + std::to_string(pluginIdentityFormatVersion) + ",\n";
    field("name", identity.name);
    field("manufacturer", identity.manufacturer);
    field("manufacturerCode", identity.manufacturerCode);
    field("pluginCode", identity.pluginCode);
    field("kind", pluginKindName(identity.kind));
    field("pluginVersion", identity.version);
    field("bundleIdentifier", identity.bundleIdentifier);
    field("description", identity.description);
    field("projectHash", identity.projectHash, true);
    text += "}\n";
    return text;
}

std::optional<PluginIdentity> parsePluginIdentity(std::string_view text, std::string* error)
{
    json::ParseLimits limits;
    limits.maximumInputBytes = 64 * 1024;
    limits.maximumNestingDepth = 4;
    limits.requireValidUtf8 = true;
    const auto parsed = json::parse(text, limits);
    if (!parsed.ok() || !parsed.value.isObject())
    {
        if (error != nullptr)
            *error = parsed.ok() ? "talas-plugin.json must hold an object"
                                 : "talas-plugin.json is not valid JSON: " + parsed.errors.front().message;
        return std::nullopt;
    }
    const auto& root = parsed.value;
    std::string format;
    if (!readString(root, "format", format, true, error))
        return std::nullopt;
    if (format != pluginIdentityFormat)
    {
        if (error != nullptr)
            *error = "talas-plugin.json has format '" + format + "', not " + std::string{pluginIdentityFormat};
        return std::nullopt;
    }
    const auto* version = member(root, "version");
    if (version == nullptr || !version->isNumber() || version->number() != pluginIdentityFormatVersion)
    {
        if (error != nullptr)
            *error = "talas-plugin.json has an unsupported version";
        return std::nullopt;
    }
    PluginIdentity identity;
    std::string kind;
    if (!readString(root, "name", identity.name, true, error)
        || !readString(root, "manufacturer", identity.manufacturer, true, error)
        || !readString(root, "manufacturerCode", identity.manufacturerCode, true, error)
        || !readString(root, "pluginCode", identity.pluginCode, true, error)
        || !readString(root, "kind", kind, true, error)
        || !readString(root, "pluginVersion", identity.version, true, error)
        || !readString(root, "bundleIdentifier", identity.bundleIdentifier, true, error)
        || !readString(root, "description", identity.description, false, error)
        || !readString(root, "projectHash", identity.projectHash, false, error))
    {
        return std::nullopt;
    }
    const auto parsedKind = parsePluginKind(kind);
    if (!parsedKind.has_value())
    {
        if (error != nullptr)
            *error = "talas-plugin.json kind must be \"instrument\" or \"effect\"";
        return std::nullopt;
    }
    identity.kind = *parsedKind;
    if (auto problem = validatePluginIdentity(identity); !problem.empty())
    {
        if (error != nullptr)
            *error = std::move(problem);
        return std::nullopt;
    }
    return identity;
}

std::string vst3ClassIdText(std::uint32_t manufacturerCode, std::uint32_t pluginCode, bool controller)
{
    // JUCE's VST3Interface::jucePluginId: two fixed words for the interface
    // type, then the manufacturer and plugin codes.
    const std::uint32_t word0 = 0xABCDEF01U;
    const std::uint32_t word1 = controller ? 0x1234ABCDU : 0x9182FAEBU;
    char text[33] = {};
    std::snprintf(text, sizeof(text), "%08X%08X%08X%08X", static_cast<unsigned>(word0), static_cast<unsigned>(word1),
        static_cast<unsigned>(manufacturerCode), static_cast<unsigned>(pluginCode));
    return text;
}
}
