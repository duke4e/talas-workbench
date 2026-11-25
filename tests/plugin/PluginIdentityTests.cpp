#include "talas/plugin/PluginIdentity.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
using namespace talas::plugin;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testFourCharCodes()
{
    require(fourCharCode("Tals") == 0x54616C73U, "Tals packs big-endian as JUCE's code macros do");
    require(fourCharCode("Tal") == 0 && fourCharCode("Talas") == 0, "only four characters make a code");
    require(fourCharCodeText(0x54696E73U) == "Tins", "a code unpacks to its characters");
    require(isValidManufacturerCode("Tals") && isValidManufacturerCode("ABCD") && isValidManufacturerCode("a1B_"),
        "a manufacturer code needs four printable characters and one upper-case letter");
    require(!isValidManufacturerCode("tals"), "Apple reserves all-lower-case manufacturer codes");
    require(!isValidManufacturerCode("Ta s") && !isValidManufacturerCode("Ta\"s") && !isValidManufacturerCode("Tal"),
        "spaces, quotes and short codes are not manufacturer codes");
    require(isValidPluginCode("Tins") && isValidPluginCode("A0b9"), "a plugin code is [A-Z][a-z0-9]{3}");
    require(!isValidPluginCode("tins") && !isValidPluginCode("TIns") && !isValidPluginCode("Ti-s"),
        "a plugin code has exactly one upper-case letter, first");
}

void testManufacturerCodeDerivation()
{
    require(deriveManufacturerCode("Talas") == "Tala", "a manufacturer's first four letters, capitalised");
    require(deriveManufacturerCode("duke4e") == "Duke", "the first letter is upper-cased, the rest lower-cased");
    require(deriveManufacturerCode("Big Fish Audio") == "Bigf", "spaces are skipped");
    const auto shortCode = deriveManufacturerCode("Ab");
    require(shortCode.size() == 4 && shortCode.starts_with("Ab") && isValidManufacturerCode(shortCode),
        "a short name is padded from its hash: " + shortCode);
    require(shortCode == deriveManufacturerCode("Ab"), "the padding is stable");
    for (const auto* name : {"", "123 Audio", "!!!", "\xc3\xa9t\xc3\xa9", "x"})
    {
        const auto code = deriveManufacturerCode(name);
        require(isValidManufacturerCode(code), std::string{"every manufacturer gives a valid code: "} + code);
    }
}

void testPluginCodeDerivation()
{
    const auto code = derivePluginCode("Talas", "Sub Bass");
    require(isValidPluginCode(code), "a derived plugin code is valid: " + code);
    require(code == derivePluginCode("Talas", "Sub Bass"), "the plugin code is a stable hash of the names");
    require(code != derivePluginCode("Talas", "Sub Bass 2"), "another name gives another code");
    require(code != derivePluginCode("Other", "Sub Bass"), "another manufacturer gives another code");
    require(code != derivePluginCode("Talas", "Sub Bass", 1), "a salt picks another code for the same names");
    require(derivePluginCode("Talas", "Sub Bass", 1) == derivePluginCode("Talas", "Sub Bass", 1),
        "a salted code is stable too");
    std::set<std::string> codes;
    constexpr int count = 2000;
    for (int index = 0; index < count; ++index)
    {
        const auto derived = derivePluginCode("Talas", "Plugin " + std::to_string(index));
        require(isValidPluginCode(derived), "every derived plugin code is valid");
        codes.insert(derived);
    }
    // 26 x 36^3 codes: two thousand names collide a couple of times at most.
    require(codes.size() >= count - 10, "derived codes spread over the code space: " + std::to_string(codes.size()));
}

void testVersionsAndNames()
{
    require(pluginVersionCode("1.2.3") == 0x010203U, "a version packs as (major << 16) | (minor << 8) | patch");
    require(pluginVersionCode("0.0.0") == 0U && pluginVersionCode("255.255.255") == 0xFFFFFFU, "parts run 0 to 255");
    for (const auto* bad : {"", "1", "1.2", "1.2.3.4", "256.0.0", "a.b.c", "1..2", "1.2.", "-1.0.0", "1.2.3 "})
        require(!pluginVersionCode(bad).has_value(), std::string{"a malformed version is rejected: "} + bad);
    require(derivePluginBundleIdentifier("Talas", "My Synth!") == "com.talas.plugin.talas.my-synth",
        "a bundle identifier keeps letters, digits and hyphens");
    require(derivePluginBundleIdentifier("", "***") == "com.talas.plugin.manufacturer.plugin",
        "an empty part has a fallback");
    require(pluginFileSystemName("A/B:C") == "A-B-C", "path separators and reserved characters become hyphens");
    require(pluginFileSystemName(" Pad. ") == "Pad", "leading and trailing dots and spaces go");
    require(pluginFileSystemName("...") == "Talas Plugin", "a name never becomes empty");
    require(stableHashHex("") == "cbf29ce484222325", "the hash is 64-bit FNV-1a");
    require(stableHashHex("a") == "af63dc4c8601ec8c", "the hash is 64-bit FNV-1a of the bytes");
}

