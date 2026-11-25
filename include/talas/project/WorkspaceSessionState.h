#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/project/ProjectVariables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
struct ProjectSoundVariableDeclaration;

inline constexpr std::size_t maximumWorkspaceGraphContexts = 512;
inline constexpr std::size_t maximumWorkspaceNavigationDepth = 256;
inline constexpr std::size_t maximumWorkspaceBookmarks = 256;
inline constexpr std::size_t maximumWorkspaceSampleRegionSelections = 1024;
inline constexpr std::size_t maximumWorkspacePerformanceSelectionIdBytes = 128;
inline constexpr std::size_t maximumWorkspaceLiveToolStates =
    maximumProjectLiveTools;
inline constexpr std::size_t maximumWorkspaceAnalysisTraces = 16;
inline constexpr std::size_t maximumWorkspaceAnalysisTraceIdBytes = 128;
inline constexpr std::size_t maximumWorkspaceAnalysisTargetIdBytes = 128;
inline constexpr std::size_t maximumWorkspaceAnalysisInstanceDepth = 64;
inline constexpr std::uint32_t minimumWorkspaceAnalysisRefreshRateHz = 5;
inline constexpr std::uint32_t maximumWorkspaceAnalysisRefreshRateHz = 60;
inline constexpr std::uint32_t minimumWorkspaceAnalysisHistoryFrames = 1;
inline constexpr std::uint32_t maximumWorkspaceAnalysisHistoryFrames = 600;
inline constexpr std::uint8_t maximumWorkspaceAnalysisColourSlot = 7;
inline constexpr std::size_t maximumWorkspaceAnalysisAuditionAssetIdBytes = 128;
inline constexpr double minimumWorkspaceAnalysisSineFrequencyHz = 1.0;
inline constexpr double maximumWorkspaceAnalysisSineFrequencyHz = 20'000.0;
inline constexpr std::size_t maximumWorkspaceSoundLibraryEntryIdBytes = 512;
inline constexpr std::size_t maximumWorkspaceSoundLibraryCategoryBytes = 128;
inline constexpr std::size_t maximumWorkspaceSoundLibraryFavorites = 128;
inline constexpr std::size_t maximumWorkspaceSoundLibraryRecents = 32;
inline constexpr std::size_t maximumWorkspaceSoundLibraryInputSubstitutions = 64;
inline constexpr std::size_t maximumWorkspaceSoundLibraryPortIdBytes = 128;
inline constexpr std::size_t maximumWorkspaceSoundLibraryAssetIdBytes = 128;
inline constexpr std::size_t maximumWorkspaceVariableValues = 128;
inline constexpr std::size_t maximumWorkspaceSoundVariableValues = 64;
inline constexpr std::size_t workspaceVariableDeclarationFingerprintBytes = 71;
inline constexpr double minimumWorkspaceSoundLibrarySineFrequencyHz = 1.0;
inline constexpr double maximumWorkspaceSoundLibrarySineFrequencyHz = 20'000.0;
inline constexpr double maximumWorkspaceSoundLibraryConstantMagnitude = 1.0e6;

enum class WorkspaceViewMode
{
    Structure,
    Performance,
    Samples,
};

enum class WorkspaceStructureSplitOrientation
{
    Adaptive,
    LeftRight,
    TopBottom,
};

enum class WorkspaceGraphViewMode
{
    Compact,
    Ports,
    PortsAndWires,
};

enum class WorkspaceAnalysisTraceKind
{
    Audio,
    Control,
    Event,
    PartialSpectrum,
    ComplexFrame,
};

enum class WorkspaceAnalysisSection
{
    Traces,
    Events,
    Runtime,
    Assets,
};

enum class WorkspaceAnalysisTestSourceKind
{
    None,
    Silence,
    Impulse,
    Sine,
    FrozenAsset,
};

enum class WorkspaceInterfaceScale
{
    Percent100 = 100,
    Percent115 = 115,
    Percent130 = 130,
    Percent150 = 150,
};

