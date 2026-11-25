#include "talas/app/SoundWorkbenchModel.h"

#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string lowercase(std::string_view text)
{
    std::string result{text};
    std::ranges::transform(
        result,
        result.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return result;
}

[[nodiscard]] const talas::project::ProjectSoundDefinition* findDefinition(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] const talas::project::ProjectNode* findRootNode(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        document.graph.nodes,
        [&](const auto& node) {
            return node.instanceId == nodeId;
        });
    return found == document.graph.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] std::string portTypeName(talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return "Audio";
    case talas::core::PortType::Control:
        return "Control";
    case talas::core::PortType::Event:
        return "Event";
    case talas::core::PortType::SampleBuffer:
        return "Sample Buffer";
    case talas::core::PortType::Wavetable:
        return "Wavetable";
    case talas::core::PortType::Spectrum:
        return "Spectrum";
    case talas::core::PortType::ComplexSpectrum:
        return "Complex Spectrum";
    case talas::core::PortType::NoteStream:
        return "Note Stream";
    }
    return "Unknown";
}

[[nodiscard]] talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}
}

bool soundDefinitionBrowserMatches(
    const SoundDefinitionBrowserEntry& entry,
    std::string_view query,
    SoundDefinitionBrowserScope scope,
    const std::unordered_set<std::string>& bookmarks,
    const std::vector<std::string>& recentDefinitionIds)
{
    if (scope == SoundDefinitionBrowserScope::Bookmarked
        && !bookmarks.contains(entry.definitionId))
    {
        return false;
    }
    if (scope == SoundDefinitionBrowserScope::Recent
        && std::ranges::find(recentDefinitionIds, entry.definitionId)
            == recentDefinitionIds.end())
    {
        return false;
    }
    if (query.empty())
        return true;
    const auto normalized = lowercase(query);
    return lowercase(entry.displayName).find(normalized) != std::string::npos
        || lowercase(entry.definitionId).find(normalized) != std::string::npos;
}

std::vector<SoundDefinitionBrowserEntry> soundDefinitionBrowserEntries(
    const talas::project::ProjectDocument& document)
{
    std::vector<SoundDefinitionBrowserEntry> entries;
    entries.reserve(document.soundDefinitions.size());
    std::unordered_map<std::string_view, std::size_t> entryIndex;
    entryIndex.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
    {
        entryIndex.emplace(definition.definitionId, entries.size());
        entries.push_back({
            definition.definitionId,
            definition.displayName,
        });
    }
    const auto countInstance = [&](const auto& node, bool nested)
    {
        if (!node.soundDefinitionId.has_value())
            return;
        const auto found = entryIndex.find(*node.soundDefinitionId);
        if (found == entryIndex.end())
            return;
        auto& entry = entries[found->second];
        if (nested)
            ++entry.nestedInstanceCount;
        else
            ++entry.rootInstanceCount;
    };
    for (const auto& node : document.graph.nodes)
        countInstance(node, false);
    for (const auto& definition : document.soundDefinitions)
    {
        for (const auto& node : definition.graph.nodes)
            countInstance(node, true);
    }
    return entries;
}

std::vector<SoundHierarchyBreadcrumb> soundHierarchyBreadcrumbs(
    const talas::project::ProjectDocument& document,
    std::span<const std::string> navigationPath)
{
    std::vector<SoundHierarchyBreadcrumb> result;
    result.reserve(navigationPath.size() + 1);
    result.push_back({std::nullopt, "Root"});
    for (const auto& definitionId : navigationPath)
    {
        const auto* definition = findDefinition(document, definitionId);
        if (definition == nullptr)
            break;
        result.push_back({
            definition->definitionId,
            definition->displayName,
        });
    }
    return result;
}

void recordRecentSoundDefinition(std::vector<std::string>& recentDefinitionIds,
    std::string definitionId,
    std::size_t limit)
{
    std::erase(recentDefinitionIds, definitionId);
    if (limit == 0 || definitionId.empty())
    {
        if (limit == 0)
            recentDefinitionIds.clear();
        return;
    }
    recentDefinitionIds.insert(
        recentDefinitionIds.begin(),
        std::move(definitionId));
    if (recentDefinitionIds.size() > limit)
        recentDefinitionIds.resize(limit);
}

