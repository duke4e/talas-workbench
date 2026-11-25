#include "talas/project/WorkspaceSessionState.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectAssetInspection.h"

#include "JsonValue.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
constexpr std::string_view workspaceSessionFormat = "talas.workspace-session";
constexpr std::uint32_t supportedWorkspaceSessionVersion = 13;
constexpr std::size_t maximumDiagnostics = 32;
constexpr double maximumPanMagnitude = 1.0e9;
constexpr double minimumZoom = 0.01;
constexpr double maximumZoom = 64.0;
constexpr double minimumAnalysisVerticalScale = 0.01;
constexpr double maximumAnalysisVerticalScale = 64.0;

[[nodiscard]] std::string_view variableDomainToken(
    talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar: return "scalar";
    case talas::expr::ValueDomain::Hertz: return "hertz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::Samples: return "samples";
    case talas::expr::ValueDomain::NoteNumber: return "noteNumber";
    }
    return {};
}

[[nodiscard]] std::optional<talas::expr::ValueDomain> variableDomainFromToken(
    std::string_view token) noexcept
{
    if (token == "scalar") return talas::expr::ValueDomain::Scalar;
    if (token == "hertz") return talas::expr::ValueDomain::Hertz;
    if (token == "seconds") return talas::expr::ValueDomain::Seconds;
    if (token == "samples") return talas::expr::ValueDomain::Samples;
    if (token == "noteNumber") return talas::expr::ValueDomain::NoteNumber;
    return std::nullopt;
}

[[nodiscard]] bool isCanonicalWorkspaceVariableFingerprint(
    std::string_view value) noexcept
{
    return value.size() == workspaceVariableDeclarationFingerprintBytes
        && value.starts_with("sha256:")
        && std::ranges::all_of(value.substr(7), [](unsigned char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool isKnownSoundLibraryCollection(
    WorkspaceSoundLibraryCollection value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryCollection::All:
    case WorkspaceSoundLibraryCollection::Favorites:
    case WorkspaceSoundLibraryCollection::Recent:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownSoundLibrarySourceFilter(
    WorkspaceSoundLibrarySourceFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibrarySourceFilter::All:
    case WorkspaceSoundLibrarySourceFilter::ProjectDefinitions:
    case WorkspaceSoundLibrarySourceFilter::Components:
    case WorkspaceSoundLibrarySourceFilter::OriginalExamples:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownSoundLibraryCompatibilityFilter(
    WorkspaceSoundLibraryCompatibilityFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryCompatibilityFilter::All:
    case WorkspaceSoundLibraryCompatibilityFilter::Ready:
    case WorkspaceSoundLibraryCompatibilityFilter::NeedsAttention:
    case WorkspaceSoundLibraryCompatibilityFilter::Unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownSoundLibraryHealthFilter(
    WorkspaceSoundLibraryHealthFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryHealthFilter::All:
    case WorkspaceSoundLibraryHealthFilter::Healthy:
    case WorkspaceSoundLibraryHealthFilter::Warning:
    case WorkspaceSoundLibraryHealthFilter::Unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownSoundLibraryAuditionSource(
    WorkspaceSoundLibraryAuditionSource value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryAuditionSource::Default:
    case WorkspaceSoundLibraryAuditionSource::Silence:
    case WorkspaceSoundLibraryAuditionSource::Sine:
    case WorkspaceSoundLibraryAuditionSource::Constant:
    case WorkspaceSoundLibraryAuditionSource::FrozenAsset:
        return true;
    }
    return false;
}

void addError(
    WorkspaceSessionLoadResult& result,
    std::string path,
    std::string message)
{
    if (result.errors.size() >= maximumDiagnostics)
        return;
    result.errors.push_back({
        std::move(message),
        std::move(path),
    });
}

[[nodiscard]] const json::Value* member(
    const json::Value& object,
    std::string_view key)
{
    return json::findObjectMember(object, key);
}

void rejectUnknownMembers(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    const std::string& path,
    std::initializer_list<std::string_view> allowed)
{
    if (!object.isObject())
        return;

    std::unordered_set<std::string_view> observed;
    for (const auto& [key, ignored] : object.object())
    {
        (void) ignored;
        if (!observed.insert(key).second)
        {
            addError(
                result,
                path + "." + key,
                "Duplicate WorkspaceSession field");
        }
        if (std::ranges::find(allowed, key) == allowed.end())
        {
            addError(
                result,
                path,
                "Unknown WorkspaceSession field: " + key);
        }
    }
}

[[nodiscard]] const json::Value* requiredMember(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path)
{
    const auto* value = member(object, key);
    if (value == nullptr)
        addError(result, path, "Missing required WorkspaceSession field");
    return value;
}

[[nodiscard]] bool readRequiredString(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::string& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (!value->isString())
    {
        addError(result, path, "Expected string");
        return false;
    }
    output = value->string();
    return true;
}

[[nodiscard]] bool readRequiredFiniteNumber(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    double& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (!value->isNumber() || !std::isfinite(value->number()))
    {
        addError(result, path, "Expected finite number");
        return false;
    }
    output = value->number();
    return true;
}

[[nodiscard]] bool readRequiredBoolean(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    bool& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (!value->isBoolean())
    {
        addError(result, path, "Expected boolean");
        return false;
    }
    output = value->boolean();
    return true;
}

[[nodiscard]] bool readRequiredUnsignedInteger(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::uint32_t& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (!value->isNumber())
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    const auto& token = value->numberToken();
    if (token.empty()
        || !std::ranges::all_of(token, [](unsigned char character) {
               return character >= '0' && character <= '9';
           }))
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), output);
    if (error != std::errc{} || end != token.data() + token.size())
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    return true;
}

[[nodiscard]] bool readRequiredUnsigned64(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::uint64_t& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (!value->isNumber())
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    const auto& token = value->numberToken();
    if (token.empty()
        || !std::ranges::all_of(token, [](unsigned char character) {
               return character >= '0' && character <= '9';
           }))
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    std::uint64_t parsed = 0;
    const auto conversion = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (conversion.ec != std::errc{}
        || conversion.ptr != token.data() + token.size())
    {
        addError(result, path, "Expected non-negative integer");
        return false;
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool readNullableNonEmptyString(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::optional<std::string>& output)
{
    const auto* value = requiredMember(result, object, key, path);
    if (value == nullptr)
        return false;
    if (value->isNull())
    {
        output.reset();
        return true;
    }
    if (!value->isString() || value->string().empty())
    {
        addError(result, path, "Expected null or non-empty string");
        return false;
    }
    output = value->string();
    return true;
}

[[nodiscard]] bool readNullableBoundedString(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::optional<std::string>& output,
    std::size_t maximumBytes)
{
    if (!readNullableNonEmptyString(result, object, key, path, output))
        return false;
    if (output.has_value() && output->size() > maximumBytes)
    {
        addError(result, path, "WorkspaceSession identifier is too long");
        return false;
    }
    return true;
}

[[nodiscard]] bool readSupportedVersion(
    WorkspaceSessionLoadResult& result,
    const json::Value& root,
    std::uint32_t& output)
{
    const auto* version = requiredMember(result, root, "version", "$.version");
    if (version == nullptr)
        return false;
    if (!version->isNumber())
    {
        addError(result, "$.version", "Expected non-negative integer version");
        return false;
    }
    const auto& token = version->numberToken();
    if (token.empty()
        || !std::ranges::all_of(
            token,
            [](unsigned char character) {
                return character >= '0' && character <= '9';
            }))
    {
        addError(result, "$.version", "Expected non-negative integer version");
        return false;
    }
    const auto [end, error] = std::from_chars(
        token.data(),
        token.data() + token.size(),
        output);
    if (error != std::errc{} || end != token.data() + token.size())
    {
        addError(result, "$.version", "Expected non-negative integer version");
        return false;
    }
    if (output == 0 || output > supportedWorkspaceSessionVersion)
    {
        addError(result, "$.version", "Unsupported WorkspaceSession version");
        return false;
    }
    return true;
}

[[nodiscard]] bool parseScope(
    WorkspaceSessionLoadResult& result,
    const json::Value& scope,
    const std::string& path,
    std::optional<std::string>& soundDefinitionId)
{
    if (!scope.isObject())
    {
        addError(result, path, "Expected WorkspaceSession graph scope object");
        return false;
    }

    std::string kind;
    if (!readRequiredString(result, scope, "kind", path + ".kind", kind))
        return false;

    if (kind == "project")
    {
        rejectUnknownMembers(result, scope, path, {"kind"});
        soundDefinitionId.reset();
        return true;
    }
    if (kind == "soundDefinition")
    {
        rejectUnknownMembers(
            result,
            scope,
            path,
            {"kind", "definitionId"});
        std::string definitionId;
        if (!readRequiredString(
                result,
                scope,
                "definitionId",
                path + ".definitionId",
                definitionId))
        {
            return false;
        }
        if (definitionId.empty())
        {
            addError(
                result,
                path + ".definitionId",
                "Sound definition id cannot be empty");
            return false;
        }
        soundDefinitionId = std::move(definitionId);
        return true;
    }

    rejectUnknownMembers(
        result,
        scope,
        path,
        {"kind", "definitionId"});
    addError(result, path + ".kind", "Expected project or soundDefinition scope");
    return false;
}

[[nodiscard]] bool graphContextLess(
    const WorkspaceGraphSessionState& left,
    const WorkspaceGraphSessionState& right)
{
    if (left.soundDefinitionId.has_value()
        != right.soundDefinitionId.has_value())
    {
        return !left.soundDefinitionId.has_value();
    }
    return left.soundDefinitionId.value_or("")
        < right.soundDefinitionId.value_or("");
}

void normalize(WorkspaceSessionState& state)
{
    if (!isKnownWorkspaceInterfaceScale(state.accessibility.interfaceScale))
        state.accessibility = {};
    if (!isKnownSoundLibraryCollection(state.soundLibrary.collection)
        || !isKnownSoundLibrarySourceFilter(state.soundLibrary.sourceFilter)
        || !isKnownSoundLibraryCompatibilityFilter(
            state.soundLibrary.compatibilityFilter)
        || !isKnownSoundLibraryHealthFilter(state.soundLibrary.healthFilter))
    {
        state.soundLibrary = {};
    }
    std::stable_sort(
        state.graphContexts.begin(),
        state.graphContexts.end(),
        graphContextLess);
    std::ranges::sort(state.hierarchyBookmarks,
        {},
        &WorkspaceHierarchyBookmark::definitionId);
    std::erase_if(state.variableValues, [&](const auto& value) {
        return !isValidProjectVariableIdentifier(value.variableId)
            || !isSupportedProjectVariableDomain(value.domain)
            || !std::isfinite(value.value)
            || !isCanonicalWorkspaceVariableFingerprint(
                value.declarationFingerprint);
    });
    if (state.variableValues.size() > maximumWorkspaceVariableValues)
        state.variableValues.resize(maximumWorkspaceVariableValues);
    std::ranges::sort(
        state.variableValues,
        {},
        &WorkspaceVariableValue::variableId);
    std::erase_if(state.soundVariableValues, [&](const auto& value) {
        return !isValidProjectVariableIdentifier(value.variableId)
            || !isValidProjectVariableIdentifier(value.selectedDefinitionId)
            || !isCanonicalWorkspaceVariableFingerprint(
                value.declarationFingerprint);
    });
    if (state.soundVariableValues.size()
        > maximumWorkspaceSoundVariableValues)
    {
        state.soundVariableValues.resize(
            maximumWorkspaceSoundVariableValues);
    }
    std::ranges::sort(
        state.soundVariableValues,
        {},
        &WorkspaceSoundVariableValue::variableId);
    std::erase_if(
        state.performance.liveToolStates,
        [](const WorkspacePerformanceSessionState::LiveToolState& tool) {
            if (!isValidProjectLiveSetIdentifier(tool.toolId)
                || !isValidProjectLiveSetIdentifier(tool.stateId)
                || !isCanonicalWorkspaceVariableFingerprint(
                    tool.definitionFingerprint)
                || tool.variables.size()
                    > maximumProjectLiveToolVariables)
            {
                return true;
            }
            return std::ranges::any_of(
                tool.variables,
                [](const auto& variable) {
                    return !isValidProjectLiveSetIdentifier(
                               variable.variableId)
                        || !std::isfinite(variable.number)
                        || (variable.kind
                                == ProjectLiveToolVariableKind::Boolean
                            ? variable.number != 0.0
                            : variable.boolean);
                });
        });
    if (state.performance.liveToolStates.size()
        > maximumWorkspaceLiveToolStates)
    {
        state.performance.liveToolStates.resize(
            maximumWorkspaceLiveToolStates);
    }
    for (auto& tool : state.performance.liveToolStates)
    {
        std::ranges::sort(
            tool.variables,
            {},
            &WorkspacePerformanceSessionState::LiveToolVariable::variableId);
        tool.variables.erase(
            std::unique(
                tool.variables.begin(),
                tool.variables.end(),
                [](const auto& left, const auto& right) {
                    return left.variableId == right.variableId;
                }),
            tool.variables.end());
    }
    std::ranges::sort(
        state.performance.liveToolStates,
        {},
        &WorkspacePerformanceSessionState::LiveToolState::toolId);
    state.performance.liveToolStates.erase(
        std::unique(
            state.performance.liveToolStates.begin(),
            state.performance.liveToolStates.end(),
            [](const auto& left, const auto& right) {
                return left.toolId == right.toolId;
            }),
        state.performance.liveToolStates.end());
    if (state.performance.selectedLiveToolId.has_value()
        && !isValidProjectLiveSetIdentifier(
            *state.performance.selectedLiveToolId))
    {
        state.performance.selectedLiveToolId.reset();
    }
}

[[nodiscard]] std::string_view soundLibraryCollectionToken(
    WorkspaceSoundLibraryCollection value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryCollection::All:
        return "all";
    case WorkspaceSoundLibraryCollection::Favorites:
        return "favorites";
    case WorkspaceSoundLibraryCollection::Recent:
        return "recent";
    }
    return "all";
}

[[nodiscard]] std::string_view soundLibrarySourceFilterToken(
    WorkspaceSoundLibrarySourceFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibrarySourceFilter::All:
        return "all";
    case WorkspaceSoundLibrarySourceFilter::ProjectDefinitions:
        return "projectDefinitions";
    case WorkspaceSoundLibrarySourceFilter::Components:
        return "components";
    case WorkspaceSoundLibrarySourceFilter::OriginalExamples:
        return "originalExamples";
    }
    return "all";
}

[[nodiscard]] std::string_view soundLibraryCompatibilityFilterToken(
    WorkspaceSoundLibraryCompatibilityFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryCompatibilityFilter::All:
        return "all";
    case WorkspaceSoundLibraryCompatibilityFilter::Ready:
        return "ready";
    case WorkspaceSoundLibraryCompatibilityFilter::NeedsAttention:
        return "needsAttention";
    case WorkspaceSoundLibraryCompatibilityFilter::Unavailable:
        return "unavailable";
    }
    return "all";
}

[[nodiscard]] std::string_view soundLibraryHealthFilterToken(
    WorkspaceSoundLibraryHealthFilter value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryHealthFilter::All:
        return "all";
    case WorkspaceSoundLibraryHealthFilter::Healthy:
        return "healthy";
    case WorkspaceSoundLibraryHealthFilter::Warning:
        return "warning";
    case WorkspaceSoundLibraryHealthFilter::Unavailable:
        return "unavailable";
    }
    return "all";
}

[[nodiscard]] std::string_view soundLibraryAuditionSourceToken(
    WorkspaceSoundLibraryAuditionSource value) noexcept
{
    switch (value)
    {
    case WorkspaceSoundLibraryAuditionSource::Default:
        return "default";
    case WorkspaceSoundLibraryAuditionSource::Silence:
        return "silence";
    case WorkspaceSoundLibraryAuditionSource::Sine:
        return "sine";
    case WorkspaceSoundLibraryAuditionSource::Constant:
        return "constant";
    case WorkspaceSoundLibraryAuditionSource::FrozenAsset:
        return "frozenAsset";
    }
    return "default";
}

[[nodiscard]] std::string_view splitOrientationToken(
    WorkspaceStructureSplitOrientation orientation) noexcept
{
    switch (orientation)
    {
    case WorkspaceStructureSplitOrientation::Adaptive:
        return "adaptive";
    case WorkspaceStructureSplitOrientation::LeftRight:
        return "leftRight";
    case WorkspaceStructureSplitOrientation::TopBottom:
        return "topBottom";
    }
    return "adaptive";
}

[[nodiscard]] std::string_view graphViewModeToken(
    WorkspaceGraphViewMode mode) noexcept
{
    switch (mode)
    {
    case WorkspaceGraphViewMode::Compact:
        return "compact";
    case WorkspaceGraphViewMode::Ports:
        return "ports";
    case WorkspaceGraphViewMode::PortsAndWires:
        return "portsAndWires";
    }
    return "portsAndWires";
}

[[nodiscard]] std::string_view analysisTraceKindToken(
    WorkspaceAnalysisTraceKind kind) noexcept
{
    switch (kind)
    {
    case WorkspaceAnalysisTraceKind::Audio:
        return "audio";
    case WorkspaceAnalysisTraceKind::Control:
        return "control";
    case WorkspaceAnalysisTraceKind::Event:
        return "event";
    case WorkspaceAnalysisTraceKind::PartialSpectrum:
        return "partialSpectrum";
    case WorkspaceAnalysisTraceKind::ComplexFrame:
        return "complexFrame";
    }
    return "audio";
}

[[nodiscard]] std::string_view analysisSectionToken(
    WorkspaceAnalysisSection section) noexcept
{
    switch (section)
    {
    case WorkspaceAnalysisSection::Traces:
        return "traces";
    case WorkspaceAnalysisSection::Events:
        return "events";
    case WorkspaceAnalysisSection::Runtime:
        return "runtime";
    case WorkspaceAnalysisSection::Assets:
        return "assets";
    }
    return "traces";
}

[[nodiscard]] std::string_view analysisTestSourceToken(
    WorkspaceAnalysisTestSourceKind source) noexcept
{
    switch (source)
    {
    case WorkspaceAnalysisTestSourceKind::None:
        return "none";
    case WorkspaceAnalysisTestSourceKind::Silence:
        return "silence";
    case WorkspaceAnalysisTestSourceKind::Impulse:
        return "impulse";
    case WorkspaceAnalysisTestSourceKind::Sine:
        return "sine";
    case WorkspaceAnalysisTestSourceKind::FrozenAsset:
        return "frozenAsset";
    }
    return "none";
}

[[nodiscard]] std::uint32_t interfaceScalePercent(
    WorkspaceInterfaceScale scale) noexcept
{
    switch (scale)
    {
    case WorkspaceInterfaceScale::Percent100:
        return 100;
    case WorkspaceInterfaceScale::Percent115:
        return 115;
    case WorkspaceInterfaceScale::Percent130:
        return 130;
    case WorkspaceInterfaceScale::Percent150:
        return 150;
    }
    return 0;
}

[[nodiscard]] bool isKnownSplitOrientation(
    WorkspaceStructureSplitOrientation orientation) noexcept
{
    switch (orientation)
    {
    case WorkspaceStructureSplitOrientation::Adaptive:
    case WorkspaceStructureSplitOrientation::LeftRight:
    case WorkspaceStructureSplitOrientation::TopBottom:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownGraphViewMode(WorkspaceGraphViewMode mode) noexcept
{
    switch (mode)
    {
    case WorkspaceGraphViewMode::Compact:
    case WorkspaceGraphViewMode::Ports:
    case WorkspaceGraphViewMode::PortsAndWires:
        return true;
    }
    return false;
}

[[nodiscard]] bool hierarchyActiveGraphIsVisible(
    const std::vector<std::string>& navigationPath,
    const std::optional<std::string>& activeSoundDefinitionId,
    bool secondaryPaneVisible) noexcept
{
    if (navigationPath.empty())
        return !activeSoundDefinitionId.has_value();
    if (!secondaryPaneVisible)
    {
        if (navigationPath.size() == 1)
            return !activeSoundDefinitionId.has_value();
        return activeSoundDefinitionId ==
               navigationPath[navigationPath.size() - 2];
    }
    if (navigationPath.size() == 1)
    {
        return !activeSoundDefinitionId.has_value() ||
               activeSoundDefinitionId == navigationPath.back();
    }
    return activeSoundDefinitionId == navigationPath.back() ||
           activeSoundDefinitionId == navigationPath[navigationPath.size() - 2];
}

[[nodiscard]] bool hierarchyBookmarkIsStructurallyValid(
    const WorkspaceHierarchyBookmark& bookmark)
{
    if (bookmark.definitionId.empty() || bookmark.navigationPath.empty() ||
        bookmark.navigationPath.size() > maximumWorkspaceNavigationDepth ||
        bookmark.navigationPath.back() != bookmark.definitionId ||
        !std::isfinite(bookmark.paneSplit) ||
        !(bookmark.paneSplit > 0.0 && bookmark.paneSplit < 1.0) ||
        !isKnownSplitOrientation(bookmark.structureSplitOrientation) ||
        !isKnownGraphViewMode(bookmark.graphViewMode) ||
        !hierarchyActiveGraphIsVisible(bookmark.navigationPath,
            bookmark.activeSoundDefinitionId,
            bookmark.secondaryPaneVisible))
    {
        return false;
    }

    std::unordered_set<std::string_view> pathIds;
    for (const auto& definitionId : bookmark.navigationPath)
    {
        if (definitionId.empty() || !pathIds.insert(definitionId).second)
            return false;
    }
    return true;
}

[[nodiscard]] bool isVisibleOrActiveGraphContext(
    const WorkspaceSessionState& state,
    const WorkspaceGraphSessionState& context)
{
    if (!context.soundDefinitionId.has_value())
        return true;
    if (state.activeSoundDefinitionId == context.soundDefinitionId)
        return true;
    return std::ranges::find(
               state.structureNavigationPath,
               *context.soundDefinitionId)
        != state.structureNavigationPath.end();
}

void trimWorkspaceGraphContextCache(WorkspaceSessionState& state)
{
    if (state.structureNavigationPath.size()
        > maximumWorkspaceNavigationDepth)
    {
        state.structureNavigationPath.erase(
            state.structureNavigationPath.begin(),
            state.structureNavigationPath.end()
                - static_cast<std::ptrdiff_t>(
                    maximumWorkspaceNavigationDepth));
    }
    normalize(state);
    while (state.graphContexts.size() > maximumWorkspaceGraphContexts)
    {
        auto eviction = std::ranges::find_if(
            state.graphContexts,
            [&](const auto& context) {
                return !isVisibleOrActiveGraphContext(state, context);
            });
        if (eviction == state.graphContexts.end())
            break;
        state.graphContexts.erase(eviction);
    }
}

void appendQuoted(std::string& output, std::string_view value)
{
    json::appendEscapedString(output, value);
}

void appendNumber(std::string& output, double value)
{
    char buffer[64]{};
    const auto converted = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (converted.ec == std::errc{})
        output.append(buffer, converted.ptr);
    else
        output.push_back('0');
}

void appendScope(
    std::string& output,
    const std::optional<std::string>& soundDefinitionId,
    int indent)
{
    const auto indentation = std::string(static_cast<std::size_t>(indent), ' ');
    const auto memberIndentation =
        std::string(static_cast<std::size_t>(indent + 2), ' ');
    output += "{\n";
    output += memberIndentation;
    output += R"json("kind": )json";
    appendQuoted(
        output,
        soundDefinitionId.has_value() ? "soundDefinition" : "project");
    if (soundDefinitionId.has_value())
    {
        output += ",\n";
        output += memberIndentation;
        output += R"json("definitionId": )json";
        appendQuoted(output, *soundDefinitionId);
    }
    output += '\n';
    output += indentation;
    output.push_back('}');
}

[[nodiscard]] std::string scopeKey(
    const std::optional<std::string>& soundDefinitionId)
{
    if (!soundDefinitionId.has_value())
        return "project";
    return "soundDefinition\n" + *soundDefinitionId;
}

void appendAnalysisSession(
    std::string& output,
    const WorkspaceAnalysisSessionState& analysis)
{
    output += R"json(  "analysis": {
    "traces": [
)json";
    for (std::size_t index = 0; index < analysis.traces.size(); ++index)
    {
        const auto& trace = analysis.traces[index];
        output += R"json(      {
        "traceId": )json";
        appendQuoted(output, trace.traceId);
        output += R"json(,
        "instancePath": [)json";
        for (std::size_t pathIndex = 0;
             pathIndex < trace.instancePath.size();
             ++pathIndex)
        {
            if (pathIndex != 0)
                output += ", ";
            appendQuoted(output, trace.instancePath[pathIndex]);
        }
        output += R"json(],
        "containingDefinitionId": )json";
        if (trace.containingDefinitionId.has_value())
            appendQuoted(output, *trace.containingDefinitionId);
        else
            output += "null";
        output += R"json(,
        "nodeId": )json";
        appendQuoted(output, trace.nodeId);
        output += R"json(,
        "portId": )json";
        if (trace.portId.has_value())
            appendQuoted(output, *trace.portId);
        else
            output += "null";
        output += R"json(,
        "kind": )json";
        appendQuoted(output, analysisTraceKindToken(trace.kind));
        output += R"json(,
        "visible": )json";
        output += trace.visible ? "true" : "false";
        output += R"json(,
        "colourSlot": )json";
        output += std::to_string(trace.colourSlot);
        output += R"json(,
        "verticalScale": )json";
        appendNumber(output, trace.verticalScale);
        output += "\n      }";
        if (index + 1 < analysis.traces.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += R"json(    ],
    "selectedTraceId": )json";
    if (analysis.selectedTraceId.has_value())
        appendQuoted(output, *analysis.selectedTraceId);
    else
        output += "null";
    output += R"json(,
    "frozen": )json";
    output += analysis.frozen ? "true" : "false";
    output += R"json(,
    "refreshRateHz": )json";
    output += std::to_string(analysis.refreshRateHz);
    output += R"json(,
    "historyFrames": )json";
    output += std::to_string(analysis.historyFrames);
    output += R"json(,
    "activeSection": )json";
    appendQuoted(output, analysisSectionToken(analysis.activeSection));
    output += R"json(,
    "audition": )json";
    if (!analysis.audition.has_value())
    {
        output += "null";
    }
    else
    {
        const auto& audition = *analysis.audition;
        output += R"json({
      "traceId": )json";
        appendQuoted(output, audition.traceId);
        output += R"json(,
      "destinationInputPortId": )json";
        if (audition.destinationInputPortId.has_value())
            appendQuoted(output, *audition.destinationInputPortId);
        else
            output += "null";
        output += R"json(,
      "source": )json";
        appendQuoted(output, analysisTestSourceToken(audition.source));
        output += R"json(,
      "sineFrequencyHz": )json";
        appendNumber(output, audition.sineFrequencyHz);
        output += R"json(,
      "frozenAssetId": )json";
        if (audition.frozenAssetId.has_value())
            appendQuoted(output, *audition.frozenAssetId);
        else
            output += "null";
        output += R"json(,
      "includeUpstream": )json";
        output += audition.includeUpstream ? "true" : "false";
        output += "\n    }";
    }
    output += "\n  },\n";
}