enum class WorkspaceSoundLibraryCollection
{
    All,
    Favorites,
    Recent,
};

enum class WorkspaceSoundLibrarySourceFilter
{
    All,
    ProjectDefinitions,
    Components,
    OriginalExamples,
};

enum class WorkspaceSoundLibraryCompatibilityFilter
{
    All,
    Ready,
    NeedsAttention,
    Unavailable,
};

enum class WorkspaceSoundLibraryHealthFilter
{
    All,
    Healthy,
    Warning,
    Unavailable,
};

enum class WorkspaceSoundLibraryAuditionSource
{
    Default,
    Silence,
    Sine,
    Constant,
    FrozenAsset,
};

struct WorkspaceSoundLibraryInputSubstitution
{
    std::string portId;
    WorkspaceSoundLibraryAuditionSource source =
        WorkspaceSoundLibraryAuditionSource::Default;
    double value = 0.0;
    std::optional<std::string> frozenAssetId;

    bool operator==(
        const WorkspaceSoundLibraryInputSubstitution&) const = default;
};

struct WorkspaceSoundLibraryState
{
    std::optional<std::string> selectedEntryId;
    WorkspaceSoundLibraryCollection collection =
        WorkspaceSoundLibraryCollection::All;
    WorkspaceSoundLibrarySourceFilter sourceFilter =
        WorkspaceSoundLibrarySourceFilter::All;
    WorkspaceSoundLibraryCompatibilityFilter compatibilityFilter =
        WorkspaceSoundLibraryCompatibilityFilter::All;
    WorkspaceSoundLibraryHealthFilter healthFilter =
        WorkspaceSoundLibraryHealthFilter::All;
    std::optional<std::string> category;
    std::optional<std::string> origin;
    std::vector<std::string> favoriteEntryIds;
    std::vector<std::string> recentEntryIds;
    std::vector<WorkspaceSoundLibraryInputSubstitution> inputSubstitutions;
    bool detailVisible = true;

    bool operator==(const WorkspaceSoundLibraryState&) const = default;
};

struct WorkspaceAccessibilityPreferences
{
    WorkspaceInterfaceScale interfaceScale =
        WorkspaceInterfaceScale::Percent100;
    bool enhancedContrast = false;
    bool reduceMotion = false;
    bool contextHelpVisible = false;

    bool operator==(const WorkspaceAccessibilityPreferences&) const = default;
};

struct WorkspaceAnalysisTraceState
{
    std::string traceId;
    std::vector<std::string> instancePath;
    std::optional<std::string> containingDefinitionId;
    std::string nodeId;
    std::optional<std::string> portId;
    WorkspaceAnalysisTraceKind kind = WorkspaceAnalysisTraceKind::Audio;
    bool visible = true;
    std::uint8_t colourSlot = 0;
    double verticalScale = 1.0;

    bool operator==(const WorkspaceAnalysisTraceState&) const = default;
};

struct WorkspaceAnalysisAuditionState
{
    std::string traceId;
    std::optional<std::string> destinationInputPortId;
    WorkspaceAnalysisTestSourceKind source =
        WorkspaceAnalysisTestSourceKind::None;
    double sineFrequencyHz = 440.0;
    std::optional<std::string> frozenAssetId;
    bool includeUpstream = true;

    bool operator==(const WorkspaceAnalysisAuditionState&) const = default;
};

struct WorkspaceAnalysisSessionState
{
    std::vector<WorkspaceAnalysisTraceState> traces;
    std::optional<std::string> selectedTraceId;
    bool frozen = false;
    std::uint32_t refreshRateHz = 30;
    std::uint32_t historyFrames = 120;
    WorkspaceAnalysisSection activeSection = WorkspaceAnalysisSection::Traces;
    std::optional<WorkspaceAnalysisAuditionState> audition;

    bool operator==(const WorkspaceAnalysisSessionState&) const = default;
};