void toggleSoundDefinitionBookmark(
    std::unordered_set<std::string>& bookmarks,
    std::string definitionId)
{
    if (definitionId.empty())
        return;
    if (const auto found = bookmarks.find(definitionId);
        found != bookmarks.end())
    {
        bookmarks.erase(found);
        return;
    }
    bookmarks.insert(std::move(definitionId));
}

void selectSoundWorkbenchSessionNode(
    talas::project::WorkspaceSessionState& session,
    std::optional<std::string> definitionId,
    std::string nodeId)
{
    auto& context = talas::project::ensureWorkspaceGraphSessionState(
        session,
        std::move(definitionId));
    if (nodeId.empty())
        context.selectedNodeId.reset();
    else
        context.selectedNodeId = std::move(nodeId);
}

std::string soundWorkbenchSessionSelection(
    const talas::project::WorkspaceSessionState& session,
    std::optional<std::string_view> definitionId)
{
    const auto* context = talas::project::findWorkspaceGraphSessionState(
        session,
        definitionId);
    return context == nullptr
            || !context->selectedNodeId.has_value()
        ? std::string{}
        : *context->selectedNodeId;
}

talas::project::WorkspaceViewMode workspaceSessionViewMode(
    WorkbenchWorkspaceMode mode) noexcept
{
    switch (mode)
    {
    case WorkbenchWorkspaceMode::Performance:
        return talas::project::WorkspaceViewMode::Performance;
    case WorkbenchWorkspaceMode::Samples:
        return talas::project::WorkspaceViewMode::Samples;
    case WorkbenchWorkspaceMode::Patch:
    case WorkbenchWorkspaceMode::Automation:
    case WorkbenchWorkspaceMode::Analysis:
        return talas::project::WorkspaceViewMode::Structure;
    }
    return talas::project::WorkspaceViewMode::Structure;
}

WorkbenchWorkspaceMode workbenchWorkspaceMode(
    talas::project::WorkspaceViewMode mode) noexcept
{
    switch (mode)
    {
    case talas::project::WorkspaceViewMode::Performance:
        return WorkbenchWorkspaceMode::Performance;
    case talas::project::WorkspaceViewMode::Samples:
        return WorkbenchWorkspaceMode::Samples;
    case talas::project::WorkspaceViewMode::Structure:
        return WorkbenchWorkspaceMode::Patch;
    }
    return WorkbenchWorkspaceMode::Patch;
}

std::vector<SoundPerformanceControl> soundPerformanceControls(
    const talas::project::ProjectDocument& document,
    std::string_view instanceId)
{
    const auto* instance = findRootNode(document, instanceId);
    if (instance == nullptr || !instance->soundDefinitionId.has_value())
        return {};
    const auto* definition = findDefinition(
        document,
        *instance->soundDefinitionId);
    if (definition == nullptr)
        return {};

    std::vector<SoundPerformanceControl> controls;
    controls.reserve(definition->exposedParameters.size());
    for (const auto& exposed : definition->exposedParameters)
    {
        auto value = exposed.defaultValue;
        const auto overrideValue = std::ranges::find_if(
            instance->parameters,
            [&](const auto& candidate) {
                return candidate.parameterId == exposed.parameterId
                    || std::ranges::find(
                           exposed.aliases,
                           candidate.parameterId)
                        != exposed.aliases.end();
            });
        if (overrideValue != instance->parameters.end())
            value = *overrideValue;
        value.parameterId = exposed.parameterId;
        controls.push_back({
            {
                instance->instanceId,
                definition->definitionId,
                exposed.parameterId,
            },
            exposed.displayName,
            exposed.kind,
            exposed.domain,
            exposed.choices,
            std::move(value),
            exposed.defaultValue,
        });
    }
    return controls;
}