void appendSamplesSession(
    std::string& output,
    const WorkspaceSamplesSessionState& samples)
{
    output += R"json(  "samples": {
    "activeMapId": )json";
    if (samples.activeMapId.has_value())
        appendQuoted(output, *samples.activeMapId);
    else
        output += "null";
    output += R"json(,
    "selectedRegionIds": [)json";
    for (std::size_t index = 0;
         index < samples.selectedRegionIds.size();
         ++index)
    {
        if (index != 0)
            output += ", ";
        appendQuoted(output, samples.selectedRegionIds[index]);
    }
    output += R"json(],
    "mapViewport": {
      "noteStart": )json";
    appendNumber(output, samples.mapViewport.noteStart);
    output += R"json(,
      "noteSpan": )json";
    appendNumber(output, samples.mapViewport.noteSpan);
    output += R"json(,
      "velocityStart": )json";
    appendNumber(output, samples.mapViewport.velocityStart);
    output += R"json(,
      "velocitySpan": )json";
    appendNumber(output, samples.mapViewport.velocitySpan);
    output += R"json(
    },
    "waveformViewport": {
      "normalizedStart": )json";
    appendNumber(output, samples.waveformViewport.normalizedStart);
    output += R"json(,
      "normalizedSpan": )json";
    appendNumber(output, samples.waveformViewport.normalizedSpan);
    output += R"json(
    },
    "mapWaveformSplit": )json";
    appendNumber(output, samples.mapWaveformSplit);
    output += "\n  }\n";
}

void appendSoundLibraryState(
    std::string& output,
    const WorkspaceSoundLibraryState& library)
{
    output += R"json(  "soundLibrary": {
    "selectedEntryId": )json";
    if (library.selectedEntryId.has_value())
        appendQuoted(output, *library.selectedEntryId);
    else
        output += "null";
    output += R"json(,
    "collection": )json";
    appendQuoted(output, soundLibraryCollectionToken(library.collection));
    output += R"json(,
    "sourceFilter": )json";
    appendQuoted(output, soundLibrarySourceFilterToken(library.sourceFilter));
    output += R"json(,
    "compatibilityFilter": )json";
    appendQuoted(output,
        soundLibraryCompatibilityFilterToken(library.compatibilityFilter));
    output += R"json(,
    "healthFilter": )json";
    appendQuoted(output, soundLibraryHealthFilterToken(library.healthFilter));
    output += R"json(,
    "category": )json";
    if (library.category.has_value())
        appendQuoted(output, *library.category);
    else
        output += "null";
    output += R"json(,
    "origin": )json";
    if (library.origin.has_value())
        appendQuoted(output, *library.origin);
    else
        output += "null";
    output += R"json(,
    "favoriteEntryIds": [)json";
    for (std::size_t index = 0; index < library.favoriteEntryIds.size(); ++index)
    {
        if (index != 0)
            output += ", ";
        appendQuoted(output, library.favoriteEntryIds[index]);
    }
    output += R"json(],
    "recentEntryIds": [)json";
    for (std::size_t index = 0; index < library.recentEntryIds.size(); ++index)
    {
        if (index != 0)
            output += ", ";
        appendQuoted(output, library.recentEntryIds[index]);
    }
    output += R"json(],
    "inputSubstitutions": [
)json";
    for (std::size_t index = 0;
         index < library.inputSubstitutions.size();
         ++index)
    {
        const auto& substitution = library.inputSubstitutions[index];
        output += R"json(      {
        "portId": )json";
        appendQuoted(output, substitution.portId);
        output += R"json(,
        "source": )json";
        appendQuoted(output,
            soundLibraryAuditionSourceToken(substitution.source));
        output += R"json(,
        "value": )json";
        appendNumber(output, substitution.value);
        output += R"json(,
        "frozenAssetId": )json";
        if (substitution.frozenAssetId.has_value())
            appendQuoted(output, *substitution.frozenAssetId);
        else
            output += "null";
        output += "\n      }";
        if (index + 1 < library.inputSubstitutions.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += R"json(    ],
    "detailVisible": )json";
    output += library.detailVisible ? "true" : "false";
    output += "\n  },\n";
}

void appendWorkspaceVariableValues(
    std::string& output,
    const std::vector<WorkspaceVariableValue>& values)
{
    output += R"json(  "variableValues": [
)json";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto& value = values[index];
        output += R"json(    {
      "variableId": )json";
        appendQuoted(output, value.variableId);
        output += R"json(,
      "domain": )json";
        appendQuoted(output, variableDomainToken(value.domain));
        output += R"json(,
      "value": )json";
        appendNumber(output, value.value);
        output += R"json(,
      "declarationFingerprint": )json";
        appendQuoted(output, value.declarationFingerprint);
        output += "\n    }";
        if (index + 1 < values.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ],\n";
}

void appendWorkspaceSoundVariableValues(
    std::string& output,
    const std::vector<WorkspaceSoundVariableValue>& values)
{
    output += R"json(  "soundVariableValues": [
)json";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto& value = values[index];
        output += R"json(    {
      "variableId": )json";
        appendQuoted(output, value.variableId);
        output += R"json(,
      "selectedDefinitionId": )json";
        appendQuoted(output, value.selectedDefinitionId);
        output += R"json(,
      "declarationFingerprint": )json";
        appendQuoted(output, value.declarationFingerprint);
        output += "\n    }";
        if (index + 1 < values.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ],\n";
}

void parseWorkspaceVariableValues(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    std::vector<WorkspaceVariableValue>& output)
{
    constexpr std::string_view path = "$.variableValues";
    output.clear();
    if (!value.isArray())
    {
        addError(result, std::string(path), "Expected variable value array");
        return;
    }
    if (value.array().size() > maximumWorkspaceVariableValues)
    {
        addError(result, std::string(path), "Too many workspace variable values");
        return;
    }
    output.reserve(value.array().size());
    for (std::size_t index = 0; index < value.array().size(); ++index)
    {
        const auto itemPath = std::string(path) + "["
            + std::to_string(index) + "]";
        const auto& item = value.array()[index];
        if (!item.isObject())
        {
            addError(result, itemPath, "Expected workspace variable value object");
            continue;
        }
        rejectUnknownMembers(
            result,
            item,
            itemPath,
            {"variableId", "domain", "value", "declarationFingerprint"});
        WorkspaceVariableValue parsed;
        bool valid = readRequiredString(
            result,
            item,
            "variableId",
            itemPath + ".variableId",
            parsed.variableId);
        if (!isValidProjectVariableIdentifier(parsed.variableId))
        {
            addError(
                result,
                itemPath + ".variableId",
                "Expected bounded project variable identifier");
            valid = false;
        }
        std::string domain;
        if (readRequiredString(
                result,
                item,
                "domain",
                itemPath + ".domain",
                domain))
        {
            const auto parsedDomain = variableDomainFromToken(domain);
            if (!parsedDomain.has_value())
            {
                addError(
                    result,
                    itemPath + ".domain",
                    "Expected supported workspace variable domain");
                valid = false;
            }
            else
            {
                parsed.domain = *parsedDomain;
            }
        }
        else
        {
            valid = false;
        }
        valid = readRequiredFiniteNumber(
            result,
            item,
            "value",
            itemPath + ".value",
            parsed.value) && valid;
        valid = readRequiredString(
            result,
            item,
            "declarationFingerprint",
            itemPath + ".declarationFingerprint",
            parsed.declarationFingerprint) && valid;
        if (!isCanonicalWorkspaceVariableFingerprint(
                parsed.declarationFingerprint))
        {
            addError(
                result,
                itemPath + ".declarationFingerprint",
                "Expected canonical sha256 declaration fingerprint");
            valid = false;
        }
        if (valid)
            output.push_back(std::move(parsed));
    }
}

void parseWorkspaceSoundVariableValues(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    std::vector<WorkspaceSoundVariableValue>& output)
{
    constexpr std::string_view path = "$.soundVariableValues";
    output.clear();
    if (!value.isArray())
    {
        addError(result, std::string(path), "Expected Sound-variable value array");
        return;
    }
    if (value.array().size() > maximumWorkspaceSoundVariableValues)
    {
        addError(result, std::string(path), "Too many workspace Sound-variable values");
        return;
    }
    output.reserve(value.array().size());
    for (std::size_t index = 0; index < value.array().size(); ++index)
    {
        const auto itemPath = std::string(path) + "["
            + std::to_string(index) + "]";
        const auto& item = value.array()[index];
        if (!item.isObject())
        {
            addError(result, itemPath, "Expected workspace Sound-variable value object");
            continue;
        }
        rejectUnknownMembers(
            result,
            item,
            itemPath,
            {"variableId", "selectedDefinitionId", "declarationFingerprint"});
        WorkspaceSoundVariableValue parsed;
        auto valid = readRequiredString(
            result,
            item,
            "variableId",
            itemPath + ".variableId",
            parsed.variableId);
        if (!isValidProjectVariableIdentifier(parsed.variableId))
        {
            addError(
                result,
                itemPath + ".variableId",
                "Expected bounded Sound-variable identifier");
            valid = false;
        }
        valid = readRequiredString(
            result,
            item,
            "selectedDefinitionId",
            itemPath + ".selectedDefinitionId",
            parsed.selectedDefinitionId) && valid;
        if (!isValidProjectVariableIdentifier(parsed.selectedDefinitionId))
        {
            addError(
                result,
                itemPath + ".selectedDefinitionId",
                "Expected exact bounded Sound definition identifier");
            valid = false;
        }
        valid = readRequiredString(
            result,
            item,
            "declarationFingerprint",
            itemPath + ".declarationFingerprint",
            parsed.declarationFingerprint) && valid;
        if (!isCanonicalWorkspaceVariableFingerprint(
                parsed.declarationFingerprint))
        {
            addError(
                result,
                itemPath + ".declarationFingerprint",
                "Expected canonical sha256 declaration fingerprint");
            valid = false;
        }
        if (valid)
            output.push_back(std::move(parsed));
    }
}

