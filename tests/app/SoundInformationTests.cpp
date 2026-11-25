#include "talas/app/SoundInformation.h"
#include "talas/core/BuiltinNodes.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Get info.
namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testGetInfoOnOneSound()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.assets.push_back({"kick", talas::project::ProjectAssetKind::Audio, "Samples/kick.wav"});
    document.assets.back().sampleRate = 8000.0;
    document.assets.back().frameCount = 20000;
    talas::project::ProjectNode player{"player", "talas.sample", {}};
    player.parameters.push_back({"sample", {}, {}, {}, std::string{"kick"}, {}});
    talas::project::ProjectNode filter{"filter", "talas.filter", {}};
    filter.parameters.push_back({"frequency", std::string{"!Cutoff * ?Brightness hz"}, {}, {}, {}, {}});
    filter.parameters.push_back({"q", std::string{"!Resonance"}, {}, {}, {}, {}});
    document.graph.nodes = {player, filter, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"player", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}};

    const auto info = talas::app::soundInformation(document, document.graph, registry, "filter");
    require(info.has_value() && info->name == "filter"
            && info->soundClass == registry.findNodeType("talas.filter")->displayName,
        "Get info should name the Sound and its class");
    require(info->inputs == std::vector<std::pair<std::string, std::vector<std::string>>>{{"audioIn", {"player"}}},
        "the names of its inputs");
    require(info->eventValues == std::vector<std::string>{"Cutoff", "Resonance"}
            && info->variables == std::vector<std::string>{"Brightness"},
        "the Event Values and variables its fields name");
    const auto frequency = std::ranges::find(info->parameters, std::string{"frequency"}, &std::pair<std::string, std::string>::first);
    require(frequency != info->parameters.end() && frequency->second == "!Cutoff * ?Brightness hz"
            && info->parameters.size() == registry.findNodeType("talas.filter")->parameters.size(),
        "every parameter's name and value, set or by default");
    require(info->complexity == 2, "a filter of a sample is two Sounds");
    const auto played = talas::app::soundInformation(document, document.graph, registry, "player");
    require(played && played->sampleFiles == std::vector<std::string>{"Samples/kick.wav"} && played->complexity == 1,
        "the sample files it plays");
    const auto text = talas::app::soundInformationText(*info);
    require(text.starts_with("filter (" + info->soundClass + ")\nComplexity: 2 Sounds")
            && text.find("\naudioIn: player") != std::string::npos
            && text.find("\nEvent Values: Cutoff, Resonance") != std::string::npos
            && text.find("\nVariables: Brightness") != std::string::npos,
        "the information window lists it all: " + text);
    // Get info lists the duration of the recording it, or a
    // Sound feeding it, plays.
    require(info->duration == std::optional<double>{2.5} && played->duration == std::optional<double>{2.5}
            && text.find("\nDuration: 2.5 s") != std::string::npos,
        "Get info should list the duration of the recording the Sound plays: " + text);
    document.graph.connections.clear();
    require(!talas::app::soundInformation(document, document.graph, registry, "filter")->duration.has_value(),
        "a Sound playing no recording has no duration");
    require(!talas::app::soundInformation(document, document.graph, registry, "missing"), "no Sound, no information");
}
}

int main()
{
    try
    {
        testGetInfoOnOneSound();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