std::optional<SoundPerformanceStructureTarget> soundPerformanceStructureTarget(
    const talas::project::ProjectDocument& document,
    const SoundPerformanceControlIdentity& identity)
{
    const auto* instance = findRootNode(document, identity.instanceId);
    if (instance == nullptr ||
        instance->soundDefinitionId !=
            std::optional<std::string>{identity.definitionId})
    {
        return std::nullopt;
    }
    const auto* definition = findDefinition(document, identity.definitionId);
    if (definition == nullptr)
        return std::nullopt;
    const auto parameter = std::ranges::find_if(definition->exposedParameters,
        [&](const auto& candidate)
        { return candidate.parameterId == identity.parameterId; });
    if (parameter == definition->exposedParameters.end())
        return std::nullopt;
    return SoundPerformanceStructureTarget{
        identity.instanceId,
        identity.definitionId,
        identity.parameterId,
    };
}

std::vector<std::string> soundWorkbenchDiagnosticNodeIds(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::span<const SoundWorkbenchDiagnosticIdentity> diagnostics)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    if (definitionId.has_value())
    {
        const auto* definition = findDefinition(document, *definitionId);
        if (definition == nullptr)
            return {};
        graph = &definition->graph;
    }
    const auto containsNode = [&](std::string_view nodeId) {
        return std::ranges::any_of(
            graph->nodes,
            [&](const auto& node) {
                return node.instanceId == nodeId;
            });
    };

    std::vector<std::string> result;
    const auto append = [&](std::string_view nodeId) {
        if (!nodeId.empty()
            && containsNode(nodeId)
            && std::ranges::find(result, nodeId) == result.end())
        {
            result.emplace_back(nodeId);
            return true;
        }
        return false;
    };

    const auto findGraphNode = [](
                                   const talas::project::ProjectGraph& candidateGraph,
                                   std::string_view nodeId)
        -> const talas::project::ProjectNode* {
        const auto found = std::ranges::find_if(
            candidateGraph.nodes,
            [&](const auto& node) {
                return node.instanceId == nodeId;
            });
        return found == candidateGraph.nodes.end() ? nullptr : &*found;
    };

    for (const auto& diagnostic : diagnostics)
    {
        const auto& path = diagnostic.structuralSourcePath;
        if (path.empty())
        {
            if (!definitionId.has_value())
                append(diagnostic.nodeId);
            continue;
        }

        std::vector<std::string_view> segments;
        std::size_t segmentStart = 0;
        while (segmentStart <= path.size())
        {
            const auto separator = path.find('/', segmentStart);
            segments.push_back(std::string_view{path}.substr(
                segmentStart,
                separator == std::string::npos
                    ? std::string::npos
                    : separator - segmentStart));
            if (separator == std::string::npos)
                break;
            segmentStart = separator + 1;
        }

        const auto qualifier = segments.back().find("::");
        if (qualifier == std::string_view::npos)
        {
            if (!definitionId.has_value() && segments.size() == 1)
                append(segments.front());
            continue;
        }
        const auto finalDefinitionId = segments.back().substr(0, qualifier);
        const auto finalLocalNodeId = segments.back().substr(qualifier + 2);
        if (finalDefinitionId.empty()
            || finalLocalNodeId.empty()
            || segments.size() < 2)
        {
            continue;
        }

        const talas::project::ProjectGraph* ancestryGraph = &document.graph;
        std::optional<std::string> ancestryContext;
        std::vector<std::pair<std::optional<std::string>, std::string>>
            boundaries;
        bool validAncestry = true;
        for (const auto instanceNodeId :
             std::span{segments}.first(segments.size() - 1))
        {
            const auto* instance = findGraphNode(
                *ancestryGraph,
                instanceNodeId);
            if (instance == nullptr
                || !instance->soundDefinitionId.has_value())
            {
                validAncestry = false;
                break;
            }
            boundaries.emplace_back(
                ancestryContext,
                instance->instanceId);
            const auto* nestedDefinition = findDefinition(
                document,
                *instance->soundDefinitionId);
            if (nestedDefinition == nullptr)
            {
                validAncestry = false;
                break;
            }
            ancestryContext = nestedDefinition->definitionId;
            ancestryGraph = &nestedDefinition->graph;
        }
        if (!validAncestry
            || !ancestryContext.has_value()
            || *ancestryContext != finalDefinitionId)
        {
            continue;
        }

        if (!definitionId.has_value())
        {
            if (!boundaries.empty())
                append(boundaries.front().second);
            continue;
        }
        if (*definitionId == finalDefinitionId)
        {
            append(finalLocalNodeId);
            continue;
        }
        const auto boundary = std::ranges::find_if(
            boundaries,
            [&](const auto& candidate) {
                return candidate.first == definitionId;
            });
        if (boundary != boundaries.end())
            append(boundary->second);
    }
    return result;
}