struct WorkspaceSampleMapViewportState
{
    double noteStart = 0.0;
    double noteSpan = 128.0;
    double velocityStart = 1.0;
    double velocitySpan = 127.0;

    bool operator==(const WorkspaceSampleMapViewportState&) const = default;
};

struct WorkspaceWaveformViewportState
{
    double normalizedStart = 0.0;
    double normalizedSpan = 1.0;

    bool operator==(const WorkspaceWaveformViewportState&) const = default;
};

struct WorkspaceSamplesSessionState
{
    std::optional<std::string> activeMapId;
    std::vector<std::string> selectedRegionIds;
    WorkspaceSampleMapViewportState mapViewport;
    WorkspaceWaveformViewportState waveformViewport;
    double mapWaveformSplit = 0.5;

    bool operator==(const WorkspaceSamplesSessionState&) const = default;
};

struct WorkspacePerformanceSessionState
{
    enum class Mode
    {
        Controls,
        Snapshots,
        LiveGrid,
    };

    std::optional<std::string> selectedControlId;
    std::optional<std::string> selectedMeterId;
    std::optional<std::string> selectedLiveSetId;
    std::optional<std::string> selectedLiveSetCellId;
    std::optional<std::string> activeLiveSetId;
    std::optional<std::string> activeLiveSetCellId;
    std::optional<std::string> queuedLiveSetId;
    std::optional<std::string> queuedLiveSetCellId;
    std::uint64_t queuedLiveSetRequestGeneration = 0;
    struct LiveToolVariable
    {
        std::string variableId;
        ProjectLiveToolVariableKind kind =
            ProjectLiveToolVariableKind::Number;
        double number = 0.0;
        bool boolean = false;

        bool operator==(const LiveToolVariable&) const = default;
    };
    struct LiveToolState
    {
        std::string toolId;
        std::string stateId;
        std::string definitionFingerprint;
        std::vector<LiveToolVariable> variables;

        bool operator==(const LiveToolState&) const = default;
    };
    std::optional<std::string> selectedLiveToolId;
    std::vector<LiveToolState> liveToolStates;
    double panX = 0.0;
    double panY = 0.0;
    double zoom = 1.0;
    bool editMode = true;
    bool compact = false;
    Mode mode = Mode::Controls;

    bool operator==(const WorkspacePerformanceSessionState&) const = default;
};

struct WorkspaceVariableValue
{
    std::string variableId;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    std::string declarationFingerprint;

    bool operator==(const WorkspaceVariableValue&) const = default;
};

struct WorkspaceSoundVariableValue
{
    std::string variableId;
    std::string selectedDefinitionId;
    std::string declarationFingerprint;

    bool operator==(const WorkspaceSoundVariableValue&) const = default;
};

struct WorkspaceGraphSessionState
{
    std::optional<std::string> soundDefinitionId;
    double panX = 0.0;
    double panY = 0.0;
    double zoom = 1.0;
    std::optional<std::string> selectedNodeId;

    bool operator==(const WorkspaceGraphSessionState&) const = default;
};

struct WorkspaceHierarchyBookmark
{
    std::string definitionId;
    std::vector<std::string> navigationPath;
    std::optional<std::string> activeSoundDefinitionId;
    double paneSplit = 0.5;
    WorkspaceStructureSplitOrientation structureSplitOrientation =
        WorkspaceStructureSplitOrientation::Adaptive;
    bool secondaryPaneVisible = true;
    bool primaryPaneFirst = true;
    WorkspaceGraphViewMode graphViewMode =
        WorkspaceGraphViewMode::PortsAndWires;

    bool operator==(const WorkspaceHierarchyBookmark&) const = default;
};