void parseWorkspaceLiveToolStates(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    std::vector<WorkspacePerformanceSessionState::LiveToolState>& output)
{
    constexpr std::string_view path = "$.performance.liveToolStates";
    output.clear();
    if (!value.isArray())
    {
        addError(result, std::string(path), "Expected Live Tool state array");
        return;
    }
    if (value.array().size() > maximumWorkspaceLiveToolStates)
    {
        addError(result, std::string(path), "Too many Live Tool session states");
        return;
    }
    std::unordered_set<std::string> toolIds;
    output.reserve(value.array().size());
    for (std::size_t index = 0; index < value.array().size(); ++index)
    {
        const auto itemPath = std::string(path) + "["
            + std::to_string(index) + "]";
        const auto& item = value.array()[index];
        if (!item.isObject())
        {
            addError(result, itemPath, "Expected Live Tool state object");
            continue;
        }
        rejectUnknownMembers(result, item, itemPath,
            {"toolId", "stateId", "definitionFingerprint", "variables"});
        WorkspacePerformanceSessionState::LiveToolState parsed;
        auto valid = readRequiredString(
            result, item, "toolId", itemPath + ".toolId", parsed.toolId);
        valid = readRequiredString(
            result, item, "stateId", itemPath + ".stateId", parsed.stateId)
            && valid;
        valid = readRequiredString(
            result,
            item,
            "definitionFingerprint",
            itemPath + ".definitionFingerprint",
            parsed.definitionFingerprint) && valid;
        if (!isValidProjectLiveSetIdentifier(parsed.toolId)
            || !isValidProjectLiveSetIdentifier(parsed.stateId)
            || !isCanonicalWorkspaceVariableFingerprint(
                parsed.definitionFingerprint)
            || !toolIds.insert(parsed.toolId).second)
        {
            addError(result, itemPath,
                "Live Tool state identity or fingerprint is invalid or duplicated");
            valid = false;
        }
        const auto* variables = requiredMember(
            result, item, "variables", itemPath + ".variables");
        if (variables == nullptr || !variables->isArray())
        {
            if (variables != nullptr)
                addError(result, itemPath + ".variables",
                    "Expected Live Tool variable array");
            valid = false;
        }
        else if (variables->array().size()
                 > maximumProjectLiveToolVariables)
        {
            addError(result, itemPath + ".variables",
                "Too many Live Tool variable values");
            valid = false;
        }
        else
        {
            std::unordered_set<std::string> variableIds;
            parsed.variables.reserve(variables->array().size());
            for (std::size_t variableIndex = 0;
                 variableIndex < variables->array().size();
                 ++variableIndex)
            {
                const auto variablePath = itemPath + ".variables["
                    + std::to_string(variableIndex) + "]";
                const auto& variable = variables->array()[variableIndex];
                if (!variable.isObject())
                {
                    addError(result, variablePath,
                        "Expected Live Tool variable object");
                    valid = false;
                    continue;
                }
                rejectUnknownMembers(result, variable, variablePath,
                    {"variableId", "kind", "number", "boolean"});
                WorkspacePerformanceSessionState::LiveToolVariable value;
                auto variableValid = readRequiredString(
                    result,
                    variable,
                    "variableId",
                    variablePath + ".variableId",
                    value.variableId);
                std::string kind;
                variableValid = readRequiredString(
                    result, variable, "kind", variablePath + ".kind", kind)
                    && variableValid;
                if (kind == "number")
                    value.kind = ProjectLiveToolVariableKind::Number;
                else if (kind == "boolean")
                    value.kind = ProjectLiveToolVariableKind::Boolean;
                else
                {
                    addError(result, variablePath + ".kind",
                        "Expected number or boolean Live Tool variable kind");
                    variableValid = false;
                }
                variableValid = readRequiredFiniteNumber(
                    result,
                    variable,
                    "number",
                    variablePath + ".number",
                    value.number) && variableValid;
                variableValid = readRequiredBoolean(
                    result,
                    variable,
                    "boolean",
                    variablePath + ".boolean",
                    value.boolean) && variableValid;
                if (!isValidProjectLiveSetIdentifier(value.variableId)
                    || !variableIds.insert(value.variableId).second
                    || (value.kind == ProjectLiveToolVariableKind::Boolean
                        ? value.number != 0.0
                        : value.boolean))
                {
                    addError(result, variablePath,
                        "Live Tool variable value is invalid or duplicated");
                    variableValid = false;
                }
                if (variableValid)
                    parsed.variables.push_back(std::move(value));
                valid = valid && variableValid;
            }
        }
        if (valid)
            output.push_back(std::move(parsed));
    }
}

[[nodiscard]] bool readSoundLibraryEntryIdArray(
    WorkspaceSessionLoadResult& result,
    const json::Value& object,
    std::string_view memberName,
    const std::string& path,
    std::size_t maximumCount,
    std::vector<std::string>& output)
{
    output.clear();
    const auto* value = requiredMember(result, object, memberName, path);
    if (value == nullptr)
        return false;
    if (!value->isArray())
    {
        addError(result, path, "Expected array");
        return false;
    }
    if (value->array().size() > maximumCount)
    {
        addError(result, path, "Too many Sound Library entry ids");
        return false;
    }
    std::unordered_set<std::string_view> ids;
    bool valid = true;
    for (std::size_t index = 0; index < value->array().size(); ++index)
    {
        const auto itemPath = path + "[" + std::to_string(index) + "]";
        const auto& item = value->array()[index];
        if (!item.isString() || item.string().empty()
            || item.string().size()
                > maximumWorkspaceSoundLibraryEntryIdBytes)
        {
            addError(result, itemPath, "Expected bounded non-empty Sound Library entry id");
            valid = false;
            continue;
        }
        if (!ids.insert(item.string()).second)
        {
            addError(result, itemPath, "Duplicate Sound Library entry id");
            valid = false;
            continue;
        }
        output.push_back(item.string());
    }
    return valid;
}

void parseSoundLibraryState(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    WorkspaceSoundLibraryState& library,
    std::uint32_t sourceVersion)
{
    constexpr std::string_view path = "$.soundLibrary";
    if (!value.isObject())
    {
        addError(result, std::string(path), "Expected Sound Library state object");
        return;
    }
    rejectUnknownMembers(result,
        value,
        std::string(path),
        {"selectedEntryId", "collection", "sourceFilter",
         "compatibilityFilter", "healthFilter", "category", "origin",
         "favoriteEntryIds", "recentEntryIds", "inputSubstitutions",
         "detailVisible"});

    (void)readNullableBoundedString(result,
        value,
        "selectedEntryId",
        "$.soundLibrary.selectedEntryId",
        library.selectedEntryId,
        maximumWorkspaceSoundLibraryEntryIdBytes);
    (void)readNullableBoundedString(result,
        value,
        "category",
        "$.soundLibrary.category",
        library.category,
        maximumWorkspaceSoundLibraryCategoryBytes);
    if (sourceVersion >= 9 || member(value, "origin") != nullptr)
    {
        (void)readNullableBoundedString(result,
            value,
            "origin",
            "$.soundLibrary.origin",
            library.origin,
            maximumWorkspaceSoundLibraryCategoryBytes);
    }

    std::string token;
    if (readRequiredString(result,
            value,
            "collection",
            "$.soundLibrary.collection",
            token))
    {
        if (token == "all")
            library.collection = WorkspaceSoundLibraryCollection::All;
        else if (token == "favorites")
            library.collection = WorkspaceSoundLibraryCollection::Favorites;
        else if (token == "recent")
            library.collection = WorkspaceSoundLibraryCollection::Recent;
        else
            addError(result, "$.soundLibrary.collection", "Unknown Sound Library collection");
    }
    if (readRequiredString(result,
            value,
            "sourceFilter",
            "$.soundLibrary.sourceFilter",
            token))
    {
        if (token == "all")
            library.sourceFilter = WorkspaceSoundLibrarySourceFilter::All;
        else if (token == "projectDefinitions")
            library.sourceFilter = WorkspaceSoundLibrarySourceFilter::ProjectDefinitions;
        else if (token == "components")
            library.sourceFilter = WorkspaceSoundLibrarySourceFilter::Components;
        else if (token == "originalExamples")
            library.sourceFilter = WorkspaceSoundLibrarySourceFilter::OriginalExamples;
        else
            addError(result, "$.soundLibrary.sourceFilter", "Unknown Sound Library source filter");
    }
    if (readRequiredString(result,
            value,
            "compatibilityFilter",
            "$.soundLibrary.compatibilityFilter",
            token))
    {
        if (token == "all")
            library.compatibilityFilter = WorkspaceSoundLibraryCompatibilityFilter::All;
        else if (token == "ready")
            library.compatibilityFilter = WorkspaceSoundLibraryCompatibilityFilter::Ready;
        else if (token == "needsAttention")
            library.compatibilityFilter = WorkspaceSoundLibraryCompatibilityFilter::NeedsAttention;
        else if (token == "unavailable")
            library.compatibilityFilter = WorkspaceSoundLibraryCompatibilityFilter::Unavailable;
        else
            addError(result, "$.soundLibrary.compatibilityFilter", "Unknown Sound Library compatibility filter");
    }
    if (readRequiredString(result,
            value,
            "healthFilter",
            "$.soundLibrary.healthFilter",
            token))
    {
        if (token == "all")
            library.healthFilter = WorkspaceSoundLibraryHealthFilter::All;
        else if (token == "healthy")
            library.healthFilter = WorkspaceSoundLibraryHealthFilter::Healthy;
        else if (token == "warning")
            library.healthFilter = WorkspaceSoundLibraryHealthFilter::Warning;
        else if (token == "unavailable")
            library.healthFilter = WorkspaceSoundLibraryHealthFilter::Unavailable;
        else
            addError(result, "$.soundLibrary.healthFilter", "Unknown Sound Library health filter");
    }

    (void)readSoundLibraryEntryIdArray(result,
        value,
        "favoriteEntryIds",
        "$.soundLibrary.favoriteEntryIds",
        maximumWorkspaceSoundLibraryFavorites,
        library.favoriteEntryIds);
    (void)readSoundLibraryEntryIdArray(result,
        value,
        "recentEntryIds",
        "$.soundLibrary.recentEntryIds",
        maximumWorkspaceSoundLibraryRecents,
        library.recentEntryIds);

    library.inputSubstitutions.clear();
    const auto* substitutions = requiredMember(result,
        value,
        "inputSubstitutions",
        "$.soundLibrary.inputSubstitutions");
    if (substitutions != nullptr)
    {
        if (!substitutions->isArray())
        {
            addError(result, "$.soundLibrary.inputSubstitutions", "Expected array");
        }
        else if (substitutions->array().size()
            > maximumWorkspaceSoundLibraryInputSubstitutions)
        {
            addError(result,
                "$.soundLibrary.inputSubstitutions",
                "Too many Sound Library input substitutions");
        }
        else
        {
            std::unordered_set<std::string> portIds;
            for (std::size_t index = 0;
                 index < substitutions->array().size();
                 ++index)
            {
                const auto itemPath = "$.soundLibrary.inputSubstitutions["
                    + std::to_string(index) + "]";
                const auto& item = substitutions->array()[index];
                if (!item.isObject())
                {
                    addError(result, itemPath, "Expected input substitution object");
                    continue;
                }
                rejectUnknownMembers(result,
                    item,
                    itemPath,
                    {"portId", "source", "value", "frozenAssetId"});
                WorkspaceSoundLibraryInputSubstitution substitution;
                bool valid = readRequiredString(result,
                    item,
                    "portId",
                    itemPath + ".portId",
                    substitution.portId);
                if (substitution.portId.empty()
                    || substitution.portId.size()
                        > maximumWorkspaceSoundLibraryPortIdBytes
                    || !portIds.insert(substitution.portId).second)
                {
                    addError(result,
                        itemPath + ".portId",
                        "Sound Library input port id is empty, too long, or duplicated");
                    valid = false;
                }
                if (readRequiredString(result,
                        item,
                        "source",
                        itemPath + ".source",
                        token))
                {
                    if (token == "default")
                        substitution.source = WorkspaceSoundLibraryAuditionSource::Default;
                    else if (token == "silence")
                        substitution.source = WorkspaceSoundLibraryAuditionSource::Silence;
                    else if (token == "sine")
                        substitution.source = WorkspaceSoundLibraryAuditionSource::Sine;
                    else if (token == "constant")
                        substitution.source = WorkspaceSoundLibraryAuditionSource::Constant;
                    else if (token == "frozenAsset")
                        substitution.source = WorkspaceSoundLibraryAuditionSource::FrozenAsset;
                    else
                    {
                        addError(result, itemPath + ".source", "Unknown Sound Library audition source");
                        valid = false;
                    }
                }
                else
                {
                    valid = false;
                }
                valid = readRequiredFiniteNumber(result,
                            item,
                            "value",
                            itemPath + ".value",
                            substitution.value)
                    && valid;
                valid = readNullableBoundedString(result,
                            item,
                            "frozenAssetId",
                            itemPath + ".frozenAssetId",
                            substitution.frozenAssetId,
                            maximumWorkspaceSoundLibraryAssetIdBytes)
                    && valid;
                const auto source = substitution.source;
                const auto valueValid =
                    ((source == WorkspaceSoundLibraryAuditionSource::Default
                         || source == WorkspaceSoundLibraryAuditionSource::Silence
                         || source == WorkspaceSoundLibraryAuditionSource::FrozenAsset)
                        && substitution.value == 0.0)
                    || (source == WorkspaceSoundLibraryAuditionSource::Sine
                        && substitution.value
                            >= minimumWorkspaceSoundLibrarySineFrequencyHz
                        && substitution.value
                            <= maximumWorkspaceSoundLibrarySineFrequencyHz)
                    || (source == WorkspaceSoundLibraryAuditionSource::Constant
                        && std::abs(substitution.value)
                            <= maximumWorkspaceSoundLibraryConstantMagnitude);
                if (!valueValid)
                {
                    addError(result,
                        itemPath + ".value",
                        "Sound Library audition value contradicts its source");
                    valid = false;
                }
                if ((source == WorkspaceSoundLibraryAuditionSource::FrozenAsset)
                    != substitution.frozenAssetId.has_value())
                {
                    addError(result,
                        itemPath + ".frozenAssetId",
                        "Frozen Sound Library source identity is incomplete or contradictory");
                    valid = false;
                }
                if (valid)
                    library.inputSubstitutions.push_back(std::move(substitution));
            }
        }
    }
    (void)readRequiredBoolean(result,
        value,
        "detailVisible",
        "$.soundLibrary.detailVisible",
        library.detailVisible);
}