std::optional<SoundWorkbenchDiagnosticTarget>
soundWorkbenchDiagnosticTarget(
    const talas::project::ProjectDocument& document,
    const SoundWorkbenchDiagnosticIdentity& diagnostic)
{
    const auto findGraphNode = [](
                                   const talas::project::ProjectGraph& graph,
                                   std::string_view nodeId)
        -> const talas::project::ProjectNode* {
        const auto found = std::ranges::find_if(
            graph.nodes,
            [&](const auto& node) {
                return node.instanceId == nodeId;
            });
        return found == graph.nodes.end() ? nullptr : &*found;
    };

    const auto& path = diagnostic.structuralSourcePath;
    if (path.empty() || path.find("::") == std::string::npos)
    {
        const auto nodeId = path.empty()
            ? std::string_view{diagnostic.nodeId}
            : std::string_view{path};
        if (nodeId.empty()
            || nodeId.find('/') != std::string_view::npos
            || findGraphNode(document.graph, nodeId) == nullptr)
        {
            return std::nullopt;
        }
        return SoundWorkbenchDiagnosticTarget{
            std::nullopt,
            std::string{nodeId},
        };
    }

    std::vector<std::string_view> segments;
    std::size_t segmentStart = 0;
    while (segmentStart <= path.size())
    {
        const auto separator = path.find('/', segmentStart);
        segments.push_back(std::string_view{path}.substr(
            segmentStart,
            separator == std::string::npos
                ? std::string::npos
                : separator - segmentStart));
        if (separator == std::string::npos)
            break;
        segmentStart = separator + 1;
    }
    if (segments.size() < 2)
        return std::nullopt;
    const auto qualifier = segments.back().find("::");
    if (qualifier == std::string_view::npos)
        return std::nullopt;
    const auto finalDefinitionId =
        segments.back().substr(0, qualifier);
    const auto finalNodeId =
        segments.back().substr(qualifier + 2);
    if (finalDefinitionId.empty() || finalNodeId.empty())
        return std::nullopt;

    const talas::project::ProjectGraph* ancestryGraph =
        &document.graph;
    const talas::project::ProjectSoundDefinition* finalDefinition =
        nullptr;
    for (const auto instanceNodeId :
         std::span{segments}.first(segments.size() - 1))
    {
        const auto* instance = findGraphNode(
            *ancestryGraph,
            instanceNodeId);
        if (instance == nullptr
            || !instance->soundDefinitionId.has_value())
        {
            return std::nullopt;
        }
        finalDefinition = findDefinition(
            document,
            *instance->soundDefinitionId);
        if (finalDefinition == nullptr)
            return std::nullopt;
        ancestryGraph = &finalDefinition->graph;
    }
    if (finalDefinition == nullptr
        || finalDefinition->definitionId != finalDefinitionId
        || findGraphNode(finalDefinition->graph, finalNodeId)
            == nullptr)
    {
        return std::nullopt;
    }
    return SoundWorkbenchDiagnosticTarget{
        finalDefinition->definitionId,
        std::string{finalNodeId},
    };
}

