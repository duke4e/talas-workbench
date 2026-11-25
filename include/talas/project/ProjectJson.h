#pragma once

#include "talas/project/ProjectDocument.h"

#include <string>
#include <string_view>
#include <vector>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::project
{
struct ProjectJsonError
{
    std::string message;
    std::string path;
};

struct ProjectLoadResult
{
    ProjectDocument document;
    std::vector<ProjectJsonError> errors;

    [[nodiscard]] bool ok() const
    {
        return errors.empty();
    }
};

[[nodiscard]] std::string serializeProjectDocument(const ProjectDocument& document);
[[nodiscard]] std::string serializeProjectDocument(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
[[nodiscard]] ProjectLoadResult parseProjectDocument(std::string_view json);
[[nodiscard]] ProjectLoadResult parseProjectDocument(
    std::string_view json,
    const talas::core::NodeRegistry& registry);
}