void parseAnalysisSession(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    WorkspaceAnalysisSessionState& analysis)
{
    constexpr std::string_view path = "$.analysis";
    if (!value.isObject())
    {
        addError(result, std::string(path), "Expected Analysis session object");
        return;
    }
    rejectUnknownMembers(result,
        value,
        std::string(path),
        {"traces", "selectedTraceId", "frozen", "refreshRateHz",
         "historyFrames", "activeSection", "audition"});

    analysis.traces.clear();
    const auto* traces = requiredMember(result, value, "traces", "$.analysis.traces");
    std::unordered_set<std::string> traceIds;
    std::unordered_set<std::string> targetKeys;
    if (traces != nullptr)
    {
        if (!traces->isArray())
        {
            addError(result, "$.analysis.traces", "Expected array");
        }
        else if (traces->array().size() > maximumWorkspaceAnalysisTraces)
        {
            addError(result, "$.analysis.traces", "Too many Analysis traces");
        }
        else
        {
            for (std::size_t index = 0; index < traces->array().size(); ++index)
            {
                const auto itemPath = "$.analysis.traces[" + std::to_string(index) + "]";
                const auto& item = traces->array()[index];
                if (!item.isObject())
                {
                    addError(result, itemPath, "Expected Analysis trace object");
                    continue;
                }
                rejectUnknownMembers(result,
                    item,
                    itemPath,
                    {"traceId", "instancePath", "containingDefinitionId", "nodeId",
                     "portId", "kind", "visible", "colourSlot", "verticalScale"});
                WorkspaceAnalysisTraceState trace;
                bool valid = readRequiredString(
                    result, item, "traceId", itemPath + ".traceId", trace.traceId);
                if (trace.traceId.empty()
                    || trace.traceId.size() > maximumWorkspaceAnalysisTraceIdBytes)
                {
                    addError(result,
                        itemPath + ".traceId",
                        "Analysis trace id must be non-empty and bounded");
                    valid = false;
                }
                else if (!traceIds.insert(trace.traceId).second)
                {
                    addError(result, itemPath + ".traceId", "Duplicate Analysis trace id");
                    valid = false;
                }

                const auto* instancePath = requiredMember(
                    result, item, "instancePath", itemPath + ".instancePath");
                if (instancePath != nullptr)
                {
                    if (!instancePath->isArray())
                    {
                        addError(result, itemPath + ".instancePath", "Expected array");
                        valid = false;
                    }
                    else if (instancePath->array().size()
                             > maximumWorkspaceAnalysisInstanceDepth)
                    {
                        addError(result,
                            itemPath + ".instancePath",
                            "Analysis instance path exceeds supported depth");
                        valid = false;
                    }
                    else
                    {
                        for (std::size_t pathIndex = 0;
                             pathIndex < instancePath->array().size();
                             ++pathIndex)
                        {
                            const auto elementPath = itemPath + ".instancePath["
                                + std::to_string(pathIndex) + "]";
                            const auto& element = instancePath->array()[pathIndex];
                            if (!element.isString() || element.string().empty()
                                || element.string().size()
                                    > maximumWorkspaceAnalysisTargetIdBytes)
                            {
                                addError(result,
                                    elementPath,
                                    "Expected bounded non-empty instance id");
                                valid = false;
                                continue;
                            }
                            trace.instancePath.push_back(element.string());
                        }
                    }
                }
                valid = readNullableBoundedString(result,
                            item,
                            "containingDefinitionId",
                            itemPath + ".containingDefinitionId",
                            trace.containingDefinitionId,
                            maximumWorkspaceAnalysisTargetIdBytes)
                    && valid;
                valid = readRequiredString(
                            result, item, "nodeId", itemPath + ".nodeId", trace.nodeId)
                    && valid;
                if (trace.nodeId.empty()
                    || trace.nodeId.size() > maximumWorkspaceAnalysisTargetIdBytes)
                {
                    addError(result,
                        itemPath + ".nodeId",
                        "Analysis node id must be non-empty and bounded");
                    valid = false;
                }
                valid = readNullableBoundedString(result,
                            item,
                            "portId",
                            itemPath + ".portId",
                            trace.portId,
                            maximumWorkspaceAnalysisTargetIdBytes)
                    && valid;
                if (trace.instancePath.empty() != !trace.containingDefinitionId.has_value())
                {
                    addError(result,
                        itemPath,
                        "Nested Analysis targets require an instance path and containing definition");
                    valid = false;
                }

                std::string kind;
                if (readRequiredString(result, item, "kind", itemPath + ".kind", kind))
                {
                    if (kind == "audio")
                        trace.kind = WorkspaceAnalysisTraceKind::Audio;
                    else if (kind == "control")
                        trace.kind = WorkspaceAnalysisTraceKind::Control;
                    else if (kind == "event")
                        trace.kind = WorkspaceAnalysisTraceKind::Event;
                    else if (kind == "partialSpectrum")
                        trace.kind = WorkspaceAnalysisTraceKind::PartialSpectrum;
                    else if (kind == "complexFrame")
                        trace.kind = WorkspaceAnalysisTraceKind::ComplexFrame;
                    else
                    {
                        addError(result, itemPath + ".kind", "Unknown Analysis trace kind");
                        valid = false;
                    }
                }
                else
                {
                    valid = false;
                }
                (void) readRequiredBoolean(
                    result, item, "visible", itemPath + ".visible", trace.visible);
                std::uint32_t colourSlot = 0;
                if (readRequiredUnsignedInteger(result,
                        item,
                        "colourSlot",
                        itemPath + ".colourSlot",
                        colourSlot))
                {
                    if (colourSlot > maximumWorkspaceAnalysisColourSlot)
                    {
                        addError(result,
                            itemPath + ".colourSlot",
                            "Analysis colour slot exceeds supported range");
                        valid = false;
                    }
                    else
                    {
                        trace.colourSlot = static_cast<std::uint8_t>(colourSlot);
                    }
                }
                else
                {
                    valid = false;
                }
                if (readRequiredFiniteNumber(result,
                        item,
                        "verticalScale",
                        itemPath + ".verticalScale",
                        trace.verticalScale)
                    && !(trace.verticalScale >= minimumAnalysisVerticalScale
                         && trace.verticalScale <= maximumAnalysisVerticalScale))
                {
                    addError(result,
                        itemPath + ".verticalScale",
                        "Analysis vertical scale exceeds supported range");
                    valid = false;
                }

                std::string targetKey;
                for (const auto& instanceId : trace.instancePath)
                    targetKey += "i:" + instanceId + "\n";
                targetKey += "d:" + trace.containingDefinitionId.value_or("")
                    + "\nn:" + trace.nodeId + "\np:"
                    + trace.portId.value_or("") + "\nk:"
                    + std::string{analysisTraceKindToken(trace.kind)};
                if (!targetKeys.insert(targetKey).second)
                {
                    addError(result, itemPath, "Duplicate Analysis trace target");
                    valid = false;
                }
                if (valid)
                    analysis.traces.push_back(std::move(trace));
            }
        }
    }

    std::unordered_set<std::string> retainedTraceIds;
    retainedTraceIds.reserve(analysis.traces.size());
    for (const auto& trace : analysis.traces)
        retainedTraceIds.insert(trace.traceId);

    (void) readNullableBoundedString(result,
        value,
        "selectedTraceId",
        "$.analysis.selectedTraceId",
        analysis.selectedTraceId,
        maximumWorkspaceAnalysisTraceIdBytes);
    if (analysis.selectedTraceId.has_value()
        && !retainedTraceIds.contains(*analysis.selectedTraceId))
    {
        addError(result,
            "$.analysis.selectedTraceId",
            "Selected Analysis trace does not exist");
    }
    (void) readRequiredBoolean(
        result, value, "frozen", "$.analysis.frozen", analysis.frozen);
    if (readRequiredUnsignedInteger(result,
            value,
            "refreshRateHz",
            "$.analysis.refreshRateHz",
            analysis.refreshRateHz)
        && (analysis.refreshRateHz < minimumWorkspaceAnalysisRefreshRateHz
            || analysis.refreshRateHz > maximumWorkspaceAnalysisRefreshRateHz))
    {
        addError(result,
            "$.analysis.refreshRateHz",
            "Analysis refresh rate exceeds supported range");
    }
    if (readRequiredUnsignedInteger(result,
            value,
            "historyFrames",
            "$.analysis.historyFrames",
            analysis.historyFrames)
        && (analysis.historyFrames < minimumWorkspaceAnalysisHistoryFrames
            || analysis.historyFrames > maximumWorkspaceAnalysisHistoryFrames))
    {
        addError(result,
            "$.analysis.historyFrames",
            "Analysis history exceeds supported range");
    }
    std::string section;
    if (readRequiredString(result,
            value,
            "activeSection",
            "$.analysis.activeSection",
            section))
    {
        if (section == "traces")
            analysis.activeSection = WorkspaceAnalysisSection::Traces;
        else if (section == "events")
            analysis.activeSection = WorkspaceAnalysisSection::Events;
        else if (section == "runtime")
            analysis.activeSection = WorkspaceAnalysisSection::Runtime;
        else if (section == "assets")
            analysis.activeSection = WorkspaceAnalysisSection::Assets;
        else
            addError(result, "$.analysis.activeSection", "Unknown Analysis section");
    }

    analysis.audition.reset();
    const auto* audition = member(value, "audition");
    if (audition != nullptr && !audition->isNull())
    {
        if (!audition->isObject())
        {
            addError(result, "$.analysis.audition", "Expected null or audition object");
        }
        else
        {
            rejectUnknownMembers(result,
                *audition,
                "$.analysis.audition",
                {"traceId", "destinationInputPortId", "source",
                 "sineFrequencyHz", "frozenAssetId", "includeUpstream"});
            WorkspaceAnalysisAuditionState candidate;
            bool valid = readRequiredString(result,
                *audition,
                "traceId",
                "$.analysis.audition.traceId",
                candidate.traceId);
            if (candidate.traceId.empty()
                || candidate.traceId.size()
                    > maximumWorkspaceAnalysisTraceIdBytes
                || !retainedTraceIds.contains(candidate.traceId))
            {
                addError(result,
                    "$.analysis.audition.traceId",
                    "Audition trace id must identify a retained Analysis trace");
                valid = false;
            }
            valid = readNullableBoundedString(result,
                        *audition,
                        "destinationInputPortId",
                        "$.analysis.audition.destinationInputPortId",
                        candidate.destinationInputPortId,
                        maximumWorkspaceAnalysisTargetIdBytes)
                && valid;
            std::string source;
            if (readRequiredString(result,
                    *audition,
                    "source",
                    "$.analysis.audition.source",
                    source))
            {
                if (source == "none")
                    candidate.source = WorkspaceAnalysisTestSourceKind::None;
                else if (source == "silence")
                    candidate.source = WorkspaceAnalysisTestSourceKind::Silence;
                else if (source == "impulse")
                    candidate.source = WorkspaceAnalysisTestSourceKind::Impulse;
                else if (source == "sine")
                    candidate.source = WorkspaceAnalysisTestSourceKind::Sine;
                else if (source == "frozenAsset")
                    candidate.source = WorkspaceAnalysisTestSourceKind::FrozenAsset;
                else
                {
                    addError(result,
                        "$.analysis.audition.source",
                        "Unknown Analysis test source");
                    valid = false;
                }
            }
            else
            {
                valid = false;
            }
            if (readRequiredFiniteNumber(result,
                    *audition,
                    "sineFrequencyHz",
                    "$.analysis.audition.sineFrequencyHz",
                    candidate.sineFrequencyHz))
            {
                if (candidate.sineFrequencyHz
                        < minimumWorkspaceAnalysisSineFrequencyHz
                    || candidate.sineFrequencyHz
                        > maximumWorkspaceAnalysisSineFrequencyHz)
                {
                    addError(result,
                        "$.analysis.audition.sineFrequencyHz",
                        "Analysis sine frequency exceeds supported range");
                    valid = false;
                }
            }
            else
            {
                valid = false;
            }
            valid = readNullableBoundedString(result,
                        *audition,
                        "frozenAssetId",
                        "$.analysis.audition.frozenAssetId",
                        candidate.frozenAssetId,
                        maximumWorkspaceAnalysisAuditionAssetIdBytes)
                && valid;
            valid = readRequiredBoolean(result,
                        *audition,
                        "includeUpstream",
                        "$.analysis.audition.includeUpstream",
                        candidate.includeUpstream)
                && valid;
            const auto usesTemporarySource = candidate.source
                != WorkspaceAnalysisTestSourceKind::None;
            if (usesTemporarySource
                != candidate.destinationInputPortId.has_value())
            {
                addError(result,
                    "$.analysis.audition.destinationInputPortId",
                    "Temporary Analysis source requires one exact destination input");
                valid = false;
            }
            if ((candidate.source
                     == WorkspaceAnalysisTestSourceKind::FrozenAsset)
                != candidate.frozenAssetId.has_value())
            {
                addError(result,
                    "$.analysis.audition.frozenAssetId",
                    "Frozen Analysis source identity is incomplete or contradictory");
                valid = false;
            }
            if (valid)
                analysis.audition = std::move(candidate);
        }
    }
}

void parseSamplesSession(
    WorkspaceSessionLoadResult& result,
    const json::Value& value,
    WorkspaceSamplesSessionState& samples)
{
    constexpr std::string_view path = "$.samples";
    if (!value.isObject())
    {
        addError(result, std::string(path), "Expected Samples session object");
        return;
    }
    rejectUnknownMembers(
        result,
        value,
        std::string(path),
        {
            "activeMapId",
            "selectedRegionIds",
            "mapViewport",
            "waveformViewport",
            "mapWaveformSplit",
        });
    (void) readNullableNonEmptyString(
        result,
        value,
        "activeMapId",
        "$.samples.activeMapId",
        samples.activeMapId);

    samples.selectedRegionIds.clear();
    const auto* selection = requiredMember(
        result,
        value,
        "selectedRegionIds",
        "$.samples.selectedRegionIds");
    if (selection != nullptr)
    {
        if (!selection->isArray())
        {
            addError(result, "$.samples.selectedRegionIds", "Expected array");
        }
        else if (selection->array().size()
            > maximumWorkspaceSampleRegionSelections)
        {
            addError(
                result,
                "$.samples.selectedRegionIds",
                "Too many selected sample regions");
        }
        else
        {
            std::unordered_set<std::string> regionIds;
            for (std::size_t index = 0;
                 index < selection->array().size();
                 ++index)
            {
                const auto itemPath = "$.samples.selectedRegionIds["
                    + std::to_string(index) + "]";
                const auto& item = selection->array()[index];
                if (!item.isString() || item.string().empty())
                {
                    addError(result, itemPath, "Expected non-empty sample-region id");
                    continue;
                }
                if (!regionIds.insert(item.string()).second)
                {
                    addError(result, itemPath, "Duplicate selected sample-region id");
                    continue;
                }
                samples.selectedRegionIds.push_back(item.string());
            }
        }
    }
    if (!samples.activeMapId.has_value()
        && !samples.selectedRegionIds.empty())
    {
        addError(
            result,
            "$.samples.selectedRegionIds",
            "Sample-region selection requires an active sample map");
    }

    const auto* mapViewport = requiredMember(
        result,
        value,
        "mapViewport",
        "$.samples.mapViewport");
    if (mapViewport != nullptr)
    {
        if (!mapViewport->isObject())
        {
            addError(result, "$.samples.mapViewport", "Expected map viewport object");
        }
        else
        {
            rejectUnknownMembers(
                result,
                *mapViewport,
                "$.samples.mapViewport",
                {"noteStart", "noteSpan", "velocityStart", "velocitySpan"});
            (void) readRequiredFiniteNumber(
                result,
                *mapViewport,
                "noteStart",
                "$.samples.mapViewport.noteStart",
                samples.mapViewport.noteStart);
            (void) readRequiredFiniteNumber(
                result,
                *mapViewport,
                "noteSpan",
                "$.samples.mapViewport.noteSpan",
                samples.mapViewport.noteSpan);
            (void) readRequiredFiniteNumber(
                result,
                *mapViewport,
                "velocityStart",
                "$.samples.mapViewport.velocityStart",
                samples.mapViewport.velocityStart);
            (void) readRequiredFiniteNumber(
                result,
                *mapViewport,
                "velocitySpan",
                "$.samples.mapViewport.velocitySpan",
                samples.mapViewport.velocitySpan);
            if (samples.mapViewport.noteStart < 0.0
                || samples.mapViewport.noteStart >= 128.0
                || samples.mapViewport.noteSpan <= 0.0
                || samples.mapViewport.noteStart
                        + samples.mapViewport.noteSpan
                    > 128.0)
            {
                addError(
                    result,
                    "$.samples.mapViewport",
                    "Note viewport must lie inside MIDI boundaries 0 through 128");
            }
            if (samples.mapViewport.velocityStart < 1.0
                || samples.mapViewport.velocityStart >= 128.0
                || samples.mapViewport.velocitySpan <= 0.0
                || samples.mapViewport.velocityStart
                        + samples.mapViewport.velocitySpan
                    > 128.0)
            {
                addError(
                    result,
                    "$.samples.mapViewport",
                    "Velocity viewport must lie inside MIDI boundaries 1 through 128");
            }
        }
    }

    const auto* waveformViewport = requiredMember(
        result,
        value,
        "waveformViewport",
        "$.samples.waveformViewport");
    if (waveformViewport != nullptr)
    {
        if (!waveformViewport->isObject())
        {
            addError(
                result,
                "$.samples.waveformViewport",
                "Expected waveform viewport object");
        }
        else
        {
            rejectUnknownMembers(
                result,
                *waveformViewport,
                "$.samples.waveformViewport",
                {"normalizedStart", "normalizedSpan"});
            (void) readRequiredFiniteNumber(
                result,
                *waveformViewport,
                "normalizedStart",
                "$.samples.waveformViewport.normalizedStart",
                samples.waveformViewport.normalizedStart);
            (void) readRequiredFiniteNumber(
                result,
                *waveformViewport,
                "normalizedSpan",
                "$.samples.waveformViewport.normalizedSpan",
                samples.waveformViewport.normalizedSpan);
            if (samples.waveformViewport.normalizedStart < 0.0
                || samples.waveformViewport.normalizedStart >= 1.0
                || samples.waveformViewport.normalizedSpan <= 0.0
                || samples.waveformViewport.normalizedStart
                        + samples.waveformViewport.normalizedSpan
                    > 1.0)
            {
                addError(
                    result,
                    "$.samples.waveformViewport",
                    "Waveform viewport must lie inside normalized generation bounds");
            }
        }
    }

    if (readRequiredFiniteNumber(
            result,
            value,
            "mapWaveformSplit",
            "$.samples.mapWaveformSplit",
            samples.mapWaveformSplit)
        && !(samples.mapWaveformSplit > 0.0
             && samples.mapWaveformSplit < 1.0))
    {
        addError(
            result,
            "$.samples.mapWaveformSplit",
            "Samples map/waveform split must be between zero and one");
    }
}
}

bool isKnownWorkspaceAnalysisTestSourceKind(
    WorkspaceAnalysisTestSourceKind source) noexcept
{
    switch (source)
    {
    case WorkspaceAnalysisTestSourceKind::None:
    case WorkspaceAnalysisTestSourceKind::Silence:
    case WorkspaceAnalysisTestSourceKind::Impulse:
    case WorkspaceAnalysisTestSourceKind::Sine:
    case WorkspaceAnalysisTestSourceKind::FrozenAsset:
        return true;
    }
    return false;
}

bool isKnownWorkspaceInterfaceScale(
    WorkspaceInterfaceScale scale) noexcept
{
    return interfaceScalePercent(scale) != 0;
}

std::string workspaceSessionVariableDeclarationFingerprint(
    const ProjectVariableDeclaration& declaration)
{
    if (declaration.owner.kind
            != ProjectVariableOwnerKind::WorkspaceSession
        || !declaration.owner.definitionId.empty()
        || !declaration.owner.nodeId.empty()
        || !isValidProjectVariableIdentifier(declaration.variableId)
        || !isSupportedProjectVariableDomain(declaration.domain))
    {
        return {};
    }
    std::string transcript =
        "talas.workspace-variable-declaration/v1\n"
        "owner=workspaceSession\n"
        "projectBoundary=project\nvariableId=";
    transcript += std::to_string(declaration.variableId.size());
    transcript += ':';
    transcript += declaration.variableId;
    transcript += "\ndomain=";
    transcript += variableDomainToken(declaration.domain);
    transcript.push_back('\n');
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(transcript.data()),
        transcript.size());
    return "sha256:" + calculateProjectAssetSha256(bytes);
}

std::string workspaceSessionSoundVariableDeclarationFingerprint(
    const ProjectSoundVariableDeclaration& declaration)
{
    if (declaration.owner.kind
            != ProjectVariableOwnerKind::WorkspaceSession
        || !declaration.owner.definitionId.empty()
        || !declaration.owner.nodeId.empty()
        || !isValidProjectVariableIdentifier(declaration.variableId)
        || (declaration.kind != ProjectSoundVariableKind::Sound
            && declaration.kind
                != ProjectSoundVariableKind::SoundCollection)
        || declaration.candidateDefinitionIds.size()
            > maximumProjectSoundVariableCandidates
        || (declaration.candidateDefinitionIds.empty()
            ? declaration.initialSelectedOrdinal != 0
            : declaration.initialSelectedOrdinal
                >= declaration.candidateDefinitionIds.size()))
    {
        return {};
    }
    std::string transcript =
        "talas.workspace-sound-variable-declaration/v1\n"
        "owner=workspaceSession\n"
        "projectBoundary=project\nvariableId=";
    transcript += std::to_string(declaration.variableId.size());
    transcript += ':';
    transcript += declaration.variableId;
    transcript += "\nkind=";
    transcript += declaration.kind == ProjectSoundVariableKind::Sound
        ? "sound"
        : "soundCollection";
    transcript += "\ninitialSelectedOrdinal=";
    transcript += std::to_string(declaration.initialSelectedOrdinal);
    transcript += "\ncandidates=";
    transcript += std::to_string(declaration.candidateDefinitionIds.size());
    transcript.push_back('\n');
    for (const auto& definitionId : declaration.candidateDefinitionIds)
    {
        if (!isValidProjectVariableIdentifier(definitionId))
            return {};
        transcript += std::to_string(definitionId.size());
        transcript.push_back(':');
        transcript += definitionId;
        transcript.push_back('\n');
    }
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(transcript.data()),
        transcript.size());
    return "sha256:" + calculateProjectAssetSha256(bytes);
}

