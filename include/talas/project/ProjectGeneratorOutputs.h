#pragma once

#include "talas/core/AutomationInterpolation.h"
#include "talas/core/NodeRegistry.h"
#include "talas/expr/Expression.h"
#include "talas/generative/ContextFreeGenerator.h"
#include "talas/project/ProjectScore.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
struct ProjectSoundDefinition;

inline constexpr int projectGeneratorOutputProfileVersion = 1;
inline constexpr std::size_t maximumProjectGeneratorTerminalBindings =
    talas::generative::maximumContextFreeSymbols;
inline constexpr std::size_t maximumProjectGeneratorActionsPerTerminal = 4;
inline constexpr std::size_t maximumProjectGeneratorTimelineEvents =
    talas::generative::maximumContextFreeOutputSymbols;
inline constexpr std::size_t maximumProjectGeneratorScoreEdits =
    talas::generative::maximumContextFreeOutputSymbols;
inline constexpr std::uint64_t maximumProjectGeneratorTimelineMicroseconds =
    24ULL * 60ULL * 60ULL * 1000ULL * 1000ULL;
inline constexpr std::size_t maximumProjectGeneratorNamespaceBytes = 96;
inline constexpr std::size_t maximumProjectGeneratorTargetRecords = 65536;
inline constexpr std::size_t maximumProjectGeneratorGraphNodes = 256;
inline constexpr std::size_t maximumProjectGeneratorNodeTypeIdBytes = 128;
inline constexpr std::size_t maximumProjectGeneratorSoundDefinitionIdBytes = 128;
inline constexpr std::size_t maximumProjectGeneratorSoundDisplayNameBytes = 256;

struct ProjectGeneratorTimelineEventAction
{
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;

    bool operator==(const ProjectGeneratorTimelineEventAction&) const = default;
};

struct ProjectGeneratorScoreClipAction
{
    std::string trackId;
    std::string soundDefinitionId;
    std::uint64_t durationTicks = 1;
    std::uint64_t contentOffsetTicks = 0;
    std::uint64_t fadeInTicks = 0;
    std::uint64_t fadeOutTicks = 0;
    std::uint32_t repeatCount = 1;

    bool operator==(const ProjectGeneratorScoreClipAction&) const = default;
};

struct ProjectGeneratorScoreAutomationAction
{
    std::string laneId;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;

    bool operator==(const ProjectGeneratorScoreAutomationAction&) const = default;
};

struct ProjectGeneratorScoreMarkerAction
{
    std::string title;
    std::optional<std::uint64_t> durationTicks;

    bool operator==(const ProjectGeneratorScoreMarkerAction&) const = default;
};

struct ProjectGeneratorGraphNodeAction
{
    std::string nodeTypeId;

    bool operator==(const ProjectGeneratorGraphNodeAction&) const = default;
};

using ProjectGeneratorOutputAction = std::variant<
    ProjectGeneratorTimelineEventAction,
    ProjectGeneratorScoreClipAction,
    ProjectGeneratorScoreAutomationAction,
    ProjectGeneratorScoreMarkerAction,
    ProjectGeneratorGraphNodeAction>;

enum class ProjectGeneratorGraphNodeValidationCode
{
    InvalidTypeId,
    UnknownType,
    UnsupportedOrigin,
    UnsupportedExecution,
    UnsupportedCapability,
    InvalidAudioPortShape,
    RequiredSideInput,
    DescriptorLimitExceeded,
};

struct ProjectGeneratorGraphNodeDiagnostic
{
    ProjectGeneratorGraphNodeValidationCode code =
        ProjectGeneratorGraphNodeValidationCode::InvalidTypeId;
    std::string message;
};

struct ProjectGeneratorGraphNodeDescriptor
{
    std::string nodeTypeId;
    std::optional<std::string> audioInputPortId;
    std::string audioOutputPortId;

    [[nodiscard]] bool isSource() const noexcept
    {
        return !audioInputPortId.has_value();
    }

    bool operator==(const ProjectGeneratorGraphNodeDescriptor&) const = default;
};

struct ProjectGeneratorGraphNodeValidationResult
{
    std::optional<ProjectGeneratorGraphNodeDescriptor> descriptor;
    std::optional<ProjectGeneratorGraphNodeDiagnostic> diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return descriptor.has_value() && !diagnostic.has_value();
    }
};

[[nodiscard]] ProjectGeneratorGraphNodeValidationResult
validateProjectGeneratorGraphNodeType(
    std::string_view nodeTypeId,
    const talas::core::NodeRegistry& registry);

struct ProjectGeneratorTerminalBinding
{
    std::string terminal;
    std::vector<ProjectGeneratorOutputAction> actions;

    bool operator==(const ProjectGeneratorTerminalBinding&) const = default;
};

struct ProjectGeneratorOutputProfile
{
    int version = projectGeneratorOutputProfileVersion;
    std::uint64_t timelineStepMicroseconds = 500000;
    std::uint64_t scoreStepTicks = 960;
    std::vector<ProjectGeneratorTerminalBinding> bindings;

    bool operator==(const ProjectGeneratorOutputProfile&) const = default;
};

enum class ProjectGeneratorOutputValidationCode
{
    InvalidVersion,
    InvalidTimelineStep,
    InvalidScoreStep,
    TooManyBindings,
    InvalidTerminal,
    UnknownTerminal,
    DuplicateTerminal,
    TooManyActions,
    InvalidAction,
    DuplicateTarget,
};

struct ProjectGeneratorOutputValidationError
{
    ProjectGeneratorOutputValidationCode code =
        ProjectGeneratorOutputValidationCode::InvalidAction;
    std::size_t bindingIndex = 0;
    std::size_t actionIndex = 0;
    std::string message;

