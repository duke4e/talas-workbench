#pragma once

#include "app/workbench/WorkbenchHost.h"

#include "app/workbench/WorkbenchAnalysisPanel.h"
#include "app/workbench/WorkbenchAnalysisTools.h"
#include "app/workbench/WorkbenchCdpProcessTool.h"
#include "app/workbench/WorkbenchDspStatus.h"
#include "app/workbench/WorkbenchFiles.h"
#include "app/workbench/WorkbenchAssetRecoveryPanel.h"
#include "app/workbench/WorkbenchContextHelpPanel.h"
#include "app/workbench/WorkbenchDiagnosticsPanel.h"
#include "app/workbench/WorkbenchFakeKeyboard.h"
#include "app/workbench/WorkbenchSynchronizingSpectra.h"
#include "app/workbench/WorkbenchTapeRecorder.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"
#include "app/workbench/WorkbenchGraphCanvas.h"
#include "app/workbench/WorkbenchInspectorPanel.h"
#include "app/workbench/WorkbenchLiveGridEditor.h"
#include "app/workbench/WorkbenchLiveTimelineRecording.h"
#include "app/workbench/WorkbenchExpressionsLibrary.h"
#include "app/workbench/WorkbenchSampleEditor.h"
#include "app/workbench/WorkbenchSoundBrowser.h"
#include "app/workbench/WorkbenchSoundBrowserSurface.h"
#include "app/workbench/WorkbenchSoundEditorTabs.h"
#include "app/workbench/WorkbenchSoundFileWindow.h"
#include "app/workbench/WorkbenchCustomPrototypes.h"
#include "app/workbench/WorkbenchToolEditor.h"
#include "app/workbench/WorkbenchToolWindow.h"
#include "app/workbench/WorkbenchGlobalMapEditor.h"
#include "app/workbench/WorkbenchClassEditor.h"
#include "app/workbench/WorkbenchFoldersPanel.h"
#include "app/workbench/WorkbenchAlternateTunings.h"
#include "app/workbench/WorkbenchOscIngress.h"
#include "app/workbench/WorkbenchOscEgress.h"
#include "app/workbench/WorkbenchOscLoopbackProfile.h"
#include "app/workbench/WorkbenchOscLoopbackProfileCoordinator.h"
#include "app/workbench/WorkbenchPartialTrackEditor.h"
#include "app/workbench/WorkbenchPerformanceCanvas.h"
#include "app/workbench/WorkbenchProjectLookup.h"
#include "app/workbench/WorkbenchPrototypePanel.h"
#include "app/workbench/WorkbenchSampleMapPanel.h"
#include "app/workbench/WorkbenchSampleRegionInspector.h"
#include "app/workbench/WorkbenchSoundDefinitionsPanel.h"
#include "app/workbench/WorkbenchSnapshotPanel.h"
#include "app/workbench/WorkbenchSplitHandle.h"
#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchTimelinePanel.h"
#include "app/workbench/WorkbenchTextFile.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "app/workbench/WorkbenchVirtualControls.h"
#include "app/workbench/WorkbenchWaveformPanel.h"
#include "talas/app/DspStatusModel.h"
#include "talas/app/AssetRecoveryWorkbenchModel.h"
#include "talas/app/FileLocator.h"
#include "talas/app/SoundClassIcon.h"
#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/AnalysisAuditionModel.h"
#include "talas/app/AnalysisDiagnosticsModel.h"
#include "talas/app/PerformanceRuntimeDispatcher.h"
#include "talas/app/SelectedSoundDocument.h"
#include "talas/app/WorkbenchDeviceMapping.h"
#include "talas/app/WorkbenchAccessibilityPolicy.h"
#include "talas/app/WorkbenchOperationCoordinator.h"
#include "talas/app/PrototypeBrowserModel.h"
#include "talas/app/SnapshotWorkbenchModel.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/app/SoundLibraryModel.h"
#include "talas/app/SoundLibraryAuditionModel.h"
#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/audio/AnalyzerDisplayMapping.h"
#include "talas/audio/DiskCacheCapture.h"
#include "talas/audio/OscilloscopeAnalysis.h"
#include "talas/audio/PartialTrackDerivative.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/ProjectAssetDisposition.h"
#include "talas/audio/ProjectPackager.h"
#include "talas/audio/SampleFileFormats.h"
#include "talas/audio/SampleMapImport.h"
#include "talas/audio/WavWriter.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/ProjectWavExporter.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/audio/RealtimeOutputTake.h"
#include "talas/audio/SpectrumAnalysis.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/VariableLiterals.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectAssetResolver.h"
#include "talas/project/ProjectAutosaveState.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/RecentProjectList.h"
#include "talas/project/ProjectRecoveryFile.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/SoundDefinitionImport.h"
#include "talas/project/TimelineInteraction.h"
#include "talas/project/ProjectEditBoundary.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/WorkspaceSessionFile.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/AnalysisTestInputSource.h"
#include "talas/runtime/ScoreClipArtifactPublication.h"
#include "talas/runtime/ScoreClipMaterialization.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <ranges>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
struct ComponentReplacementControllerState;
struct ComponentReplacementControllerStateDeleter
{
    void operator()(ComponentReplacementControllerState* state) const
        noexcept;
};
#endif

std::optional<std::string_view> optionalStringView(
    const std::optional<std::string>& value);

juce::String preparedFrames(
    const std::optional<std::size_t>& frames);

juce::String preparedWindow(
    const std::optional<talas::dsp::SpectralWindowKind>& window);

juce::String preparedStreamHealthDetail(
    const talas::runtime::AudioStreamHealthSnapshot& health);

bool sameAudioStreamHealth(
    const talas::runtime::AudioStreamHealthSnapshot& left,
    const talas::runtime::AudioStreamHealthSnapshot& right) noexcept;

bool sameRealtimeStreamHealth(
    const talas::runtime::RealtimeStreamHealthSnapshot& left,
    const talas::runtime::RealtimeStreamHealthSnapshot& right) noexcept;

bool samePreparedStaticStatus(
    const talas::runtime::PreparedGraphStatusSnapshot& left,
    const talas::runtime::PreparedGraphStatusSnapshot& right) noexcept;

bool sameRealtimeMetricsPresentation(
    const talas::runtime::RealtimeMetricsSnapshotV1& left,
    const talas::runtime::RealtimeMetricsSnapshotV1& right) noexcept;

bool preparedRecordTargetsSelection(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    std::string_view runtimeNodeId,
    std::string_view structuralSourcePath);

bool preparedSelectionTargetsStream(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status);

juce::String preparedStatusHeadline(
    const talas::runtime::PreparedGraphStatusSnapshot& status);

juce::String preparedAccessibleSummary(
    const talas::runtime::PreparedGraphStatusSnapshot& status);

juce::String preparedAnalysisSummary(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status);

juce::String preparedInspectorContract(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status);

std::vector<WorkbenchStatusDiagnostic>
preparedRuntimeDiagnostics(
    const talas::runtime::PreparedGraphStatusSnapshot& status,
    const talas::runtime::RealtimeMetricsSnapshotV1& metrics);

std::filesystem::path filesystemPath(const juce::String& value);

juce::String jucePathString(const std::filesystem::path& value);

talas::project::ProjectParameterExpression expressionParameter(std::string id, std::string expression);

void appendNode(
    talas::project::ProjectDocument& document,
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters,
    talas::project::CanvasPosition position);

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort);

talas::project::ProjectDocument makeDemoDocument();

talas::project::ProjectDocument makeUntitledDocument();

std::filesystem::path workbenchRecoveryPath();

std::filesystem::path workbenchRecentProjectsPath();

std::filesystem::path workbenchWorkspaceSessionsPath();

std::filesystem::path workbenchCleanupLedgerPath();

talas::project::ProjectParameterExpression defaultParameter(const talas::core::ParameterDescriptor& descriptor);

enum WorkbenchCommandId
{
    commandNewProject = 0x2000,
    commandOpenProject,
    commandSaveProject,
    commandSaveProjectAs,
    commandExportAudio,
    commandPackageProject,
    commandAudioDeviceSettings,
    commandUndo,
    commandRedo,
    commandDeleteSelectedNode,
    commandPlay,
    commandPause,
    commandStop,
    commandTogglePlayback,
    commandToggleLibrary,
    commandToggleRightWorkspace,
    commandFocusGraph,
    commandFocusTimeline,
    commandZoomGraphToFit,
    commandZoomIn,
    commandZoomOut,
    commandWorkspacePatch,
    commandWorkspaceAutomation,
    commandWorkspaceAnalysis,
    commandWorkspacePerformance,
    commandWorkspaceSamples,
    commandStructureSplitAdaptive,
    commandStructureSplitLeftRight,
    commandStructureSplitTopBottom,
    commandSwapStructurePanes,
    commandToggleSecondaryStructurePane,
    commandGraphViewCompact,
    commandGraphViewPorts,
    commandGraphViewPortsAndWires,
    commandShowWorkbenchGuide,
    commandToggleContextHelp,
    commandDuplicateSelectedNodes,
    commandSelectAllGraphNodes,
    commandFocusPrototypeSearch,
    commandCaptureDiskCacheLoop,
    commandCutSelectedNodes,
    commandCopySelectedNodes,
    commandPasteGraphNodes,
    commandShowFakeKeyboard,
    commandShowLiveGridEditor,
    commandShowSpectralAnalysis,
    commandShowReAnalysis,
    commandShowCdpProcess,
    commandShowGaAnalysis,
    commandRenameGraphNode,
    commandCleanUpGraph,
    commandShowExpressionsLibrary,
    commandShowSampleEditor,
    commandShowGlobalMap,
    commandImportMidiToTimeline,
    commandShowClassEditor,
    commandShowAlternateTunings,
    commandShowStructureAsText,
    commandEvaluateSelection,
    commandShowEnvironment,
    commandResetEnvironment,
    commandShowDspStatus,
    commandFindText,
    commandFindTextAgain,
    commandReplaceText,
    commandReplaceTextAgain,
    commandCompileAndLoad,
    commandRestart,
    commandRevertSound,
    commandPasteHot,
    commandPasteSpecial,
    commandShowPreferences,
    commandShowFileOrganizer,
    commandShowFullWaveform,
    commandShowSoundBrowser,
    commandShowSoundFileWindow,
    commandOpenCustomPrototypes,
    commandShowSystemPrototypes,
    commandNewTool,
    commandOpenTool,
    commandUseTool,
    commandView,
    commandNewFile,
    commandFileArchivist,
    commandShowMidiMessages,
    commandDescribeSound,
    commandGetInfo,
    commandRecordToDisk,
    commandCompileToDisk,
    commandShowVirtualControlSurface,
    commandNewClassFromExample,
    commandEditClass,
    commandRetrieveExampleFromClass,
    commandSetDefaultSound,
    commandSetDefaultCollection,
    commandChooseGlobalMap,
    commandInfoOscilloscope,
    commandInfoSpectrumAnalyzer,
    commandExpandSound,
    commandMidiNotesOff,
    commandCloseWindow,
    commandPlayMarked,
    commandPlayAll,
    commandOpenAny,
    commandPlayFile,
    commandChooseWindow,
    commandNextWindow,
    commandPreviousWindow,
    commandLargeWindow,
    commandShowTapeRecorder,
    commandShowSynchronizingSpectra,
    commandConfigureMidi,
    commandInitializeDsp,
    // ADR-0029: File > Export Plugin...
    commandExportPlugin,
};

enum class CompactDock
{
    None,
    Library,
    RightWorkspace
};

#if defined(TALAS_PRODUCT_PERFORMANCE_PROBE_SUPPORT)
struct ProductDeviceProbeObservation
{
    std::string name;
    std::string type;
    std::uint64_t sampleRate = 0;
    std::uint64_t blockFrames = 0;
    std::uint64_t inputChannels = 0;
    std::uint64_t outputChannels = 0;
    talas::runtime::RealtimeMetricsSnapshotV1 metrics;
};
#endif

inline constexpr int recentProjectMenuItemBase = 0x6000;
// The clipboard's text while it holds samples the sample editor copied,
// which a Timeline pastes as a clip.
inline constexpr std::string_view sampleClipboardMarker{"talas-sample-editor-clip"};

// The menu an operation is in, as a
// command's category.
[[nodiscard]] inline juce::String menuFor(talas::app::TalasMenuOperation operation)
{
    return juceString(talas::app::talasMenuName(talas::app::talasMenuOf(operation)));
}

inline constexpr int savedToolMenuItemBase = 0x6400;
inline constexpr int documentationMenuItemBase = 0x6800;
// The Edit menu's Field Reading items.
inline constexpr int fieldReadingMenuItemBase = 0x6C00;

