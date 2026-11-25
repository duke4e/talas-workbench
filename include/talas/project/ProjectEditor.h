#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectAssetResolver.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectGraphClipboard.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::project
{
enum class EditError
{
    DuplicateNodeId,
    InvalidNodeId,
    UnknownNodeType,
    UnresolvedComponent,
    UnknownNodeId,
    UnknownPort,
    IncompatiblePortTypes,
    DuplicateInputConnection,
    InvalidGraph,
    UnknownParameter,
    DuplicateParameter,
    InvalidParameterValue,
    InvalidProjectVariable,
    DuplicateProjectVariable,
    UnknownProjectVariable,
    ProjectVariableInUse,
    StaleProjectVariableCapture,
    InvalidProjectGenerator,
    InvalidLiveSet,
    DuplicateProjectGenerator,
    UnknownProjectGenerator,
    StaleProjectGeneratorOutput,
    InvalidExpressionDefinition,
    DuplicateExpressionDefinition,
    UnknownExpressionDefinition,
    ExpressionDefinitionInUse,
    InvalidVirtualControl,
    DuplicateVirtualControl,
    UnknownVirtualControl,
    InvalidPerformancePresentation,
    DuplicatePerformancePresentation,
    UnknownPerformancePresentation,
    DuplicateMidiMapping,
    DuplicateOscMapping,
    InvalidAsset,
    DuplicateAsset,
    InvalidDeviceSettings,
    InvalidTimeline,
    InvalidScore,
    InvalidSampleMap,
    InvalidDiskCacheCapture,
    StaleDiskCacheCapture,
    DuplicateSampleMap,
    UnknownSampleMap,
    DuplicateSampleRegion,
    UnknownSampleRegion,
    StaleSampleMapConfirmation,
    StaleAssetDisposition,
    UnknownTimelineEvent,
    DuplicateSoundDefinition,
    InvalidSoundDefinition,
    UnknownSoundDefinition,
    SoundDefinitionInUse,
    InvalidSnapshot,
    DuplicateSnapshot,
    UnknownSnapshotBank,
    UnknownSnapshot,
    InvalidSelection,
    AllocationFailed,
    NoUndoAvailable,
    NoRedoAvailable
};

struct EditResult
{
    std::optional<EditError> error;
    std::string message;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

struct ProjectGeneratorEditAuthority
{
    std::uint64_t documentGeneration = 0;
    std::string canonicalDocumentBytes;
};

enum class MidiMappingConflictPolicy
{
    Reject,
    ReplaceExisting
};

struct ProjectSampleMapImportBatch
{
    std::vector<ProjectAsset> assets;
    std::vector<ProjectAsset> expectedAssets;
    std::vector<ProjectSampleRegion> regions;
};

struct ScoreClipRenderArtifactPublication
{
    std::uint64_t expectedDocumentAuthorityGeneration = 0;
    std::string expectedCanonicalDocumentBytes;
    std::string clipId;
    ScoreClipRenderArtifact artifact;
    std::vector<ProjectAsset> assets;
};

struct DiskCacheCapturePublication
{
    std::uint64_t expectedDocumentAuthorityGeneration = 0;
    std::string expectedCanonicalDocumentBytes;
    std::string sourcePath;
    ProjectAsset asset;
};

class ProjectEditor;
struct ProjectEventValueSyncResult;

class PreparedVirtualControlValueEdit
{
public:
    PreparedVirtualControlValueEdit() = default;
    PreparedVirtualControlValueEdit(
        const PreparedVirtualControlValueEdit&) = delete;
    PreparedVirtualControlValueEdit& operator=(
        const PreparedVirtualControlValueEdit&) = delete;
    PreparedVirtualControlValueEdit(
        PreparedVirtualControlValueEdit&&) noexcept = default;
    PreparedVirtualControlValueEdit& operator=(
        PreparedVirtualControlValueEdit&&) noexcept = default;

    [[nodiscard]] const ProjectDocument& document() const noexcept
    {
        return document_;
    }

    [[nodiscard]] bool changesDocument() const noexcept
    {
        return changesDocument_;
    }

private:
    friend class ProjectEditor;

    ProjectDocument document_;
    std::string eventName_;
    double value_ = 0.0;
    bool changesDocument_ = false;
};

class PreparedDiskCacheCaptureEdit
{
public:
    PreparedDiskCacheCaptureEdit() = default;
    PreparedDiskCacheCaptureEdit(
        const PreparedDiskCacheCaptureEdit&) = delete;
    PreparedDiskCacheCaptureEdit& operator=(
        const PreparedDiskCacheCaptureEdit&) = delete;
    PreparedDiskCacheCaptureEdit(
        PreparedDiskCacheCaptureEdit&&) noexcept = default;
    PreparedDiskCacheCaptureEdit& operator=(
        PreparedDiskCacheCaptureEdit&&) noexcept = default;

    [[nodiscard]] const ProjectDocument& document() const noexcept
    {
        return document_;
    }

    [[nodiscard]] bool changesDocument() const noexcept
    {
        return changesDocument_;
    }

private:
    friend class ProjectEditor;

    ProjectDocument document_;
    std::uint64_t expectedDocumentAuthorityGeneration_ = 0;
    bool changesDocument_ = false;
};

class ProjectSampleMapReplaceConfirmation
{
public:
    [[nodiscard]] std::string_view mapId() const
    {
        return mapId_;
    }

    [[nodiscard]] std::size_t regionCount() const
    {
        return expectedMap_.regions.size();
    }

private:
    friend class ProjectEditor;

    ProjectSampleMapReplaceConfirmation(
        std::string mapId,
        ProjectSampleMap expectedMap)
        : mapId_(std::move(mapId))
        , expectedMap_(std::move(expectedMap))
    {
    }

    std::string mapId_;
    ProjectSampleMap expectedMap_;
};

class ProjectEditor
{
public:
    using HistoryTransactionToken = std::uint64_t;

    ProjectEditor(ProjectDocument document, const talas::core::NodeRegistry& registry);

    [[nodiscard]] const ProjectDocument& document() const;
    // Monotonic authority for immutable snapshots copied from document().
    // It advances before every accepted live-document mutation so an
    // off-thread publisher can linearize against the edit boundary without
    // reading ProjectDocument concurrently.
    [[nodiscard]] std::uint64_t documentAuthorityGeneration() const noexcept;
    [[nodiscard]] EditResult replaceDocument(ProjectDocument document);
    // Runs replaceDocument()'s normalization and validation on a copy
    // without adopting it, so a caller can keep the current project and its
    // live state intact when the replacement would be rejected.
    [[nodiscard]] EditResult checkReplacementDocument(
        ProjectDocument document) const;
    // Validates and adopts a complete prepared document as one undoable edit.
    // Unlike replaceDocument(), this preserves existing history.
    [[nodiscard]] EditResult applyDocumentEdit(
        ProjectDocument document,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult prepareDiskCacheCapture(
        DiskCacheCapturePublication publication,
        PreparedDiskCacheCaptureEdit& prepared);
    [[nodiscard]] bool diskCacheCaptureStillCurrent(
        const PreparedDiskCacheCaptureEdit& prepared) const noexcept;
    // The caller performs its no-replace file publication only after the
    // preceding current-authority check, then immediately commits on the same
    // serialized control thread. No allocation or validation occurs here.
    [[nodiscard]] bool commitPreparedDiskCacheCapture(
        PreparedDiskCacheCaptureEdit prepared) noexcept;
    [[nodiscard]] EditResult upgradeExpressionsToPureV3();
    [[nodiscard]] EditResult upgradeExpressionsToPureV4();
    [[nodiscard]] EditResult upgradeExpressionsToSessionV1();
    [[nodiscard]] EditResult upgradeExpressionsToVariablesV1();
    [[nodiscard]] EditResult upgradeExpressionsToVariablesV2();
    [[nodiscard]] EditResult upgradeExpressionsToVariablesV3();
    [[nodiscard]] EditResult upgradeExpressionsToVariablesV4();
    // Moves a Variables project to Val syntax, provided every
    // expression keeps its meaning under Val's precedence.
    [[nodiscard]] EditResult upgradeExpressionsToValV1();
    [[nodiscard]] EditResult upgradeExpressionsToVoiceV1();
    [[nodiscard]] EditResult upsertProjectVariable(
        ProjectVariableDeclaration declaration);
    [[nodiscard]] EditResult replaceProjectVariable(
        ProjectVariableOwner expectedOwner,
        std::string_view expectedVariableId,
        ProjectVariableDeclaration declaration);
    [[nodiscard]] EditResult removeProjectVariable(
        ProjectVariableOwner owner,
        std::string_view variableId);
    [[nodiscard]] EditResult captureProjectVariableInitialValue(
        std::string_view variableId,
        double value,
        std::uint64_t expectedDocumentAuthorityGeneration);
    [[nodiscard]] EditResult upsertProjectSoundVariable(
        ProjectSoundVariableDeclaration declaration);
    [[nodiscard]] EditResult replaceProjectSoundVariable(
        ProjectVariableOwner expectedOwner,
        std::string_view expectedVariableId,
        ProjectSoundVariableDeclaration declaration);
    [[nodiscard]] EditResult removeProjectSoundVariable(
        ProjectVariableOwner owner,
        std::string_view variableId);
    [[nodiscard]] EditResult captureProjectSoundVariableInitialSelection(
        std::string_view variableId,
        std::string_view definitionId,
        std::uint64_t expectedDocumentAuthorityGeneration);
    [[nodiscard]] EditResult upsertProjectGenerator(
        ProjectGeneratorProgram program);
    [[nodiscard]] EditResult replaceProjectGenerator(
        std::string_view expectedGeneratorId,
        std::uint64_t expectedDocumentAuthorityGeneration,
        ProjectGeneratorProgram program);
    [[nodiscard]] EditResult removeProjectGenerator(
        std::string_view generatorId);
    [[nodiscard]] EditResult applyGeneratedTimelineEvents(
        std::vector<ProjectGeneratorTimelineEvent> events,
        const ProjectGeneratorEditAuthority& authority);
    [[nodiscard]] EditResult applyGeneratedScoreEdits(
        ProjectScoreEditBatch edits,
        const ProjectGeneratorEditAuthority& authority);
    [[nodiscard]] EditResult applyGeneratedSoundDefinition(
        ProjectGeneratorSoundResult candidate,
        const ProjectGeneratorEditAuthority& authority);
    [[nodiscard]] EditResult upsertExpressionDefinition(
        talas::expr::ExpressionDefinition definition);
    [[nodiscard]] EditResult removeExpressionDefinition(
        std::string_view definitionName);
    [[nodiscard]] EditResult addNode(
        ProjectNode node,
        CanvasPosition position = {});
    [[nodiscard]] EditResult addSoundInstance(
        std::string instanceId,
        std::string soundDefinitionId,
        CanvasPosition position);
    [[nodiscard]] EditResult addNodeToSoundDefinition(
        std::string_view definitionId,
        ProjectNode node,
        CanvasPosition position = {});
    [[nodiscard]] EditResult duplicateNodesInSoundDefinition(
        std::string_view definitionId,
        std::span<const std::string> nodeIds,
        std::vector<std::string>& duplicatedNodeIds,
        CanvasPosition offset = {32, 32});
    [[nodiscard]] EditResult removeNodesFromSoundDefinition(
        std::string_view definitionId,
        std::span<const std::string> nodeIds);
    [[nodiscard]] EditResult moveNodesInSoundDefinition(
        std::string_view definitionId,
        std::span<const std::string> nodeIds,
        CanvasPosition delta,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult addSoundInstanceToSoundDefinition(
        std::string_view parentDefinitionId,
        std::string instanceId,
        std::string soundDefinitionId,
        CanvasPosition position);
    [[nodiscard]] EditResult removeNodeFromSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId);
    [[nodiscard]] EditResult moveNodeInSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId,
        CanvasPosition position,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult connectInSoundDefinition(
        std::string_view definitionId,
        talas::core::Connection connection);
    [[nodiscard]] EditResult disconnectInSoundDefinition(
        std::string_view definitionId,
        std::string_view toNode,
        std::string_view toPort);
    [[nodiscard]] EditResult disconnectPreservedComponentInSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId);
    [[nodiscard]] EditResult setParameterInSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId,
        ProjectParameterExpression parameter);
    [[nodiscard]] EditResult setResourceParameterAssetInSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId,
        std::string_view parameterId,
        ProjectAsset asset);
    [[nodiscard]] EditResult repairPreservedComponentInSoundDefinition(
        std::string_view definitionId,
        std::string_view nodeId) const;
    [[nodiscard]] EditResult removeNode(std::string_view nodeId);
    // Deleting a Sound from a signal path replaces
    // it with its input, which then feeds what it fed; the first Sound of a
    // chain, with nothing to take its place, cannot be deleted while
    // another Sound reads it, and one that nothing reads is simply removed.
    // `inputNodeId` names which of several inputs takes its place, the
    // first otherwise. One undo step.
    [[nodiscard]] EditResult replaceNodeWithItsInput(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::optional<std::string_view> inputNodeId = std::nullopt);
    // The Sounds that could take a deleted Sound's place, one for each
    // Sound feeding it, in the order of their lines: where there are
    // several, the editor asks which.
    [[nodiscard]] std::vector<std::string> inputsReplacingNode(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId) const;
    [[nodiscard]] EditResult duplicateNodes(
        std::span<const std::string> nodeIds,
        std::vector<std::string>& duplicatedNodeIds,
        CanvasPosition offset = {32, 32});
    [[nodiscard]] EditResult removeNodes(
        std::span<const std::string> nodeIds);
    // A Sound dropped on a line goes between the Sounds the line joined
    //: the line's source feeds the new
    // node's input `inputPortId`, or else its first input that takes it,
    // and the new node's first output that fits feeds the line's
    // destination. One undo step.
    [[nodiscard]] EditResult insertNodeOnConnection(
        std::optional<std::string_view> definitionId,
        ProjectNode node,
        CanvasPosition position,
        const talas::core::Connection& connection,
        std::optional<std::string_view> inputPortId = std::nullopt);
    // A Sound dropped on the gray circle with a plus at a line's middle
    //: `mixer` takes the line, the line's
    // source and the dropped Sound both feeding its Inputs, so the dropped
    // Sound is added to the one already there. One undo step.
    [[nodiscard]] EditResult mixNodeOnConnection(
        std::optional<std::string_view> definitionId,
        ProjectNode mixer,
        ProjectNode node,
        CanvasPosition position,
        const talas::core::Connection& connection);
    // The inputs of `node` of the line's own kind that could take its
    // signal were it dropped on the line, in order (or else the one of
    // another kind insertion would take): where there are several, the editor
    // asks which. Empty for none or a line no longer there.
    [[nodiscard]] std::vector<std::string> inputsTakingConnection(
        std::optional<std::string_view> definitionId,
        const ProjectNode& node,
        const talas::core::Connection& connection) const;
    // A Sound dropped on top of another replaces it: the replacement takes the
    // old node's place and its outgoing connections; the old node's inputs
    // move to the replacement's input of the same name, or else its first free
    // input that takes them, and are otherwise left unconnected. Replace all
    // (`withItsInputs`) replaces the old Sound and its inputs too, the Sounds
    // feeding only it, and the replacement takes its default input, a Noise in
    // each required audio input, as a prototype comes with one. One undo step.
    [[nodiscard]] EditResult replaceNode(
        std::optional<std::string_view> definitionId,
        std::string_view replacedNodeId,
        ProjectNode replacement,
        bool withItsInputs = false);
    // Paste: copies of the fragment's nodes and their
    // connections, the group's top-left corner at origin; a node keeps its
    // id when the graph has none like it. One undo step.
    [[nodiscard]] EditResult pasteGraphFragment(
        std::optional<std::string_view> definitionId,
        const ProjectGraphFragment& fragment,
        CanvasPosition origin,
        std::vector<std::string>& pastedNodeIds);
    // Changing the name of a Sound: the node's id
    // in its graph, with the lines, layout, a Sound definition's ports and
    // parameter bindings, variable owners, snapshot and performance targets
    // that name it. One undo step. A name is 1 to 128 bytes without control
    // characters, '/' or space at either end, and free in the graph.
    // A multi-input field's Sounds:
    // the Sounds of the graph that feed a Sound's input, in order; and the
    // input set to Sounds given in order, one removed by leaving it out and
    // the order changed by moving it, in one undo step.
    [[nodiscard]] std::vector<std::string> inputsOf(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::string_view portId) const;
    [[nodiscard]] EditResult setInputs(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::string_view portId,
        std::vector<std::string> sources);
    // A Sound dropped into the field: a new Sound
    // added to the graph as the input's last, placed to the left of the Sound
    // it feeds, in one undo step; or, `replacing` one of the input's Sounds
    // (from 0), taking its place, the Sound replaced going with the Sounds
    // that fed only it unless it feeds another.
    [[nodiscard]] EditResult addInputSound(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::string_view portId,
        ProjectNode node,
        std::optional<std::size_t> replacing = std::nullopt);
    // Paste special: the Sound itself,
    // not a copy, takes another's place, so both places share it; the
    // replaced Sound goes with the Sounds that fed only it.
    [[nodiscard]] EditResult shareNodeInPlaceOf(
        std::optional<std::string_view> definitionId,
        std::string_view replacedNodeId,
        std::string_view sharedNodeId);
    // Paste special into an input field: the Sound
    // in the clipboard itself, not a copy, joins a field of many Sounds as
    // its last, or takes the place of the Sound in a field of one, which goes
    // with the Sounds that fed only it when nothing else uses it; pasted
    // into one field after another, the fields share it. One undo step.
    [[nodiscard]] EditResult shareNodeInInput(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::string_view portId,
        std::string_view sharedNodeId);
    // Revert: a Sound's parameters as they
    // were when it was opened, in one undo step.
    [[nodiscard]] EditResult revertNodeParameters(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::vector<ProjectParameterExpression> parameters);
    [[nodiscard]] EditResult renameNode(
        std::optional<std::string_view> definitionId,
        std::string_view nodeId,
        std::string newNodeId);
    // Action > Set replaceable input: the
    // Sound of a Sound's graph that a replacement input, a track's input or
    // a line the Sound is dropped on takes the place of, named by its first
    // Audio output as the Sound's first input (format 138), in place of the
    // one before. One undo step.
    [[nodiscard]] EditResult setReplaceableInput(
        std::string_view definitionId,
        std::string_view nodeId);
    // Edit > Clean up: each listed node moves to
    // its position, as one undo step.
    [[nodiscard]] EditResult arrangeNodes(
        std::optional<std::string_view> definitionId,
        std::span<const ProjectNodePresentation> positions);
    [[nodiscard]] EditResult moveNodes(
        std::span<const std::string> nodeIds,
        CanvasPosition delta,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult moveNode(
        std::string_view nodeId,
        CanvasPosition position,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult connect(talas::core::Connection connection);
    [[nodiscard]] EditResult disconnect(std::string_view toNode, std::string_view toPort);
    [[nodiscard]] EditResult disconnectPreservedComponent(
        std::string_view nodeId);
    [[nodiscard]] EditResult setParameter(std::string_view nodeId, ProjectParameterExpression parameter);
    [[nodiscard]] EditResult repairPreservedComponent(
        std::string_view nodeId) const;
    [[nodiscard]] EditResult addVirtualControl(
        VirtualControl control,
        CanvasPosition position = {});
    [[nodiscard]] EditResult moveVirtualControl(
        std::string_view eventName,
        CanvasPosition position);
    [[nodiscard]] EditResult setVirtualControlVisible(
        std::string_view eventName,
        bool visible);
    [[nodiscard]] EditResult addPerformanceControl(
        SnapshotTargetIdentity target,
        ProjectPerformanceControlKind kind,
        std::string displayName,
        CanvasPosition position = {});
    [[nodiscard]] EditResult removePerformanceControl(
        std::string_view controlId);
    [[nodiscard]] EditResult movePerformanceControl(
        std::string_view controlId,
        CanvasPosition position,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult setPerformanceControlVisible(
        std::string_view controlId,
        bool visible);
    [[nodiscard]] EditResult setPerformanceControlKind(
        std::string_view controlId,
        ProjectPerformanceControlKind kind);
    [[nodiscard]] EditResult setPerformanceControlDisplayName(
        std::string_view controlId,
        std::string displayName);
    [[nodiscard]] EditResult setPerformanceControlModulationSlots(
        std::string_view controlId,
        std::vector<ProjectPerformanceModulationSlot> slots);
    [[nodiscard]] EditResult addPerformanceMeter(
        ProjectPerformanceMeterPresentation meter);
    [[nodiscard]] EditResult removePerformanceMeter(
        std::string_view meterId);
    [[nodiscard]] EditResult movePerformanceMeter(
        std::string_view meterId,
        CanvasPosition position,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult setPerformanceMeterVisible(
        std::string_view meterId,
        bool visible);
    [[nodiscard]] EditResult setVirtualControlValue(std::string_view eventName, double value);
    [[nodiscard]] EditResult prepareVirtualControlValue(
        std::string_view eventName,
        double value,
        PreparedVirtualControlValueEdit& prepared);
    void commitVirtualControlValue(
        PreparedVirtualControlValueEdit prepared) noexcept;
    [[nodiscard]] EditResult setVirtualControlMidiMapping(std::string_view eventName,
                                                           std::optional<MidiControlMapping> mapping);
    // The global map editor: the project's own map, or the
    // Default map for an empty text. One undo step; a map that does not
    // parse is refused.
    [[nodiscard]] EditResult setGlobalMap(std::string text);
    // Format 156: how the project's fields read rounded and truncated, a beat
    // and a number given to inSOSPitch, as one undo step; a beat tempo outside
    // 1 to 1000 is refused.
    [[nodiscard]] EditResult setFieldReadingSettings(talas::expr::RoundingRule rounding,
        std::optional<double> fieldBeatTempoBpm, talas::expr::UnitlessSOSPitch unitlessSOSPitch);
    // The surface arranged, each widget at its
    // place (or laid out automatically again without one) and the layout
    // they follow, as one edit.
    // The layouts made in the VCS Editor, as one
    // edit.
    [[nodiscard]] EditResult setVirtualControlLayoutPages(std::vector<VirtualControlLayoutPage> pages);
    // What the layouts keep of their own (their
    // presets' banks, arrangements and widgets), with the layouts made in
    // the editor when given, as one edit. A preset bank named must exist.
    [[nodiscard]] EditResult setVirtualControlLayoutSettings(std::vector<VirtualControlLayoutSettings> settings,
        std::optional<std::vector<VirtualControlLayoutPage>> pages = std::nullopt);
    // The surface's texts and boxes, as
    // one edit.
    [[nodiscard]] EditResult setVirtualControlDecorations(std::vector<VirtualControlDecoration> decorations);
    // Widgets copied from this surface or from
    // another Sound's pasted in as one edit, each onto the control of its
    // Event Value (its type, place, size and range) and into the layout
    // made in the editor that is named; the rest kept dormant.
    [[nodiscard]] EditResult pasteVirtualControlWidgets(std::vector<VirtualControl> pasted, std::string_view layoutPage = {});
    [[nodiscard]] EditResult arrangeVirtualControls(
        VirtualControlLayout layout,
        const std::vector<std::pair<std::string, std::optional<VirtualControlBounds>>>& places);
    // The VCS Editor: a widget's options, and
    // its Min / Max (the value kept within them). One undo step each.
    [[nodiscard]] EditResult setVirtualControlWidget(
        std::string_view eventName,
        VirtualControlWidget widget);
    [[nodiscard]] EditResult setVirtualControlRange(
        std::string_view eventName,
        double minimum,
        double maximum);
    [[nodiscard]] EditResult setVirtualControlOscMapping(
        std::string_view eventName,
        std::optional<OscControlMapping> mapping);
    [[nodiscard]] EditResult setVirtualControlOscEgressMapping(
        std::string_view eventName,
        std::optional<OscEgressMapping> mapping);
    [[nodiscard]] EditResult learnVirtualControlMidiMapping(
        std::string_view eventName,
        MidiControlMapping mapping,
        MidiMappingConflictPolicy conflictPolicy =
            MidiMappingConflictPolicy::Reject);
    [[nodiscard]] EditResult removeVirtualControl(std::string_view eventName);
    [[nodiscard]] EditResult setResourceParameterAsset(std::string_view nodeId,
                                                       std::string_view parameterId,
                                                       ProjectAsset asset);
    // A file the project holds that no Resource parameter names yet, such
    // as one a DiskSplicer's Segments splice; the
    // same asset again changes nothing.
    [[nodiscard]] EditResult addAsset(ProjectAsset asset);
    [[nodiscard]] EditResult acceptProjectAssetInspection(
        ProjectAsset inspectedAsset);
    [[nodiscard]] EditResult relocateProjectAsset(
        ProjectAsset relocatedAsset);
    [[nodiscard]] EditResult acceptProjectAssetReplacement(
        ProjectAsset replacementAsset);
    [[nodiscard]] EditResult acceptProjectAssetDisposition(
        ProjectAsset dispositionAsset);
    [[nodiscard]] EditResult acceptProjectAssetDisposition(
        ProjectAsset dispositionAsset,
        const ProjectAsset& expectedAsset);
    [[nodiscard]] EditResult applyProjectAssetResolutions(
        std::vector<ProjectAssetResolution> resolutions);
    [[nodiscard]] EditResult reconcileProjectAssetRuntimeIdentity(
        ProjectAsset inspectedAsset,
        std::uint64_t acceptedGeneration,
        std::string_view acceptedContentHash);
    [[nodiscard]] EditResult upsertPartialTrackEditSet(
        ProjectPartialTrackEditSet edits,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult applyPartialTrackEditBatch(
        const PartialTrackAsset& source,
        ProjectPartialTrackEditBatch batch,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult removePartialTrackEditSet(
        std::string_view editSetId);
    [[nodiscard]] EditResult rebaseAssetPathsForProjectMove(
        const std::optional<std::filesystem::path>& sourceProjectFilePath,
        const std::filesystem::path& targetProjectFilePath);
    [[nodiscard]] EditResult setDeviceSettings(ProjectDeviceSettings settings);
    [[nodiscard]] EditResult replaceLiveSetLibrary(
        ProjectLiveSetLibrary library);
    [[nodiscard]] EditResult upsertProjectLiveSet(ProjectLiveSet liveSet);
    [[nodiscard]] EditResult removeProjectLiveSet(
        std::string_view liveSetId);
    [[nodiscard]] EditResult moveProjectLiveSet(
        std::string_view liveSetId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult upsertProjectLiveSetCell(
        std::string_view liveSetId,
        ProjectLiveSetCell cell);
    [[nodiscard]] EditResult removeProjectLiveSetCell(
        std::string_view liveSetId,
        std::string_view cellId);
    [[nodiscard]] EditResult moveProjectLiveSetCell(
        std::string_view liveSetId,
        std::string_view cellId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult upsertProjectLiveTool(ProjectLiveTool tool);
    [[nodiscard]] EditResult removeProjectLiveTool(
        std::string_view toolId);
    [[nodiscard]] EditResult moveProjectLiveTool(
        std::string_view toolId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult setTimelineLoop(
        TimelineLoopRegion loop,
        HistoryTransactionToken transactionToken = 0);
    // The MIDI channel whose program changes jump
    // the Timeline's cursor to markers, 0 for none.
    [[nodiscard]] EditResult setTimelineMarkerProgramChannel(std::uint8_t channel);
    [[nodiscard]] EditResult setMusicalTime(ProjectMusicalTime musicalTime);
    [[nodiscard]] EditResult addTempoEvent(MusicalTempoEvent event);
    [[nodiscard]] EditResult updateTempoEvent(
        std::size_t index,
        MusicalTempoEvent event);
    [[nodiscard]] EditResult removeTempoEvent(std::size_t index);
    [[nodiscard]] EditResult addMeterEvent(MusicalMeterEvent event);
    [[nodiscard]] EditResult updateMeterEvent(
        std::size_t index,
        MusicalMeterEvent event);
    [[nodiscard]] EditResult removeMeterEvent(std::size_t index);
    [[nodiscard]] EditResult addTimelineEventValue(TimelineEventValue event);
    [[nodiscard]] EditResult updateTimelineEventValue(
        std::size_t index,
        TimelineEventValue event,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult removeTimelineEventValue(std::size_t index);
    [[nodiscard]] EditResult applyScoreEdits(
        ProjectScoreEditBatch edits,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult addScoreTrack(ScoreTrack track);
    [[nodiscard]] EditResult updateScoreTrack(ScoreTrack track);
    [[nodiscard]] EditResult removeScoreTrack(std::string_view trackId);
    [[nodiscard]] EditResult moveScoreTrack(
        std::string_view trackId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult addScoreClip(ScoreClip clip);
    [[nodiscard]] EditResult updateScoreClip(
        ScoreClip clip,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult publishScoreClipRenderArtifact(
        ScoreClipRenderArtifactPublication publication);
    [[nodiscard]] EditResult clearScoreClipRenderArtifact(
        std::string_view clipId);
    [[nodiscard]] EditResult removeScoreClip(std::string_view clipId);
    // An effect dropped on a clip plays the
    // clip's Sound through it, as a Sound definition named Effect(Sound)
    // whose "input" is an instance of the clip's Sound; minus removes the
    // last effect; a Sound dropped with Option (Control) replaces the
    // affected Sound, the input. One undo step each.
    [[nodiscard]] EditResult applyEffectToScoreClip(
        std::string_view clipId,
        std::string effectTypeId);
    // The effect may be a Sound of the project as well, from the
    // Sound Browser, a Sound file window or a Sound editor, one with an
    // Input for the clip's Sound.
    [[nodiscard]] EditResult applySoundEffectToScoreClip(
        std::string_view clipId,
        std::string_view effectSoundDefinitionId);
    [[nodiscard]] EditResult removeLastScoreClipEffect(std::string_view clipId);
    [[nodiscard]] EditResult replaceScoreClipInput(
        std::string_view clipId,
        std::string_view soundDefinitionId);
    [[nodiscard]] EditResult moveScoreClip(
        std::string_view clipId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult addScoreAutomationLane(ScoreAutomationLane lane);
    [[nodiscard]] EditResult updateScoreAutomationLane(
        ScoreAutomationLane lane,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult removeScoreAutomationLane(std::string_view laneId);
    [[nodiscard]] EditResult moveScoreAutomationLane(
        std::string_view laneId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult addScoreMarker(ScoreMarker marker);
    [[nodiscard]] EditResult updateScoreMarker(
        ScoreMarker marker,
        HistoryTransactionToken transactionToken = 0);
    [[nodiscard]] EditResult removeScoreMarker(std::string_view markerId);
    [[nodiscard]] EditResult moveScoreMarker(
        std::string_view markerId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult addSampleMap(ProjectSampleMap map);
    [[nodiscard]] EditResult renameSampleMap(
        std::string_view mapId,
        std::string displayName);
    [[nodiscard]] EditResult removeSampleMap(std::string_view mapId);
    [[nodiscard]] EditResult moveSampleMap(
        std::string_view mapId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult addSampleRegion(
        std::string_view mapId,
        ProjectSampleRegion region);
    [[nodiscard]] EditResult updateSampleRegion(
        std::string_view mapId,
        std::string_view regionId,
        ProjectSampleRegion region);
    [[nodiscard]] EditResult updateSampleRegions(
        std::string_view mapId,
        std::vector<ProjectSampleRegion> regions);
    [[nodiscard]] EditResult removeSampleRegion(
        std::string_view mapId,
        std::string_view regionId);
    [[nodiscard]] EditResult removeSampleRegions(
        std::string_view mapId,
        std::vector<std::string> regionIds);
    [[nodiscard]] EditResult moveSampleRegion(
        std::string_view mapId,
        std::string_view regionId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult autoMapSampleRegions(
        std::string_view mapId,
        ProjectSampleMapAutoMapRequest request);
    [[nodiscard]] EditResult mergeSampleMapImport(
        std::string_view mapId,
        ProjectSampleMapImportBatch batch);
    [[nodiscard]] std::optional<ProjectSampleMapReplaceConfirmation>
    sampleMapReplaceConfirmation(std::string_view mapId) const;
    [[nodiscard]] EditResult replaceSampleMapImport(
        std::string_view mapId,
        ProjectSampleMapImportBatch batch,
        const ProjectSampleMapReplaceConfirmation& confirmation);
    [[nodiscard]] EditResult addSoundDefinition(
        ProjectSoundDefinition definition);
    [[nodiscard]] EditResult replaceSoundDefinition(
        std::string_view definitionId,
        ProjectSoundDefinition definition);
    [[nodiscard]] EditResult renameSoundDefinition(
        std::string_view definitionId,
        std::string displayName);
    // Collect...: the
    // selected Sounds and collections moved into a new collection of that
    // name, in the collection given or at the top level; its id is set.
    // One undo step.
    [[nodiscard]] EditResult collectSounds(
        std::optional<std::string_view> parentCollectionId,
        std::vector<std::string> soundIds,
        std::vector<std::string> collectionIds,
        std::string name,
        std::string& collectionId);
    [[nodiscard]] EditResult renameSoundCollection(std::string_view collectionId, std::string name);
    // A collection deleted: its Sounds and collections go where it was.
    [[nodiscard]] EditResult removeSoundCollection(std::string_view collectionId);
    // Duplicate: a collection holding a duplicate of each
    // of its Sounds, beside it; its id is set. One undo step.
    [[nodiscard]] EditResult duplicateSoundCollection(std::string_view collectionId, std::string& duplicateId);
    // Duplicate: a copy of the Sound, its signal
    // flow laid out as the original's, under a new id and name. One undo
    // step.
    [[nodiscard]] EditResult duplicateSoundDefinition(
        std::string_view definitionId,
        std::string newDefinitionId,
        std::string displayName);
    [[nodiscard]] EditResult removeSoundDefinition(
        std::string_view definitionId);
    [[nodiscard]] EditResult extractNodesToSound(
        std::vector<std::string> nodeIds,
        std::string definitionId,
        std::string displayName,
        std::string instanceId);
    [[nodiscard]] EditResult inlineSoundInstance(
        std::string_view instanceId);
    // Expand in a Sound editor: the Sound instance
    // in a Sound's signal flow replaced by its Sounds, one level, as the
    // project's own are inlined. One undo step.
    [[nodiscard]] EditResult inlineSoundInstance(
        std::string_view definitionId,
        std::string_view instanceId);
    // Action > Expand on a Sound of a signal flow, the project's or a Sound's:
    // the instance inlined as inlineSoundInstance does, the fields of its
    // Sounds showing the Event Sources where they had the Event Values the
    // global map names (writeGlobalMapEventSources), each new source's Virtual
    // Control starting where its Event Value's was. One undo step.
    [[nodiscard]] EditResult expandSoundInstance(
        std::string_view instanceId);
    [[nodiscard]] EditResult expandSoundInstance(
        std::string_view definitionId,
        std::string_view instanceId);
    // Expand in the Sound file window: a copy
    // of the Sound beside it, under a new id and name, each Sound instance in
    // it expanded one level and its fields showing Event Sources as
    // expandSoundInstance's do; the Sound itself unchanged. One undo step.
    [[nodiscard]] EditResult expandSoundDefinition(
        std::string_view definitionId,
        std::string newDefinitionId,
        std::string displayName);
    [[nodiscard]] EditResult addSnapshotBank(
        ProjectSnapshotBank bank);
    [[nodiscard]] EditResult renameSnapshotBank(
        std::string_view bankId,
        std::string displayName);
    [[nodiscard]] EditResult cloneSnapshotBank(
        std::string_view sourceBankId,
        std::string newBankId,
        std::string displayName);
    [[nodiscard]] EditResult removeSnapshotBank(
        std::string_view bankId);
    [[nodiscard]] EditResult moveSnapshotBank(
        std::string_view bankId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult setSnapshotBankSmoothing(
        std::string_view bankId,
        double smoothingSeconds);
    [[nodiscard]] EditResult storeSnapshot(
        std::string_view bankId,
        ProjectSnapshot snapshot);
    [[nodiscard]] EditResult initializeSnapshot(
        std::string_view bankId,
        std::string snapshotId,
        std::string displayName);
    [[nodiscard]] EditResult renameSnapshot(
        std::string_view bankId,
        std::string_view snapshotId,
        std::string displayName);
    [[nodiscard]] EditResult cloneSnapshot(
        std::string_view bankId,
        std::string_view sourceSnapshotId,
        std::string newSnapshotId,
        std::string displayName);
    [[nodiscard]] EditResult removeSnapshot(
        std::string_view bankId,
        std::string_view snapshotId);
    [[nodiscard]] EditResult moveSnapshot(
        std::string_view bankId,
        std::string_view snapshotId,
        std::size_t destinationIndex);
    [[nodiscard]] EditResult setSnapshotParameterPolicy(
        std::string_view bankId,
        SnapshotParameterPolicy policy);
    [[nodiscard]] EditResult removeSnapshotParameterPolicy(
        std::string_view bankId,
        const SnapshotTargetIdentity& target);
    [[nodiscard]] EditResult captureSnapshotValues(
        std::string_view bankId,
        std::string_view snapshotId,
        std::vector<SnapshotValueEntry> values);
    [[nodiscard]] EditResult migrateSnapshotTarget(
        std::string_view bankId,
        const SnapshotTargetIdentity& staleTarget,
        SnapshotTargetIdentity replacementTarget);
    // An unowned transaction groups the edits one caller makes before it
    // returns. Beginning one closes any open transaction; ending or
    // cancelling one leaves a transaction with an owner open.
    void beginHistoryTransaction();
    void endHistoryTransaction();
    void cancelHistoryTransaction();
    // An owned transaction records only the edits that carry its token.
    [[nodiscard]] HistoryTransactionToken beginOwnedHistoryTransaction();
    // A value transaction records Event Value edits from every source, such
    // as one slider gesture or one MIDI burst; any other edit closes it.
    [[nodiscard]] HistoryTransactionToken beginValueHistoryTransaction();
    void endOwnedHistoryTransaction(HistoryTransactionToken token);
    void cancelOwnedHistoryTransaction(HistoryTransactionToken token);
    [[nodiscard]] bool ownsHistoryTransaction(
        HistoryTransactionToken token) const noexcept;
    [[nodiscard]] bool hasValueHistoryTransaction() const noexcept;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] EditResult undo();
    [[nodiscard]] EditResult redo();
    // Undo belongs to the window in front. Each
    // edit is kept with the window it was made in (the one set when it was
    // made, or when its gesture began); Undo and Redo in a window take back
    // or do again the newest edit only when that window made it, and are
    // grayed out there otherwise, until the window that made it is in front.
    // Window 0, the default, is no window in particular, so an editor whose
    // windows are never set undoes as undo() does.
    using HistoryWindow = std::uint64_t;
    void setHistoryWindow(HistoryWindow window) noexcept;
    [[nodiscard]] HistoryWindow historyWindow() const noexcept;
    [[nodiscard]] bool canUndoIn(HistoryWindow window) const noexcept;
    [[nodiscard]] bool canRedoIn(HistoryWindow window) const noexcept;
    [[nodiscard]] EditResult undoIn(HistoryWindow window);
    [[nodiscard]] EditResult redoIn(HistoryWindow window);
    // How many times replaceDocument() has put a new document in place,
    // as opening a file does: what was open on the old one can tell.
    [[nodiscard]] std::uint64_t documentReplacements() const noexcept;
    [[nodiscard]] talas::core::ValidationResult validateForRender() const;

private:
    [[nodiscard]] EditResult applyEffectNodeToScoreClip(std::string_view clipId, ProjectNode effect);
    void advanceDocumentAuthority() noexcept;
    [[nodiscard]] EditResult prepareReplacementDocument(
        ProjectDocument& document,
        ProjectEventValueSyncResult& sync) const;
    [[nodiscard]] EditResult applyDocumentEditInternal(
        ProjectDocument document,
        HistoryTransactionToken transactionToken,
        bool synchronizeDerivedState,
        bool isolateHistory = false);
    void commit(
        ProjectDocument candidate,
        HistoryTransactionToken transactionToken = 0);
    void pushUndo(ProjectDocument snapshot);
    void pushUndo(ProjectDocument snapshot, HistoryWindow window);
    // The window tags beside undoHistory_ and redoHistory_; their capacity
    // is reserved up front, so keeping them never allocates.
    void pushUndoWindow(HistoryWindow window) noexcept;
    void clearRedoHistory() noexcept;
    [[nodiscard]] HistoryTransactionToken openOwnedHistoryTransaction(
        bool valueEdits);
    void closeHistoryTransaction();
    void rollbackHistoryTransaction();

    ProjectDocument document_;
    const talas::core::NodeRegistry& registry_;
    std::vector<ProjectDocument> undoHistory_;
    std::vector<ProjectDocument> redoHistory_;
    std::vector<HistoryWindow> undoWindows_;
    std::vector<HistoryWindow> redoWindows_;
    HistoryWindow historyWindow_ = 0;
    HistoryWindow transactionWindow_ = 0;
    std::uint64_t documentReplacements_ = 0;
    std::optional<ProjectDocument> transactionBefore_;
    bool transactionChanged_ = false;
    HistoryTransactionToken transactionOwner_ = 0;
    bool valueTransaction_ = false;
    HistoryTransactionToken nextTransactionToken_ = 1;
    std::atomic<std::uint64_t> documentAuthorityGeneration_{1};
};
}