std::string workspaceSessionLiveToolDefinitionFingerprint(
    const ProjectLiveTool& tool)
{
    if (!isValidProjectLiveSetIdentifier(tool.toolId)
        || tool.states.size() > maximumProjectLiveToolStates
        || tool.variables.size() > maximumProjectLiveToolVariables
        || tool.transitions.size() > maximumProjectLiveToolTransitions)
    {
        return {};
    }
    std::string transcript = "talas.workspace-live-tool-definition/v1\n";
    const auto appendString = [&](std::string_view label,
                                  std::string_view value) {
        transcript += label;
        transcript.push_back('=');
        transcript += std::to_string(value.size());
        transcript.push_back(':');
        transcript.append(value);
        transcript.push_back('\n');
    };
    appendString("toolId", tool.toolId);
    appendString("displayName", tool.displayName);
    appendString("initialStateId", tool.initialStateId);
    transcript += "states=" + std::to_string(tool.states.size()) + "\n";
    for (const auto& state : tool.states)
    {
        appendString("stateId", state.stateId);
        appendString("stateName", state.displayName);
    }
    transcript += "variables=" + std::to_string(tool.variables.size()) + "\n";
    for (const auto& variable : tool.variables)
    {
        appendString("variableId", variable.variableId);
        appendString("variableName", variable.displayName);
        transcript += "variableKind="
            + std::to_string(static_cast<unsigned>(variable.kind)) + "\n";
        transcript += "initialNumber=";
        appendNumber(transcript, variable.initialNumber);
        transcript.push_back('\n');
        transcript += std::string{"initialBoolean="}
            + (variable.initialBoolean ? "1\n" : "0\n");
        appendString("stateScope", variable.stateId.value_or(""));
    }
    transcript += "transitions=" + std::to_string(tool.transitions.size()) + "\n";
    for (const auto& transition : tool.transitions)
    {
        appendString("transitionId", transition.transitionId);
        appendString("from", transition.fromStateId);
        appendString("to", transition.toStateId);
        transcript += "triggerKind="
            + std::to_string(static_cast<unsigned>(transition.trigger.kind))
            + "\n";
        appendString("triggerEvent", transition.trigger.eventName);
        transcript += "triggerDomain="
            + std::to_string(static_cast<unsigned>(
                transition.trigger.eventDomain)) + "\n";
        appendString("triggerVariable", transition.trigger.variableId);
        appendString("midiSource",
            transition.trigger.midiProgram.sourceIdentifier);
        transcript += "midiChannel="
            + std::to_string(transition.trigger.midiProgram.channel) + "\n";
        transcript += "midiProgram="
            + std::to_string(transition.trigger.midiProgram.program) + "\n";
        appendString("osc", transition.trigger.osc.address);
        transcript += "period="
            + std::to_string(transition.trigger.periodTicks) + "\n";
        appendString("guard", transition.guardExpression);
        transcript += "commands="
            + std::to_string(transition.commands.size()) + "\n";
        for (const auto& command : transition.commands)
        {
            transcript += "commandKind="
                + std::to_string(static_cast<unsigned>(command.kind)) + "\n";
            appendString("commandLiveSet", command.liveSetId);
            appendString("commandCell", command.cellId);
            appendString("commandVariable", command.variableId);
            appendString("commandEvent", command.eventName);
            transcript += "commandDomain="
                + std::to_string(static_cast<unsigned>(command.eventDomain))
                + "\ncommandValue=";
            appendNumber(transcript, command.value);
            transcript.push_back('\n');
        }
    }
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(transcript.data()),
        transcript.size());
    return "sha256:" + calculateProjectAssetSha256(bytes);
}

std::string serializeWorkspaceSessionState(const WorkspaceSessionState& source)
{
    auto state = source;
    state.schemaVersion = supportedWorkspaceSessionVersion;
    normalize(state);

    std::string output;
    output += "{\n";
    output += R"json(  "format": "talas.workspace-session",
  "version": )json";
    output += std::to_string(state.schemaVersion);
    output += R"json(,
  "viewMode": )json";
    switch (state.viewMode)
    {
    case WorkspaceViewMode::Structure:
        appendQuoted(output, "structure");
        break;
    case WorkspaceViewMode::Performance:
        appendQuoted(output, "performance");
        break;
    case WorkspaceViewMode::Samples:
        appendQuoted(output, "samples");
        break;
    }
    output += R"json(,
  "paneSplit": )json";
    appendNumber(output, state.paneSplit);
    output += R"json(,
  "structureSplitOrientation": )json";
    appendQuoted(output,
        splitOrientationToken(state.structureSplitOrientation));
    output += R"json(,
  "secondaryPaneVisible": )json";
    output += state.secondaryPaneVisible ? "true" : "false";
    output += R"json(,
  "primaryPaneFirst": )json";
    output += state.primaryPaneFirst ? "true" : "false";
    output += R"json(,
  "graphViewMode": )json";
    appendQuoted(output, graphViewModeToken(state.graphViewMode));
    output += R"json(,
  "activeGraph": )json";
    appendScope(output, state.activeSoundDefinitionId, 2);
    output += R"json(,
  "structureNavigationPath": [)json";
    for (std::size_t index = 0;
         index < state.structureNavigationPath.size();
         ++index)
    {
        if (index != 0)
            output += ", ";
        appendQuoted(output, state.structureNavigationPath[index]);
    }
    output += R"json(],
  "graphContexts": [
)json";

    for (std::size_t index = 0; index < state.graphContexts.size(); ++index)
    {
        const auto& context = state.graphContexts[index];
        output += "    {\n";
        output += R"json(      "scope": )json";
        appendScope(output, context.soundDefinitionId, 6);
        output += R"json(,
      "panX": )json";
        appendNumber(output, context.panX);
        output += R"json(,
      "panY": )json";
        appendNumber(output, context.panY);
        output += R"json(,
      "zoom": )json";
        appendNumber(output, context.zoom);
        output += R"json(,
      "selectedNodeId": )json";
        if (context.selectedNodeId.has_value())
            appendQuoted(output, *context.selectedNodeId);
        else
            output += "null";
        output += "\n    }";
        if (index + 1 < state.graphContexts.size())
            output.push_back(',');
        output.push_back('\n');
    }

    output += R"json(  ],
  "focusedPerformanceEventName": )json";
    if (state.focusedPerformanceEventName.has_value())
        appendQuoted(output, *state.focusedPerformanceEventName);
    else
        output += "null";
    output += R"json(,
  "performance": {
    "selectedControlId": )json";
    if (state.performance.selectedControlId.has_value())
        appendQuoted(output, *state.performance.selectedControlId);
    else
        output += "null";
    output += R"json(,
    "selectedMeterId": )json";
    if (state.performance.selectedMeterId.has_value())
        appendQuoted(output, *state.performance.selectedMeterId);
    else
        output += "null";
    output += R"json(,
    "selectedLiveSetId": )json";
    if (state.performance.selectedLiveSetId.has_value())
        appendQuoted(output, *state.performance.selectedLiveSetId);
    else
        output += "null";
    output += R"json(,
    "selectedLiveSetCellId": )json";
    if (state.performance.selectedLiveSetCellId.has_value())
        appendQuoted(output, *state.performance.selectedLiveSetCellId);
    else
        output += "null";
    output += R"json(,
    "activeLiveSetId": )json";
    if (state.performance.activeLiveSetId.has_value())
        appendQuoted(output, *state.performance.activeLiveSetId);
    else
        output += "null";
    output += R"json(,
    "activeLiveSetCellId": )json";
    if (state.performance.activeLiveSetCellId.has_value())
        appendQuoted(output, *state.performance.activeLiveSetCellId);
    else
        output += "null";
    output += R"json(,
    "queuedLiveSetId": )json";
    if (state.performance.queuedLiveSetId.has_value())
        appendQuoted(output, *state.performance.queuedLiveSetId);
    else
        output += "null";
    output += R"json(,
    "queuedLiveSetCellId": )json";
    if (state.performance.queuedLiveSetCellId.has_value())
        appendQuoted(output, *state.performance.queuedLiveSetCellId);
    else
        output += "null";
    output += R"json(,
    "queuedLiveSetRequestGeneration": )json";
    output += std::to_string(
        state.performance.queuedLiveSetRequestGeneration);
    output += R"json(,
    "selectedLiveToolId": )json";
    if (state.performance.selectedLiveToolId.has_value())
        appendQuoted(output, *state.performance.selectedLiveToolId);
    else
        output += "null";
    output += R"json(,
    "liveToolStates": [)json";
    for (std::size_t toolIndex = 0;
         toolIndex < state.performance.liveToolStates.size();
         ++toolIndex)
    {
        const auto& tool = state.performance.liveToolStates[toolIndex];
        if (toolIndex != 0)
            output += ",";
        output += R"json(
      {
        "toolId": )json";
        appendQuoted(output, tool.toolId);
        output += R"json(,
        "stateId": )json";
        appendQuoted(output, tool.stateId);
        output += R"json(,
        "definitionFingerprint": )json";
        appendQuoted(output, tool.definitionFingerprint);
        output += R"json(,
        "variables": [)json";
        for (std::size_t variableIndex = 0;
             variableIndex < tool.variables.size();
             ++variableIndex)
        {
            const auto& variable = tool.variables[variableIndex];
            if (variableIndex != 0)
                output += ",";
            output += R"json(
          {
            "variableId": )json";
            appendQuoted(output, variable.variableId);
            output += R"json(,
            "kind": )json";
            appendQuoted(
                output,
                variable.kind == ProjectLiveToolVariableKind::Boolean
                    ? "boolean"
                    : "number");
            output += R"json(,
            "number": )json";
            appendNumber(output, variable.number);
            output += R"json(,
            "boolean": )json";
            output += variable.boolean ? "true" : "false";
            output += "\n          }";
        }
        if (!tool.variables.empty())
            output.push_back('\n');
        output += "        ]\n      }";
    }
    if (!state.performance.liveToolStates.empty())
        output.push_back('\n');
    output += R"json(    ],
    "panX": )json";
    appendNumber(output, state.performance.panX);
    output += R"json(,
    "panY": )json";
    appendNumber(output, state.performance.panY);
    output += R"json(,
    "zoom": )json";
    appendNumber(output, state.performance.zoom);
    output += R"json(,
    "editMode": )json";
    output += state.performance.editMode ? "true" : "false";
    output += R"json(,
    "compact": )json";
    output += state.performance.compact ? "true" : "false";
    output += R"json(,
    "mode": )json";
    switch (state.performance.mode)
    {
    case WorkspacePerformanceSessionState::Mode::Controls:
        appendQuoted(output, "controls");
        break;
    case WorkspacePerformanceSessionState::Mode::Snapshots:
        appendQuoted(output, "snapshots");
        break;
    case WorkspacePerformanceSessionState::Mode::LiveGrid:
        appendQuoted(output, "liveGrid");
        break;
    }
    output += "\n  },\n";
    output += R"json(  "accessibility": {
    "interfaceScalePercent": )json";
    output += std::to_string(
        interfaceScalePercent(state.accessibility.interfaceScale));
    output += R"json(,
    "enhancedContrast": )json";
    output += state.accessibility.enhancedContrast ? "true" : "false";
    output += R"json(,
    "reduceMotion": )json";
    output += state.accessibility.reduceMotion ? "true" : "false";
    output += R"json(,
    "contextHelpVisible": )json";
    output += state.accessibility.contextHelpVisible ? "true" : "false";
    output += "\n  }";
    output += R"json(,
  "hierarchyBookmarks": [
)json";
    for (std::size_t index = 0; index < state.hierarchyBookmarks.size();
        ++index)
    {
        const auto& bookmark = state.hierarchyBookmarks[index];
        output += R"json(    {
      "definitionId": )json";
        appendQuoted(output, bookmark.definitionId);
        output += R"json(,
      "navigationPath": [)json";
        for (std::size_t pathIndex = 0;
            pathIndex < bookmark.navigationPath.size();
            ++pathIndex)
        {
            if (pathIndex != 0)
                output += ", ";
            appendQuoted(output, bookmark.navigationPath[pathIndex]);
        }
        output += R"json(],
      "activeGraph": )json";
        appendScope(output, bookmark.activeSoundDefinitionId, 6);
        output += R"json(,
      "paneSplit": )json";
        appendNumber(output, bookmark.paneSplit);
        output += R"json(,
      "structureSplitOrientation": )json";
        appendQuoted(output,
            splitOrientationToken(bookmark.structureSplitOrientation));
        output += R"json(,
      "secondaryPaneVisible": )json";
        output += bookmark.secondaryPaneVisible ? "true" : "false";
        output += R"json(,
      "primaryPaneFirst": )json";
        output += bookmark.primaryPaneFirst ? "true" : "false";
        output += R"json(,
      "graphViewMode": )json";
        appendQuoted(output, graphViewModeToken(bookmark.graphViewMode));
        output += "\n    }";
        if (index + 1 < state.hierarchyBookmarks.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ],\n";
    appendAnalysisSession(output, state.analysis);
    appendSoundLibraryState(output, state.soundLibrary);
    appendWorkspaceVariableValues(output, state.variableValues);
    appendWorkspaceSoundVariableValues(output, state.soundVariableValues);
    appendSamplesSession(output, state.samples);
    output += "}\n";
    return output;
}

