#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"

#include <optional>
#include <string>
#include <vector>

namespace talas::app
{
enum class AnalysisAuditionPreparationOutcome
{
    Prepared,
    InvalidRequest,
    TargetUnavailable,
    IncompatibleTarget,
    ExpansionFailed,
    OverlayInvalid,
    CommitUnavailable,
};

struct AnalysisAuditionPreparation
{
    AnalysisAuditionPreparationOutcome outcome =
        AnalysisAuditionPreparationOutcome::InvalidRequest;
    std::optional<talas::project::ProjectDocument> overlayDocument;
    std::optional<talas::project::ProjectDocument> portableCommitDocument;
    std::string flatTargetNodeId;
    std::string targetOutputPortId;
    std::optional<std::string> destinationInputPortId;
    std::string sourceLabel;
    std::string destinationLabel;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome == AnalysisAuditionPreparationOutcome::Prepared
            && overlayDocument.has_value();
    }
};

[[nodiscard]] std::string analysisTestSourceLabel(
    talas::project::WorkspaceAnalysisTestSourceKind source,
    double sineFrequencyHz,
    const std::optional<std::string>& frozenAssetId);

[[nodiscard]] AnalysisAuditionPreparation prepareAnalysisAudition(
    const talas::project::ProjectDocument& sourceDocument,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisTraceState& target,
    const talas::project::WorkspaceAnalysisAuditionState& request);
}