void testIdentityFile()
{
    auto identity = templatePluginIdentity(PluginKind::Instrument);
    require(validatePluginIdentity(identity).empty(), "the template identity is valid");
    require(validatePluginIdentity(templatePluginIdentity(PluginKind::Effect)).empty(), "so is the effect's");
    identity.name = "Sub \"Bass\" \xe2\x80\x93 Deluxe";
    identity.manufacturer = "duke4e";
    identity.manufacturerCode = deriveManufacturerCode(identity.manufacturer);
    identity.pluginCode = derivePluginCode(identity.manufacturer, identity.name);
    identity.version = "2.0.1";
    identity.bundleIdentifier = derivePluginBundleIdentifier(identity.manufacturer, identity.name);
    identity.projectHash = stableHashHex("project");
    const auto text = serializePluginIdentity(identity);
    std::string error;
    const auto parsed = parsePluginIdentity(text, &error);
    require(parsed.has_value(), "the identity file parses back: " + error);
    require(*parsed == identity, "the identity file round-trips every field");

    const auto rejects = [](std::string text, std::string_view expected) {
        std::string problem;
        const auto parsed = parsePluginIdentity(text, &problem);
        require(!parsed.has_value() && problem.find(expected) != std::string::npos,
            "a bad identity file is rejected with '" + std::string{expected} + "', got '" + problem + "'");
    };
    const auto replace = [&](std::string_view from, std::string_view to) {
        auto changed = text;
        const auto at = changed.find(from);
        require(at != std::string::npos, "the identity file holds " + std::string{from});
        changed.replace(at, from.size(), to);
        return changed;
    };
    rejects("not json", "not valid JSON");
    rejects("[]", "must hold an object");
    rejects(replace("talas.plugin-identity", "talas.other"), "format");
    rejects(replace("\"version\": 1", "\"version\": 2"), "unsupported version");
    rejects(replace("\"kind\": \"instrument\"", "\"kind\": \"synth\""), "kind");
    rejects(replace("\"manufacturerCode\": \"Duke\"", "\"manufacturerCode\": \"duke\""), "manufacturer code");
    rejects(replace("\"pluginVersion\": \"2.0.1\"", "\"pluginVersion\": \"2.0\""), "version '2.0'");
    rejects(replace("\"name\": ", "\"title\": "), "lacks \"name\"");

    auto invalid = identity;
    invalid.name.clear();
    require(!validatePluginIdentity(invalid).empty(), "a plugin needs a name");
    invalid = identity;
    invalid.name = std::string(64, 'x');
    require(!validatePluginIdentity(invalid).empty(), "a name is at most 63 bytes");
    invalid = identity;
    invalid.name = "Tab\there";
    require(!validatePluginIdentity(invalid).empty(), "a name holds no control characters");
    invalid = identity;
    invalid.bundleIdentifier = "com.talas plugin";
    require(!validatePluginIdentity(invalid).empty(), "a bundle identifier holds no spaces");
}

void testVst3ClassIds()
{
    // JUCE's VST3Interface::jucePluginId for 'Tals'/'Tins', as
    // VST3::UID::toString prints it; the integration test checks the same
    // against JUCE itself.
    require(vst3ClassIdText(fourCharCode("Tals"), fourCharCode("Tins"), false)
            == "ABCDEF019182FAEB54616C7354696E73",
        "the component class ID is JUCE's: two fixed words, then the codes");
    require(vst3ClassIdText(fourCharCode("Tals"), fourCharCode("Tins"), true)
            == "ABCDEF011234ABCD54616C7354696E73",
        "the edit controller's second word differs");
}
}

int main()
{
    try
    {
        testFourCharCodes();
        testManufacturerCodeDerivation();
        testPluginCodeDerivation();
        testVersionsAndNames();
        testIdentityFile();
        testVst3ClassIds();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "talas_plugin_identity_tests passed\n";
    return 0;
}