WorkspaceSessionLoadResult parseWorkspaceSessionState(std::string_view source)
{
    WorkspaceSessionLoadResult result;
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    const std::array boundedStringMembers{
        json::ParseLimits::BoundedStringMember{
            "variableId",
            maximumProjectVariableIdentifierBytes,
            "variableValues"},
        json::ParseLimits::BoundedStringMember{
            "domain", 32U, "variableValues"},
        json::ParseLimits::BoundedStringMember{
            "declarationFingerprint",
            workspaceVariableDeclarationFingerprintBytes,
            "variableValues"},
        json::ParseLimits::BoundedStringMember{
            "variableId",
            maximumProjectVariableIdentifierBytes,
            "soundVariableValues"},
        json::ParseLimits::BoundedStringMember{
            "selectedDefinitionId",
            maximumProjectVariableIdentifierBytes,
            "soundVariableValues"},
        json::ParseLimits::BoundedStringMember{
            "declarationFingerprint",
            workspaceVariableDeclarationFingerprintBytes,
            "soundVariableValues"},
        json::ParseLimits::BoundedStringMember{
            "toolId", maximumProjectLiveSetIdentifierBytes,
            "liveToolStates"},
        json::ParseLimits::BoundedStringMember{
            "stateId", maximumProjectLiveSetIdentifierBytes,
            "liveToolStates"},
        json::ParseLimits::BoundedStringMember{
            "definitionFingerprint",
            workspaceVariableDeclarationFingerprintBytes,
            "liveToolStates"},
        json::ParseLimits::BoundedStringMember{
            "variableId", maximumProjectLiveSetIdentifierBytes,
            "variables"},
        json::ParseLimits::BoundedStringMember{
            "kind", 16U, "variables"},
    };
    const std::array boundedContainerMembers{
        json::ParseLimits::BoundedContainerMember{
            "variableValues", maximumWorkspaceVariableValues},
        json::ParseLimits::BoundedContainerMember{
            "soundVariableValues", maximumWorkspaceSoundVariableValues},
        json::ParseLimits::BoundedContainerMember{
            "liveToolStates", maximumWorkspaceLiveToolStates},
        json::ParseLimits::BoundedContainerMember{
            "variables", maximumProjectLiveToolVariables},
    };
    limits.boundedStringMembers = boundedStringMembers;
    limits.boundedContainerMembers = boundedContainerMembers;
    const auto parsed = json::parse(source, limits);
    if (!parsed.ok())
    {
        for (const auto& error : parsed.errors)
            addError(result, error.path, error.message);
        return result;
    }
    if (!parsed.value.isObject())
    {
        addError(result, "$", "Expected WorkspaceSession object");
        return result;
    }

    const auto& root = parsed.value;
    std::string format;
    const bool hasFormat = readRequiredString(
        result,
        root,
        "format",
        "$.format",
        format);
    if (hasFormat && format != workspaceSessionFormat)
        addError(result, "$.format", "Expected talas.workspace-session format");

    WorkspaceSessionState candidate;
    std::uint32_t sourceVersion = 0;
    const bool hasSupportedVersion =
        readSupportedVersion(result, root, sourceVersion);
    if (!hasFormat || format != workspaceSessionFormat || !hasSupportedVersion)
        return result;
    candidate.schemaVersion = supportedWorkspaceSessionVersion;

    if (sourceVersion == 1)
    {
        rejectUnknownMembers(
            result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "bookmarkedSoundDefinitions",
            });
    }
    else if (sourceVersion == 2)
    {
        rejectUnknownMembers(
            result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "bookmarkedSoundDefinitions",
                "samples",
            });
    }
    else if (sourceVersion == 3)
    {
        rejectUnknownMembers(
            result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "bookmarkedSoundDefinitions",
                "samples",
            });
    }
    else if (sourceVersion == 4)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "hierarchyBookmarks",
                "samples",
            });
    }
    else if (sourceVersion == 5)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "hierarchyBookmarks",
                "samples",
            });
    }
    else if (sourceVersion == 6)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "hierarchyBookmarks",
                "analysis",
                "samples",
            });
    }
    else if (sourceVersion == 7)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "accessibility",
                "hierarchyBookmarks",
                "analysis",
                "samples",
            });
    }
    else if (sourceVersion < 10)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "accessibility",
                "hierarchyBookmarks",
                "analysis",
                "soundLibrary",
                "samples",
            });
    }
    else if (sourceVersion == 10)
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "accessibility",
                "hierarchyBookmarks",
                "analysis",
                "soundLibrary",
                "variableValues",
                "samples",
            });
    }
    else
    {
        rejectUnknownMembers(result,
            root,
            "$",
            {
                "format",
                "version",
                "viewMode",
                "paneSplit",
                "structureSplitOrientation",
                "secondaryPaneVisible",
                "primaryPaneFirst",
                "graphViewMode",
                "activeGraph",
                "structureNavigationPath",
                "graphContexts",
                "focusedPerformanceEventName",
                "performance",
                "accessibility",
                "hierarchyBookmarks",
                "analysis",
                "soundLibrary",
                "variableValues",
                "soundVariableValues",
                "samples",
            });
    }

    std::string viewMode;
    if (readRequiredString(
            result,
            root,
            "viewMode",
            "$.viewMode",
            viewMode))
    {
        if (viewMode == "structure")
            candidate.viewMode = WorkspaceViewMode::Structure;
        else if (viewMode == "performance")
            candidate.viewMode = WorkspaceViewMode::Performance;
        else if (viewMode == "samples" && sourceVersion >= 2)
            candidate.viewMode = WorkspaceViewMode::Samples;
        else
            addError(
                result,
                "$.viewMode",
                sourceVersion >= 2
                    ? "Expected structure, performance, or samples"
                    : "Expected structure or performance");
    }

    if (readRequiredFiniteNumber(
            result,
            root,
            "paneSplit",
            "$.paneSplit",
            candidate.paneSplit)
        && !(candidate.paneSplit > 0.0 && candidate.paneSplit < 1.0))
    {
        addError(result, "$.paneSplit", "Pane split must be between zero and one");
    }

    if (sourceVersion >= 3)
    {
        std::string splitOrientation;
        if (readRequiredString(
                result,
                root,
                "structureSplitOrientation",
                "$.structureSplitOrientation",
                splitOrientation))
        {
            if (splitOrientation == "adaptive")
            {
                candidate.structureSplitOrientation =
                    WorkspaceStructureSplitOrientation::Adaptive;
            }
            else if (splitOrientation == "leftRight")
            {
                candidate.structureSplitOrientation =
                    WorkspaceStructureSplitOrientation::LeftRight;
            }
            else if (splitOrientation == "topBottom")
            {
                candidate.structureSplitOrientation =
                    WorkspaceStructureSplitOrientation::TopBottom;
            }
            else
            {
                addError(
                    result,
                    "$.structureSplitOrientation",
                    "Expected adaptive, leftRight, or topBottom");
            }
        }

        (void) readRequiredBoolean(
            result,
            root,
            "secondaryPaneVisible",
            "$.secondaryPaneVisible",
            candidate.secondaryPaneVisible);
        (void) readRequiredBoolean(
            result,
            root,
            "primaryPaneFirst",
            "$.primaryPaneFirst",
            candidate.primaryPaneFirst);

        std::string graphViewMode;
        if (readRequiredString(
                result,
                root,
                "graphViewMode",
                "$.graphViewMode",
                graphViewMode))
        {
            if (graphViewMode == "compact")
                candidate.graphViewMode = WorkspaceGraphViewMode::Compact;
            else if (graphViewMode == "ports")
                candidate.graphViewMode = WorkspaceGraphViewMode::Ports;
            else if (graphViewMode == "portsAndWires")
            {
                candidate.graphViewMode =
                    WorkspaceGraphViewMode::PortsAndWires;
            }
            else
            {
                addError(
                    result,
                    "$.graphViewMode",
                    "Expected compact, ports, or portsAndWires");
            }
        }
    }

    const auto* activeGraph = requiredMember(
        result,
        root,
        "activeGraph",
        "$.activeGraph");
    bool activeGraphValid = false;
    if (activeGraph != nullptr)
    {
        activeGraphValid = parseScope(
            result,
            *activeGraph,
            "$.activeGraph",
            candidate.activeSoundDefinitionId);
    }

    candidate.structureNavigationPath.clear();
    std::unordered_set<std::string> navigationDefinitionIds;
    const auto* navigationPath = requiredMember(
        result,
        root,
        "structureNavigationPath",
        "$.structureNavigationPath");
    if (navigationPath != nullptr)
    {
        if (!navigationPath->isArray())
        {
            addError(result, "$.structureNavigationPath", "Expected array");
        }
        else
        {
            if (navigationPath->array().size()
                > maximumWorkspaceNavigationDepth)
            {
                addError(
                    result,
                    "$.structureNavigationPath",
                    "Structure navigation path exceeds supported depth");
            }
            for (std::size_t index = 0;
                 index < navigationPath->array().size();
                 ++index)
            {
                const auto path = "$.structureNavigationPath["
                    + std::to_string(index) + "]";
                const auto& definitionId = navigationPath->array()[index];
                if (!definitionId.isString()
                    || definitionId.string().empty())
                {
                    addError(result, path, "Expected non-empty sound definition id");
                    continue;
                }
                if (!navigationDefinitionIds.insert(
                        definitionId.string()).second)
                {
                    addError(result, path, "Duplicate Structure navigation scope");
                    continue;
                }
                candidate.structureNavigationPath.push_back(
                    definitionId.string());
            }
        }
    }
    if (sourceVersion >= 3
        && activeGraphValid
        && !candidate.secondaryPaneVisible
        && candidate.activeSoundDefinitionId.has_value()
        && !candidate.structureNavigationPath.empty()
        && candidate.activeSoundDefinitionId
            == candidate.structureNavigationPath.back())
    {
        addError(
            result,
            "$.activeGraph",
            "Hidden secondary Structure pane cannot own the active edit scope");
    }

    candidate.graphContexts.clear();
    std::unordered_set<std::string> graphScopes;
    bool hasProjectContext = false;
    const auto* graphContexts = requiredMember(
        result,
        root,
        "graphContexts",
        "$.graphContexts");
    if (graphContexts != nullptr)
    {
        if (!graphContexts->isArray())
        {
            addError(result, "$.graphContexts", "Expected array");
        }
        else if (graphContexts->array().empty())
        {
            addError(result, "$.graphContexts", "Expected at least one graph context");
        }
        else if (graphContexts->array().size()
            > maximumWorkspaceGraphContexts)
        {
            addError(result, "$.graphContexts", "Too many graph contexts");
        }
        else
        {
            for (std::size_t index = 0;
                 index < graphContexts->array().size();
                 ++index)
            {
                const auto path =
                    "$.graphContexts[" + std::to_string(index) + "]";
                const auto& value = graphContexts->array()[index];
                if (!value.isObject())
                {
                    addError(result, path, "Expected graph context object");
                    continue;
                }
                rejectUnknownMembers(
                    result,
                    value,
                    path,
                    {"scope", "panX", "panY", "zoom", "selectedNodeId"});

                WorkspaceGraphSessionState context;
                bool contextValid = true;
                const auto* scope = requiredMember(
                    result,
                    value,
                    "scope",
                    path + ".scope");
                if (scope == nullptr
                    || !parseScope(
                        result,
                        *scope,
                        path + ".scope",
                        context.soundDefinitionId))
                {
                    contextValid = false;
                }

                contextValid = readRequiredFiniteNumber(
                    result,
                    value,
                    "panX",
                    path + ".panX",
                    context.panX) && contextValid;
                if (std::isfinite(context.panX)
                    && std::abs(context.panX) > maximumPanMagnitude)
                {
                    addError(result, path + ".panX", "Pan exceeds supported range");
                    contextValid = false;
                }
                contextValid = readRequiredFiniteNumber(
                    result,
                    value,
                    "panY",
                    path + ".panY",
                    context.panY) && contextValid;
                if (std::isfinite(context.panY)
                    && std::abs(context.panY) > maximumPanMagnitude)
                {
                    addError(result, path + ".panY", "Pan exceeds supported range");
                    contextValid = false;
                }
                contextValid = readRequiredFiniteNumber(
                    result,
                    value,
                    "zoom",
                    path + ".zoom",
                    context.zoom) && contextValid;
                if (!(context.zoom >= minimumZoom && context.zoom <= maximumZoom))
                {
                    addError(result, path + ".zoom", "Zoom exceeds supported range");
                    contextValid = false;
                }
                contextValid = readNullableNonEmptyString(
                    result,
                    value,
                    "selectedNodeId",
                    path + ".selectedNodeId",
                    context.selectedNodeId) && contextValid;

                if (scope != nullptr)
                {
                    const auto key = scopeKey(context.soundDefinitionId);
                    if (!graphScopes.insert(key).second)
                    {
                        addError(
                            result,
                            path + ".scope",
                            "Duplicate WorkspaceSession graph scope");
                        contextValid = false;
                    }
                    if (!context.soundDefinitionId.has_value())
                        hasProjectContext = true;
                }

                if (contextValid)
                    candidate.graphContexts.push_back(std::move(context));
            }
        }
    }

    if (!hasProjectContext)
        addError(result, "$.graphContexts", "Missing project graph context");
    if (activeGraphValid
        && !graphScopes.contains(scopeKey(candidate.activeSoundDefinitionId)))
    {
        addError(
            result,
            "$.activeGraph",
            "Active graph has no matching WorkspaceSession graph context");
    }
    for (std::size_t index = 0;
         index < candidate.structureNavigationPath.size();
         ++index)
    {
        if (!graphScopes.contains(
                scopeKey(candidate.structureNavigationPath[index])))
        {
            addError(
                result,
                "$.structureNavigationPath[" + std::to_string(index) + "]",
                "Structure navigation scope has no matching graph context");
        }
    }
    if (activeGraphValid)
    {
        if (!hierarchyActiveGraphIsVisible(candidate.structureNavigationPath,
                candidate.activeSoundDefinitionId,
                candidate.secondaryPaneVisible))
        {
            addError(
                result,
                "$.activeGraph",
                "Active graph is not visible in the Structure navigation path");
        }
    }

    (void) readNullableNonEmptyString(
        result,
        root,
        "focusedPerformanceEventName",
        "$.focusedPerformanceEventName",
        candidate.focusedPerformanceEventName);

    if (sourceVersion >= 5)
    {
        const auto* performance = requiredMember(
            result,
            root,
            "performance",
            "$.performance");
        if (performance != nullptr)
        {
            if (!performance->isObject())
            {
                addError(result, "$.performance", "Expected Performance session object");
            }
            else
            {
                if (sourceVersion >= 13)
                {
                    rejectUnknownMembers(result,
                        *performance,
                        "$.performance",
                        {
                            "selectedControlId",
                            "selectedMeterId",
                            "selectedLiveSetId",
                            "selectedLiveSetCellId",
                            "activeLiveSetId",
                            "activeLiveSetCellId",
                            "queuedLiveSetId",
                            "queuedLiveSetCellId",
                            "queuedLiveSetRequestGeneration",
                            "selectedLiveToolId",
                            "liveToolStates",
                            "panX",
                            "panY",
                            "zoom",
                            "editMode",
                            "compact",
                            "mode",
                        });
                }
                else if (sourceVersion >= 12)
                {
                    rejectUnknownMembers(result,
                        *performance,
                        "$.performance",
                        {
                            "selectedControlId",
                            "selectedMeterId",
                            "selectedLiveSetId",
                            "selectedLiveSetCellId",
                            "activeLiveSetId",
                            "activeLiveSetCellId",
                            "queuedLiveSetId",
                            "queuedLiveSetCellId",
                            "queuedLiveSetRequestGeneration",
                            "panX",
                            "panY",
                            "zoom",
                            "editMode",
                            "compact",
                            "mode",
                        });
                }
                else
                {
                    rejectUnknownMembers(result,
                        *performance,
                        "$.performance",
                        {
                            "selectedControlId",
                            "selectedMeterId",
                            "panX",
                            "panY",
                            "zoom",
                            "editMode",
                            "compact",
                        });
                }
                (void) readNullableBoundedString(result,
                    *performance,
                    "selectedControlId",
                    "$.performance.selectedControlId",
                    candidate.performance.selectedControlId,
                    maximumWorkspacePerformanceSelectionIdBytes);
                (void) readNullableBoundedString(result,
                    *performance,
                    "selectedMeterId",
                    "$.performance.selectedMeterId",
                    candidate.performance.selectedMeterId,
                    maximumWorkspacePerformanceSelectionIdBytes);
                if (candidate.performance.selectedControlId.has_value()
                    && candidate.performance.selectedMeterId.has_value())
                {
                    addError(result,
                        "$.performance",
                        "Performance selection must identify a control or a meter, not both");
                }
                if (sourceVersion >= 12)
                {
                    (void) readNullableBoundedString(result,
                        *performance,
                        "selectedLiveSetId",
                        "$.performance.selectedLiveSetId",
                        candidate.performance.selectedLiveSetId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readNullableBoundedString(result,
                        *performance,
                        "selectedLiveSetCellId",
                        "$.performance.selectedLiveSetCellId",
                        candidate.performance.selectedLiveSetCellId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readNullableBoundedString(result,
                        *performance,
                        "activeLiveSetId",
                        "$.performance.activeLiveSetId",
                        candidate.performance.activeLiveSetId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readNullableBoundedString(result,
                        *performance,
                        "activeLiveSetCellId",
                        "$.performance.activeLiveSetCellId",
                        candidate.performance.activeLiveSetCellId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readNullableBoundedString(result,
                        *performance,
                        "queuedLiveSetId",
                        "$.performance.queuedLiveSetId",
                        candidate.performance.queuedLiveSetId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readNullableBoundedString(result,
                        *performance,
                        "queuedLiveSetCellId",
                        "$.performance.queuedLiveSetCellId",
                        candidate.performance.queuedLiveSetCellId,
                        maximumWorkspacePerformanceSelectionIdBytes);
                    (void) readRequiredUnsigned64(result,
                        *performance,
                        "queuedLiveSetRequestGeneration",
                        "$.performance.queuedLiveSetRequestGeneration",
                        candidate.performance.queuedLiveSetRequestGeneration);
                    const auto* mode = requiredMember(result,
                        *performance,
                        "mode",
                        "$.performance.mode");
                    if (mode != nullptr && mode->isString())
                    {
                        if (mode->string() == "controls")
                            candidate.performance.mode =
                                WorkspacePerformanceSessionState::Mode::Controls;
                        else if (mode->string() == "snapshots")
                            candidate.performance.mode =
                                WorkspacePerformanceSessionState::Mode::Snapshots;
                        else if (mode->string() == "liveGrid")
                            candidate.performance.mode =
                                WorkspacePerformanceSessionState::Mode::LiveGrid;
                        else
                            addError(result,
                                "$.performance.mode",
                                "Expected controls, snapshots, or liveGrid");
                    }
                    else if (mode != nullptr)
                    {
                        addError(result,
                            "$.performance.mode",
                            "Expected Performance mode string");
                    }
                    const auto paired = [&](const auto& left,
                                            const auto& right,
                                            std::string_view label) {
                        if (left.has_value() != right.has_value())
                        {
                            addError(result,
                                "$.performance",
                                std::string{label}
                                    + " requires both live-set and cell identities");
                        }
                    };
                    paired(candidate.performance.selectedLiveSetId,
                        candidate.performance.selectedLiveSetCellId,
                        "Selected Live Grid identity");
                    paired(candidate.performance.activeLiveSetId,
                        candidate.performance.activeLiveSetCellId,
                        "Active Live Grid identity");
                    paired(candidate.performance.queuedLiveSetId,
                        candidate.performance.queuedLiveSetCellId,
                        "Queued Live Grid identity");
                    if (!candidate.performance.queuedLiveSetId.has_value()
                        && candidate.performance.queuedLiveSetRequestGeneration
                            != 0)
                    {
                        addError(result,
                            "$.performance.queuedLiveSetRequestGeneration",
                            "Queued generation requires a queued Live Grid identity");
                    }
                    if (sourceVersion >= 13)
                    {
                        (void) readNullableBoundedString(
                            result,
                            *performance,
                            "selectedLiveToolId",
                            "$.performance.selectedLiveToolId",
                            candidate.performance.selectedLiveToolId,
                            maximumWorkspacePerformanceSelectionIdBytes);
                        const auto* liveToolStates = requiredMember(
                            result,
                            *performance,
                            "liveToolStates",
                            "$.performance.liveToolStates");
                        if (liveToolStates != nullptr)
                        {
                            parseWorkspaceLiveToolStates(
                                result,
                                *liveToolStates,
                                candidate.performance.liveToolStates);
                        }
                    }
                }
                if (readRequiredFiniteNumber(result,
                        *performance,
                        "panX",
                        "$.performance.panX",
                        candidate.performance.panX)
                    && std::abs(candidate.performance.panX)
                        > maximumPanMagnitude)
                {
                    addError(result,
                        "$.performance.panX",
                        "Pan exceeds supported range");
                }
                if (readRequiredFiniteNumber(result,
                        *performance,
                        "panY",
                        "$.performance.panY",
                        candidate.performance.panY)
                    && std::abs(candidate.performance.panY)
                        > maximumPanMagnitude)
                {
                    addError(result,
                        "$.performance.panY",
                        "Pan exceeds supported range");
                }
                if (readRequiredFiniteNumber(result,
                        *performance,
                        "zoom",
                        "$.performance.zoom",
                        candidate.performance.zoom)
                    && !(candidate.performance.zoom >= minimumZoom
                        && candidate.performance.zoom <= maximumZoom))
                {
                    addError(result,
                        "$.performance.zoom",
                        "Zoom exceeds supported range");
                }
                (void) readRequiredBoolean(result,
                    *performance,
                    "editMode",
                    "$.performance.editMode",
                    candidate.performance.editMode);
                (void) readRequiredBoolean(result,
                    *performance,
                    "compact",
                    "$.performance.compact",
                    candidate.performance.compact);
            }
        }
    }

    if (sourceVersion >= 7)
    {
        const auto* accessibility = requiredMember(
            result,
            root,
            "accessibility",
            "$.accessibility");
        if (accessibility != nullptr)
        {
            if (!accessibility->isObject())
            {
                addError(
                    result,
                    "$.accessibility",
                    "Expected accessibility preferences object");
            }
            else
            {
                rejectUnknownMembers(
                    result,
                    *accessibility,
                    "$.accessibility",
                    {
                        "interfaceScalePercent",
                        "enhancedContrast",
                        "reduceMotion",
                        "contextHelpVisible",
                    });
                std::uint32_t scale = 0;
                if (readRequiredUnsignedInteger(
                        result,
                        *accessibility,
                        "interfaceScalePercent",
                        "$.accessibility.interfaceScalePercent",
                        scale))
                {
                    switch (scale)
                    {
                    case 100:
                        candidate.accessibility.interfaceScale =
                            WorkspaceInterfaceScale::Percent100;
                        break;
                    case 115:
                        candidate.accessibility.interfaceScale =
                            WorkspaceInterfaceScale::Percent115;
                        break;
                    case 130:
                        candidate.accessibility.interfaceScale =
                            WorkspaceInterfaceScale::Percent130;
                        break;
                    case 150:
                        candidate.accessibility.interfaceScale =
                            WorkspaceInterfaceScale::Percent150;
                        break;
                    default:
                        addError(
                            result,
                            "$.accessibility.interfaceScalePercent",
                            "Expected 100, 115, 130, or 150");
                        break;
                    }
                }
                (void)readRequiredBoolean(
                    result,
                    *accessibility,
                    "enhancedContrast",
                    "$.accessibility.enhancedContrast",
                    candidate.accessibility.enhancedContrast);
                (void)readRequiredBoolean(
                    result,
                    *accessibility,
                    "reduceMotion",
                    "$.accessibility.reduceMotion",
                    candidate.accessibility.reduceMotion);
                (void)readRequiredBoolean(
                    result,
                    *accessibility,
                    "contextHelpVisible",
                    "$.accessibility.contextHelpVisible",
                    candidate.accessibility.contextHelpVisible);
            }
        }
    }

    if (sourceVersion >= 6)
    {
        const auto* analysis = requiredMember(
            result,
            root,
            "analysis",
            "$.analysis");
        if (analysis != nullptr)
            parseAnalysisSession(result, *analysis, candidate.analysis);
    }

    if (sourceVersion >= 8)
    {
        const auto* soundLibrary = requiredMember(
            result,
            root,
            "soundLibrary",
            "$.soundLibrary");
        if (soundLibrary != nullptr)
            parseSoundLibraryState(
                result, *soundLibrary, candidate.soundLibrary, sourceVersion);
    }

    if (sourceVersion >= 10)
    {
        const auto* variableValues = requiredMember(
            result,
            root,
            "variableValues",
            "$.variableValues");
        if (variableValues != nullptr)
        {
            parseWorkspaceVariableValues(
                result,
                *variableValues,
                candidate.variableValues);
        }
    }

    if (sourceVersion >= 11)
    {
        const auto* soundVariableValues = requiredMember(
            result,
            root,
            "soundVariableValues",
            "$.soundVariableValues");
        if (soundVariableValues != nullptr)
        {
            parseWorkspaceSoundVariableValues(
                result,
                *soundVariableValues,
                candidate.soundVariableValues);
        }
    }

    candidate.hierarchyBookmarks.clear();
    const auto bookmarkMember =
        sourceVersion >= 4 ? std::string_view{"hierarchyBookmarks"}
                           : std::string_view{"bookmarkedSoundDefinitions"};
    const auto bookmarkPath = sourceVersion >= 4
                                  ? std::string{"$.hierarchyBookmarks"}
                                  : std::string{"$.bookmarkedSoundDefinitions"};
    const auto* bookmarks =
        requiredMember(result, root, bookmarkMember, bookmarkPath);
    if (bookmarks != nullptr)
    {
        if (!bookmarks->isArray())
        {
            addError(result, bookmarkPath, "Expected array");
        }
        else if (bookmarks->array().size() > maximumWorkspaceBookmarks)
        {
            addError(result, bookmarkPath, "Too many bookmarks");
        }
        else
        {
            std::unordered_set<std::string> bookmarkIds;
            for (std::size_t index = 0;
                 index < bookmarks->array().size();
                 ++index)
            {
                const auto path =
                    bookmarkPath + "[" + std::to_string(index) + "]";
                const auto& bookmark = bookmarks->array()[index];
                if (sourceVersion < 4)
                {
                    if (!bookmark.isString() || bookmark.string().empty())
                    {
                        addError(result,
                            path,
                            "Expected non-empty sound definition id");
                        continue;
                    }
                    if (!bookmarkIds.insert(bookmark.string()).second)
                    {
                        addError(result,
                            path,
                            "Duplicate sound definition bookmark");
                        continue;
                    }
                    candidate.hierarchyBookmarks.push_back({
                        bookmark.string(),
                        {bookmark.string()},
                        candidate.secondaryPaneVisible
                            ? std::optional<std::string>{bookmark.string()}
                            : std::optional<std::string>{},
                        candidate.paneSplit,
                        candidate.structureSplitOrientation,
                        candidate.secondaryPaneVisible,
                        candidate.primaryPaneFirst,
                        candidate.graphViewMode,
                    });
                    continue;
                }

                if (!bookmark.isObject())
                {
                    addError(result,
                        path,
                        "Expected hierarchy bookmark object");
                    continue;
                }
                rejectUnknownMembers(result,
                    bookmark,
                    path,
                    {
                        "definitionId",
                        "navigationPath",
                        "activeGraph",
                        "paneSplit",
                        "structureSplitOrientation",
                        "secondaryPaneVisible",
                        "primaryPaneFirst",
                        "graphViewMode",
                    });
                WorkspaceHierarchyBookmark parsedBookmark;
                if (!readRequiredString(result,
                        bookmark,
                        "definitionId",
                        path + ".definitionId",
                        parsedBookmark.definitionId) ||
                    parsedBookmark.definitionId.empty())
                {
                    addError(result,
                        path + ".definitionId",
                        "Expected non-empty sound definition id");
                    continue;
                }
                if (!bookmarkIds.insert(parsedBookmark.definitionId).second)
                {
                    addError(result,
                        path + ".definitionId",
                        "Duplicate hierarchy bookmark");
                    continue;
                }
                const auto* navigation = requiredMember(result,
                    bookmark,
                    "navigationPath",
                    path + ".navigationPath");
                if (navigation != nullptr)
                {
                    if (!navigation->isArray() || navigation->array().empty())
                    {
                        addError(result,
                            path + ".navigationPath",
                            "Expected non-empty array");
                    }
                    else if (navigation->array().size() >
                             maximumWorkspaceNavigationDepth)
                    {
                        addError(result,
                            path + ".navigationPath",
                            "Hierarchy bookmark path exceeds supported depth");
                    }
                    else
                    {
                        std::unordered_set<std::string> pathIds;
                        for (std::size_t pathIndex = 0;
                            pathIndex < navigation->array().size();
                            ++pathIndex)
                        {
                            const auto itemPath = path + ".navigationPath[" +
                                                  std::to_string(pathIndex) +
                                                  "]";
                            const auto& item = navigation->array()[pathIndex];
                            if (!item.isString() || item.string().empty())
                            {
                                addError(result,
                                    itemPath,
                                    "Expected non-empty sound definition id");
                                continue;
                            }
                            if (!pathIds.insert(item.string()).second)
                            {
                                addError(result,
                                    itemPath,
                                    "Duplicate hierarchy bookmark scope");
                                continue;
                            }
                            parsedBookmark.navigationPath.push_back(
                                item.string());
                        }
                    }
                }
                const auto* bookmarkActive = requiredMember(result,
                    bookmark,
                    "activeGraph",
                    path + ".activeGraph");
                if (bookmarkActive != nullptr)
                {
                    (void)parseScope(result,
                        *bookmarkActive,
                        path + ".activeGraph",
                        parsedBookmark.activeSoundDefinitionId);
                }
                if (readRequiredFiniteNumber(result,
                        bookmark,
                        "paneSplit",
                        path + ".paneSplit",
                        parsedBookmark.paneSplit) &&
                    !(parsedBookmark.paneSplit > 0.0 &&
                        parsedBookmark.paneSplit < 1.0))
                {
                    addError(result,
                        path + ".paneSplit",
                        "Pane split must be between zero and one");
                }
                std::string orientation;
                if (readRequiredString(result,
                        bookmark,
                        "structureSplitOrientation",
                        path + ".structureSplitOrientation",
                        orientation))
                {
                    if (orientation == "adaptive")
                        parsedBookmark.structureSplitOrientation =
                            WorkspaceStructureSplitOrientation::Adaptive;
                    else if (orientation == "leftRight")
                        parsedBookmark.structureSplitOrientation =
                            WorkspaceStructureSplitOrientation::LeftRight;
                    else if (orientation == "topBottom")
                        parsedBookmark.structureSplitOrientation =
                            WorkspaceStructureSplitOrientation::TopBottom;
                    else
                        addError(result,
                            path + ".structureSplitOrientation",
                            "Expected adaptive, leftRight, or topBottom");
                }
                (void)readRequiredBoolean(result,
                    bookmark,
                    "secondaryPaneVisible",
                    path + ".secondaryPaneVisible",
                    parsedBookmark.secondaryPaneVisible);
                (void)readRequiredBoolean(result,
                    bookmark,
                    "primaryPaneFirst",
                    path + ".primaryPaneFirst",
                    parsedBookmark.primaryPaneFirst);
                std::string viewModeToken;
                if (readRequiredString(result,
                        bookmark,
                        "graphViewMode",
                        path + ".graphViewMode",
                        viewModeToken))
                {
                    if (viewModeToken == "compact")
                        parsedBookmark.graphViewMode =
                            WorkspaceGraphViewMode::Compact;
                    else if (viewModeToken == "ports")
                        parsedBookmark.graphViewMode =
                            WorkspaceGraphViewMode::Ports;
                    else if (viewModeToken == "portsAndWires")
                        parsedBookmark.graphViewMode =
                            WorkspaceGraphViewMode::PortsAndWires;
                    else
                        addError(result,
                            path + ".graphViewMode",
                            "Expected compact, ports, or portsAndWires");
                }
                if (parsedBookmark.navigationPath.empty() ||
                    parsedBookmark.navigationPath.back() !=
                        parsedBookmark.definitionId)
                {
                    addError(result,
                        path + ".navigationPath",
                        "Hierarchy bookmark path must end at its definition");
                }
                if (!hierarchyActiveGraphIsVisible(
                        parsedBookmark.navigationPath,
                        parsedBookmark.activeSoundDefinitionId,
                        parsedBookmark.secondaryPaneVisible))
                {
                    addError(result,
                        path + ".activeGraph",
                        "Hierarchy bookmark active graph must be visible");
                }
                candidate.hierarchyBookmarks.push_back(
                    std::move(parsedBookmark));
            }
        }
    }

    if (sourceVersion >= 2)
    {
        const auto* samples = requiredMember(
            result,
            root,
            "samples",
            "$.samples");
        if (samples != nullptr)
            parseSamplesSession(result, *samples, candidate.samples);
    }

    if (result.ok())
    {
        normalize(candidate);
        result.state = std::move(candidate);
    }
    return result;
}

const WorkspaceGraphSessionState* findWorkspaceGraphSessionState(
    const WorkspaceSessionState& state,
    std::optional<std::string_view> soundDefinitionId)
{
    const auto found = std::ranges::find_if(
        state.graphContexts,
        [&](const auto& context) {
            if (context.soundDefinitionId.has_value()
                != soundDefinitionId.has_value())
            {
                return false;
            }
            return !soundDefinitionId.has_value()
                || *context.soundDefinitionId == *soundDefinitionId;
        });
    return found == state.graphContexts.end()
        ? nullptr
        : &*found;
}

WorkspaceGraphSessionState* findWorkspaceGraphSessionState(
    WorkspaceSessionState& state,
    std::optional<std::string_view> soundDefinitionId)
{
    return const_cast<WorkspaceGraphSessionState*>(
        findWorkspaceGraphSessionState(
            std::as_const(state),
            soundDefinitionId));
}

WorkspaceGraphSessionState& ensureWorkspaceGraphSessionState(
    WorkspaceSessionState& state,
    std::optional<std::string> soundDefinitionId)
{
    const auto identity = soundDefinitionId;
    const auto identityView = identity.has_value()
        ? std::optional<std::string_view>{*identity}
        : std::nullopt;
    if (auto* existing = findWorkspaceGraphSessionState(state, identityView))
        return *existing;

    if (state.graphContexts.size() >= maximumWorkspaceGraphContexts)
    {
        trimWorkspaceGraphContextCache(state);
        auto eviction = std::ranges::find_if(
            state.graphContexts,
            [&](const auto& context) {
                return !isVisibleOrActiveGraphContext(state, context);
            });
        if (eviction != state.graphContexts.end())
            state.graphContexts.erase(eviction);
    }

    state.graphContexts.push_back({
        std::move(soundDefinitionId),
        0.0,
        0.0,
        1.0,
        {},
    });
    normalize(state);
    return *findWorkspaceGraphSessionState(state, identityView);
}

bool replaceWorkspaceSessionHierarchyBookmarks(WorkspaceSessionState& state,
    std::vector<WorkspaceHierarchyBookmark> bookmarks)
{
    const auto originalSize = bookmarks.size();
    std::erase_if(bookmarks,
        [](const auto& bookmark)
        { return !hierarchyBookmarkIsStructurallyValid(bookmark); });
    std::ranges::sort(bookmarks, {}, &WorkspaceHierarchyBookmark::definitionId);
    bookmarks.erase(std::unique(bookmarks.begin(),
                        bookmarks.end(),
                        [](const auto& left, const auto& right)
                        { return left.definitionId == right.definitionId; }),
        bookmarks.end());
    const auto complete = originalSize == bookmarks.size() &&
                          bookmarks.size() <= maximumWorkspaceBookmarks;
    if (bookmarks.size() > maximumWorkspaceBookmarks)
        bookmarks.resize(maximumWorkspaceBookmarks);
    state.hierarchyBookmarks = std::move(bookmarks);
    return complete;
}

std::vector<std::string> workspaceSessionBookmarkedDefinitionIds(
    const WorkspaceSessionState& state)
{
    std::vector<std::string> result;
    result.reserve(state.hierarchyBookmarks.size());
    for (const auto& bookmark : state.hierarchyBookmarks)
        result.push_back(bookmark.definitionId);
    return result;
}

bool captureWorkspaceSessionHierarchyBookmark(WorkspaceSessionState& state,
    std::string definitionId)
{
    if (definitionId.empty())
        return false;
    auto path = state.structureNavigationPath;
    const auto visible = std::ranges::find(path, definitionId);
    if (visible == path.end())
        path = {definitionId};
    else
        path.erase(visible + 1, path.end());

    WorkspaceHierarchyBookmark bookmark{
        definitionId,
        std::move(path),
        state.activeSoundDefinitionId,
        state.paneSplit,
        state.structureSplitOrientation,
        state.secondaryPaneVisible,
        state.primaryPaneFirst,
        state.graphViewMode,
    };
    if (!hierarchyActiveGraphIsVisible(bookmark.navigationPath,
            bookmark.activeSoundDefinitionId,
            bookmark.secondaryPaneVisible))
    {
        bookmark.activeSoundDefinitionId = definitionId;
    }
    if (!bookmark.secondaryPaneVisible &&
        bookmark.activeSoundDefinitionId ==
            std::optional<std::string>{definitionId})
    {
        bookmark.activeSoundDefinitionId =
            bookmark.navigationPath.size() > 1
                ? std::optional<std::string>{bookmark
                          .navigationPath[bookmark.navigationPath.size() - 2]}
                : std::optional<std::string>{};
    }

    const auto existing = std::ranges::find(state.hierarchyBookmarks,
        definitionId,
        &WorkspaceHierarchyBookmark::definitionId);
    if (existing == state.hierarchyBookmarks.end())
        state.hierarchyBookmarks.push_back(std::move(bookmark));
    else
        *existing = std::move(bookmark);
    return replaceWorkspaceSessionHierarchyBookmarks(state,
        std::move(state.hierarchyBookmarks));
}

bool removeWorkspaceSessionHierarchyBookmark(WorkspaceSessionState& state,
    std::string_view definitionId)
{
    const auto originalSize = state.hierarchyBookmarks.size();
    std::erase_if(state.hierarchyBookmarks,
        [&](const auto& bookmark)
        { return bookmark.definitionId == definitionId; });
    return state.hierarchyBookmarks.size() != originalSize;
}

bool restoreWorkspaceSessionHierarchyBookmark(WorkspaceSessionState& state,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(state.hierarchyBookmarks,
        definitionId,
        &WorkspaceHierarchyBookmark::definitionId);
    if (found == state.hierarchyBookmarks.end() ||
        !hierarchyBookmarkIsStructurallyValid(*found))
    {
        return false;
    }
    state.viewMode = WorkspaceViewMode::Structure;
    state.structureNavigationPath = found->navigationPath;
    state.activeSoundDefinitionId = found->activeSoundDefinitionId;
    state.paneSplit = found->paneSplit;
    state.structureSplitOrientation = found->structureSplitOrientation;
    state.secondaryPaneVisible = found->secondaryPaneVisible;
    state.primaryPaneFirst = found->primaryPaneFirst;
    state.graphViewMode = found->graphViewMode;
    return true;
}

void reconcileWorkspaceSoundLibraryState(
    WorkspaceSoundLibraryState& state,
    std::span<const std::string> validEntryIds)
{
    const auto entryExists = [&](std::string_view entryId) {
        return std::ranges::find(validEntryIds, entryId)
            != validEntryIds.end();
    };
    if (!isKnownSoundLibraryCollection(state.collection)
        || !isKnownSoundLibrarySourceFilter(state.sourceFilter)
        || !isKnownSoundLibraryCompatibilityFilter(
            state.compatibilityFilter)
        || !isKnownSoundLibraryHealthFilter(state.healthFilter))
    {
        state = {};
        return;
    }
    if (state.selectedEntryId.has_value()
        && !entryExists(*state.selectedEntryId))
    {
        state.selectedEntryId.reset();
    }
    const auto pruneEntryIds = [&](auto& ids, std::size_t maximumCount) {
        std::unordered_set<std::string> retained;
        std::erase_if(ids, [&](const auto& id) {
            return id.empty()
                || id.size() > maximumWorkspaceSoundLibraryEntryIdBytes
                || !entryExists(id)
                || !retained.insert(id).second;
        });
        if (ids.size() > maximumCount)
            ids.resize(maximumCount);
    };
    pruneEntryIds(
        state.favoriteEntryIds,
        maximumWorkspaceSoundLibraryFavorites);
    pruneEntryIds(state.recentEntryIds, maximumWorkspaceSoundLibraryRecents);
    if (state.category.has_value()
        && (state.category->empty()
            || state.category->size()
                > maximumWorkspaceSoundLibraryCategoryBytes))
    {
        state.category.reset();
    }
    if (state.origin.has_value()
        && (state.origin->empty()
            || state.origin->size()
                > maximumWorkspaceSoundLibraryCategoryBytes))
    {
        state.origin.reset();
    }
    std::unordered_set<std::string> retainedPorts;
    std::erase_if(state.inputSubstitutions, [&](const auto& substitution) {
        if (retainedPorts.size()
            >= maximumWorkspaceSoundLibraryInputSubstitutions)
        {
            return true;
        }
        const auto source = substitution.source;
        const auto valueValid = std::isfinite(substitution.value)
            && (((source == WorkspaceSoundLibraryAuditionSource::Default
                     || source == WorkspaceSoundLibraryAuditionSource::Silence
                     || source == WorkspaceSoundLibraryAuditionSource::FrozenAsset)
                    && substitution.value == 0.0)
                || (source == WorkspaceSoundLibraryAuditionSource::Sine
                    && substitution.value
                        >= minimumWorkspaceSoundLibrarySineFrequencyHz
                    && substitution.value
                        <= maximumWorkspaceSoundLibrarySineFrequencyHz)
                || (source == WorkspaceSoundLibraryAuditionSource::Constant
                    && std::abs(substitution.value)
                        <= maximumWorkspaceSoundLibraryConstantMagnitude));
        return !isKnownSoundLibraryAuditionSource(source)
            || substitution.portId.empty()
            || substitution.portId.size()
                > maximumWorkspaceSoundLibraryPortIdBytes
            || !retainedPorts.insert(substitution.portId).second
            || !valueValid
            || ((source == WorkspaceSoundLibraryAuditionSource::FrozenAsset)
                != substitution.frozenAssetId.has_value())
            || (substitution.frozenAssetId.has_value()
                && (substitution.frozenAssetId->empty()
                    || substitution.frozenAssetId->size()
                        > maximumWorkspaceSoundLibraryAssetIdBytes));
    });
}

void reconcileWorkspaceSessionState(
    WorkspaceSessionState& state,
    const ProjectDocument& document)
{
    if (!isKnownWorkspaceInterfaceScale(
            state.accessibility.interfaceScale))
    {
        state.accessibility = {};
    }
    const auto definitionExists = [&](std::string_view definitionId) {
        return std::ranges::any_of(
            document.soundDefinitions,
            [&](const auto& definition) {
                return definition.definitionId == definitionId;
            });
    };
    const auto definitionForId =
        [&](std::string_view definitionId) -> const ProjectSoundDefinition*
    {
        const auto definition = std::ranges::find_if(document.soundDefinitions,
            [&](const auto& candidate)
            { return candidate.definitionId == definitionId; });
        return definition == document.soundDefinitions.end() ? nullptr
                                                             : &*definition;
    };
    const auto isDirectHierarchyChild =
        [&](std::string_view parentId, std::string_view childId)
    {
        const auto* parent = definitionForId(parentId);
        return parent != nullptr &&
               std::ranges::any_of(parent->graph.nodes,
                   [&](const auto& node)
                   {
                       return node.soundDefinitionId ==
                              std::optional<std::string>{childId};
                   });
    };
    const auto firstInvalidHierarchyEdge = [&](const auto& path)
    {
        if (path.size() < 2)
            return path.end();
        for (auto current = std::next(path.begin()); current != path.end();
            ++current)
        {
            if (!isDirectHierarchyChild(*std::prev(current), *current))
                return current;
        }
        return path.end();
    };
    const auto graphForScope = [&](const auto& context) -> const ProjectGraph*
    {
        if (!context.soundDefinitionId.has_value())
            return &document.graph;
        const auto definition = std::ranges::find_if(
            document.soundDefinitions,
            [&](const auto& candidate) {
                return candidate.definitionId
                    == *context.soundDefinitionId;
            });
        return definition == document.soundDefinitions.end()
            ? nullptr
            : &definition->graph;
    };

    std::erase_if(
        state.graphContexts,
        [&](const auto& context) {
            return context.soundDefinitionId.has_value()
                && !definitionExists(*context.soundDefinitionId);
        });
    const auto firstStaleNavigationScope = std::ranges::find_if(
        state.structureNavigationPath,
        [&](const auto& definitionId) {
            return !definitionExists(definitionId);
        });
    state.structureNavigationPath.erase(
        firstStaleNavigationScope,
        state.structureNavigationPath.end());
    state.structureNavigationPath.erase(
        firstInvalidHierarchyEdge(state.structureNavigationPath),
        state.structureNavigationPath.end());
    trimWorkspaceGraphContextCache(state);
    for (auto& context : state.graphContexts)
    {
        const auto* graph = graphForScope(context);
        if (graph == nullptr || !context.selectedNodeId.has_value())
            continue;
        const auto selectedExists = std::ranges::any_of(
            graph->nodes,
            [&](const auto& node) {
                return node.instanceId == *context.selectedNodeId;
            });
        if (!selectedExists)
            context.selectedNodeId.reset();
    }

    normalize(state);
    state.graphContexts.erase(
        std::unique(
            state.graphContexts.begin(),
            state.graphContexts.end(),
            [](const auto& left, const auto& right) {
                return left.soundDefinitionId
                    == right.soundDefinitionId;
            }),
        state.graphContexts.end());
    (void) ensureWorkspaceGraphSessionState(state, std::nullopt);

    if (state.activeSoundDefinitionId.has_value()
        && !definitionExists(*state.activeSoundDefinitionId))
    {
        state.activeSoundDefinitionId.reset();
    }
    if (!hierarchyActiveGraphIsVisible(state.structureNavigationPath,
            state.activeSoundDefinitionId,
            state.secondaryPaneVisible))
    {
        if (state.structureNavigationPath.empty() ||
            (!state.secondaryPaneVisible &&
                state.structureNavigationPath.size() == 1))
        {
            state.activeSoundDefinitionId.reset();
        }
        else if (!state.secondaryPaneVisible)
        {
            state.activeSoundDefinitionId =
                state.structureNavigationPath
                    [state.structureNavigationPath.size() - 2];
        }
        else
        {
            state.activeSoundDefinitionId =
                state.structureNavigationPath.back();
        }
    }
    (void) ensureWorkspaceGraphSessionState(
        state,
        state.activeSoundDefinitionId);
    for (const auto& definitionId : state.structureNavigationPath)
    {
        (void) ensureWorkspaceGraphSessionState(
            state,
            definitionId);
    }

    std::erase_if(state.hierarchyBookmarks,
        [&](auto& bookmark)
        {
            if (!definitionExists(bookmark.definitionId))
                return true;
            const auto firstStale =
                std::ranges::find_if(bookmark.navigationPath,
                    [&](const auto& definitionId)
                    { return !definitionExists(definitionId); });
            bookmark.navigationPath.erase(firstStale,
                bookmark.navigationPath.end());
            const auto firstInvalidEdge =
                firstInvalidHierarchyEdge(bookmark.navigationPath);
            if (firstInvalidEdge != bookmark.navigationPath.end())
                bookmark.navigationPath.erase(firstInvalidEdge,
                    bookmark.navigationPath.end());
            if (bookmark.navigationPath.empty() ||
                bookmark.navigationPath.back() != bookmark.definitionId)
            {
                bookmark.navigationPath = {bookmark.definitionId};
            }
            if (!hierarchyActiveGraphIsVisible(bookmark.navigationPath,
                    bookmark.activeSoundDefinitionId,
                    bookmark.secondaryPaneVisible))
            {
                bookmark.activeSoundDefinitionId = bookmark.definitionId;
            }
            if (!bookmark.secondaryPaneVisible &&
                bookmark.activeSoundDefinitionId ==
                    std::optional<std::string>{bookmark.definitionId})
            {
                bookmark.activeSoundDefinitionId =
                    bookmark.navigationPath.size() > 1
                        ? std::optional<std::string>{bookmark.navigationPath
                                  [bookmark.navigationPath.size() - 2]}
                        : std::optional<std::string>{};
            }
            return false;
        });
    (void)replaceWorkspaceSessionHierarchyBookmarks(state,
        std::move(state.hierarchyBookmarks));

    if (state.focusedPerformanceEventName.has_value())
    {
        const auto eventExists = std::ranges::any_of(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName
                    == *state.focusedPerformanceEventName;
            });
        if (!eventExists)
            state.focusedPerformanceEventName.reset();
    }
    if (state.performance.selectedControlId.has_value()
        && findProjectPerformanceControlPresentationById(
               document,
               *state.performance.selectedControlId)
            == nullptr)
    {
        state.performance.selectedControlId.reset();
    }
    if (state.performance.selectedMeterId.has_value()
        && std::ranges::none_of(document.presentation.performanceMeters,
            [&](const auto& meter) {
                return meter.meterId
                    == *state.performance.selectedMeterId;
            }))
    {
        state.performance.selectedMeterId.reset();
    }
    if (state.performance.selectedControlId.has_value())
        state.performance.selectedMeterId.reset();

    const auto liveSetCellExists = [&](const auto& liveSetId,
                                       const auto& cellId) {
        if (!liveSetId.has_value() || !cellId.has_value())
            return false;
        const auto liveSet = std::ranges::find(
            document.liveSetLibrary.liveSets,
            *liveSetId,
            &ProjectLiveSet::liveSetId);
        return liveSet != document.liveSetLibrary.liveSets.end()
            && std::ranges::any_of(
                liveSet->cells,
                [&](const auto& cell) { return cell.cellId == *cellId; });
    };
    const auto clearLiveSetIdentity = [](auto& liveSetId, auto& cellId) {
        liveSetId.reset();
        cellId.reset();
    };
    if (!liveSetCellExists(
            state.performance.selectedLiveSetId,
            state.performance.selectedLiveSetCellId))
    {
        clearLiveSetIdentity(
            state.performance.selectedLiveSetId,
            state.performance.selectedLiveSetCellId);
    }
    if (!liveSetCellExists(
            state.performance.activeLiveSetId,
            state.performance.activeLiveSetCellId))
    {
        clearLiveSetIdentity(
            state.performance.activeLiveSetId,
            state.performance.activeLiveSetCellId);
    }
    if (!liveSetCellExists(
            state.performance.queuedLiveSetId,
            state.performance.queuedLiveSetCellId))
    {
        clearLiveSetIdentity(
            state.performance.queuedLiveSetId,
            state.performance.queuedLiveSetCellId);
        state.performance.queuedLiveSetRequestGeneration = 0;
    }

    const auto findLiveTool = [&](std::string_view toolId)
        -> const ProjectLiveTool* {
        const auto found = std::ranges::find(
            document.liveSetLibrary.tools,
            toolId,
            &ProjectLiveTool::toolId);
        return found == document.liveSetLibrary.tools.end()
            ? nullptr
            : std::addressof(*found);
    };
    if (state.performance.selectedLiveToolId.has_value()
        && findLiveTool(*state.performance.selectedLiveToolId) == nullptr)
    {
        state.performance.selectedLiveToolId.reset();
    }
    std::erase_if(
        state.performance.liveToolStates,
        [&](const WorkspacePerformanceSessionState::LiveToolState& saved) {
            const auto* tool = findLiveTool(saved.toolId);
            if (tool == nullptr
                || workspaceSessionLiveToolDefinitionFingerprint(*tool)
                    != saved.definitionFingerprint
                || std::ranges::none_of(
                    tool->states,
                    [&](const auto& candidate) {
                        return candidate.stateId == saved.stateId;
                    })
                || saved.variables.size() != tool->variables.size())
            {
                return true;
            }
            return std::ranges::any_of(
                tool->variables,
                [&](const auto& declaration) {
                    const auto value = std::ranges::find(
                        saved.variables,
                        declaration.variableId,
                        &WorkspacePerformanceSessionState::LiveToolVariable::variableId);
                    return value == saved.variables.end()
                        || value->kind != declaration.kind
                        || !std::isfinite(value->number)
                        || (value->kind
                                == ProjectLiveToolVariableKind::Boolean
                            ? value->number != 0.0
                            : value->boolean);
                });
        });

    const auto analysisTargetExists = [&](const WorkspaceAnalysisTraceState& trace) {
        const ProjectGraph* graph = &document.graph;
        std::optional<std::string_view> resolvedDefinition;
        for (const auto& instanceId : trace.instancePath)
        {
            const auto instance = std::ranges::find(
                graph->nodes,
                instanceId,
                &ProjectNode::instanceId);
            if (instance == graph->nodes.end()
                || !instance->soundDefinitionId.has_value())
            {
                return false;
            }
            const auto* definition = definitionForId(*instance->soundDefinitionId);
            if (definition == nullptr)
                return false;
            resolvedDefinition = definition->definitionId;
            graph = &definition->graph;
        }
        if (trace.instancePath.empty())
        {
            if (trace.containingDefinitionId.has_value())
                return false;
        }
        else if (!trace.containingDefinitionId.has_value()
                 || resolvedDefinition
                    != std::optional<std::string_view>{
                        *trace.containingDefinitionId})
        {
            return false;
        }
        return std::ranges::any_of(graph->nodes, [&](const auto& node) {
            return node.instanceId == trace.nodeId;
        });
    };
    const auto analysisTargetKey = [](const WorkspaceAnalysisTraceState& trace) {
        std::string key;
        for (const auto& instanceId : trace.instancePath)
            key += "i:" + instanceId + "\n";
        key += "d:" + trace.containingDefinitionId.value_or("")
            + "\nn:" + trace.nodeId + "\np:"
            + trace.portId.value_or("") + "\nk:"
            + std::string{analysisTraceKindToken(trace.kind)};
        return key;
    };
    std::unordered_set<std::string> analysisTraceIds;
    std::unordered_set<std::string> analysisTargets;
    std::erase_if(state.analysis.traces, [&](const auto& trace) {
        if (analysisTraceIds.size() >= maximumWorkspaceAnalysisTraces
            || trace.traceId.empty()
            || trace.traceId.size() > maximumWorkspaceAnalysisTraceIdBytes
            || trace.nodeId.empty()
            || trace.nodeId.size() > maximumWorkspaceAnalysisTargetIdBytes
            || trace.instancePath.size() > maximumWorkspaceAnalysisInstanceDepth
            || trace.colourSlot > maximumWorkspaceAnalysisColourSlot
            || !std::isfinite(trace.verticalScale)
            || trace.verticalScale < minimumAnalysisVerticalScale
            || trace.verticalScale > maximumAnalysisVerticalScale
            || !analysisTargetExists(trace))
        {
            return true;
        }
        if (!analysisTraceIds.insert(trace.traceId).second)
            return true;
        return !analysisTargets.insert(analysisTargetKey(trace)).second;
    });
    if (state.analysis.selectedTraceId.has_value()
        && !analysisTraceIds.contains(*state.analysis.selectedTraceId))
    {
        state.analysis.selectedTraceId.reset();
    }
    if (state.analysis.audition.has_value())
    {
        const auto& audition = *state.analysis.audition;
        const auto frozenAssetExists = !audition.frozenAssetId.has_value()
            || std::ranges::any_of(document.assets, [&](const auto& asset) {
                   return asset.id == *audition.frozenAssetId
                       && asset.kind == ProjectAssetKind::Audio;
               });
        const auto usesTemporarySource = audition.source
            != WorkspaceAnalysisTestSourceKind::None;
        if (!isKnownWorkspaceAnalysisTestSourceKind(audition.source)
            || !analysisTraceIds.contains(audition.traceId)
            || !std::isfinite(audition.sineFrequencyHz)
            || audition.sineFrequencyHz
                < minimumWorkspaceAnalysisSineFrequencyHz
            || audition.sineFrequencyHz
                > maximumWorkspaceAnalysisSineFrequencyHz
            || usesTemporarySource
                != audition.destinationInputPortId.has_value()
            || (audition.source
                    == WorkspaceAnalysisTestSourceKind::FrozenAsset)
                != audition.frozenAssetId.has_value()
            || !frozenAssetExists)
        {
            state.analysis.audition.reset();
        }
    }
    state.analysis.refreshRateHz = std::clamp(
        state.analysis.refreshRateHz,
        minimumWorkspaceAnalysisRefreshRateHz,
        maximumWorkspaceAnalysisRefreshRateHz);
    state.analysis.historyFrames = std::clamp(
        state.analysis.historyFrames,
        minimumWorkspaceAnalysisHistoryFrames,
        maximumWorkspaceAnalysisHistoryFrames);

    const auto findMap = [&](std::string_view mapId)
        -> const ProjectSampleMap* {
        const auto found = std::ranges::find(
            document.sampleMapLibrary.maps,
            mapId,
            &ProjectSampleMap::mapId);
        return found == document.sampleMapLibrary.maps.end()
            ? nullptr
            : &*found;
    };
    const auto* activeMap = state.samples.activeMapId.has_value()
        ? findMap(*state.samples.activeMapId)
        : nullptr;
    const auto activeMapChanged = state.samples.activeMapId.has_value()
        && activeMap == nullptr;
    if (activeMap == nullptr)
    {
        if (document.sampleMapLibrary.maps.empty())
        {
            state.samples.activeMapId.reset();
        }
        else
        {
            activeMap = &document.sampleMapLibrary.maps.front();
            state.samples.activeMapId = activeMap->mapId;
        }
    }
    if (activeMapChanged)
        state.samples.selectedRegionIds.clear();
    if (activeMap != nullptr)
    {
        std::unordered_set<std::string> selectedRegionIds;
        std::erase_if(
            state.samples.selectedRegionIds,
            [&](const auto& regionId) {
                return !selectedRegionIds.insert(regionId).second
                    || findProjectSampleRegion(*activeMap, regionId) == nullptr;
            });
    }
    else
    {
        state.samples.selectedRegionIds.clear();
    }

    std::unordered_map<std::string, std::size_t> variableValueCounts;
    variableValueCounts.reserve(state.variableValues.size());
    for (const auto& value : state.variableValues)
        ++variableValueCounts[value.variableId];
    std::erase_if(state.variableValues, [&](const auto& value) {
        if (variableValueCounts[value.variableId] != 1
            || !std::isfinite(value.value)
            || !isCanonicalWorkspaceVariableFingerprint(
                value.declarationFingerprint))
        {
            return true;
        }
        const auto declaration = std::ranges::find_if(
            document.variableLibrary.variables,
            [&](const auto& candidate) {
                return candidate.owner.kind
                        == ProjectVariableOwnerKind::WorkspaceSession
                    && candidate.variableId == value.variableId;
            });
        return declaration == document.variableLibrary.variables.end()
            || declaration->domain != value.domain
            || workspaceSessionVariableDeclarationFingerprint(*declaration)
                != value.declarationFingerprint;
    });

    std::unordered_map<std::string, std::size_t> soundVariableValueCounts;
    soundVariableValueCounts.reserve(state.soundVariableValues.size());
    for (const auto& value : state.soundVariableValues)
        ++soundVariableValueCounts[value.variableId];
    std::erase_if(state.soundVariableValues, [&](const auto& value) {
        if (soundVariableValueCounts[value.variableId] != 1
            || !isCanonicalWorkspaceVariableFingerprint(
                value.declarationFingerprint))
        {
            return true;
        }
        const auto declaration = std::ranges::find_if(
            document.soundVariableLibrary.variables,
            [&](const auto& candidate) {
                return candidate.owner.kind
                        == ProjectVariableOwnerKind::WorkspaceSession
                    && candidate.variableId == value.variableId;
            });
        return declaration == document.soundVariableLibrary.variables.end()
            || std::ranges::find(
                   declaration->candidateDefinitionIds,
                   value.selectedDefinitionId)
                == declaration->candidateDefinitionIds.end()
            || workspaceSessionSoundVariableDeclarationFingerprint(
                   *declaration)
                != value.declarationFingerprint;
    });
    state.schemaVersion = supportedWorkspaceSessionVersion;
    normalize(state);
}
}
