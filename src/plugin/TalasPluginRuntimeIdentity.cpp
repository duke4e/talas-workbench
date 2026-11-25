#include "plugin/TalasPluginRuntimeIdentity.h"

#include "talas/plugin/PluginPlatform.h"

#include <fstream>
#include <iterator>

// The JUCE wrappers call these from static initialisers (the VST3 class IDs
// are inline static members), before JUCE itself is usable, so this file
// uses the standard library alone: it finds its own binary with dladdr (or
// GetModuleHandleEx), which is also what juce::File::currentExecutableFile
// returns inside a plugin.
namespace talas::plugin::runtime_identity
{
namespace
{
// Any address inside this plugin binary.
const char moduleAnchor = 0;

struct Embedded
{
    PluginIdentity identity;
    std::filesystem::path resources;
    std::string problem;
    bool embedded = false;
};

[[nodiscard]] Embedded loadEmbedded()
{
    Embedded loaded;
    loaded.identity = templatePluginIdentity(templateKind());
    const auto binary = platform::modulePathContaining(&moduleAnchor);
    if (!binary.has_value())
        return loaded;
    loaded.resources = platform::bundleResourcesForBinary(*binary);
    const auto file = loaded.resources / std::filesystem::path{std::string{pluginIdentityFileName}};
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error))
        return loaded;
    std::ifstream stream(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::string problem;
    auto parsed = parsePluginIdentity(text, &problem);
    if (!parsed.has_value())
    {
        loaded.problem = "talas-plugin.json was not used: " + problem;
        return loaded;
    }
    if (parsed->kind != templateKind())
    {
        loaded.problem = "talas-plugin.json describes an " + std::string{pluginKindName(parsed->kind)}
            + " but this is the " + std::string{pluginKindName(templateKind())} + " template";
    }
    loaded.identity = std::move(*parsed);
    loaded.embedded = true;
    return loaded;
}

[[nodiscard]] const Embedded& embedded() noexcept
{
    static const Embedded value = loadEmbedded();
    return value;
}
}

PluginKind templateKind() noexcept
{
#if JucePlugin_IsSynth
    return PluginKind::Instrument;
#else
    return PluginKind::Effect;
#endif
}

const PluginIdentity& identity() noexcept
{
    return embedded().identity;
}

bool hasEmbeddedIdentity() noexcept
{
    return embedded().embedded;
}

const std::string& identityProblem() noexcept
{
    return embedded().problem;
}

const std::filesystem::path& resourcesDirectory() noexcept
{
    return embedded().resources;
}

const char* name() noexcept
{
    return identity().name.c_str();
}

const char* manufacturer() noexcept
{
    return identity().manufacturer.c_str();
}

const char* description() noexcept
{
    return identity().description.c_str();
}

const char* versionString() noexcept
{
    return identity().version.c_str();
}

std::uint32_t versionCode() noexcept
{
    return pluginVersionCode(identity().version).value_or(0x10000U);
}

std::uint32_t manufacturerCode() noexcept
{
    return fourCharCode(identity().manufacturerCode);
}

std::uint32_t pluginCode() noexcept
{
    return fourCharCode(identity().pluginCode);
}
}