SoundDefinitionAuditionDocumentResult makeSoundDefinitionAuditionDocument(
    const talas::project::ProjectDocument& source,
    std::string_view definitionId,
    const talas::core::NodeRegistry& registry)
{
    const auto sourceAvailability =
        talas::project::validateProjectComponentAvailabilityForExecution(
            source,
            registry);
    if (!sourceAvailability.ok())
    {
        return {
            std::nullopt,
            sourceAvailability.errors.empty()
                ? std::string{"Source project has unavailable components"}
                : sourceAvailability.errors.front().message,
        };
    }
    const auto* definition = findDefinition(source, definitionId);
    if (definition == nullptr)
    {
        return {
            std::nullopt,
            "Unknown reusable Sound: " + std::string(definitionId),
        };
    }
    if (registry.findNodeType("talas.audioOutput") == nullptr
        || registry.findNodeType("talas.oscillator") == nullptr
        || registry.findNodeType("talas.constant") == nullptr)
    {
        return {
            std::nullopt,
            "Audition test-source prototypes are unavailable",
        };
    }

    talas::project::ProjectDocument audition = source;
    audition.metadata.title = definition->displayName + " Audition";
    audition.graph = {};
    audition.timeline = {};
    const auto addAuditionNode = [&](talas::project::ProjectNode node,
                                     talas::project::CanvasPosition position) {
        const auto nodeId = node.instanceId;
        audition.graph.nodes.push_back(std::move(node));
        talas::project::reconcileProjectPresentation(audition);
        talas::project::findProjectNodePresentation(
            audition,
            std::nullopt,
            nodeId)->position = position;
    };
    addAuditionNode(
        {
            "auditionSound",
            {},
            {},
            std::optional<std::string>{definition->definitionId},
        },
        {256, 96});

    std::size_t sourceIndex = 0;
    for (const auto& input : definition->inputs)
    {
        ++sourceIndex;
        const auto nodeId = "auditionInput" + std::to_string(sourceIndex);
        std::string typeId;
        std::string outputPort;
        std::vector<talas::project::ProjectParameterExpression> parameters;
        switch (input.type)
        {
        case talas::core::PortType::Audio:
            typeId = "talas.oscillator";
            outputPort = "audioOut";
            parameters.push_back(
                expressionParameter("amplitude", "0.2"));
            break;
        case talas::core::PortType::Control:
            typeId = "talas.constant";
            outputPort = "value";
            parameters.push_back(
                expressionParameter("value", "0.5"));
            break;
        case talas::core::PortType::Event:
        case talas::core::PortType::SampleBuffer:
        case talas::core::PortType::Wavetable:
        case talas::core::PortType::Spectrum:
        case talas::core::PortType::ComplexSpectrum:
        case talas::core::PortType::NoteStream:
            return {
                std::nullopt,
                portTypeName(input.type)
                    + " audition input substitution is not supported",
            };
        }
        addAuditionNode(
            {
                nodeId,
                std::move(typeId),
                std::move(parameters),
                {},
            },
            {32, static_cast<std::int32_t>(64 + sourceIndex * 120)});
        audition.graph.connections.push_back({
            nodeId,
            std::move(outputPort),
            "auditionSound",
            input.portId,
        });
    }

    std::size_t outputIndex = 0;
    for (const auto& output : definition->outputs)
    {
        if (output.type != talas::core::PortType::Audio)
            continue;
        ++outputIndex;
        const auto nodeId = "auditionOutput" + std::to_string(outputIndex);
        addAuditionNode(
            {
                nodeId,
                "talas.audioOutput",
                {},
                {},
            },
            {520, static_cast<std::int32_t>(64 + outputIndex * 120)});
        audition.graph.connections.push_back({
            "auditionSound",
            output.portId,
            nodeId,
            "audioIn",
        });
    }
    if (outputIndex == 0)
    {
        return {
            std::nullopt,
            "Reusable Sound has no Audio output to audition",
        };
    }

    const auto validation = talas::project::validateProjectDocument(
        audition,
        registry);
    if (!validation.ok())
    {
        return {
            std::nullopt,
            validation.errors.empty()
                ? std::string{"Audition patch is invalid"}
                : validation.errors.front().message,
        };
    }
    return {std::move(audition), {}};
}

bool soundEditorChanged(const talas::project::ProjectSoundDefinition& opened, const talas::project::ProjectDocument& document)
{
    const auto now = std::ranges::find(document.soundDefinitions, opened.definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    return now != document.soundDefinitions.end() && *now != opened;
}

std::optional<talas::project::ProjectDocument> soundEditorDropped(
    const talas::project::ProjectDocument& document, const talas::project::ProjectSoundDefinition& opened)
{
    auto dropped = document;
    const auto now = std::ranges::find(dropped.soundDefinitions, opened.definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (now == dropped.soundDefinitions.end())
        return std::nullopt;
    *now = opened;
    return dropped;
}
}
