#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Get info:
// what one particular Sound is, as against Describe Sound's account of its
// class: its class, its parameters' values, the Sounds feeding each input,
// the Event Values and variables its fields name, the files they play, and
// its complexity, the number of Sounds it is made of (itself and every
// Sound upstream of it), a rough measure of what it costs to run.
namespace talas::app
{
struct SoundInformation
{
    std::string name;
    std::string soundClass;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<std::pair<std::string, std::vector<std::string>>> inputs;
    std::vector<std::string> eventValues;
    std::vector<std::string> variables;
    std::vector<std::string> sampleFiles;
    std::size_t complexity = 1;
    // The duration of the longest recording it or a Sound
    // feeding it plays, in seconds; none when it plays none.
    std::optional<double> duration;
};

[[nodiscard]] std::optional<SoundInformation> soundInformation(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectGraph& graph,
    const talas::core::NodeRegistry& registry,
    std::string_view nodeId);
[[nodiscard]] std::string soundInformationText(const SoundInformation& information);
}
