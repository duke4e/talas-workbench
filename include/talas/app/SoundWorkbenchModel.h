#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace talas::app
{
enum class SoundDefinitionBrowserScope
{
    All,
    Bookmarked,
    Recent
};

struct SoundDefinitionBrowserEntry
{
    std::string definitionId;
    std::string displayName;
    std::size_t rootInstanceCount = 0;
    std::size_t nestedInstanceCount = 0;
};

[[nodiscard]] std::vector<SoundDefinitionBrowserEntry>
soundDefinitionBrowserEntries(const talas::project::ProjectDocument& document);

struct SoundHierarchyBreadcrumb
{
    std::optional<std::string> definitionId;
    std::string displayName;

    bool operator==(const SoundHierarchyBreadcrumb&) const = default;
};

[[nodiscard]] std::vector<SoundHierarchyBreadcrumb> soundHierarchyBreadcrumbs(
    const talas::project::ProjectDocument& document,
    std::span<const std::string> navigationPath);

[[nodiscard]] bool soundDefinitionBrowserMatches(
    const SoundDefinitionBrowserEntry& entry,
    std::string_view query,
    SoundDefinitionBrowserScope scope,
    const std::unordered_set<std::string>& bookmarks,
    const std::vector<std::string>& recentDefinitionIds);

void recordRecentSoundDefinition(
    std::vector<std::string>& recentDefinitionIds,
    std::string definitionId,
    std::size_t limit = 12);

void toggleSoundDefinitionBookmark(
    std::unordered_set<std::string>& bookmarks,
    std::string definitionId);

void selectSoundWorkbenchSessionNode(
    talas::project::WorkspaceSessionState& session,
    std::optional<std::string> definitionId,
    std::string nodeId);

[[nodiscard]] std::string soundWorkbenchSessionSelection(
    const talas::project::WorkspaceSessionState& session,
    std::optional<std::string_view> definitionId);

[[nodiscard]] talas::project::WorkspaceViewMode workspaceSessionViewMode(
    WorkbenchWorkspaceMode mode) noexcept;

[[nodiscard]] WorkbenchWorkspaceMode workbenchWorkspaceMode(
    talas::project::WorkspaceViewMode mode) noexcept;

struct SoundPerformanceControlIdentity
{
    std::string instanceId;
    std::string definitionId;
    std::string parameterId;

    bool operator==(const SoundPerformanceControlIdentity&) const = default;
};

struct SoundPerformanceControl
{
    SoundPerformanceControlIdentity identity;
    std::string displayName;
    talas::core::ParameterKind kind = talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;
    talas::project::ProjectParameterExpression value;
    talas::project::ProjectParameterExpression defaultValue;
};

struct SoundPerformanceStructureTarget
{
    std::string instanceId;
    std::string definitionId;
    std::string parameterId;

    bool operator==(const SoundPerformanceStructureTarget&) const = default;
};

[[nodiscard]] std::optional<SoundPerformanceStructureTarget>
soundPerformanceStructureTarget(const talas::project::ProjectDocument& document,
    const SoundPerformanceControlIdentity& identity);

[[nodiscard]] std::vector<SoundPerformanceControl> soundPerformanceControls(
    const talas::project::ProjectDocument& document, std::string_view instanceId);

struct SoundWorkbenchDiagnosticIdentity
{
    std::string nodeId;
    std::string structuralSourcePath;
};

struct SoundWorkbenchDiagnosticTarget
{
    std::optional<std::string> definitionId;
    std::string nodeId;
};

[[nodiscard]] std::optional<SoundWorkbenchDiagnosticTarget>
soundWorkbenchDiagnosticTarget(
    const talas::project::ProjectDocument& document,
    const SoundWorkbenchDiagnosticIdentity& diagnostic);

[[nodiscard]] std::vector<std::string>
soundWorkbenchDiagnosticNodeIds(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::span<const SoundWorkbenchDiagnosticIdentity> diagnostics);

struct SoundDefinitionAuditionDocumentResult
{
    std::optional<talas::project::ProjectDocument> document;
    std::string message;

    [[nodiscard]] bool ok() const
    {
        return document.has_value();
    }
};

[[nodiscard]] SoundDefinitionAuditionDocumentResult
makeSoundDefinitionAuditionDocument(
    const talas::project::ProjectDocument& source,
    std::string_view definitionId,
    const talas::core::NodeRegistry& registry);

// Closing a Sound's editor: whether the
// Sound changed since the editor opened on it, so that closing asks to keep
// the changes; and Drop, the project with the Sound as it was then, every
// other Sound as it is; nothing when the Sound is gone.
[[nodiscard]] bool soundEditorChanged(
    const talas::project::ProjectSoundDefinition& opened, const talas::project::ProjectDocument& document);
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundEditorDropped(
    const talas::project::ProjectDocument& document, const talas::project::ProjectSoundDefinition& opened);
}