    bool operator==(const ProjectGeneratorOutputValidationError&) const = default;
};

[[nodiscard]] std::optional<ProjectGeneratorOutputValidationError>
projectGeneratorOutputValidationError(
    const ProjectGeneratorOutputProfile& profile,
    std::span<const std::string> declaredTerminals);

struct PreparedProjectGeneratorOutputProfile
{
    std::uint64_t timelineStepMicroseconds = 0;
    std::uint64_t scoreStepTicks = 0;
    std::vector<std::int16_t> bindingBySymbol;
    std::vector<ProjectGeneratorTerminalBinding> bindings;
};

struct PrepareProjectGeneratorOutputResult
{
    PreparedProjectGeneratorOutputProfile prepared;
    std::optional<ProjectGeneratorOutputValidationError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] PrepareProjectGeneratorOutputResult
prepareProjectGeneratorOutputProfile(
    const ProjectGeneratorOutputProfile& profile,
    std::span<const std::string> declaredTerminals,
    const talas::generative::PreparedContextFreeGenerator& generator);

struct ProjectGeneratorProjectionCapacity
{
    std::size_t scratchSymbols = 0;
    std::size_t maximumTimelineActions = 0;
    std::size_t maximumScoreActions = 0;
    std::size_t maximumGraphActions = 0;

    bool operator==(const ProjectGeneratorProjectionCapacity&) const = default;
};

[[nodiscard]] std::optional<ProjectGeneratorProjectionCapacity>
projectGeneratorProjectionCapacity(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output) noexcept;

inline constexpr std::size_t maximumProjectGeneratorTargetDiagnostics = 64;

enum class ProjectGeneratorTargetValidationCode
{
    UnknownEventValue,
    EventValueDomainMismatch,
    UnknownScoreTrack,
    InvalidScoreTrackDomain,
    UnknownSoundDefinition,
    UnknownAutomationLane,
    AutomationDomainMismatch,
    InvalidAutomationLaneDomain,
    InvalidGraphNodeType,
    TargetModelLimitExceeded,
    DiagnosticLimitExceeded,
};

struct ProjectGeneratorTargetDiagnostic
{
    ProjectGeneratorTargetValidationCode code =
        ProjectGeneratorTargetValidationCode::UnknownEventValue;
    std::string path;
    std::string message;

    bool operator==(const ProjectGeneratorTargetDiagnostic&) const = default;
};

struct ProjectGeneratorTargetValidationResult
{
    std::vector<ProjectGeneratorTargetDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] ProjectGeneratorTargetValidationResult
validateProjectGeneratorOutputTargets(const ProjectDocument& document);
[[nodiscard]] ProjectGeneratorTargetValidationResult
validateProjectGeneratorOutputTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] std::optional<double>
projectGeneratorTimelineSeconds(std::uint64_t microseconds) noexcept;

enum class ProjectGeneratorProjectionStatus
{
    Succeeded,
    InvalidRequest,
    InvalidPreparedAuthority,
    GenerationFailed,
    InvalidTarget,
    IdentityCollision,
    CapacityExceeded,
    TimeOverflow,
    Cancelled,
    InvalidGraphSequence,
    AllocationFailed,
};

struct ProjectGeneratorTimelineRequest
{
    std::uint64_t baseMicroseconds = 0;
};

struct ProjectGeneratorScoreRequest
{
    std::uint64_t baseTick = 0;
    std::string outputNamespace;
};

struct ProjectGeneratorSoundRequest
{
    std::string definitionId;
    std::string displayName;
    std::string outputNamespace;
};

struct ProjectGeneratorTimelineEvent
{
    std::uint64_t timeMicroseconds = 0;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;

    bool operator==(const ProjectGeneratorTimelineEvent&) const = default;
};

struct ProjectGeneratorTimelineResult
{
    ProjectGeneratorProjectionStatus status =
        ProjectGeneratorProjectionStatus::InvalidRequest;
    std::vector<ProjectGeneratorTimelineEvent> events;
    std::size_t generatedSymbols = 0;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == ProjectGeneratorProjectionStatus::Succeeded;
    }
};

struct ProjectGeneratorScoreResult
{
    ProjectGeneratorProjectionStatus status =
        ProjectGeneratorProjectionStatus::InvalidRequest;
    ProjectScoreEditBatch edits;
    std::size_t generatedSymbols = 0;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == ProjectGeneratorProjectionStatus::Succeeded;
    }
};

struct ProjectGeneratorSoundResult
{
    ProjectGeneratorProjectionStatus status =
        ProjectGeneratorProjectionStatus::InvalidRequest;
    std::shared_ptr<const ProjectSoundDefinition> definition;
    std::size_t generatedSymbols = 0;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;
    std::optional<std::uint64_t> catalogAuthorityFingerprint;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == ProjectGeneratorProjectionStatus::Succeeded
            && definition != nullptr
            && catalogAuthorityFingerprint.has_value();
    }
};

[[nodiscard]] std::optional<std::uint64_t>
projectGeneratorSoundCatalogAuthority(
    const ProjectSoundDefinition& definition,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] ProjectGeneratorTimelineResult generateProjectTimelineEvents(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    ProjectGeneratorTimelineRequest request,
    std::stop_token stopToken = {});

[[nodiscard]] ProjectGeneratorScoreResult generateProjectScoreEdits(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    const ProjectDocument& document,
    ProjectGeneratorScoreRequest request,
    std::stop_token stopToken = {});

[[nodiscard]] ProjectGeneratorSoundResult generateProjectSoundDefinition(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    ProjectGeneratorSoundRequest request,
    std::stop_token stopToken = {});
}
