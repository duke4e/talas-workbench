#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <optional>
#include <string>
#include <string_view>

// New Class from Example in the Sound file window, and the instance of the new
// class saved there when the Class Editor first closes.
namespace talas::project
{
// A class made from a Sound of the Sound file window: a copy of the example
// under `classId` and `className`, the variables its fields read (?name)
// its parameters, each bound to its ?name, as Extract makes them from the
// signal flow's Sounds; the example itself is left as it is. Nothing for a
// Sound the document does not have, an empty id or name, or an id taken.
[[nodiscard]] std::optional<ProjectSoundDefinition> classFromSoundFileExample(const ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view exampleId, std::string classId, std::string className);

// The instance of a class saved in the Sound file window:
// a Sound named as the class, its signal flow one Sound made from the
// class, its parameters at their defaults, the class's outputs its own;
// its id the class's with Instance, the first number free when that is
// taken. Nothing for no such class.
[[nodiscard]] std::optional<ProjectSoundDefinition> soundFileClassInstance(
    const ProjectDocument& document, std::string_view classId);
}