class WorkbenchComponent final : public talas::app::workbench::WorkbenchHost,
                                 public juce::DragAndDropContainer,
                                 public juce::FileDragAndDropTarget,
                                 public juce::ApplicationCommandTarget,
                                 private juce::AudioIODeviceCallback,
                                 private juce::ChangeListener,
                                 private juce::FocusChangeListener,
                                 private juce::Timer
{
public:
    using TitleCallback =
        talas::app::workbench::WorkbenchTitleCallback;

    explicit WorkbenchComponent(
        TitleCallback onTitleChange,
        WorkbenchHostOptions options = {});

    ~WorkbenchComponent() override;

    void beginHardwareInitialisation() override;

    void requestQuit() override;
    void usePreferencesFile(const std::filesystem::path& file) override;

    [[nodiscard]] std::optional<std::string> runAutomatedSmoke(
        const std::filesystem::path& smokeDirectory) override;

    [[nodiscard]] std::optional<std::string> renderScreenshots(
        const std::filesystem::path& directory) override;

#if defined(TALAS_PRODUCT_PERFORMANCE_PROBE_SUPPORT)
    [[nodiscard]] bool installProductPerformanceDocumentForProbe(
        talas::project::ProjectDocument document);

    [[nodiscard]] bool productPerformanceDeviceReadyForProbe();

    [[nodiscard]] bool beginProductPerformancePlaybackForProbe();

    [[nodiscard]] std::optional<ProductDeviceProbeObservation>
    productPerformanceDeviceObservationForProbe();

    void stopProductPerformancePlaybackForProbe();
#endif

    void paint(juce::Graphics& graphics) override;

    void paintOverChildren(juce::Graphics& graphics) override;

    void resized() override;

    juce::ApplicationCommandTarget* getNextCommandTarget() override;

    void getAllCommands(juce::Array<juce::CommandID>& commands) override;

    void getCommandInfo(
        juce::CommandID commandId,
        juce::ApplicationCommandInfo& info) override;

    bool perform(const juce::ApplicationCommandTarget::InvocationInfo& info) override;

    juce::StringArray getMenuBarNames() override;

    juce::PopupMenu getMenuForIndex(
        int topLevelMenuIndex,
        const juce::String&) override;

    void menuItemSelected(
        int menuItemId,
        int) override;

private:
    [[nodiscard]] std::optional<std::string>
    runExpressionLanguageSmoke(
        const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runMidiMapperEditorSmoke(
        const std::filesystem::path& smokeDirectory,
        const std::string& returnSelectionId);
    [[nodiscard]] std::optional<std::string>
    runMidiMapperVirtualOnlyControlSmoke();
    [[nodiscard]] std::optional<std::string>
    runMidiMapperRangedLocalControlSmoke();
    [[nodiscard]] std::optional<std::string>
    runBuiltInMidiButtonSwitchSmoke();
    // The showcase project the screenshots are taken of, and the samples
    // its Samples workspace imports, written into the directory.
    [[nodiscard]] std::optional<std::string> installScreenshotShowcase(
        const std::filesystem::path& sampleDirectory);
    [[nodiscard]] std::optional<std::string> writeScreenshot(
        const std::filesystem::path& file);
    // The smokes of the Workbench survey's authoring tools, in order.
    [[nodiscard]] std::optional<std::string>
    runAuthoringToolSmokes(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runStructureAsTextSmoke();
    [[nodiscard]] std::optional<std::string> runVariableEnvironmentSmoke();
    [[nodiscard]] std::optional<std::string> runDspStatusSmoke();
    [[nodiscard]] std::optional<std::string> runPluginExportSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runFileArchivistSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runInfoDisplaySmoke();
    [[nodiscard]] std::optional<std::string> runFindReplaceSmoke();
    [[nodiscard]] std::optional<std::string> runCompileAndLoadSmoke();
    [[nodiscard]] std::optional<std::string> runRevertSmoke();
    [[nodiscard]] std::optional<std::string> runPasteHotSmoke();
    [[nodiscard]] std::optional<std::string> runPasteSpecialSmoke();
    [[nodiscard]] std::optional<std::string> runInputFieldSmoke();
    [[nodiscard]] std::optional<std::string> runFilesSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runSampleEditorPlaySmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runGraphEditingSmoke();
    [[nodiscard]] std::optional<std::string>
    runFakeKeyboardSmoke();
    [[nodiscard]] std::optional<std::string> runTapeRecorderSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runLiveGridEditorSmoke();
    [[nodiscard]] std::optional<std::string>
    runAnalysisToolsSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runSpectralAnalysisWizardSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runSynchronizingSpectraSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string> runHotValueCompletionSmoke();
    [[nodiscard]] std::optional<std::string> runDiskSplicerFieldSmoke();
    [[nodiscard]] std::optional<std::string> runDiskButtonSmoke();
    [[nodiscard]] std::optional<std::string> runKeyEquivalentsSmoke();
    [[nodiscard]] std::optional<std::string>
    runCdpProcessToolSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runTimelineAutomationSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimelineInfoSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimelineInputSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimelinePointsSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimelineGridSmoke();
    [[nodiscard]] std::optional<std::string>
    runSoundBrowserSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runVcsPresetsSmoke();
    [[nodiscard]] std::optional<std::string>
    runSoundDragSmoke();
    [[nodiscard]] std::optional<std::string>
    runSoundFileSmoke();
    [[nodiscard]] std::optional<std::string>
    runToolEditorSmoke();
    [[nodiscard]] std::optional<std::string>
    runPresetCaptureSmoke();
    [[nodiscard]] std::optional<std::string>
    runGraphArrangementSmoke();
    [[nodiscard]] std::optional<std::string>
    runParameterFieldEditorSmoke();
    [[nodiscard]] std::optional<std::string>
    runSampleEditorSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runVcsWidgetSmoke();
    [[nodiscard]] std::optional<std::string>
    runGlobalMapSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimelineEditingSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runClassEditorSmoke();
    [[nodiscard]] std::optional<std::string>
    runAlternateTuningsSmoke(const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runGeneratorWorkflowSmoke(
        const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runParameterTransformerSmoke(
        const std::string& oscillatorId,
        const std::string& vcaId);

    [[nodiscard]] std::optional<std::string> runE10StartupSmoke(
        const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runPartialTrackEditorShellSmoke(
        const std::filesystem::path& smokeDirectory);
    [[nodiscard]] std::optional<std::string>
    runPartialTrackOperationSmoke(
        const std::filesystem::path& smokeDirectory,
        const talas::project::PartialTrackAsset& source);

    [[nodiscard]] std::optional<std::string> runTimelineScoreShellSmoke(
        const std::filesystem::path& smokeDirectory,
        std::string_view editableDefinitionId);

    void addTimelineScoreGainFixture(
        talas::project::ProjectSoundDefinition& definition);

    [[nodiscard]] std::optional<std::string>
    runMidiIngressGenerationWorkflowForSmoke(
        std::string_view eventName);
    [[nodiscard]] std::optional<std::string>
    runVirtualControlIngressWorkflowForSmoke(
        std::string_view eventName);

    void refreshWorkspaceButtons();

    void activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode mode);
    // File > Virtual Control Surface.
    void showVirtualControlSurfaceInFront();

    void refreshSampleMapWorkspace();

    void selectSampleMap(std::string mapId);

    void createSampleMap();

    void renameSampleMap(std::string displayName);

    void removeSampleMap(std::string mapId);

    void selectSampleRegion(
        std::string regionId,
        talas::app::SampleMapSelectionMode mode);

    void selectSampleRegionsByMidi(
        std::uint8_t note,
        std::uint8_t velocity);

    void moveSelectedSampleRegions(int noteDelta, int velocityDelta);

    void resizePrimarySampleRegion(
        talas::app::SampleMapResizeEdge edge,
        int delta);

    void applySampleRegion(talas::project::ProjectSampleRegion region);

    void removeSelectedSampleRegions();

    void chooseSampleMapImportFiles();

    void chooseSampleMapReplaceFiles();

    void chooseSampleMapFiles(bool replace);

    void startSampleMapImport(
        std::vector<std::filesystem::path> sourcePaths,
        bool replace = false);

    void cancelSampleMapImport();

    void updateSampleMapImportPreview(
        std::uint8_t lowNote,
        std::uint8_t anchorRootNote,
        std::uint8_t highNote);

    void applySampleMapImportPreview();

    void finishSampleMapImportIfReady();

    void stopSampleMapImportWorker();

    [[nodiscard]] bool sampleMapImportOwnsProjectBoundary();

    [[nodiscard]] std::optional<std::string>
    runSampleMapImportPreviewSmoke(
        const std::filesystem::path& sampleOne,
        const std::filesystem::path& sampleTwo,
        const std::function<std::optional<std::string>()>& waitForImport,
        const std::function<std::optional<std::string>(std::string_view)>&
            snapshot);

    [[nodiscard]] std::optional<std::string>
    runSampleMapAuditionLifecycleSmoke(
        std::string_view firstRegionId,
        const std::filesystem::path& outputTakePath);

    [[nodiscard]] std::string
    discardOrRetainSampleMapImportCleanup(
        talas::audio::PreparedSampleMapImport& candidate);

    void showSampleAsset(std::string assetId);

    void embedSampleAsset(std::string assetId);

    void chooseReferencedSampleAsset(std::string assetId);

    void startSampleAssetDisposition(
        std::string assetId,
        talas::project::ProjectAssetDisposition disposition,
        std::optional<std::filesystem::path> selectedPath = std::nullopt);

    void finishSampleAssetDispositionIfReady();

    void stopSampleAssetDispositionWorker();

    [[nodiscard]] std::string
    discardOrRetainSampleAssetDispositionCleanup(
        talas::audio::PreparedProjectAssetDisposition& candidate);

    void retryPendingSampleCleanup();

    [[nodiscard]] std::string loadPendingCleanupLedger();

    [[nodiscard]] std::string persistPendingCleanupLedger();

    [[nodiscard]] std::size_t pendingCleanupArtifactCount() const;

    [[nodiscard]] std::string retainOwnedCleanupFile(
        const std::filesystem::path& path,
        std::string_view expectedContentHash);

    void requestSelectedSampleWaveform();

    void finishSampleWaveformIfReady();

    void stopSampleWaveformWorker();

    void zoomSampleWaveform(double zoomFactor);

    void setSampleWaveformCursor(std::uint64_t frame);

    void moveSampleLoopMarker(
        bool releaseLoop,
        talas::app::WaveformLoopMarker marker,
        std::int64_t delta);

    void auditionActiveSampleMap();

    struct SampleMapAuditionVoice
    {
        int channel = 1;
        std::uint8_t note = 60;
        std::uint8_t velocity = 127;
        double arrivalTimeSeconds = 0.0;
        bool physicalKey = false;
        bool keyDown = true;
    };

    void beginSampleMapAudition(SampleMapAuditionVoice voice);

    void handleSampleMapMidiNoteOn(
        int channel,
        std::uint8_t note,
        std::uint8_t velocity,
        double arrivalTimeSeconds);

    void handleSampleMapMidiNoteOff(
        int channel,
        std::uint8_t note,
        double arrivalTimeSeconds);

    void stopSampleMapAudition();

    void finishSampleMapAuditionIfReady();

    void finishSampleMapAuditionRestoreIfReady();

    void stopSampleMapAuditionWorker();

    void scheduleHardwareInitialisationAfterFirstPaint();

    void globalFocusChanged(juce::Component*) override;

    [[nodiscard]] std::unique_ptr<juce::ComponentTraverser>
    createKeyboardFocusTraverser() override;

    void configureWorkbenchFocusTopology();

    [[nodiscard]] std::optional<WorkbenchFocusIdentity>
    currentWorkbenchFocusIdentity() const;
    [[nodiscard]] std::optional<WorkbenchFocusIdentity>
    workbenchFocusIdentityForComponent(
        const juce::Component* focused) const;

    void restoreWorkbenchFocusAfterLayout();

    void scheduleWorkbenchFocusRestoration();

    [[nodiscard]] std::vector<juce::Component*>
    visibleWorkbenchKeyboardFocusOrder();

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;

    void audioDeviceStopped() override;

    void handleIncomingMidiMessageFromSource(
        std::uint8_t sourceSlot,
        std::uint64_t sourceGeneration,
        const juce::MidiMessage& message);

    void rebuildMidiInputSourceRegistry();
    void rebuildTransportSyncAuthority();
    void setTransportSyncSource(
        talas::project::ProjectTransportSyncSettings sync);
    [[nodiscard]] juce::String setOscIngressSettings(
        talas::project::ProjectOscIngressSettings settings);
    [[nodiscard]] juce::String setOscEgressSettings(
        talas::project::ProjectOscEgressSettings settings);
    [[nodiscard]] juce::String setOscLoopbackProfileSettings(
        talas::project::ProjectOscIngressSettings ingress,
        talas::project::ProjectOscEgressSettings egress);
    [[nodiscard]] bool isOscRouteAvailable(
        std::string_view eventName,
        std::string_view address) const;
    [[nodiscard]] bool isOscEgressRouteAvailable(
        std::string_view eventName,
        std::string_view address) const;
    [[nodiscard]] juce::String oscProfileAccessibleDetail() const;
    [[nodiscard]] juce::String oscProfileStatusText() const;
    void publishOscEgressValue(
        std::string_view eventName,
        double persistedValue) noexcept;
    [[nodiscard]] bool usesExternalMidiClock() const noexcept;
    [[nodiscard]] bool usesExternalMtc() const noexcept;
    [[nodiscard]] std::uint64_t
    invalidateMidiInputSourceRegistryState() noexcept;

    [[nodiscard]] std::string_view midiSourceIdentifier(
        std::uint8_t sourceSlot,
        std::uint64_t sourceGeneration) const noexcept;
    [[nodiscard]] std::optional<std::string>
    midiInputSourceDisplayName(std::string_view identifier) const;

    [[nodiscard]] juce::String outputMappingStatus(std::size_t requiredChannels) const;

    void refreshPreparedGraphPresentation();

    void refreshPreparedGraphLivePresentation();

    void refreshRuntimeDiagnostics();

    void refreshPreparedProjectStatusPresentation();

    void refreshPreparedGraphStatus();

    void requestDeviceMetricsReset() noexcept;

    void publishDeviceCallbackDuration(
        std::uint64_t elapsedNanoseconds) noexcept;

    [[nodiscard]] bool realtimeMetricsFixtureIsValidForSmoke();
    [[nodiscard]] bool midiSourceCapacityFixtureIsValidForSmoke();
    [[nodiscard]] bool midiClockAuthorityFixtureIsValidForSmoke();

    void refreshOutputMeterLabel();

    void timerCallback() override;

    void processPendingMidiControllers();
    void publishMidiMonitor(
        const talas::runtime::RealtimeControlCommand& command,
        std::string outcome,
        talas::app::AnalysisLiveState liveState,
        std::uint64_t stageId,
        std::string sourceIdentifier);
    void processPendingOscPerformanceControls();
    [[nodiscard]] std::optional<juce::String>
    dispatchPerformanceEventValue(
        std::string_view eventName,
        double value,
        talas::app::PerformanceActivitySource source,
        talas::runtime::ControllerRecordingIngressEvidence recording = {});
    void finishPerformanceValueGesture();

    void addPrototype(const talas::core::NodeTypeDescriptor& descriptor);

    void handleSoundAction(
        SoundDefinitionsPanel::Action action,
        std::string definitionId);

    void refreshSoundLibraryProjection();

    [[nodiscard]] const talas::app::SoundLibraryEntry*
    soundLibraryEntry(std::string_view stableId) const;

    void useSoundLibraryEntry(std::string stableId);

    void requestOpenSoundLibraryExample(std::string stableId);

    void openSoundLibraryHelp(std::string stableId);

    void openSoundLibraryExampleNow(std::string stableId);

    void importSoundLibraryExample(std::string stableId);

    void commitSoundLibraryImport(
        std::string stableId,
        talas::project::SoundDefinitionImportRequest request,
        std::string expectedDestinationBytes);

    [[nodiscard]] std::optional<std::string>
    runE8SoundLibraryWorkflowSmoke(
        std::string_view editableDefinitionId);

    void showSoundDefinition(std::string definitionId);
    // The editor of the Sound being edited to the
    // front, its signal flow, selection and open parameters as they are.
    void bringSoundEditorToFront(const std::string& definitionId);
    // Whether a command tests the value typed into the field.
    [[nodiscard]] talas::app::FieldTestMoment fieldTestMomentOfCommand(juce::CommandID commandId);

    void showRootGraph();

    void navigateStatusDiagnostic(
        const talas::app::SoundWorkbenchDiagnosticIdentity& identity);

    void activateStatusDiagnostic(
        const WorkbenchStatusDiagnostic& diagnostic);

    void openChildDefinition(
        std::optional<std::string> parentDefinitionId,
        std::string childDefinitionId);

    void openParentDefinition();

    void applyHierarchyContexts();

    [[nodiscard]] static juce::String graphContextPresentation(
        const std::optional<std::string>& definitionId);

    void refreshGraphContextPresentation();

    void reconcileHierarchyAfterHistoryEdit();

    void setAuditionPresentation(
        std::optional<std::string> definitionId,
        bool controlsCurrentProject = false);

    void auditionSoundDefinition(std::string definitionId);

    void startSoundLibraryAudition(
        std::string stableEntryId,
        bool routeControllerToProject = false);

    void startSoundLibraryAuditionWithProfile(
        std::string stableEntryId,
        double sampleRate,
        std::size_t maxBlockFrames,
        std::uint64_t sourceDeviceGeneration,
        bool routeControllerToProject = false);

    void stopSoundLibraryAudition();

    void finishSoundLibraryAuditionIfReady();

    void processSoundLibraryControllerRoute();

    void startPendingSoundLibraryAuditionIfReady();

    void retireSoundLibraryAuditionIfQuiescent();

    [[nodiscard]] std::optional<std::string>
    runSoundLibraryPrivateAuditionSmoke(
        const std::filesystem::path& smokeDirectory,
        std::string_view definitionId);

    [[nodiscard]] std::optional<std::string>
    runSoundLibraryPendingSwitchPauseSmoke(
        std::string_view retainedDefinitionId,
        std::string_view replacementDefinitionId);

    [[nodiscard]] std::optional<std::string>
    waitForSoundLibraryAuditionSmoke();

    [[nodiscard]] std::optional<std::string>
    waitForSoundLibraryAuditionBarrierSmoke() const;

    [[nodiscard]] std::optional<bool> renderSoundLibraryAudition(
        double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t outputChannelCount,
        std::size_t frameCount,
        talas::runtime::RealtimeAudioInputBlock projectInputs = {}) noexcept;

    [[nodiscard]] std::pair<std::string, std::string>
    nextSoundDefinitionIdentity() const;

    void createSoundDefinition();

    void renameSoundDefinition(std::string definitionId);

    void deleteSoundDefinition(std::string definitionId);

    [[nodiscard]] const talas::project::ProjectGraph* graphForContext(
        const std::optional<std::string>& definitionId) const;

    [[nodiscard]] const talas::project::ProjectNode* nodeForContext(
        const std::optional<std::string>& definitionId,
        std::string_view nodeId) const;

    // The disk button beside a file name field: the file the field names, an
    // editor on it, and what its header says.
    struct FieldFile
    {
        std::string id;
        std::optional<talas::project::ProjectAsset> asset;
        std::filesystem::path path;
    };
    [[nodiscard]] std::optional<FieldFile> fieldFile(
        const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId) const;
    bool openFieldFileEditor(
        const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId);
    [[nodiscard]] std::optional<std::string> fieldFileInformation(
        const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId) const;
    void showFieldFileInformation(
        const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId);
    [[nodiscard]] std::optional<std::string> soundForPastedField();
    // Describe Sound: a help window on a Sound's
    // class and its parameters; the text it shows, for the smoke.
    void describeSound(const talas::core::NodeTypeDescriptor& descriptor);
    // Get info when info, else Describe Sound.
    void describeGraphNode(const std::optional<std::string>& definitionId, std::string_view nodeId, bool info = false);
    // Editable, it takes typing and Evaluate (Ctrl+Y) as a text window does
    //.
    // `red`: the ranges of the text shown red, in order.
    void showHelpWindow(const juce::String& title, const juce::String& text, bool editable = false,
        const std::vector<juce::Range<int>>& red = {});
    // What self debug: and self debugWithLabel:value: showed as an
    // evaluation ran: a dialog of the values,
    // in order; the last shown, for the smoke.
    void showEvaluationDebugLines(const std::vector<std::string>& lines);
    std::vector<std::string> evaluationDebugLines_;
    // XYGraphView's graphs: a window each, the
    // points joined by lines; the last shown, for the smoke.
    void showEvaluationGraphs(const std::vector<talas::project::smalltalk::XYGraph>& graphs);
    std::vector<talas::project::smalltalk::XYGraph> evaluationGraphs_;
    juce::String soundHelpText_;
    // A file name typed into a file name field: located and
    // bound, true when it was; the files and folders found recently.
    bool locateResourceForField(const std::optional<std::string>& definitionId, const std::string& nodeId,
        const std::string& parameterId, const std::string& name);
    // looks: how often Keep looking has searched further in vain.
    void askToLocateFile(const std::optional<std::string>& definitionId, const std::string& nodeId,
        const std::string& parameterId, const std::string& name, bool searchedAgain, std::size_t looks = 0);
    std::unique_ptr<juce::FileChooser> locateFileChooser_;
    // The answer the smokes give the file search's question.
    std::optional<talas::app::FileSearchAnswer> smokeFileSearchAnswers_;
    talas::app::FileLocatorMemory fileLocatorMemory_;
    // The class icons' pictures, which Recycle flushes.
    talas::app::SoundIconPictureCache<juce::Image> classIconPictures_;
    // The active field: the Workbench's text field
    // with the keyboard focus, which the Edit menu's Cut, Copy, Paste and
    // Select All edit.
    [[nodiscard]] juce::TextEditor* activeTextField() const;
    bool activeFieldCommandInfo(juce::ApplicationCommandInfo& info) const;
    bool editActiveField(juce::CommandID command);
    void applyAppearancePreferences();
    // The key a transport command takes.
    [[nodiscard]] static juce::KeyPress transportKeyPress(talas::app::TalasTransportCommand command);
    bool playOrganizerFile(const std::filesystem::path& path);
    // Save as in the spectrum editor: the spectrum written as
    // a spectrum file, and a Sound playing it in the Sound list.
    bool copySpectrumRegion(const talas::project::PartialTrackAsset& region, const std::filesystem::path& destination);
    // Copy in the spectrum editor: the region written as
    // a spectrum file, and a SumOfSines that stretches and scales it in the
    // Sound list, a SpectrumInRAM on its table controlling an OscillatorBank
    //.
    bool copySpectrumRegionAsSumOfSines(const talas::project::PartialTrackAsset& region, const std::filesystem::path& destination);
    // The smoke's active field, the Workbench not being on screen.
    juce::TextEditor* smokeActiveField_ = nullptr;

    [[nodiscard]] std::string nextSoundInstanceId(
        const std::optional<std::string>& parentDefinitionId,
        std::string_view definitionId) const;

    void addSoundInstance(std::string definitionId);

    void extractSelectedNodeToSound();

    void inlineSelectedSound();
    // Action > Expand in a signal flow.
    void expandSelectedSound();

    void addPrototypeAt(
        std::optional<std::string> definitionId,
        std::string typeId,
        talas::project::CanvasPosition position);

    void addPrototypeAt(
        std::string typeId,
        talas::project::CanvasPosition position);

    // A prototype of several Sounds, as AR Drag&Drop, placed on the
    // target's line or at a position as one edit.
    void placePrototypeAssembly(
        const std::optional<std::string>& definitionId,
        std::string_view typeId,
        talas::project::CanvasPosition position,
        const GraphDropTarget& target);

    // A prototype dropped on a line or a Sound.
    void dropPrototypeAt(
        std::optional<std::string> definitionId,
        std::string typeId,
        talas::project::CanvasPosition position,
        GraphDropTarget target);

    // A prototype or a project Sound dropped on the graph.
    void dropItemAt(
        std::optional<std::string> definitionId,
        GraphDropItem item,
        talas::project::CanvasPosition position,
        GraphDropTarget target);
    // A drop on the signal flow, a Sound Browser's Sound or file brought
    // into the project's Sounds first.
    void dropDraggedItemAt(
        std::optional<std::string> definitionId,
        GraphDropItem item,
        talas::project::CanvasPosition position,
        GraphDropTarget target);

    // Asks which of several inputs, a Sound dropped
    // on a line's or a deleted Sound's, then calls `chosen` with it; the
    // smokes choose through smokeInputChoice_.
    void chooseInput(
        juce::String question,
        std::vector<std::string> inputs,
        std::function<void(std::string)> chosen);

    // Paste of one Sound while one is selected.
    void pasteReplacing(
        std::string replacedNodeId, talas::project::ProjectNode replacement, std::optional<bool> replaceAll);

    // Whether a Sound dropped on another asks Replace or Replace all: when
    // both have inputs.
    bool confirmSourceReplacement(std::optional<std::string_view> scope, std::string_view replacedNodeId,
        const talas::project::ProjectNode& replacement, std::function<void()> confirmed);
    [[nodiscard]] bool replacingAsks(
        std::optional<std::string_view> scope,
        std::string_view replacedNodeId,
        const talas::project::ProjectNode& replacement) const;

    [[nodiscard]] std::optional<talas::project::ProjectNode> makePrototypeNode(
        const std::optional<std::string>& definitionId,
        std::string_view typeId) const;

    // Cut, Copy and Paste of graph nodes through the clipboard.
    void cutSelectedNodes(std::vector<std::string> nodeIds = {});

    bool copySelectedNodes(std::vector<std::string> nodeIds = {});

    void pasteGraphNodes(
        std::optional<talas::project::CanvasPosition> position = std::nullopt);

    [[nodiscard]] juce::String graphClipboardText() const;

    // The Fake Keyboard in the Tools menu.
    void showFakeKeyboard();
    void showTapeRecorder();

    void playOnScreenKeyboardNote(const talas::app::OnScreenKeyboardNote& note);

    // The Live Grid editor in the Tools menu.
    void showLiveGridEditor();

    // The Tools menu's Spectral Analysis and RE Analysis.
    void showAnalysisTool(AnalysisToolPanel::Kind kind);

    void addAnalysisExample(const AnalysisToolPanel::Completed& completed);
    void showSynchronizingSpectra();
    // A spectrum file chosen for a field that reads a table: its table
    // written beside it and bound in its place; false when the file or
    // field is no such pair.
    bool bindSpectrumFileAsTable(const std::optional<std::string>& definitionId, const std::string& nodeId,
        const std::string& parameterId, const std::filesystem::path& file);
    void addSpectrumExample(const SynchronizingSpectraPanel::Completed& completed);

    // The Tools menu's CDP Process to Sample (ADR-0018 amendment).
    void showCdpProcessTool();

    void addCdpProcessExample(const CdpProcessToolPanel::Completed& completed);

    void setGraphClipboardText(const juce::String& text);

    std::string nextNodeId(
        const std::optional<std::string>& definitionId,
        const talas::core::NodeTypeDescriptor& descriptor) const;

    void refreshPerformancePanelSelection();

    void focusPerformanceControlInStructure(
        const talas::app::SoundPerformanceControlIdentity& identity);

    [[nodiscard]] std::optional<std::string>
    runE4HierarchyNavigationSmoke(
        const std::string& definitionId,
        const std::string& instanceId,
        const std::string& scopedSelection,
        const talas::project::WorkspaceSessionState& restoreSession);

    [[nodiscard]] std::optional<std::string> runAccessibilityScaleSmoke(
        const std::filesystem::path& smokeDirectory,
        std::string_view nodeId);
    [[nodiscard]] std::optional<std::string>
    verifyAssetRecoveryContextHelpForSmoke();

    void selectNode(std::optional<std::string> definitionId,
        std::string nodeId,
        bool extendSelection = false);

    void publishGraphSelection(
        std::optional<std::string> definitionId,
        std::vector<std::string> nodeIds,
        std::string primaryNodeId);

    void selectNode(std::string nodeId);
    // A Sound clicked in a signal flow: refused while a field holds an
    // illegal value.
    void selectNodeFromCanvas(std::optional<std::string> definitionId, std::string nodeId, bool extendSelection);
    // The graph's Rename and Clean Up.
    void promptRenameGraphNode(
        std::optional<std::string> definitionId,
        std::string nodeId);
    bool renameGraphNode(
        const std::optional<std::string>& definitionId,
        const std::string& nodeId,
        std::string newNodeId);
    bool cleanUpGraphLayout(const std::optional<std::string>& definitionId);
    void cleanUpFrontWindow();
    // Tools > Expressions Library.
    void showExpressionsLibrary();
    // Tools > Sample Editor; a saved sample the
    // project then plays.
    void showSampleEditor();
    void useEditedSample(const std::filesystem::path& file);
    // Tools > Global Map.
    void showGlobalMap();
    // A MIDI file imported into the Timeline: a Sound of a MIDIVoice that
    // plays it, on a new track for the file's length.
    void chooseMidiFileForTimeline();
    // Tools > Class Editor.
    void showClassEditor();
    // Action > New Class from Example... and Edit Class: the selection
    // extracted into a Sound of its
    // own, or the selected Sound's class, in the Class Editor.
    void newClassFromExample();
    void editSelectedClass();
    // Action > Retrieve Example from Class: the Sound
    // the class in the Class Editor (or the selection's) was made from.
    void retrieveExampleFromClass();
    // Action > Compile to disk...: the grid in the Live Grid editor, its boxes
    // out of date or All Sounds, else the selected Sound into a file of its
    // own, which File > Play... plays. The editor's On box plays the grid's
    // compiled Sounds.
    // Where the dimmed operations apply.
    [[nodiscard]] talas::app::TalasMenuContext talasMenuContext() const;
    void compileToDisk();
    bool compileSoundToDisk(const std::string& definitionId, const std::filesystem::path& file);
    bool compileGridToDisk(const std::string& liveSetId, bool allSounds);
    bool playCompiledSoundFile(const std::filesystem::path& path);
    bool setLiveGridOn(const std::string& liveSetId, bool on);
    // A Tool's playSoundNumber: or playSoundNamed: plays the
    // box's compiled Sound; the grid must be compiled first.
    bool playToolGridSound(const std::string& grid, std::size_t number, const std::string& name, bool compile);
    [[nodiscard]] std::filesystem::path compiledGridFolder() const;
    // Action > Set default Sound and Set default collection: the
    // prototype selected, else the Sounds selected.
    void setDefaultSounds(bool collection);
    // Closing the Class Editor:
    // asked to save the changes not kept, drop them or stay, `answer`
    // choosing without asking; whether it closed.
    bool closeClassEditor(std::optional<bool> answer);
    bool keepOrDropClassChanges(bool save);
    // The first close on a class New Class from Example made asks whether
    // to save an instance of it; No starts over. The project before the class, while the question is to come.
    bool keepNewClass(bool keep);
    std::optional<talas::project::ProjectDocument> newClassStart_;
    // New Class from Example in the Sound file window: the class
    // made from the Sound selected there, whose
    // instance Yes on the editor's first close saves in that window.
    void newClassFromSoundFileExample(const std::string& exampleId);
    void saveNewClassInstanceInSoundFile(const std::string& classId);
    std::optional<std::string> newClassInSoundFile_;
    // Tools > Design Alternate Tunings; its example
    // Sound joins the project.
    void showAlternateTunings();
    void showStructureAsText();
    std::optional<std::string> frontWindowStructureText();
    // The Structure as Text window showing a Sound read as text (read only).
    void showStructureText(const std::string& text);
    // The Sound editor's environment:
    // a variable no declaration names asks for a value as its Sound plays,
    // and keeps it until the environment is reset; it is not saved.
    // `then` follows the values given, playing the project when empty.
    // Operations that stage nothing ask without GenericSource's Ask box.
    [[nodiscard]] bool askForVariableValues(std::function<void()> then = {}, bool askGenericSource = true);
    // GenericSource's Ask box: each play asks for
    // the source, Disk on Enter; the answer stages a copy of the project.
    [[nodiscard]] bool askForGenericSource(std::function<void()> then);
    std::optional<talas::app::GenericSourceAnswer> smokeGenericSourceAnswer_;
    bool genericSourceAnswered_ = false;
    void showVariableEnvironment();
    void resetVariableEnvironment();
    // The preferences file, the File Organizer and files dragged
    // in.
    void loadPreferences();
    void savePreferences();
    void showSpeakerPlacement();
    void applyStartupPreferences();
    void showPreferencesDialog();
    // The Frequently Used Folders preference's window.
    void showFrequentlyUsedFolders();
    void setPreferences(talas::app::WorkbenchPreferences preferences);
    [[nodiscard]] std::vector<std::filesystem::path> fileOrganizerRoots() const;
    void showFileOrganizer();
    // From File > Open..., always Talas's own editor.
    talas::app::TalasFileOpening openTalasFile(const std::filesystem::path& path, bool fromOpenCommand = false);
    // The new Sound's definition, if the file made one.
    std::optional<std::string> addSoundForTalasFile(const std::filesystem::path& path);
    bool openSpectrumEditor(const std::filesystem::path& path);
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    talas::app::WorkbenchPreferences preferences_;
    std::unique_ptr<FileOrganizerPanel> fileOrganizerPanel_;
    std::unique_ptr<juce::DocumentWindow> fileOrganizerWindow_;
    // The frequently used folders still to search for missing assets, and
    // whether a search of them began.
    std::vector<std::filesystem::path> pendingFrequentFolderSearches_;
    bool frequentFolderSearchStarted_ = false;
    // Smokes ask how a file would open without opening it.
    bool smokeRecordsFileOpenings_ = false;
    // The add-or-replace menu's item the smoke picks for a Sound dropped into
    // an input that holds Sounds, in place of the menu.
    std::optional<int> smokeInputDropChoice_;
    // The Sample Editor's Play: a buffer the audio
    // callback adds to the outputs until it runs out.
    struct SampleAudition
    {
        std::vector<std::vector<float>> channels;
        double step = 1.0;
        bool loop = false;
    };
    void playSampleBuffer(std::vector<std::vector<double>> channels, double sampleRate, bool loop = false);
    // Zoom in or out (Ctrl+] and Ctrl+[): the active field's graphic editor,
    // or else the signal flow.
    void zoomActiveDisplay(bool in);
    // A DiskSplicer's Segments field: the project's sample files, playing,
    // and the file list of its disk button.
    void connectDiskSplicerField();
    void chooseSplicedFile(std::function<void(std::string)> chosen);
    [[nodiscard]] std::optional<std::string> holdSampleFile(const std::filesystem::path& file);
    void stopSampleBuffer() noexcept;
    void mixSampleAudition(float* const* outputChannelData, int numOutputChannels, std::size_t outputFrames) noexcept;
    std::unique_ptr<SampleAudition> sampleAuditionOwned_;
    std::atomic<const SampleAudition*> sampleAudition_{nullptr};
    std::atomic<double> sampleAuditionPosition_{0.0};
    std::atomic<int> sampleAuditionReaders_{0};
    std::atomic<bool> sampleAuditionPaused_{false};
    // The Sound Browser: its windows, the one
    // replacement input they share, and the Sound it plays on a bridge of
    // its own that the audio callback renders in place of the project.
    struct SoundBrowserWindow
    {
        std::unique_ptr<SoundBrowserPanel> panel;
        std::unique_ptr<juce::DocumentWindow> window;
        std::optional<std::filesystem::path> folder;
    };
    struct SoundBrowserAudition
    {
        talas::project::ProjectDocument document;
        std::unique_ptr<talas::runtime::ProjectRuntimeBridge> bridge;
        std::size_t outputs = 0;
        bool playing = true;
    };
    // The Timeline's marked Sounds compiled and
    // played alone, or all of them again.
    void playTimelineMarked(bool markedOnly);
    // The File and Edit menu items:
    // Open any..., Play..., Choose window..., Ctrl+Up and Ctrl+Down through
    // the open windows, and Large window... for the active field.
    [[nodiscard]] static bool isWindowMenuCommand(juce::CommandID id) noexcept;
    void windowMenuCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info);
    void performWindowMenuCommand(juce::CommandID id);
    void openAnyFile();
    // File > Open...: the file
    // type chosen first, Edit Tool and Use Tool among them, then a file.
    void openFileOfChosenType();
    void playFileChosen();
    bool playFileAtPath(const std::filesystem::path& path);
    void chooseWindow();
    void cycleWindows(int step);
    void showLargeWindow();
    void showSoundBrowser(std::optional<std::filesystem::path> folder = std::nullopt);
    bool playFromSoundBrowser(const talas::app::SoundBrowserRow& row, bool once = false);
    bool startSoundBrowserAudition(talas::project::ProjectDocument document, const std::filesystem::path& sourcePath, std::size_t outputs,
        std::vector<std::pair<std::string, double>> variables = {});
    void stopSoundBrowserAudition() noexcept;
    // A MIDI key reaches the audition's Sound as well as the project's.
    void forwardMidiNoteToAudition(bool on, int channel, std::uint8_t note, std::uint8_t velocity, double arrivalTimeSeconds) noexcept;
    void stopSoundBrowser();
    void pauseOrResumeSoundBrowser();
    // The Sound's definition in the project, if it was brought in.
    std::optional<std::string> addSoundFromBrowser(const talas::app::SoundBrowserRow& row);
    // A Sound Browser drag dropped in a window: the Sound or the
    // file's Sound brought into the project's Sounds.
    std::optional<std::string> importBrowserDrag(const std::string& description);
    // Ctrl+C on a Sound or Sound file: its surface's
    // widgets, for pasting into an unlocked surface.
    bool copyWidgetsFromSoundBrowser(const talas::app::SoundBrowserRow& row, bool toSystemClipboard = true);
    [[nodiscard]] std::optional<bool> renderSoundBrowserAudition(double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t outputChannelCount,
        std::size_t frameCount,
        talas::runtime::RealtimeAudioInputBlock inputs) noexcept;
    [[nodiscard]] std::size_t soundBrowserAuditionOutputs() const noexcept;
    [[nodiscard]] SoundBrowserPanel* soundBrowserForSmoke(std::size_t index) const noexcept;
    std::vector<SoundBrowserWindow> soundBrowsers_;
    // The Sound file window: the project's Sounds as icons.
    void showSoundFileWindow();
    void playFromSoundFileWindow(const std::string& definitionId);
    void playInTurnFromSoundFileWindow(const std::vector<std::string>& selected);
    void playPrototype(const std::string& typeId);
    // A Sound not of the project, played in a copy of it with the Sound.
    bool playAddedSound(talas::project::ProjectDocument document, talas::project::ProjectSoundDefinition sound);
    void duplicateSoundDefinitions(const std::vector<std::string>& definitionIds);
    void saveSoundCopy(const std::string& definitionId);
    bool sampleEditorMayClose();
    // A parameter field's problem dropped, or editing
    // continued, before the Sound editor closes.
    bool illegalValueMayClose();
    // The Sound editor's close box:
    // a changed Sound asks Keep, Drop or Cancel; the editor keeps the Sound
    // as it opened on it (or as last saved), for Drop; the smokes' answer,
    // 0 to 2.
    void closeSoundEditor();
    juce::TextButton soundEditorCloseBox_;
    std::optional<int> smokeSoundEditorCloseAnswer_;
    // Several Sound editors, one on each Sound: the
    // one in front in the Sound editor pane, the others behind it as tabs.
    // Brought up to date with the project and the pane's path: the editors,
    // their tabs, and the window Undo belongs to.
    void syncSoundEditors();
    void refreshSoundEditors();
    void layoutSoundEditorTabs();
    // A tab's close box: the editor in front closes as its close box does;
    // one behind closes at once unless its Sound changed, when it comes to
    // the front to ask.
    void closeSoundEditorTab(const std::string& definitionId);
    talas::app::SoundEditorWindows soundEditors_;
    std::uint64_t soundEditorsReplacement_ = 0;
    SoundEditorTabs soundEditorTabs_;
    // The window in front, which Edit > Undo takes
    // its edits from: a tool window by its name, or (empty) the main
    // window, the Sound editors' and the project's signal flow's.
    void noteFrontWindow(juce::Component* focused);
    [[nodiscard]] talas::project::ProjectEditor::HistoryWindow frontHistoryWindow() const;
    void updateHistoryWindow();
    [[nodiscard]] bool soundFileWindowInFront() const;
    std::string frontWindowName_;
    // A Sound of the Sound file window that is a
    // SoundCollectionVariable (or Variable) alone renames the variable with
    // it; whether the Sound was one.
    bool renameSoundFileVariable(const std::string& definitionId, const std::string& name);
    std::optional<std::string> addSoundCopiedOnClipboard();
    // The sample file the sample editor last copied from.
    std::filesystem::path sampleClipSource_;
    void pasteIntoSoundFile();
    void expandSoundDefinitions(const std::vector<std::string>& definitionIds);
    // MIDI notes off (Ctrl+M), from the note
    // given, a quarter of the control queue at a time.
    void sendMidiNotesOff(std::size_t from = 0);
    void showMidiMessages();
    // DSP > Configure MIDI...: the Default Input Channel, and the MIDI messages arriving.
    void showConfigureMidi();
    void setDefaultMidiInputChannel(int channel);
    // DSP > Initialize DSP: the audio device closed
    // and opened again; and the dialog when it stops answering.
    void initializeDsp();
    void watchDspDevice();
    // A file looked for in the folders with the eyeglasses for a cursor
    //, for some time: `looks` budgets of entries
    //.
    [[nodiscard]] talas::app::FileSearchOutcome locateFileLooking(
        std::string_view name, const std::vector<std::filesystem::path>& folders, std::size_t looks);
    // Describe Sound: the prototype
    // focused, else the Sound selected in the signal flow.
    void describeFrontSound();
    // A Sound or prototype dropped on a line's plus: a Mixer takes the
    // line and mixes it with the Sound.
    void mixDroppedItemIntoLine(std::optional<std::string> definitionId, GraphDropItem item,
        talas::project::CanvasPosition position, const talas::core::Connection& line);
    // Global map files: one opened in
    // the global map editor, or chosen as the project's map, from the
    // Global Maps folder (Default.map written there when it is missing).
    void openGlobalMapFile(const std::filesystem::path& path);
    void chooseGlobalMap();
    bool useGlobalMapFile(const std::filesystem::path& path);
    // From the MIDI input's thread: the message, to the window if it is open.
    void noteMidiMessageForWindow(const juce::MidiMessage& message);
    void appendMidiMessageLine(std::span<const std::uint8_t> bytes);
    // Before Sounds are removed, a question about each: Yes, Remove
    // all or Cancel; those to remove go together.
    void askToRemove(talas::app::SoundRemoval removal, std::function<juce::String(const std::string&)> question,
        std::function<void(std::vector<std::string>)> remove);
    // A signal flow's question for each Sound deleted, one alone with inputs
    // replaced by its input.
    std::function<juce::String(const std::string&)> graphRemovalQuestion(
        const std::optional<std::string>& definitionId, bool alone);
    std::optional<std::string> addSoundForPrototype(const std::string& typeId);
    // The text file editor: a new text file, or
    // one opened.
    void showTextFile(std::optional<std::filesystem::path> path);
    std::unique_ptr<TextFilePanel> textFilePanel_;
    std::unique_ptr<juce::DocumentWindow> textFileWindow_;
    // Sound collections: Collect... at a level, the
    // name asked for when none is given, and renaming one.
    void collectSounds(const std::optional<std::string>& level, const std::vector<std::string>& sounds,
        const std::vector<std::string>& collections, const std::optional<std::string>& name);
    void renameSoundCollection(const std::string& collectionId);
    // A class's own icon: its PNG picture, and the
    // inner icon it shows.
    [[nodiscard]] juce::Image classIconImage(const std::string& definitionId);
    [[nodiscard]] std::vector<talas::app::IconGlyph> classIconGlyphs(const std::string& definitionId);
    [[nodiscard]] SoundFilePanel* soundFileForSmoke() const noexcept { return soundFilePanel_.get(); }
    std::unique_ptr<SoundFilePanel> soundFilePanel_;
    std::unique_ptr<juce::DocumentWindow> soundFileWindow_;
    std::optional<std::string> soundFileLastPlayed_;
    // A Sound file opened as a custom prototype strip.
    void openCustomPrototypes();
    bool showCustomPrototypes(const std::filesystem::path& path);
    [[nodiscard]] CustomPrototypesPanel* customPrototypesForSmoke() const noexcept { return customPrototypesPanel_.get(); }
    std::unique_ptr<CustomPrototypesPanel> customPrototypesPanel_;
    std::unique_ptr<juce::DocumentWindow> customPrototypesWindow_;
    std::filesystem::path customPrototypesPath_;
    // The Tool editor on a Tool file, and the
    // Tool window running it.
    void showToolEditor(talas::project::tool::Definition tool, std::optional<std::filesystem::path> path);
    // Open with Edit tool, or with Use tool to run it.
    void openToolFile(bool use);
    // File > New...: a new file of the type chosen.
    void newFileOfType(const std::string& type);
    // Tools > File Archivist: a Sound file
    // found on the disk, then where its archive goes; the package made there
    // holds the file and a copy of each file it uses.
    void showFileArchivist();
    // Save as...: the obsolete layouts and presets
    // offered for removal first, then `then`.
    void offerObsoleteMaterialRemoval(std::function<void()> then);
    // Info > Oscilloscope and Spectrum analyzer: the
    // selected Sound with the display after it, staged next in the project's
    // place and shown in the Analysis workspace; or, for Ctrl+Space in the
    // signal flow, the selected Sound alone, with no display.
    bool prepareInfoDisplay(talas::app::InfoDisplay display);
    void showInfoDisplay(talas::app::InfoDisplay display);
    [[nodiscard]] bool signalFlowHasFocus() const;
    // MIDI pitch bend scrubbing the spectrum shown.
    void scrubSpectraForPitchBend(int bend);
    bool scrubShownSpectrum(int bend);
    std::optional<talas::app::InfoDisplayDocument> infoDisplayStage_;
    bool signalFlowFocusForSmoke_ = false;
    // The surface taken as in front, and the columns its Clean up is given.
    bool vcsFocusForSmoke_ = false;
    std::optional<unsigned> smokeCleanUpColumns_;
    bool archiveProjectFile(const std::filesystem::path& path);
    std::optional<std::pair<talas::project::ProjectDocument, std::filesystem::path>> archivedProject_;
    struct PackageSource
    {
        talas::project::ProjectDocument document;
        std::optional<std::filesystem::path> path;
        bool archived = false;
    };
    [[nodiscard]] PackageSource takePackageSource();
    // Edit > View...: the front window's view choices.
    [[nodiscard]] talas::app::ViewedWindow viewedWindow() const;
    void showViewChoices();
    bool applyViewChoice(talas::app::ViewedWindow window, const std::string& name);
    [[nodiscard]] bool viewChoiceTicked(talas::app::ViewedWindow window, const std::string& name) const;
    // The Tools of the Tools folders on the Tools menu.
    void appendSavedToolsMenu(juce::PopupMenu& menu);
    // The project's field reading settings (format 156), each ticked where
    // the project takes that item's reading, and a choice of one toggling
    // it; false for an item of another menu.
    void appendFieldReadingMenu(juce::PopupMenu& menu);
    bool selectFieldReading(int menuItemId);
    bool selectSavedTool(int menuItemId);
    std::vector<std::filesystem::path> savedTools_;
    // File > Documentation, the documents of the Documentation folders
    //.
    [[nodiscard]] juce::PopupMenu documentationMenu();
    bool selectDocumentation(int menuItemId);
    std::vector<std::filesystem::path> documentationFiles_;
    bool openToolAt(const std::filesystem::path& path, bool use);
    void openCompiledSoundGridFile(const std::filesystem::path& path);
    void saveToolFile(const talas::project::tool::Definition& tool, bool saveAs);
    void runTool(const talas::project::tool::Definition& tool);
    [[nodiscard]] ToolEditorPanel* toolEditorForSmoke() const noexcept { return toolEditorPanel_.get(); }
    [[nodiscard]] ToolRunPanel* toolRunForSmoke() const noexcept { return toolRunPanel_.get(); }
    std::unique_ptr<ToolEditorPanel> toolEditorPanel_;
    std::unique_ptr<juce::DocumentWindow> toolEditorWindow_;
    std::optional<std::filesystem::path> toolPath_;
    std::unique_ptr<ToolRunPanel> toolRunPanel_;
    std::unique_ptr<juce::DocumentWindow> toolRunWindow_;
    talas::app::SoundBrowserReplacementInput soundBrowserInput_;
    std::optional<talas::app::SoundBrowserRow> soundBrowserLastPlayed_;
    std::unique_ptr<SoundBrowserAudition> soundBrowserAudition_;
    std::atomic<SoundBrowserAudition*> soundBrowserAuditionLive_{nullptr};
    std::atomic<int> soundBrowserAuditionReaders_{0};
    bool soundBrowserUsesPreparedProfileForSmoke_ = false;
    // The surface of the Sound the browser plays, in
    // a window of its own, taking changes only when the Sound was played
    // with the substitute input disengaged, and saved with the Sound in its
    // Sound file. A value reaches the audition it was shown for, not one
    // started since (the Design Alternate Tunings Tool's).
    void showSoundBrowserSurface(const talas::project::ProjectDocument& soundFile, const talas::app::SoundBrowserRow& row);
    bool saveSoundBrowserSurface(const talas::app::SoundBrowserSurface& surface);
    std::unique_ptr<SoundBrowserSurfacePanel> soundBrowserSurfacePanel_;
    std::unique_ptr<juce::DocumentWindow> soundBrowserSurfaceWindow_;
    std::filesystem::path soundBrowserSurfaceFile_;
    std::uint64_t soundBrowserAuditionGeneration_ = 0;
    std::uint64_t soundBrowserSurfaceGeneration_ = 0;
    // Info > Full waveform: the selected Sound
    // recorded for a time, drawn with its minimum and maximum.
    void showFullWaveform();
    bool recordFullWaveform(double seconds);
    std::unique_ptr<juce::Component> fullWaveformPanel_;
    std::unique_ptr<juce::DocumentWindow> fullWaveformWindow_;
    std::optional<talas::app::WaveformExtremes> lastFullWaveformExtremes_;
    // The DSP status window.
    void showDspStatus();
    [[nodiscard]] DspStatusReading dspStatusReading() const;
    // Evaluate (Ctrl+Y) runs
    // a text field's selection as Smalltalk and puts what it prints just
    // after it, selected; false, with the error in the status, when it
    // fails.
    // answers and seeds: the questions answered so far and the random
    // seeds drawn before them.
    bool evaluateSelectionIn(juce::TextEditor& editor, std::vector<talas::project::smalltalk::PromptAnswer> answers = {},
        std::vector<std::uint32_t> seeds = {});
    void askEvaluationQuestion(juce::TextEditor& editor, const talas::project::smalltalk::Prompt& prompt,
        std::vector<talas::project::smalltalk::PromptAnswer> answers, std::vector<std::uint32_t> seeds);
    // The answers the smokes give the questions, in place of a dialog.
    std::deque<talas::project::smalltalk::PromptAnswer> smokePromptAnswers_;
    std::unique_ptr<juce::FileChooser> evaluationFileChooser_;
    // Edit > Find, Find again, Replace and Replace again in the active text
    // field; not found, the computer beeps.
    // Action > Revert: the selected Sound's
    // parameters as they were when it was opened.
    void revertSelectedSound();
    // Edit > Paste hot: an expression or an
    // Event Value name from a list, pasted at the caret of the active field.
    void pasteHotIntoFocusedField();
    // Edit > Paste special: the copied
    // Sound itself, not a copy, in the selected Sound's place.
    void pasteSpecialGraphNode();
    bool pasteSpecialIntoInput(const std::string& nodeId, const std::string& portId);
    // Set replaceable input on the selected
    // Sound of the edited Sound.
    void setReplaceableInputOf(const std::optional<std::string>& definitionId, std::string_view nodeId);
    bool pasteHotInto(juce::TextEditor& editor, const juce::String& typed, int listIndex);
    struct RevertSnapshot
    {
        std::optional<std::string> definitionId;
        std::string nodeId;
        std::vector<talas::project::ProjectParameterExpression> parameters;
    };
    std::optional<RevertSnapshot> revertSnapshot_;
    void findInFocusedField(bool again);
    void replaceInFocusedField(bool again);
    bool findInEditor(juce::TextEditor& editor, const juce::String& needle);
    bool replaceInEditor(juce::TextEditor& editor, const juce::String& needle, const juce::String& replacement, bool all);
    juce::String lastFindText_;
    juce::String lastReplaceText_;
    // Escape in a
    // parameter's value field, then MIDI, types the Event Value the MIDI
    // names there (talas::project::EventValueLearning).
    void beginEventValueLearning(juce::TextEditor& field);
    void learnFromMidi(const juce::MidiMessage& message);
    // Ctrl+Y in the focused text field, which a TextEditor would otherwise
    // take as Redo, and Escape in a parameter's value field.
    struct EvaluateKeys final : juce::KeyListener
    {
        explicit EvaluateKeys(WorkbenchComponent& workbench) noexcept : owner(workbench) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override;
        WorkbenchComponent& owner;
    };
    // A click with Shift held, anywhere, stops a
    // recording to disk or a Sound playing from the disk.
    struct DiskShiftClick final : juce::MouseListener
    {
        explicit DiskShiftClick(WorkbenchComponent& workbench) noexcept : owner(workbench) {}
        void mouseDown(const juce::MouseEvent& event) override { (void)owner.stopDiskOperationOnClick(event.mods); }
        WorkbenchComponent& owner;
    };
    bool stopDiskOperationOnClick(juce::ModifierKeys modifiers);
    bool addTuningExample(const talas::project::tunings::Tuning& tuning,
        const talas::project::tunings::KeyFrequencies& frequencies, talas::app::TuningExampleInstrument instrument,
        const std::filesystem::path& sample);
    // While the tuning Tool is open its
    // example plays as the audition, for the MIDI keyboard to try out; the
    // audition it started stops as the Tool closes.
    void tryOutTuning(const talas::project::tunings::Tuning& tuning, const talas::project::tunings::KeyFrequencies& frequencies,
        talas::app::TuningExampleInstrument instrument, const std::filesystem::path& sample);
    void stopTuningTryOut() noexcept;
    const void* tuningTryOutAudition_ = nullptr;
    bool importMidiFileToTimeline(const std::filesystem::path& file);

    void documentChanged(bool refreshVirtualControls = true);

    void requestProjectExpressionsUpgradeToSessionV1();

    void performProjectExpressionsUpgradeToSessionV1();

    void requestProjectExpressionsUpgradeToVariablesV1();

    void performProjectExpressionsUpgradeToVariablesV1();

    void requestProjectExpressionsUpgradeToVariablesV2();
    void requestProjectExpressionsUpgradeToVariablesV3();
    void requestProjectExpressionsUpgradeToVariablesV4();
    void requestProjectExpressionsUpgradeToValV1();
    void requestProjectExpressionsUpgradeToVoiceV1();

    void performProjectExpressionsUpgradeToVariablesV2();
    void performProjectExpressionsUpgradeToVariablesV3();
    void performProjectExpressionsUpgradeToVariablesV4();
    void performProjectExpressionsUpgradeToValV1();
    void performProjectExpressionsUpgradeToVoiceV1();

    void snapshotAuthoringChanged();

    void scheduleStartupRecoveryCheck();

    void offerStartupRecovery();

    static juce::String recoveryVersionLabel(
        const talas::project::ProjectRecoveryVersion& version,
        bool newest);

    void offerInvalidRecoveryChoice(const std::string& error);

    void restoreRecovery(
        talas::project::ProjectRecoveryRecord record,
        std::size_t versionIndex,
        std::optional<std::filesystem::path> safeProjectPath,
        std::optional<
            talas::project::ProjectFileRevision>
            safeProjectRevision);

    void enableRecoveryMonitoring();

    void updateRecoveryAfterDocumentChange();

    void startRecoveryWriteIfDue();

    void finishRecoveryWriteIfReady();

    void consumeRecoveryWriteResult();

    void stopRecoveryWriter();

    bool clearProjectRecovery();

    // In a window: only its own newest edit; with
    // none given, the project's newest edit whichever window made it.
    void undoProject(std::optional<talas::project::ProjectEditor::HistoryWindow> window = std::nullopt);

    void redoProject(std::optional<talas::project::ProjectEditor::HistoryWindow> window = std::nullopt);

    void deleteSelectedNode();

    void duplicateSelectedNodes(
        std::vector<std::string> nodeIds = {});

    void selectAllGraphNodes();

    void focusPrototypeSearch();

    void performGraphKeyboardCommand(
        GraphKeyboardCommandRequest request);

    // Delete asks about each Sound first; Cut does not.
    void requestDeleteSelectedNodes(
        std::optional<std::string> definitionId,
        std::vector<std::string> nodeIds,
        bool askFirst = true);

    void performDeleteNodes(
        const std::optional<std::string>& definitionId,
        std::span<const std::string> nodeIds,
        std::optional<std::string> inputNodeId = std::nullopt);

    void handlePreservedNodeAction(
        std::optional<std::string> definitionId,
        std::string nodeId,
        PreservedNodeAction action);

    void beginComponentReplacement(
        std::optional<std::string> definitionId,
        std::string nodeId);

    void confirmComponentReplacement(
        std::optional<std::string> definitionId,
        std::string_view nodeId);

    void cancelComponentReplacement(bool dismissOnly = false);

    void finishComponentReplacementIfReady();

    void stopComponentReplacement() noexcept;

    void invalidateComponentReplacementForDocumentChange() noexcept;

    void requestDeleteNode(
        std::optional<std::string> definitionId,
        std::string nodeId);

    void performDeleteNode(
        const std::optional<std::string>& definitionId,
        std::string_view nodeId);

    void requestNewProject();

    void requestOpenProject();

    void requestOpenRecentProject(std::filesystem::path path);

    void showOverflowMenu();

    void showWorkbenchGuide();

    void requestAfterDiscardConfirmation(std::function<void()> action);

    std::string currentDocumentSnapshot() const;

    void scheduleDestructiveContinuation(
        std::function<void()> action,
        std::string expectedSnapshot);

    void scheduleContinuation(std::function<void()> action);

    void newProjectNow();

    void openProject();

    void browseResource(
        std::optional<std::string> definitionId,
        std::string nodeId,
        std::string parameterId);

    void bindResourceFile(
                          std::optional<std::string> definitionId,
                          std::string nodeId,
                          std::string parameterId,
                          const std::filesystem::path& selectedPath);

    talas::project::ProjectAsset makeAssetReference(const std::filesystem::path& selectedPath) const;
    [[nodiscard]] bool importAssetIntoPackage(talas::project::ProjectAsset& asset,
        const std::filesystem::path& selectedPath, std::optional<std::filesystem::path>& importedPath);

    static talas::project::ProjectAssetKind assetKindForPath(const std::filesystem::path& path);

    static std::filesystem::path normalizedAbsolutePath(const std::filesystem::path& path);

    const std::optional<std::filesystem::path>& projectPath() const noexcept;

    juce::String persistWorkspaceSessionForPath(
        const std::filesystem::path& path);

    juce::String persistCurrentWorkspaceSession();

    void noteWorkspaceSessionChanged();

    [[nodiscard]] bool applyWorkspaceSessionVariableValuesToCurrentStage();

    [[nodiscard]] bool
    applyWorkspaceSessionSoundVariableValuesToCurrentStage();

    void synchronizeWorkspaceSessionVariableValues();

    void synchronizeWorkspaceSessionSoundVariableValues();

    void updateWorkspaceGraphView(
        std::optional<std::string> definitionId,
        double panX,
        double panY,
        double zoom);

    void updateWorkspacePaneSplit(juce::Point<int> position);

    void setWorkspaceStructureSplitOrientation(
        talas::project::WorkspaceStructureSplitOrientation orientation);

    void swapWorkspaceStructurePanes();

    void toggleSecondaryWorkspacePane();

    void setWorkspaceGraphViewMode(
        talas::project::WorkspaceGraphViewMode mode);

    void applyWorkspaceGraphViewMode();

    void applyWorkspaceGraphView(
        GraphCanvas& canvas,
        const std::optional<std::string>& definitionId);

    struct WorkspaceSessionRestore
    {
        bool restored = false;
        juce::String warning;
    };

    WorkspaceSessionRestore restoreWorkspaceSessionForPath(
        const std::filesystem::path& path);

    void applyWorkspaceSessionProjection(bool useStoredTransforms);
    void applyAccessibilityPreferences();
    void refreshContextHelp(juce::Component* focused = nullptr);

    using SaveCompletion = std::function<void(bool)>;

    void saveProject(SaveCompletion completion = {});

    void saveProjectAs(SaveCompletion completion = {});

    void loadProjectFromPath(const std::filesystem::path& path);

    bool saveProjectToPath(
        const std::filesystem::path& path,
        const std::optional<
            talas::project::ProjectFileRevision>&
            expectedRevision = {});

    void loadRecentProjectsState();

    juce::String persistRecentProjects();

    juce::String rememberRecentProject(
        const std::filesystem::path& path);

    void choosePackageDestination();

    void startProjectPackage(
        std::filesystem::path outputPath);

    void cancelProjectPackage();

    [[nodiscard]] TimelineScoreClipArtifactState
    scoreClipArtifactState(std::string_view clipId);

    void performScoreClipArtifactAction(
        std::string clipId,
        TimelineScoreClipArtifactAction action);

    void startScoreClipArtifactPublication(
        std::string clipId,
        talas::project::ScoreClipRenderMode mode);

    void cancelScoreClipArtifactPublication();

    void finishScoreClipArtifactPublicationIfReady();

    void stopScoreClipArtifactPublicationForLifecycle();

    [[nodiscard]] DiskCacheCapturePresentation
    diskCacheCapturePresentation(
        const std::optional<std::string>& definitionId,
        std::string_view nodeId);

    void performDiskCacheCaptureAction(
        std::optional<std::string> definitionId,
        std::string nodeId);

    void startDiskCacheCapture(std::string sourcePath);

    void cancelDiskCacheCapture();

    void finishDiskCacheCaptureIfReady();

    void refreshDiskCacheCaptureStatus();

    void stopDiskCacheCaptureForLifecycle();

    [[nodiscard]] bool fileOperationShouldCancel(
        talas::app::WorkbenchOperationToken token,
        std::uint64_t documentAuthorityGeneration) const noexcept;

    [[nodiscard]] bool admitFileOperationPublication(
        talas::app::WorkbenchOperationToken token,
        std::uint64_t documentAuthorityGeneration) noexcept;

    [[nodiscard]] std::optional<std::string> runFileOperationSmoke(
        const std::filesystem::path& smokeDirectory,
        std::string_view oscillatorId);

    [[nodiscard]] std::optional<std::string> runPreparedStreamDetailSmoke();

    void applyDiskRecorderOutputTakeState(
        AnalysisOutputTakePanelState& state,
        const talas::runtime::PreparedGraphStatusSnapshot& stageStatus);

    [[nodiscard]] std::optional<std::string>
    runDiskRecorderOutputTakeSmoke(
        const std::filesystem::path& smokeDirectory);

    [[nodiscard]] std::optional<std::string>
    runAnalogSequencerExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runStereoRoutingExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runShelvingPresenceExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runCheckedBiquadExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runStaticFirExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runVcfExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runHarmonicResonatorExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runDynamicRangeControllerExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runThresholdExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimeControlExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimeStopperExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runTimeOffsetExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runQuadOscillatorExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runOverlappingMixerExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runOscillatorBankExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runFormantBankExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runFormantBankOscillatorExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumModifierExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runPairedStereoOutputExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSyntheticSpectrumFromSoundsExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runWarpedTimeIndexExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumOnDiskExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumOnDiskDefaultMetadataExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumVoicedUnvoicedExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumInRamExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSpectrumInRamDefaultMetadataExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runGaOscillatorsExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSumOfSinesDefaultAnalysisFrequenciesExampleSmoke();
    std::optional<std::string>
    runSumOfSinesControlledTimeFramesExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSumOfSinesAutomaticGateLoopExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runGenericSourceExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runSampleExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runMultisampleExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runKeyMappedMultisampleExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runBoundedRawMidiOutputExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runBoundedMidiControllerOutputExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runBoundedMidiNoteOutputExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runBoundedMidiFileEchoExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runBoundedGlobalControllerOutputExampleSmoke();
    [[nodiscard]] std::optional<std::string>
    runTriggeredGlobalControllerProjectRouteSmoke();

    [[nodiscard]] std::optional<std::string> runProcessorPlacementSmoke();
    [[nodiscard]] std::optional<std::string>
    runDeviceStopOffMessageThreadSmoke();
    [[nodiscard]] std::optional<std::string> runValueGestureHistorySmoke();

    [[nodiscard]] std::optional<std::string> runDiskCacheCaptureSmoke(
        const std::filesystem::path& smokeDirectory);

    void publishAnalysisOutputTakeFromAudio(
        float* const* projectOutputChannels,
        std::size_t outputFrames,
        bool sourceRendered) noexcept;

    double defaultExportDurationSeconds() const;

    double defaultExportSampleRate();

    static std::optional<double> parsePositiveFiniteDouble(const juce::String& text);

    void showExportAudioDialog();

    // ADR-0029: the open project as a VST3/AU plugin
    // (WorkbenchPluginExport.cpp).
    void showExportPluginDialog();

    void chooseExportDestination(double durationSeconds, double sampleRate);

    void startAudioExport(
        std::filesystem::path outputPath,
        double durationSeconds,
        double sampleRate);

    void cancelAudioExport();

    void resetForDocumentReplacement();

    void prepareForDocumentReplacement();

    void playCurrentProject();
    // Compile & load: the project is
    // compiled and loaded without starting; Restart starts it from the
    // beginning.
    bool compileAndLoadCurrentProject(std::optional<talas::runtime::RenderSettings> profile = std::nullopt);
    void restartCurrentProject();
    // Space and Ctrl+R as the transport's state has them.
    [[nodiscard]] talas::app::TalasTransportState talasTransportState() const noexcept;
    void pressTransportKey(talas::app::TalasTransportPress press);

    void refreshAnalysisWorkspace();

    void pinSelectedAnalysisTarget();

    void removeAnalysisTrace(std::string traceId);

    void selectAnalysisTrace(std::string traceId);

    void moveAnalysisTrace(std::string traceId, int direction);

    void setAnalysisTraceVisible(std::string traceId, bool visible);

    void setAnalysisTraceScale(std::string traceId, double scale);

    void setAnalysisRefreshRateHz(std::uint32_t refreshRateHz);

    void setAnalysisHistoryFrames(std::uint32_t historyFrames);

    void setAnalysisFrozen(bool frozen);

    void restageAnalysisCaptureIfActive();

    void startAnalysisAudition(
        talas::project::WorkspaceAnalysisAuditionState request);

    void startAnalysisAuditionWithProfile(
        talas::project::WorkspaceAnalysisAuditionState request,
        double sampleRate,
        std::size_t maxBlockFrames,
        std::uint64_t sourceDeviceGeneration);

    void stopAnalysisAudition();

    void commitAnalysisAudition();

    void finishAnalysisAuditionIfReady();

    void retireAnalysisAuditionIfQuiescent();

    void chooseAnalysisOutputTakeDestination();

    void chooseAnalysisOutputTakeFile();
    // Record to disk... of the Timeline played live:
    // the job that waits for its end and saves the files.
    [[nodiscard]] std::function<talas::audio::TimelineRecordingResult()> startLiveTimelineRecording(
        talas::audio::TimelineRecordingOptions options);
    [[nodiscard]] bool liveTimelineRecordingActive() const noexcept;
    void countOffThenRecord(
        std::filesystem::path destination,
        talas::audio::ExistingAudioFilePolicy existingPolicy);

    void startAnalysisOutputTake(
        std::filesystem::path destination,
        talas::audio::ExistingAudioFilePolicy existingPolicy);

    void startAnalysisOutputTakeWithProfile(
        std::filesystem::path destination,
        talas::audio::ExistingAudioFilePolicy existingPolicy,
        double sampleRate,
        std::size_t maxBlockFrames,
        std::uint64_t sourceDeviceEpoch,
        std::uint64_t sourceDeviceGeneration,
        juce::AudioIODevice* sourceDevice,
        bool requireCurrentDevice);

    void stopAnalysisOutputTake();

    void cancelAnalysisOutputTake();

    void invalidateAnalysisOutputTakeForProjectAuthorityChange();

    void requestAnalysisOutputTakeStopFromDevice() noexcept;

    void revokeAnalysisOutputTakeCallbackAndWait() noexcept;

    [[nodiscard]] bool analysisOutputTakeDestinationConflicts(
        const std::filesystem::path& destination) const noexcept;

    void collectAnalysisOutputTakeProtectedPaths(
        std::vector<std::filesystem::path>& protectedFiles,
        std::vector<std::filesystem::path>& protectedDirectories) const;

    void finishAnalysisOutputTakeIfReady();

    void stopAnalysisOutputTakeForLifecycle();
    [[nodiscard]] bool adoptPreparedOutputTakeStageForSmoke();

    [[nodiscard]] std::optional<bool> renderAnalysisAudition(
        std::array<const float*, 8> physicalInputChannels,
        double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t outputChannelCount,
        std::size_t frameCount) noexcept;

    void startPartialTrackPreview();
    void startPartialTrackPreviewWithProfile(
        double sampleRate,
        std::size_t maxBlockFrames,
        std::uint64_t sourceDeviceGeneration);
    void stopPartialTrackPreview();
    void finishPartialTrackPreviewIfReady();
    void retirePartialTrackPreviewIfQuiescent();
    [[nodiscard]] std::optional<bool> renderPartialTrackPreview(
        float* const* outputChannels,
        std::size_t outputChannelCount,
        std::size_t frameCount) noexcept;
    void refreshPartialTrackOperationState();
    void choosePartialTrackDerivativeDestination(
        talas::audio::PartialTrackDerivativeKind kind);
    void startPartialTrackDerivative(
        talas::audio::PartialTrackDerivativeKind kind,
        std::filesystem::path destination);
    void cancelPartialTrackDerivative();
    void finishPartialTrackDerivativeIfReady();
    void importLastPartialTrackDerivative();

    bool stageProject(talas::runtime::RenderSettings settings, const juce::String& deviceName);

    [[nodiscard]] std::optional<std::string>
    currentPackageStagingError() const;

    void pollProjectFileChanges();

    [[nodiscard]] std::vector<talas::app::AssetRecoveryWorkbenchItem>
    assetRecoveryItemsForFailure(
        const talas::audio::ProjectAssetLoadFailure& failure) const;

    [[nodiscard]] std::vector<talas::app::AssetRecoveryInventoryEntry>
    assetRecoveryInventoryEntries() const;

    void refreshAssetRecoveryPanel();

    void stopAssetRecoveryWorker();

    void finishAssetRecoveryWorkerIfReady();

    void resetAssetRecovery();

    void startAssetRecoveryInventoryInspection(
        std::vector<talas::app::AssetRecoveryInventoryEntry> entries);

    [[nodiscard]] bool openAssetRecovery(
        const talas::audio::ProjectAssetLoadFailure& failure);

    void openAssetRecoveryForAsset(
        std::string assetId,
        std::string message,
        bool replacementMayBeAccepted);

    void locateAssetRecoveryFile(std::string assetId);

    void startAssetRecoveryCandidateInspection(
        std::string assetId,
        std::filesystem::path candidatePath);

    void searchAssetRecoveryFolder();

    void startAssetRecoveryDirectorySearch(
        std::filesystem::path root);

    void pollAssetRecoveryProgress();

    void cancelAssetRecoveryOperation();

    void applyAssetRecoveryRelocations();

    void replaceAssetRecoveryCandidate(std::string assetId);

    void ignoreAssetRecoveryItem(std::string assetId);

    void selectAssetRecoveryCandidate(
        std::string assetId,
        std::size_t candidateIndex);

    void pollProjectAssetChanges();

    void refreshPartialTrackEditor(
        const talas::core::ResourceSnapshotPtr& snapshot);
    void refreshPartialTrackEditorEdits();
    [[nodiscard]] talas::core::ResourceSnapshotPtr
    partialTrackEditorSnapshotFor(
        const talas::runtime::RenderSettings& settings) const;

    bool acceptPendingProjectAssetReplacement();

    [[nodiscard]] std::optional<std::string>
    firstProjectAssetIdentityConflict() const;

    [[nodiscard]] std::optional<std::string>
    firstChangedLoadedResourceSnapshot(
        std::span<
            const talas::core::ResourceSnapshotPtr>
            snapshots) const;

    bool loadProjectAssets(
        const talas::project::ProjectDocument& document,
        talas::runtime::RenderSettings& settings,
        bool* acceptedInitialInspection = nullptr);

    [[nodiscard]] const talas::project::ProjectDocument&
    compileDiagnosticDocument() const;

    void publishCompileDiagnostics(
        std::span<const talas::runtime::Diagnostic> diagnostics,
        const talas::project::ProjectDocument* sourceDocument = nullptr);
    // A Sound compiled: its diagnostics cleared, and its Scripts' warnings
    // shown.
    void publishCompiledSound();
    // The project, or the document given, compiled and staged with the
    // watch for a cursor.
    [[nodiscard]] talas::runtime::ProjectStageResult compileShowingWatch(
        const talas::project::ProjectDocument* document, talas::runtime::RenderSettings settings);
    // The cursor for what the Workbench is busy with, kept current.
    void refreshActivityCursor(bool compiling = false, bool loadingSamples = false);

    void rebuildMidiRuntimeRoutes(
        bool invalidateRealtimeControlIngress = true);
    void rebuildOscIngress();
    void rebuildOscEgress();
    void rebuildOscEgress(
        const PreparedWorkbenchOscLoopbackProfile& profile);
    void suspendOscLoopbackProfile() noexcept;

    void setPlaying(bool playing);

    void pauseCurrentProject();

    void stopCurrentProject();

    void refreshTransportLabel();

    void refreshWindowTitle();

    void refreshPersistentStatus();

    void setStatus(const juce::String& text);

    void publishDeviceStatus(juce::String status);

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    juce::String applyProjectDeviceSettings();

    void scheduleClearAfterAudioStopped();

    void showAudioSettings();

    juce::AudioDeviceManager deviceManager_;
    talas::core::NodeRegistry registry_;
    std::filesystem::path soundLibraryRoot_;
    talas::app::SoundLibraryManifest soundLibraryManifest_;
    std::vector<talas::app::SoundLibraryEntry> soundLibraryEntries_;
    std::string soundLibraryDiagnostic_;
    talas::runtime::ProjectRuntimeBridge projectBridge_;
    struct SoundLibraryAuditionRuntime
    {
        std::unique_ptr<talas::runtime::ProjectRuntimeBridge> bridge;
        talas::app::SoundLibraryAuditionPreparation preparation;
        talas::app::SoundLibraryEntry entry;
        std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution>
            inputSubstitutions;
        std::string editableDocumentBytes;
        talas::audio::ProjectAssetWatchSnapshot verifiedAssetWatch;
        std::optional<std::filesystem::path> sourceProjectPath;
        std::uint64_t requestGeneration = 0;
        std::uint64_t editableDocumentGeneration = 0;
        std::uint64_t sourceDeviceGeneration = 0;
        bool previousTransportPlaying = false;
        bool routeControllerToProject = false;
        std::string routedControllerName;
        std::uint64_t routedSourceStageId = 0;
        struct RoutedControllerEvent
        {
            double value = 0.0;
            bool allowLiveOverride = false;
            std::uint64_t targetStageGeneration = 0;
        };
        static constexpr std::size_t routedEventCapacity = 512;
        std::array<RoutedControllerEvent, routedEventCapacity>
            routedEvents{};
        std::atomic<std::uint64_t> routedWriteIndex{0};
        std::atomic<std::uint64_t> routedReadIndex{0};
        std::atomic<bool> routedEventsOverflowed{false};
        std::atomic<bool> renderFailed{false};
        std::array<talas::runtime::RealtimeMidiOutputEvent, 256>
            terminalMidiEvents{};
        std::atomic<std::size_t> terminalMidiEventCount{0};
        std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 256>
            terminalGlobalControllerEvents{};
        std::array<char, 4096> terminalGlobalControllerName{};
        std::atomic<std::size_t> terminalGlobalControllerEventCount{0};
        std::atomic<std::size_t> terminalGlobalControllerNameLength{0};
        std::atomic<std::uint64_t> terminalStageId{0};
    };
    struct SoundLibraryAuditionPublication
    {
        std::unique_ptr<SoundLibraryAuditionRuntime> runtime;
        std::optional<talas::audio::ProjectAssetLoadFailure> assetFailure;
        std::optional<talas::project::ProjectDocument> diagnosticDocument;
        std::string diagnostic;
        bool cancelled = false;
    };
    std::unique_ptr<SoundLibraryAuditionRuntime> soundLibraryAuditionActive_;
    std::unique_ptr<SoundLibraryAuditionRuntime> soundLibraryAuditionRetiring_;
    std::atomic<SoundLibraryAuditionRuntime*> soundLibraryAuditionCallback_{nullptr};
    std::atomic<std::uint32_t> soundLibraryAuditionAudioOperations_{0};
    std::atomic<std::uint8_t> soundLibraryAuditionLifecycle_{0};
    std::atomic<std::size_t> soundLibraryAuditionOutputChannels_{0};
    std::jthread soundLibraryAuditionThread_;
    std::atomic<bool> soundLibraryAuditionWorkerFinished_{true};
    std::mutex soundLibraryAuditionResultMutex_;
    std::optional<SoundLibraryAuditionPublication>
        soundLibraryAuditionPublication_;
    std::atomic<std::uint64_t> soundLibraryAuditionRequestGeneration_{0};
    // Deterministic native-smoke barrier; product UI never enables it.
    bool soundLibraryAuditionSmokeHoldNextWorker_ = false;
    bool soundLibraryAuditionSmokeUsePreparedProfile_ = false;
    std::atomic<bool> soundLibraryAuditionSmokeHoldWorker_{false};
    std::atomic<bool> soundLibraryAuditionSmokeWorkerPaused_{false};
    struct PendingSoundLibraryAudition
    {
        std::string stableEntryId;
        double sampleRate = 0.0;
        std::size_t maxBlockFrames = 0;
        std::uint64_t sourceDeviceGeneration = 0;
        bool routeControllerToProject = false;
    };
    std::optional<PendingSoundLibraryAudition>
        soundLibraryAuditionPending_;
    juce::String soundLibraryAuditionStatus_;
    struct AnalysisAuditionRuntime
    {
        std::unique_ptr<talas::runtime::ProjectRuntimeBridge> bridge;
        talas::runtime::AnalysisTestInputSource testSource;
        std::vector<float> testInputScratch;
        talas::app::AnalysisAuditionPreparation preparation;
        std::string sourceDocumentBytes;
        std::uint64_t requestGeneration = 0;
        std::uint64_t sourceDocumentGeneration = 0;
        std::uint64_t sourceStageGeneration = 0;
        std::uint64_t sourceDeviceGeneration = 0;
        bool usesTemporarySource = false;
    };
    struct AnalysisAuditionPublication
    {
        std::unique_ptr<AnalysisAuditionRuntime> runtime;
        std::string sourceDocumentBytes;
        std::string diagnostic;
        talas::audio::ProjectAssetWatchSnapshot verifiedAssetWatch;
        std::uint64_t requestGeneration = 0;
        std::uint64_t sourceDocumentGeneration = 0;
        bool cancelled = false;
    };
    std::unique_ptr<AnalysisAuditionRuntime> analysisAuditionActive_;
    std::unique_ptr<AnalysisAuditionRuntime> analysisAuditionRetiring_;
    std::atomic<AnalysisAuditionRuntime*> analysisAuditionCallback_{nullptr};
    std::atomic<std::uint32_t> analysisAuditionAudioOperations_{0};
    std::atomic<std::uint8_t> analysisAuditionLifecycle_{0};
    std::jthread analysisAuditionThread_;
    std::atomic<bool> analysisAuditionWorkerFinished_{true};
    std::mutex analysisAuditionResultMutex_;
    std::optional<AnalysisAuditionPublication> analysisAuditionPublication_;
    std::atomic<std::uint64_t> analysisAuditionRequestGeneration_{0};
    juce::String analysisAuditionStatus_;
    std::unique_ptr<talas::audio::RealtimeOutputTake> analysisOutputTake_;
    std::atomic<talas::audio::RealtimeOutputTake*>
        analysisOutputTakeCallback_{nullptr};
    std::atomic<std::uint32_t> analysisOutputTakeAudioOperations_{0};
    std::atomic<std::uint8_t> analysisOutputTakeLifecycle_{0};
    std::atomic<std::uint64_t> analysisOutputTakeDeviceEpoch_{0};
    std::atomic<std::uint64_t> analysisOutputTakeAuthorityGeneration_{1};
    std::optional<talas::audio::RealtimeOutputTakeSnapshot>
        analysisOutputTakeLastSnapshot_;
    std::optional<talas::runtime::PreparedDiskRecorderCaptureSummary>
        analysisOutputTakeDiskRecorderCapture_;
    juce::String analysisOutputTakeStatus_{"Output take idle"};
    // How Record to disk takes the output: in stereo or monaural (the left
    // channel alone) and at 8, 16 or 24 bits.
    struct AnalysisOutputTakeFormat
    {
        bool monaural = false;
        std::uint16_t bits = 16;
        // The file's format, WAV, AIFF, IRCAM, SD-I or SD-II.
        talas::audio::SampleFileFormat file = talas::audio::SampleFileFormat::Wave;
    };
    AnalysisOutputTakeFormat analysisOutputTakeFormat_;
    // A take in a format other than WAV recorded as a WAV beside it: the
    // file, then the WAV.
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> analysisOutputTakeConversion_;
    // Its format and bits: Record to disk's, or a DiskRecorder's Format and
    // WordSize.
    talas::audio::SampleFileFormat analysisOutputTakeConversionFormat_ = talas::audio::SampleFileFormat::Wave;
    std::uint16_t analysisOutputTakeConversionBits_ = 16;
    void saveAnalysisOutputTakeInItsFormat();
    // A take of the DiskRecorders within the Sound
    // records the first into analysisOutputTake_ and each other into a take
    // of its own here, from what the bridge hands over as it renders; they
    // stop, and are kept or dropped, with the first.
    struct DiskRecorderTapTake
    {
        std::unique_ptr<talas::audio::RealtimeOutputTake> take;
        // The file in its Format and the WAV recorded beside it, for a
        // Format other than WAV.
        std::optional<std::pair<std::filesystem::path, std::filesystem::path>> conversion;
        talas::audio::SampleFileFormat format = talas::audio::SampleFileFormat::Wave;
        std::uint16_t bits = 16;
    };
    std::vector<DiskRecorderTapTake> diskRecorderTapTakes_;
    std::array<std::atomic<talas::audio::RealtimeOutputTake*>, 8> diskRecorderTapTakeCallbacks_{};
    std::atomic<bool> analysisOutputTakeFromTaps_{false};
    std::vector<std::filesystem::path> diskRecorderTapDestinations_;
    static void diskRecorderTapAudio(void* context, std::size_t tap, float* const* channels,
        std::size_t channelCount, std::size_t frameCount) noexcept;
    void publishDiskRecorderTapFromAudio(
        std::size_t tap, float* const* channels, std::size_t channelCount, std::size_t frameCount) noexcept;
    void finishDiskRecorderTapTakes();
    // The file a DiskRecorder records into: the project's file of
    // its FileName, a full path as it is, or a name alone in the Program
    // folder; nothing, with the reason, for one with the wrong extension.
    [[nodiscard]] std::optional<std::filesystem::path> diskRecorderDestination(
        const talas::runtime::PreparedDiskRecorderCaptureSummary& recorder, juce::String& error) const;
    struct PartialTrackPreviewRuntime
    {
        std::unique_ptr<talas::audio::PreparedPartialTrackResynthesizer>
            synthesizer;
        std::vector<double> scratch;
        std::string sourceDocumentBytes;
        talas::project::ProjectAsset sourceManifest;
        std::uint64_t sourceFrame = 0;
        std::uint64_t requestGeneration = 0;
        std::uint64_t sourceDocumentGeneration = 0;
        std::uint64_t sourceStageGeneration = 0;
        std::uint64_t sourceDeviceGeneration = 0;
        std::atomic<bool> renderFailed{false};
    };
    struct PartialTrackPreviewPublication
    {
        std::unique_ptr<PartialTrackPreviewRuntime> runtime;
        std::string sourceDocumentBytes;
        std::string diagnostic;
        std::uint64_t requestGeneration = 0;
        std::uint64_t sourceDocumentGeneration = 0;
        bool cancelled = false;
        bool allocationFailed = false;
        bool preparationFailed = false;
    };
    std::unique_ptr<PartialTrackPreviewRuntime> partialTrackPreviewActive_;
    std::unique_ptr<PartialTrackPreviewRuntime> partialTrackPreviewRetiring_;
    std::unique_ptr<PartialTrackPreviewRuntime>
        partialTrackPreviewRetiringNext_;
    std::atomic<PartialTrackPreviewRuntime*> partialTrackPreviewCallback_{
        nullptr};
    std::atomic<std::uint32_t> partialTrackPreviewAudioOperations_{0};
    std::atomic<std::uint8_t> partialTrackPreviewLifecycle_{0};
    std::jthread partialTrackPreviewThread_;
    std::atomic<bool> partialTrackPreviewWorkerFinished_{true};
    std::mutex partialTrackPreviewResultMutex_;
    std::optional<PartialTrackPreviewPublication>
        partialTrackPreviewPublication_;
    std::atomic<std::uint64_t> partialTrackPreviewRequestGeneration_{0};
    std::atomic<std::uint16_t> partialTrackPreviewProgress_{0};
    juce::String partialTrackPreviewStatus_{"Preview idle"};
    std::thread partialTrackDerivativeThread_;
    std::atomic<bool> partialTrackDerivativeFinished_{true};
    std::mutex partialTrackDerivativeResultMutex_;
    std::optional<talas::audio::PartialTrackDerivativeResult>
        partialTrackDerivativePublication_;
    std::optional<talas::app::WorkbenchOperationToken>
        partialTrackDerivativeToken_;
    std::optional<talas::audio::PartialTrackDerivativeResult>
        lastPartialTrackDerivative_;
    juce::String partialTrackDerivativeStatus_{"Render idle"};
    talas::app::PerformanceRuntimeDispatcher performanceDispatcher_{
        projectBridge_};
    talas::runtime::RealtimeControlIngress oscPerformanceIngress_{257};
    WorkbenchOscIngress oscIngress_{
        projectBridge_, &oscPerformanceIngress_};
    WorkbenchOscEgress oscEgress_;
    WorkbenchOscLoopbackProfileCoordinator oscLoopbackProfile_{
        oscIngress_, oscEgress_};
    talas::project::ProjectEditBoundary editBoundary_;
    talas::project::WorkspaceSessionState workspaceSession_;
    std::uint64_t workspaceVariableRestoreStageGeneration_ = 0;
    bool workspaceVariableRestorePending_ = false;
    std::uint64_t workspaceSoundVariableRestoreStageGeneration_ = 0;
    bool workspaceSoundVariableRestorePending_ = false;
    std::filesystem::path recoveryPath_;
    std::filesystem::path recentProjectsPath_;
    std::filesystem::path workspaceSessionsPath_;
    std::filesystem::path cleanupLedgerPath_;
    juce::String applicationName_;
    bool hardwareEnabled_ = true;
    bool hardwareInitialisationRequested_ = false;
    bool hardwareInitialisationStarted_ = false;
    bool hardwareInitialised_ = false;
    bool persistentStateEnabled_ = true;
    // The user's own preferences file; empty for the shared
    // one, workbenchPreferencesFileName.
    std::filesystem::path preferencesFile_;
    [[nodiscard]] std::filesystem::path preferencesPath() const;
    // The clipboard of a Workbench without persistent state (smoke runs),
    // so tests leave the system clipboard alone.
    juce::String isolatedGraphClipboard_;
    // Created when first shown; the window only for a Workbench on screen.
    std::unique_ptr<FakeKeyboardPanel> fakeKeyboardPanel_;
    std::unique_ptr<juce::DocumentWindow> fakeKeyboardWindow_;
    // The Tape Recorder's audio, in the device callback after the auditions.
    talas::audio::TapeRecorderEngine tapeRecorder_;
    std::unique_ptr<TapeRecorderPanel> tapeRecorderPanel_;
    std::unique_ptr<juce::DocumentWindow> tapeRecorderWindow_;
    std::unique_ptr<LiveGridEditorPanel> liveGridEditorPanel_;
    std::unique_ptr<juce::DocumentWindow> liveGridEditorWindow_;
    std::unique_ptr<AnalysisToolPanel> spectralAnalysisPanel_;
    std::unique_ptr<juce::DocumentWindow> spectralAnalysisWindow_;
    std::unique_ptr<AnalysisToolPanel> reAnalysisPanel_;
    std::unique_ptr<juce::DocumentWindow> reAnalysisWindow_;
    std::unique_ptr<SynchronizingSpectraPanel> synchronizingSpectraPanel_;
    std::unique_ptr<juce::DocumentWindow> synchronizingSpectraWindow_;
    std::unique_ptr<AnalysisToolPanel> gaAnalysisPanel_;
    std::unique_ptr<juce::DocumentWindow> gaAnalysisWindow_;
    std::unique_ptr<CdpProcessToolPanel> cdpProcessPanel_;
    std::unique_ptr<juce::DocumentWindow> cdpProcessWindow_;
    std::unique_ptr<ExpressionsLibraryPanel> expressionsLibraryPanel_;
    std::unique_ptr<juce::DocumentWindow> expressionsLibraryWindow_;
    std::unique_ptr<SampleEditorPanel> sampleEditorPanel_;
    std::unique_ptr<juce::DocumentWindow> sampleEditorWindow_;
    std::unique_ptr<GlobalMapEditorPanel> globalMapPanel_;
    std::unique_ptr<juce::DocumentWindow> globalMapWindow_;
    std::unique_ptr<ClassEditorPanel> classEditorPanel_;
    std::unique_ptr<juce::DocumentWindow> classEditorWindow_;
    std::unique_ptr<AlternateTuningsPanel> alternateTuningsPanel_;
    std::unique_ptr<juce::DocumentWindow> alternateTuningsWindow_;
    std::unique_ptr<juce::TextEditor> structureText_;
    // Show MIDI Messages: the keyboard's and
    // controllers' messages as they arrive, while the window is open.
    std::unique_ptr<juce::TextEditor> midiMessagesText_;
    std::unique_ptr<juce::DocumentWindow> midiMessagesWindow_;
    std::unique_ptr<FrequentlyUsedFoldersPanel> foldersPanel_;
    std::unique_ptr<juce::DocumentWindow> foldersWindow_;
    talas::app::MidiMessageLog midiMessageLog_;
    std::atomic<bool> midiMessagesShown_{false};
    std::unique_ptr<juce::DocumentWindow> structureWindow_;
    std::unique_ptr<DspStatusPanel> dspStatusPanel_;
    std::unique_ptr<juce::DocumentWindow> dspStatusWindow_;
    talas::app::DspStatusClock dspStatusClock_;
    bool suppressWorkspaceSessionPersistence_ = false;
    talas::app::WorkbenchSessionWriteCoalescer
        workspaceSessionWriteCoalescer_;
    talas::project::RecentProjectList recentProjects_;
    std::thread recoveryThread_;
    std::atomic<bool> recoveryStopRequested_ = false;
    std::atomic<bool> recoveryWriteFinished_ = true;
    std::mutex recoveryResultMutex_;
    talas::project::ProjectRecoverySaveResult recoveryWriteResult_;
    std::vector<talas::project::ProjectRecoveryVersion>
        recoveryVersions_;
    std::vector<talas::project::ProjectRecoveryVersion>
        recoveryWriteVersions_;
    talas::project::ProjectAutosaveState recoveryAutosaveState_;
    bool recoveryMonitoringEnabled_ = false;
    bool preserveRecoveryFile_ = false;
    TitleCallback onTitleChange_;
    WorkbenchLookAndFeel lookAndFeel_;
    // The backdrop and the panels' shadows, drawn once per layout and scale.
    juce::Image backdropCache_;
    std::vector<juce::Rectangle<int>> backdropCacheLayout_;
    float backdropCacheScale_ = 0.0F;
    bool backdropCacheEnhanced_ = false;
    std::unique_ptr<juce::TimedCallback> activityCursorTimer_;
    talas::app::WorkbenchCursor shownActivityCursor_ = talas::app::WorkbenchCursor::Arrow;
    bool dspProblemShown_ = false;
    std::optional<std::pair<std::uint64_t, bool>> diskUseOfDocument_;
    talas::app::WorkbenchAccessibilityProjection accessibilityProjection_ =
        *talas::app::workbenchAccessibilityProjection({}, 1.0, false);
    float appliedInterfaceScale_ = 1.0F;
    WorkbenchFocusCoordinator focusCoordinator_;
    std::optional<WorkbenchFocusIdentity>
        lastFocusRestorationTarget_;
    juce::ApplicationCommandManager commandManager_;
    juce::TooltipWindow tooltipWindow_;
    std::atomic<std::uint64_t> deviceGeneration_ = 0;
    std::atomic<std::uint64_t> deviceMetricsResetGeneration_ = 1;
    std::atomic<std::uint64_t> deviceMetricsPublicationSequence_ = 0;
    std::atomic<std::uint64_t> publishedDeviceMetricsGeneration_ = 0;
    std::atomic<std::uint64_t> deviceCallbackCount_ = 0;
    std::atomic<std::uint64_t> deviceDeadlineOverrunCount_ = 0;
    std::atomic<std::uint64_t> deviceBaselineCallbackCount_ = 0;
    std::atomic<std::uint64_t> deviceBaselineDeadlineOverrunCount_ = 0;
    std::atomic<std::uint64_t> deviceMaximumCallbackNanoseconds_ = 0;
    std::atomic<std::uint64_t> deviceLatestCallbackNanoseconds_ = 0;
    std::atomic<std::uint64_t> devicePeriodNanoseconds_ = 0;
    std::atomic<std::uint64_t> publishedDevicePeriodNanoseconds_ = 0;
    std::atomic<std::uint64_t> deviceTimingTicksPerSecond_ = 0;
    std::uint64_t adoptedDeviceMetricsResetGeneration_ = 0;
    std::uint64_t adoptedDevicePeriodNanoseconds_ = 0;
    std::uint64_t presentedDeviceCallbackNanoseconds_ = 0;
    bool applyingProjectDeviceSettings_ = false;
    std::uint64_t deviceSettingsApplicationGeneration_ = 0;
    // Non-null while Audio & MIDI Settings is open; only its changes are
    // recorded as project device settings.
    juce::Component::SafePointer<juce::Component> audioSettingsComponent_;
    std::array<int, 8> physicalInputChannelMap_ = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::atomic<bool> deviceMappingAttached_ = false;
    std::array<std::atomic<int>, 8> projectOutputChannelMap_{};
    std::array<std::atomic<int>, 8>
        projectOutputPhysicalChannels_{};
    std::array<std::atomic<float>, 8> outputMeterPeaks_{};
    std::array<std::atomic<float>, 2> inputMeterPeaks_{};
    // The System Output Level Pad's gain, set from the preferences.
    std::atomic<float> systemOutputGain_{1.0F};
    std::atomic<std::uint64_t> outputMeterStageId_ = 0;
    struct MidiInputSourceCallback final : juce::MidiInputCallback
    {
        WorkbenchComponent* owner = nullptr;
        std::uint8_t slot = 0;
        std::uint64_t generation = 0;

        void handleIncomingMidiMessage(
            juce::MidiInput*,
            const juce::MidiMessage& message) override;
    };
    struct MidiInputSourceRecord
    {
        juce::String identifier;
        juce::String displayName;
        std::uint8_t slot = 0;
        std::uint64_t generation = 0;
        std::unique_ptr<MidiInputSourceCallback> callback;
    };
    std::vector<MidiInputSourceRecord> midiInputSources_;
    bool midiInputSourceCapacityExceeded_ = false;
    bool midiInputSourceRegistryFailed_ = false;
    std::atomic<std::uint64_t> midiInputSourceGeneration_ = 1;
    // Message-thread writer; MIDI callbacks only acquire-load the exact
    // currently admitted Clock authority generation.
    std::atomic<std::uint64_t> midiClockAuthorityGeneration_ = 0;
    std::uint64_t nextMidiClockAuthorityGeneration_ = 1;
    talas::runtime::MidiClockTelemetrySnapshot latestMidiClockTelemetry_;
    std::atomic<std::uint64_t> mtcAuthorityGeneration_ = 0;
    std::uint64_t nextMtcAuthorityGeneration_ = 1;
    talas::runtime::MtcTelemetrySnapshot latestMtcTelemetry_;
    juce::String midiClockDiagnostic_;
    juce::String midiClockAccessibleDetail_;
    std::string midiSmokeSourceIdentifier_;
    talas::project::RealtimeMidiRouteAdmissionGate
        midiRouteAdmissionGate_;
    talas::project::RealtimeMidiControlRoutes midiRuntimeRoutes_;
    talas::runtime::RealtimeControlIngress midiUiIngress_{257};
    std::array<std::atomic<std::uint64_t>, 16>
        midiUiDroppedBySource_{};
    std::array<talas::app::AnalysisMonitorRecordInput, 16>
        midiMonitorRecords_{};
    std::size_t midiMonitorRecordCount_ = 0;
    std::uint64_t nextMidiMonitorSequence_ = 1;
    std::atomic<std::uint64_t> uiDocumentGeneration_ = 1;
    // Message-thread writer; MIDI/device callbacks only acquire-load.
    // Low bit is learn-active; upper bits identify route/learn generation.
    std::atomic<std::uint64_t> midiUiStateToken_ = 2;
    std::vector<talas::project::VirtualControl> stagedVirtualControls_;
    bool stagedMidiRoutesValid_ = false;
    juce::String midiRuntimeRouteDiagnostic_;
    juce::String oscIngressDiagnostic_;
    WorkbenchOscIngressTelemetry latestOscIngressTelemetry_;
    std::uint64_t oscRouteStageGeneration_ = 0;
    bool oscRoutesAvailable_ = false;
    juce::String oscEgressDiagnostic_;
    WorkbenchOscEgressTelemetry latestOscEgressTelemetry_;
    WorkbenchOscLoopbackProfileTelemetry latestOscLoopbackTelemetry_;
    std::uint64_t oscEgressRouteStageGeneration_ = 0;
    bool oscEgressRoutesAvailable_ = false;
    bool oscProfileAvailabilityInvalidated_ = false;
    bool currentDocumentStaged_ = false;
    std::uint8_t variablePresentationTick_ = 0;
    talas::runtime::PreparedGraphStatusSnapshot
        latestPreparedGraphStatus_;
    talas::runtime::RealtimeMetricsSnapshotV1 latestRealtimeMetrics_;
    juce::String preparedProjectStatusHeadline_;
    juce::String preparedAccessibleStatus_;
    std::uint64_t preparedPresentationRefreshCountForSmoke_ = 0;
    bool preparedPresentationDocumentStaged_ = false;
    talas::audio::ProjectAssetWatchSnapshot
        assetWatchBaseline_;
    double nextAssetWatchPollMilliseconds_ = 0.0;
    juce::String pendingAssetWatchStatus_;
    std::optional<std::string>
        pendingAssetReplacementId_;
    talas::app::AssetRecoveryWorkbenchCoordinator
        assetRecoveryCoordinator_;
    std::thread assetRecoveryThread_;
    std::atomic<bool> assetRecoveryStopRequested_ = false;
    std::atomic<bool> assetRecoveryWorkerFinished_ = true;
    std::mutex assetRecoveryProgressMutex_;
    std::optional<std::pair<
        std::uint64_t,
        talas::project::ProjectAssetSearchProgress>>
        assetRecoveryPendingProgress_;
    std::optional<talas::project::ProjectFileRevision>
        projectFileRevision_;
    double nextProjectFilePollMilliseconds_ = 0.0;
    bool projectChangedOnDisk_ = false;
    bool transportPlaying_ = false;
    // Stopped by Stop (Ctrl+K), not paused, until it plays again.
    bool transportStopped_ = false;
    void recycleMemory();
    // The status window's lines on the Sound last loaded.
    std::vector<std::string> loadedSoundInputLines_;
    bool libraryVisible_ = true;
    bool rightWorkspaceVisible_ = true;
    talas::app::WorkbenchWorkspaceMode workspaceMode_ =
        talas::app::WorkbenchWorkspaceMode::Patch;
    CompactDock compactDock_ = CompactDock::None;
    talas::app::WorkbenchWidthClass lastLayoutWidthClass_ =
        talas::app::WorkbenchWidthClass::Regular;
    juce::String pendingProjectFileStatus_;
    juce::TextButton playButton_;
    juce::TextButton pauseButton_;
    juce::TextButton stopButton_;
    juce::TextButton undoButton_;
    juce::TextButton redoButton_;
    juce::TextButton deleteButton_;
    juce::TextButton saveButton_;
    juce::TextButton moreButton_;
    juce::TextButton workspacePatchButton_;
    juce::TextButton workspaceAutomationButton_;
    juce::TextButton workspaceAnalysisButton_;
    juce::TextButton workspacePerformanceButton_;
    juce::TextButton workspaceSamplesButton_;
    juce::Label transportLabel_;
    juce::Label projectStateLabel_;
    juce::Label outputMeterLabel_;
    juce::Label statusLabel_;
    std::string selectedNodeId_;
    std::vector<std::string> selectedNodeIds_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    std::unique_ptr<juce::AlertWindow> recoveryDialog_;
    std::unique_ptr<juce::AlertWindow> soundDialog_;
    std::unique_ptr<juce::AlertWindow> exportDialog_;
    std::thread exportThread_;
    bool exportRunning_ = false;
    std::thread packageThread_;
    bool packageRunning_ = false;
    std::thread scoreClipArtifactThread_;
    bool scoreClipArtifactRunning_ = false;
    std::optional<talas::app::WorkbenchOperationToken>
        scoreClipArtifactToken_;
    std::string scoreClipArtifactActiveClipId_;
    std::optional<talas::project::ScoreClipRenderMode>
        scoreClipArtifactActiveMode_;
    std::unordered_map<std::string, TimelineScoreClipArtifactState>
        scoreClipArtifactStateCache_;
    struct ScoreClipArtifactCompletion
    {
        talas::app::WorkbenchOperationToken operation;
        std::uint64_t documentAuthorityGeneration = 0;
        std::uint64_t deviceGeneration = 0;
        std::string expectedDocument;
        std::string clipId;
        talas::runtime::ScoreClipArtifactPublicationResult result;
    };
    std::mutex scoreClipArtifactCompletionMutex_;
    std::optional<ScoreClipArtifactCompletion>
        scoreClipArtifactCompletion_;
    std::thread diskCacheCaptureThread_;
    bool diskCacheCaptureRunning_ = false;
    std::optional<talas::app::WorkbenchOperationToken>
        diskCacheCaptureToken_;
    std::string diskCacheCaptureSourcePath_;
    struct DiskCacheCaptureCompletion
    {
        talas::app::WorkbenchOperationToken operation;
        std::uint64_t documentAuthorityGeneration = 0;
        std::uint64_t deviceGeneration = 0;
        talas::audio::DiskCacheCaptureResult result;
    };
    std::mutex diskCacheCaptureCompletionMutex_;
    std::optional<DiskCacheCaptureCompletion>
        diskCacheCaptureCompletion_;
    talas::app::WorkbenchOperationCoordinator fileOperationCoordinator_;
    std::optional<talas::app::WorkbenchOperationToken>
        fileOperationToken_;
    bool fileChooserActive_ = false;
    bool destructivePromptActive_ = false;
    // The input chooseInput takes at once, by its place in the list, for
    // the smokes.
    std::optional<std::size_t> smokeInputChoice_;
    std::vector<std::pair<std::string, double>> variableEnvironment_;
    // The environment's values that are more than numbers: strings,
    // symbols, characters and arrays.
    talas::project::VariableLiteralEnvironment variableLiteralEnvironment_;
    // Every value the environment gives, numbers first.
    [[nodiscard]] talas::project::VariableLiteralEnvironment wholeVariableEnvironment() const;
    // The value the smokes type for each variable asked for, in place of
    // smokeVariableValue_'s number when set.
    std::optional<std::string> smokeVariableText_;
    // The default Sound and collection kept for Variables, and the Sounds the
    // environment has given the
    // Variables no declaration names.
    std::optional<talas::project::ProjectSoundDefinition> defaultSound_;
    std::vector<talas::project::ProjectSoundDefinition> defaultCollection_;
    std::vector<std::pair<std::string, std::vector<talas::project::ProjectSoundDefinition>>> soundVariableEnvironment_;
    // The value the smokes give each variable asked for, and how often
    // values were asked for.
    std::optional<double> smokeVariableValue_;
    std::size_t variableValueRequests_ = 0;
    EvaluateKeys evaluateKeys_{*this};
    DiskShiftClick diskShiftClick_{*this};
    juce::Component::SafePointer<juce::TextEditor> evaluateKeysEditor_;
    talas::project::EventValueLearning eventValueLearning_;
    juce::Component::SafePointer<juce::TextEditor> learningField_;
    std::atomic<bool> midiLearnArmed_{false};
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    std::unique_ptr<
        ComponentReplacementControllerState,
        ComponentReplacementControllerStateDeleter>
        componentReplacementState_;
#endif
    std::unique_ptr<PrototypePanel> prototypePanel_;
    std::unique_ptr<SoundDefinitionsPanel> soundDefinitionsPanel_;
    std::unique_ptr<GraphCanvas> graphCanvas_;
    std::unique_ptr<GraphCanvas> secondaryGraphCanvas_;
    std::unique_ptr<WorkspaceSplitHandle> workspaceSplitHandle_;
    // The line between the signal flow and the parameters: the parameters'
    // width dragged, and whether they take
    // over the whole area; the area they share.
    std::unique_ptr<WorkspaceSplitHandle> parametersSplitHandle_;
    // The signal flow's scroll bar, laid over the
    // bottom of the canvas, outside its own child tree.
    struct GraphScrollListener final : juce::ScrollBar::Listener
    {
        std::function<void(double)> moved;
        void scrollBarMoved(juce::ScrollBar*, double start) override
        {
            if (moved)
                moved(start);
        }
    };
    juce::ScrollBar graphScroll_{false};
    GraphScrollListener graphScrollListener_;
    void updateGraphScroll();
    std::optional<int> parametersWidth_;
    bool parametersTakeOver_ = false;
    // Edit > View...'s Parameters below the signal flow.
    bool parametersBelow_ = false;
    // Edit > View...'s Prototypes across the top.
    bool prototypeStripAtTop_ = false;
    juce::Rectangle<int> parametersSplitArea_;
    void dragParametersLine(juce::Point<int> position);
    void toggleParametersTakeOver();
    std::unique_ptr<InspectorPanel> inspectorPanel_;
    std::unique_ptr<PerformancePanel> performancePanel_;
    std::unique_ptr<WorkbenchPerformanceCanvas> performanceCanvas_;
    std::unique_ptr<VirtualControlSurface> virtualControlSurface_;
    std::unique_ptr<TimelinePanel> timelinePanel_;
    // The Timeline recorded live, which the audio
    // callback hands each block to, and the shield that keeps the mouse off
    // the surface's faders while it records.
    std::shared_ptr<LiveTimelineRecordingSlot> liveTimelineRecordingSlot_ = std::make_shared<LiveTimelineRecordingSlot>();
    std::unique_ptr<LiveRecordingMouseShield> liveRecordingShield_;
    std::unique_ptr<DiagnosticsPanel> diagnosticsPanel_;
    std::unique_ptr<ContextHelpPanel> contextHelpPanel_;
    std::unique_ptr<AssetRecoveryPanel> assetRecoveryPanel_;
    std::unique_ptr<AnalysisPanel> analysisPanel_;
    std::unique_ptr<PartialTrackEditorPanel> partialTrackEditorPanel_;
    // The spectrum a spectrum file opened made the editor's.
    std::optional<std::string> spectrumEditorAssetId_;
    std::unique_ptr<SampleMapPanel> sampleMapPanel_;
    std::unique_ptr<WaveformPanel> waveformPanel_;
    std::unique_ptr<SampleRegionInspector> sampleRegionInspector_;
    bool sampleImportEmbed_ = false;
    SampleMapImportPresentation sampleImportPresentation_ =
        SampleMapImportPresentation::Idle;
    juce::String sampleImportStatus_;
    std::thread sampleImportThread_;
    std::atomic<bool> sampleImportStopRequested_ = false;
    std::atomic<bool> sampleImportWorkerFinished_ = true;
    std::atomic<double> sampleImportProgress_ = 0.0;
    std::atomic<std::uint64_t> sampleImportGeneration_ = 0;
    struct SampleImportPublication
    {
        std::uint64_t generation = 0;
        std::string mapId;
        bool replace = false;
        std::optional<talas::project::ProjectSampleMapReplaceConfirmation>
            replaceConfirmation;
        talas::audio::SampleMapImportResult result;
    };
    std::mutex sampleImportResultMutex_;
    std::optional<SampleImportPublication> sampleImportPublication_;
    std::optional<SampleImportPublication> sampleImportPreview_;
    talas::app::SampleMapAutoMapPreview sampleImportPreviewProjection_;
    std::uint8_t sampleImportLowNote_ = 0;
    std::uint8_t sampleImportAnchorRootNote_ = 60;
    std::uint8_t sampleImportHighNote_ = 127;
    std::vector<talas::audio::PreparedSampleMapImport>
        pendingSampleMapImportCleanup_;
    std::thread sampleAssetDispositionThread_;
    std::atomic<bool> sampleAssetDispositionStopRequested_ = false;
    std::atomic<bool> sampleAssetDispositionWorkerFinished_ = true;
    std::atomic<std::uint64_t> sampleAssetDispositionGeneration_ = 0;
    struct SampleAssetDispositionPublication
    {
        std::uint64_t generation = 0;
        talas::project::ProjectAsset expectedAsset;
        talas::audio::ProjectAssetDispositionResult result;
    };
    std::mutex sampleAssetDispositionResultMutex_;
    std::optional<SampleAssetDispositionPublication>
        sampleAssetDispositionPublication_;
    std::vector<talas::audio::PreparedProjectAssetDisposition>
        pendingSampleAssetDispositionCleanup_;
    std::vector<talas::audio::ProjectPackageImportResult>
        pendingProjectPackageImportCleanup_;
    struct PendingOwnedCleanupFile
    {
        std::filesystem::path path;
        std::string contentHash;
        std::string packageProjectHash;
    };
    std::vector<PendingOwnedCleanupFile> pendingOwnedCleanupFiles_;
    std::string sampleAssetDispositionAssetId_;
    juce::String sampleAssetDispositionStatus_;
    std::thread sampleWaveformThread_;
    std::atomic<bool> sampleWaveformStopRequested_ = false;
    std::atomic<bool> sampleWaveformWorkerFinished_ = true;
    std::atomic<std::uint64_t> sampleWaveformGeneration_ = 0;
    struct SampleWaveformPublication
    {
        std::uint64_t generation = 0;
        talas::audio::WaveformOverviewResult result;
    };
    std::mutex sampleWaveformResultMutex_;
    std::optional<SampleWaveformPublication> sampleWaveformPublication_;
    std::shared_ptr<const talas::audio::WaveformOverview>
        selectedWaveformOverview_;
    juce::String sampleWaveformError_;
    std::string sampleWaveformAssetId_;
    std::uint64_t sampleWaveformAssetGeneration_ = 0;
    std::uint64_t sampleWaveformCursorFrame_ = 0;
    std::optional<talas::app::SampleMapMidiPoint> selectedSampleMidi_;
    std::atomic<std::uint64_t> sampleMidiSelectionGeneration_ = 0;
    std::atomic<bool> sampleMidiRoutingEnabled_ = false;
    std::atomic<std::uint64_t> sampleMapAuditionGeneration_ = 0;
    SampleMapAuditionPresentation sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Idle;
    bool sampleMapAuditionActive_ = false;
    std::thread sampleMapAuditionThread_;
    std::atomic<bool> sampleMapAuditionStopRequested_ = false;
    std::atomic<bool> sampleMapAuditionWorkerFinished_ = true;
    // Deterministic automated-smoke barrier; never enabled by product UI.
    bool sampleMapAuditionSmokeHoldNextWorker_ = false;
    std::atomic<bool> sampleMapAuditionSmokeHoldWorker_ = false;
    std::atomic<bool> sampleMapAuditionSmokeWorkerPaused_ = false;
    struct SampleMapAuditionPublication
    {
        std::uint64_t generation = 0;
        std::shared_ptr<const talas::project::ProjectDocument> document;
        talas::runtime::RenderSettings settings;
        SampleMapAuditionVoice voice;
        talas::audio::ProjectResourceSnapshotsLoadResult loaded;
    };
    std::mutex sampleMapAuditionResultMutex_;
    std::optional<SampleMapAuditionPublication>
        sampleMapAuditionPublication_;
    std::optional<SampleMapAuditionVoice> sampleMapAuditionPendingVoice_;
    std::optional<SampleMapAuditionVoice> sampleMapAuditionActiveVoice_;
    std::optional<talas::runtime::RenderSettings>
        sampleMapAuditionRestoreSettings_;
    juce::String sampleMapAuditionRestoreDeviceName_;
    bool sampleMapAuditionPreviousDocumentStaged_ = false;
    std::optional<std::string> primaryGraphContext_;
    std::optional<std::string> secondaryGraphContext_;
    std::optional<std::string> activeGraphContext_;
    std::vector<std::string> hierarchyPath_;
    juce::Rectangle<int> graphSplitArea_;
    bool graphSplitUsesVerticalDivider_ = true;
    std::optional<std::string> auditionDefinitionId_;
    bool auditionActive_ = false;
    bool auditionControlsCurrentProject_ = false;
    std::vector<talas::runtime::Diagnostic> latestCompileDiagnostics_;
    std::optional<talas::project::ProjectDocument>
        compileDiagnosticSourceDocument_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorkbenchComponent)
};
}
