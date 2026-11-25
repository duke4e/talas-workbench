#pragma once

#include "talas/core/Graph.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
enum class ProjectPersistenceValidationCode
{
    InvalidAsset,
    DuplicateAsset,
    InvalidSampleMap,
    InvalidScore,
    InvalidVariable,
    InvalidGenerator,
    InvalidLiveSet,
    InvalidExpressionLanguage,
    InvalidExpression,
    InvalidParameterTransformation,
    NotCanonicallyRepresentable,
};

struct ProjectPersistenceDiagnostic
{
    ProjectPersistenceValidationCode code =
        ProjectPersistenceValidationCode::NotCanonicallyRepresentable;
    std::string path;
    std::string message;
};

struct ProjectPersistenceValidationResult
{
    std::vector<ProjectPersistenceDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty();
    }
};

// Registry-independent publication preflight for portable project state.
// This accepts preserved unavailable components but rejects state the
// canonical writer's own parser cannot reopen.
[[nodiscard]] ProjectPersistenceValidationResult
validateProjectDocumentForPersistence(
    const ProjectDocument& document);

// Validates the optional exact root-graph Sound-instance authority carried by
// Score clips. This is registry-independent and shared by parsing, editing,
// persistence, and runtime preparation.
[[nodiscard]] std::optional<ScoreValidationError>
validateProjectScoreSourceBindings(
    const ProjectDocument& document);

[[nodiscard]] std::optional<talas::core::NodeTypeDescriptor>
describeProjectNode(
    const ProjectDocument& document,
    const ProjectNode& node,
    const talas::core::NodeRegistry& registry);
[[nodiscard]] std::optional<talas::core::NodeTypeDescriptor>
describeProjectNodeForEditing(
    const ProjectDocument& document,
    const ProjectNode& node,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] talas::core::ValidationResult validateProjectDocument(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
[[nodiscard]] talas::core::ValidationResult
validateProjectComponentAvailabilityForExecution(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
[[nodiscard]] talas::core::ValidationResult validateProjectDocumentForEditing(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] talas::core::ValidationResult validateProjectSoundDefinitions(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] bool isNoAudioTerminalNodeType(
    std::string_view typeId);
[[nodiscard]] bool isTerminalNodeType(std::string_view typeId);
}
