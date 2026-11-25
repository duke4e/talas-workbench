#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <string>
#include <utility>
#include <vector>

// The default Sound for Variables and the default collection for
// SoundCollectionVariables, set from the Sounds selected with Action > Set
// default Sound and Set default collection. Played with a Variable no
// declaration names, a Sound asks for it, and the Default Sound or Default
// Collection button gives it the default.
namespace talas::app
{
struct UndeclaredSoundVariable
{
    std::string variableId;
    bool collection = false;

    bool operator==(const UndeclaredSoundVariable&) const = default;
};

// The Variables and SoundCollectionVariables of the signal flow and of the
// project's Sounds that no declaration names, each once, in order.
[[nodiscard]] std::vector<UndeclaredSoundVariable> undeclaredSoundVariables(const talas::project::ProjectDocument& document);

// A Sound of a signal flow, or a prototype, as a default: a Sound of its own
// holding it and the Sounds feeding it; its output the Sound's first audio
// output. A class's Sound is its class.
[[nodiscard]] talas::project::ProjectSoundDefinition defaultSoundOfNode(const talas::project::ProjectDocument& document,
    const talas::project::ProjectGraph& graph, std::string_view nodeId, const talas::core::NodeRegistry& registry);

// The default before any is set: a sine Oscillator.
[[nodiscard]] talas::project::ProjectSoundDefinition factoryDefaultSound(const talas::core::NodeRegistry& registry);

// The environment the defaults give the Variables asked for: the default
// Sound for each Variable, the default collection for each
// SoundCollectionVariable (the default Sound when the collection is empty).
[[nodiscard]] std::vector<std::pair<std::string, std::vector<talas::project::ProjectSoundDefinition>>> defaultSoundEnvironment(
    const std::vector<UndeclaredSoundVariable>& asked, const talas::project::ProjectSoundDefinition& defaultSound,
    const std::vector<talas::project::ProjectSoundDefinition>& defaultCollection);
}
