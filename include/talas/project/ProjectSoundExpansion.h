#pragma once

#include "talas/core/Graph.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectSoundVariables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumProjectSoundExpansionWorkItems = 8192;
inline constexpr std::size_t maximumGeneratedProjectSoundNodeIdBytes = 4096;

struct ExpandedSoundNodeIdentity
{
    std::string flatNodeId;
    std::vector<std::string> instancePath;
    std::string definitionId;
    std::string localNodeId;
    std::uint32_t scopeOrdinal = 0;
    std::string sourcePath;
    std::vector<std::string> qualifiedParameterIds;

    bool operator==(const ExpandedSoundNodeIdentity&) const = default;
};

struct ExpandedSoundEventValueBinding
{
    std::uint32_t scopeOrdinal = 0;
    std::uint32_t sourceScopeOrdinal = 0;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    ProjectParameterExpression value;
    std::vector<std::string> qualifiedParameterIds;
    std::string sourcePath;

    bool operator==(const ExpandedSoundEventValueBinding&) const = default;
};

struct ExpandedSoundNodeParameterBinding
{
    std::string flatNodeId;
    std::string parameterId;
    std::uint32_t sourceScopeOrdinal = 0;
    std::vector<std::string> qualifiedParameterIds;

    bool operator==(const ExpandedSoundNodeParameterBinding&) const = default;
};

struct ExpandedSoundScopeIdentity
{
    std::uint32_t scopeOrdinal = 0;
    std::uint32_t parentScopeOrdinal = 0;
    std::vector<std::string> instancePath;
    std::string definitionId;

    bool operator==(const ExpandedSoundScopeIdentity&) const = default;
};

struct ProjectSoundExpansionManifest
{
    std::vector<ExpandedSoundScopeIdentity> scopes;
    std::vector<ExpandedSoundNodeIdentity> nodes;
    std::vector<ExpandedSoundEventValueBinding> eventValues;
    std::vector<ExpandedSoundNodeParameterBinding> nodeParameters;
    struct SplitSurroundResourceGroup
    {
        std::string flatNodeId;
        std::vector<std::string> resourceIds;
        std::size_t expectedChannels = 0;
        bool antialiased = false;

        bool operator==(const SplitSurroundResourceGroup&) const = default;
    };
    std::vector<SplitSurroundResourceGroup>
        splitSurroundResourceGroups;
    struct SoundVariableSelector
    {
        std::string flatNodeId;
        std::uint32_t scopeOrdinal = 0;
        std::string variableId;
        ProjectSoundVariableKind kind = ProjectSoundVariableKind::Sound;
        std::vector<std::string> candidateDefinitionIds;

        bool operator==(const SoundVariableSelector&) const = default;
    };
    std::vector<SoundVariableSelector> soundVariableSelectors;
};

struct ProjectSoundExpansionResult
{
    ProjectDocument document;
    ProjectSoundExpansionManifest manifest;
    std::vector<std::string> preparationOpenBoundaryNodeIds;
    talas::core::ValidationResult validation;

    [[nodiscard]] bool ok() const
    {
        return validation.ok();
    }
};

struct ProjectSoundPreparationDocumentResult
{
    ProjectDocument document;
    std::size_t outputChannelCount = 0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty() && outputChannelCount != 0;
    }
};

// Builds one closed root program around an exact reusable Sound. When the
// Sound has boundary inputs, sourceInstanceId retains only that instance and
// its complete upstream root-graph closure. A zero requested channel count
// selects the Sound's exact Audio output count; Score callers may request a
// wider supported target and duplicate one mono output deterministically.
[[nodiscard]] ProjectSoundPreparationDocumentResult
projectSoundPreparationDocument(
    const ProjectDocument& document,
    std::string_view soundDefinitionId,
    std::optional<std::string_view> sourceInstanceId = {},
    std::size_t requestedOutputChannelCount = 0,
    std::string_view syntheticSourceId = "liveSetPreparationSource");

// On a Timeline track with an audio input, a
// Sound's replaceable input (or, without one, its first Audio input, its
// first two on a track of two or more channels) takes the track's input in
// place of its Sound or of what feeds it, without changing the Sound. The project's root graph holds the
// Sound as its own source instance, or syntheticSourceId, fed by an
// AudioInput reading the track's input; none when the Sound has no Audio
// input to replace.
struct ProjectTrackInputSubstitution
{
    ProjectDocument document;
    std::string sourceInstanceId;
};

[[nodiscard]] std::optional<ProjectTrackInputSubstitution> projectWithTrackInputSubstituted(
    const ProjectDocument& document,
    std::string_view soundDefinitionId,
    std::optional<std::string_view> sourceInstanceId,
    std::size_t channelCount,
    std::string_view syntheticSourceId);

// A field's expression with each ?name and @name of a class's variable
// parameters replaced by the instance's value, in parentheses: `values` pairs
// a variable's name with its value.
[[nodiscard]] std::string withClassVariables(
    std::string_view expression,
    std::span<const std::pair<std::string, std::string>> values);

[[nodiscard]] ProjectSoundExpansionResult expandProjectSounds(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

// The root nodes expandProjectSounds prepares: every terminal node and the
// nodes that reach one.
[[nodiscard]] ProjectGraph reachableProjectRootGraph(
    const ProjectDocument& document);

// Expands one reusable Sound as a synthetic root without requiring it to be
// instantiated in the authored root graph. Closed structural preflights use
// this to validate dormant library definitions.
[[nodiscard]] ProjectSoundExpansionResult
expandProjectSoundDefinitionForPreparation(
    const ProjectDocument& document,
    std::string_view definitionId,
    const talas::core::NodeRegistry& registry);

// A Sound's own duration in seconds when it has one to give: the length of
// the longest sample it or a Sound inside it plays, else none.
[[nodiscard]] std::optional<double> soundDefinitionSampleDuration(
    const ProjectDocument& document,
    std::string_view definitionId);

struct ProjectMidiVoiceIsland;

[[nodiscard]] talas::core::ValidationResult
materializeProjectSoundVariableBranches(
    const ProjectDocument& source,
    ProjectSoundExpansionResult& expanded,
    const std::vector<ProjectMidiVoiceIsland>& midiVoiceIslands);
}