struct WorkspaceSessionState
{
    std::uint32_t schemaVersion = 13;
    WorkspaceViewMode viewMode = WorkspaceViewMode::Structure;
    double paneSplit = 0.5;
    WorkspaceStructureSplitOrientation structureSplitOrientation =
        WorkspaceStructureSplitOrientation::Adaptive;
    bool secondaryPaneVisible = true;
    bool primaryPaneFirst = true;
    WorkspaceGraphViewMode graphViewMode =
        WorkspaceGraphViewMode::PortsAndWires;
    std::optional<std::string> activeSoundDefinitionId;
    std::vector<std::string> structureNavigationPath;
    std::vector<WorkspaceGraphSessionState> graphContexts = {
        WorkspaceGraphSessionState{},
    };
    std::optional<std::string> focusedPerformanceEventName;
    std::vector<WorkspaceHierarchyBookmark> hierarchyBookmarks;
    WorkspacePerformanceSessionState performance;
    WorkspaceSamplesSessionState samples;
    WorkspaceAnalysisSessionState analysis;
    WorkspaceAccessibilityPreferences accessibility;
    WorkspaceSoundLibraryState soundLibrary;
    std::vector<WorkspaceVariableValue> variableValues;
    std::vector<WorkspaceSoundVariableValue> soundVariableValues;

    bool operator==(const WorkspaceSessionState&) const = default;
};

struct WorkspaceSessionDiagnostic
{
    std::string message;
    std::string path;

    bool operator==(const WorkspaceSessionDiagnostic&) const = default;
};

struct WorkspaceSessionLoadResult
{
    WorkspaceSessionState state;
    std::vector<WorkspaceSessionDiagnostic> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

[[nodiscard]] bool isKnownWorkspaceAnalysisTestSourceKind(
    WorkspaceAnalysisTestSourceKind source) noexcept;

[[nodiscard]] bool isKnownWorkspaceInterfaceScale(
    WorkspaceInterfaceScale scale) noexcept;

[[nodiscard]] std::string serializeWorkspaceSessionState(
    const WorkspaceSessionState& state);

[[nodiscard]] WorkspaceSessionLoadResult parseWorkspaceSessionState(
    std::string_view source);

[[nodiscard]] std::string workspaceSessionVariableDeclarationFingerprint(
    const ProjectVariableDeclaration& declaration);

[[nodiscard]] std::string workspaceSessionSoundVariableDeclarationFingerprint(
    const ProjectSoundVariableDeclaration& declaration);

[[nodiscard]] std::string workspaceSessionLiveToolDefinitionFingerprint(
    const ProjectLiveTool& tool);

[[nodiscard]] const WorkspaceGraphSessionState*
findWorkspaceGraphSessionState(
    const WorkspaceSessionState& state,
    std::optional<std::string_view> soundDefinitionId);

[[nodiscard]] WorkspaceGraphSessionState*
findWorkspaceGraphSessionState(
    WorkspaceSessionState& state,
    std::optional<std::string_view> soundDefinitionId);

[[nodiscard]] WorkspaceGraphSessionState&
ensureWorkspaceGraphSessionState(
    WorkspaceSessionState& state,
    std::optional<std::string> soundDefinitionId);

[[nodiscard]] bool replaceWorkspaceSessionHierarchyBookmarks(
    WorkspaceSessionState& state,
    std::vector<WorkspaceHierarchyBookmark> bookmarks);

[[nodiscard]] std::vector<std::string> workspaceSessionBookmarkedDefinitionIds(
    const WorkspaceSessionState& state);

[[nodiscard]] bool captureWorkspaceSessionHierarchyBookmark(
    WorkspaceSessionState& state, std::string definitionId);

[[nodiscard]] bool removeWorkspaceSessionHierarchyBookmark(
    WorkspaceSessionState& state, std::string_view definitionId);

[[nodiscard]] bool restoreWorkspaceSessionHierarchyBookmark(
    WorkspaceSessionState& state, std::string_view definitionId);

void reconcileWorkspaceSessionState(
    WorkspaceSessionState& state,
    const ProjectDocument& document);

void reconcileWorkspaceSoundLibraryState(
    WorkspaceSoundLibraryState& state,
    std::span<const std::string> validEntryIds);
}
