#include "app/workbench/WorkbenchComponentInternal.h"
namespace talas::app::workbench
{
std::optional<std::string_view> optionalStringView(const std::optional<std::string>& value);
talas::project::ProjectParameterExpression expressionParameter(std::string id, std::string expression);
talas::core::Connection connection(std::string sourceNode, std::string sourcePort, std::string targetNode, std::string targetPort);
[[nodiscard]] std::optional<std::string> WorkbenchComponent::runAutomatedSmoke(const std::filesystem::path& smokeDirectory)
{
    try
    {
        if (hardwareEnabled_ || persistentStateEnabled_)
            return "smoke component was not isolated";
        auto require = [](bool condition, std::string message)
            -> std::optional<std::string> {
            if (!condition)
                return message;
            return {};
        };
        auto edit = [](const talas::project::EditResult& result, std::string action)
            -> std::optional<std::string> {
            if (result.ok())
                return {};
            return action + ": " + result.message;
        };
        auto snapshot = [&](std::string_view fileName)
            -> std::optional<std::string> {
            const auto image = createComponentSnapshot(getLocalBounds(), true, 1.0F);
            if (!image.isValid())
                return "Workbench snapshot did not produce a valid image";
            const auto path = (smokeDirectory / fileName).lexically_normal();
            auto stream = juce::File{juceString(path.string())}
                              .createOutputStream();
            if (stream == nullptr || !stream->openedOk())
                return "Workbench snapshot output could not be opened";
            if (!stream->setPosition(0)
                || stream->truncate().failed())
            {
                return "Workbench snapshot output could not be replaced";
            }
            if (!juce::PNGImageFormat{}.writeImageToStream(
                    image,
                    *stream))
            {
                return "Workbench snapshot PNG encoding failed";
            }
            stream->flush();
            return {};
        };
        if (auto error = runE10StartupSmoke(smokeDirectory))
            return error;
        if (auto error = runExpressionLanguageSmoke(smokeDirectory))
            return error;
        if (auto error = runGeneratorWorkflowSmoke(smokeDirectory); error) return error;
        if (auto error = runMidiMapperVirtualOnlyControlSmoke()) return error;
        if (auto error = runMidiMapperRangedLocalControlSmoke()) return error;
        if (auto error = runBuiltInMidiButtonSwitchSmoke()) return error;
        if (auto error = runAuthoringToolSmokes(smokeDirectory)) return error;
        addPrototypeAt("talas.audioOutput", {660, 170});
        addPrototypeAt("talas.vca", {400, 170});
        addPrototypeAt("talas.oscillator", {110, 170});
        addPrototypeAt("talas.constant", {400, 360});
        const auto& created = projectBridge_.editor().document();
        if (auto error = require(
                created.graph.nodes.size() == 4,
                "node creation did not produce four nodes"))
        {
            return error;
        }
        auto nodeIdForType = [&](std::string_view typeId)
            -> std::optional<std::string> {
            const auto& document = projectBridge_.editor().document();
            const auto found = std::ranges::find_if(
                document.graph.nodes,
                [&](const auto& candidate) {
                    return candidate.typeId == typeId;
                });
            if (found == document.graph.nodes.end())
                return {};
            return found->instanceId;
        };
        const auto outputId = nodeIdForType("talas.audioOutput");
        const auto vcaId = nodeIdForType("talas.vca");
        const auto oscillatorId = nodeIdForType("talas.oscillator");
        const auto constantId = nodeIdForType("talas.constant");
        if (auto error = require(
                outputId.has_value() && vcaId.has_value()
                    && oscillatorId.has_value() && constantId.has_value(),
                "created node types could not be resolved"))
        {
            return error;
        }

        if (auto error = runFileOperationSmoke(smokeDirectory, *oscillatorId))
            return error;
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(*oscillatorId, "audioOut", *vcaId, "audioIn")),
                "oscillator-to-VCA connection failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(*constantId, "value", *vcaId, "gain")),
                "constant-to-VCA connection failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(*vcaId, "audioOut", *outputId, "audioIn")),
                "VCA-to-output connection failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().setParameter(
                    *oscillatorId,
                    expressionParameter("frequency", "!SmokePitch")),
                "expression edit failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().setParameter(
                    *constantId,
                    expressionParameter(
                        "value",
                        "series(abs(-0.5), 0.25, 4)[0]")),
                "Pure v3 series/function expression edit failed"))
        {
            return error;
        }
        documentChanged();

        if (auto error = runParameterTransformerSmoke(*oscillatorId, *vcaId))
            return error;

        selectNode(*vcaId);
        if (auto error = require(
                prototypePanel_->descriptorDiscoveryReadyForSmoke(
                    "talas.oscillator",
                    "Prepared realtime: Supported")
                    && graphCanvas_->semanticPortHintContainsForSmoke(
                        *oscillatorId,
                        "audioOut",
                        true,
                        "rate sample")
                    && graphCanvas_->selectedWireEmphasisForSmoke(
                        *oscillatorId,
                        *vcaId)
                    && graphCanvas_->adapterSuggestionContainsForSmoke(
                        "talas.oscillator",
                        "audioOut",
                        "talas.inverseFftV1",
                        "spectrumIn",
                        "talas.forwardFftV1")
                    && inspectorPanel_->parameterHintContainsForSmoke(
                        "gain: expression, domain scalar"),
                "E3 descriptor discovery, semantic wire selection, Info Hints, or adapter guidance was not wired into the Workbench"))
        {
            return error;
        }
        if (auto error = runMidiMapperEditorSmoke(smokeDirectory, *vcaId)) return error;
        const auto& editedDocument = projectBridge_.editor().document();
        const auto smokeControl = std::ranges::find_if(
            editedDocument.virtualControls,
            [](const auto& control) {
                return control.eventName == "SmokePitch"
                    && control.domain == talas::expr::ValueDomain::Hertz;
            });
        if (auto error = require(
                editedDocument.graph.connections.size() == 3
                    && smokeControl != editedDocument.virtualControls.end(),
                "graph connection or Event Value discovery was not restored"))
        {
            return error;
        }
        const auto eventFocusAccepted =
            virtualControlSurface_->focusEventValueForSmoke(
                "SmokePitch");
        const auto focusedEvent = virtualControlSurface_
                                      ->focusedEventValueForSmoke();
        if (auto error = require(
                eventFocusAccepted
                    && focusedEvent
                        == std::optional<std::string>{
                            "SmokePitch"}
                    && workspaceSession_
                           .focusedPerformanceEventName
                        == std::optional<std::string>{
                            "SmokePitch"},
                "Event Value focus did not project into WorkspaceSession: accepted="
                    + std::string{
                        eventFocusAccepted ? "true" : "false"}
                    + "; focused="
                    + focusedEvent.value_or("<none>")
                    + "; session="
                    + workspaceSession_
                          .focusedPerformanceEventName
                          .value_or("<none>")))
        {
            return error;
        }

        if (auto error = runVirtualControlIngressWorkflowForSmoke(
                "SmokePitch"))
            return *error;
        selectNode(*vcaId);
        extractSelectedNodeToSound();
        const auto& extractedDocument =
            projectBridge_.editor().document();
        const auto extractedInstance =
            std::ranges::find_if(
                extractedDocument.graph.nodes,
                [](const auto& candidate) {
                    return candidate.soundDefinitionId.has_value();
                });
        if (auto error = require(
                extractedDocument.soundDefinitions.size() == 1
                    && extractedInstance
                        != extractedDocument.graph.nodes.end()
                    && extractedDocument.graph.connections.size()
                        == 3,
                "Workbench Sound extraction did not preserve topology"))
        {
            return error;
        }

        auto editableDefinition =
            extractedDocument.soundDefinitions.front();
        const auto internalVca = std::ranges::find_if(
            editableDefinition.graph.nodes,
            [](const auto& candidate) {
                return candidate.typeId == "talas.vca";
            });
        if (auto error = require(
                internalVca != editableDefinition.graph.nodes.end(),
                "extracted definition did not retain its internal VCA"))
        {
            return error;
        }
        talas::project::ProjectSoundExposedParameter gain;
        gain.parameterId = "gainScale";
        gain.displayName = "Gain Scale";
        gain.kind = talas::core::ParameterKind::Expression;
        gain.domain = talas::expr::ValueDomain::Scalar;
        gain.defaultValue = expressionParameter("gainScale", "0.5");
        gain.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            internalVca->instanceId,
            "gain",
            {},
            talas::expr::ValueDomain::Scalar,
        });
        editableDefinition.exposedParameters.push_back(std::move(gain));
        addTimelineScoreGainFixture(editableDefinition);
        if (auto error = edit(
                projectBridge_.editor().replaceSoundDefinition(
                    editableDefinition.definitionId,
                    editableDefinition),
                "Workbench exposed-interface fixture failed"))
        {
            return error;
        }
        documentChanged();
        selectNode(extractedInstance->instanceId);
        showSoundDefinition(editableDefinition.definitionId);
        selectNode(
            std::optional<std::string>{editableDefinition.definitionId},
            internalVca->instanceId);
        const auto scopedSelection = selectedNodeId_;
        const auto* internalPresentation =
            talas::project::findProjectNodePresentation(
                projectBridge_.editor().document(),
                editableDefinition.definitionId,
                internalVca->instanceId);
        if (auto error = require(
                internalPresentation != nullptr,
                "extracted definition lost its portable layout"))
        {
            return error;
        }
        const auto beforeMove = internalPresentation->position;
        if (auto error = edit(
                projectBridge_.editor().moveNodeInSoundDefinition(
                    editableDefinition.definitionId,
                    internalVca->instanceId,
                    {beforeMove.x + 32, beforeMove.y + 32}),
                "editable definition move failed"))
        {
            return error;
        }
        documentChanged();
        undoProject();
        redoProject();
        if (auto error = require(
                talas::app::soundWorkbenchSessionSelection(
                    workspaceSession_,
                    std::nullopt)
                        == extractedInstance->instanceId
                    && talas::app::soundWorkbenchSessionSelection(
                           workspaceSession_,
                           std::optional<std::string_view>{
                               editableDefinition.definitionId})
                        == scopedSelection
                    && selectedNodeId_ == scopedSelection,
                "Structure definition selection did not survive scoped edit undo/redo"))
        {
            return error;
        }

        const std::array scopedDiagnostic = {
            talas::runtime::Diagnostic{
                talas::runtime::DiagnosticCode::ParameterEvaluationError,
                "expandedScopedVca",
                {},
                "gain",
                "Scoped diagnostic smoke fixture",
                extractedInstance->instanceId
                    + "/"
                    + editableDefinition.definitionId
                    + "::"
                    + internalVca->instanceId,
            },
        };
        publishCompileDiagnostics(scopedDiagnostic);
        if (auto error = require(
                graphCanvas_->hasCompileDiagnostic(
                    extractedInstance->instanceId)
                    && secondaryGraphCanvas_->hasCompileDiagnostic(
                        internalVca->instanceId),
                "nested compile diagnostic did not reach both visible structural boundaries"))
        {
            return error;
        }
        publishCompileDiagnostics({});

        const auto isolatedDiagnosticDocument =
            talas::app::makeSoundDefinitionAuditionDocument(
                projectBridge_.editor().document(),
                editableDefinition.definitionId,
                registry_);
        if (!isolatedDiagnosticDocument.ok())
            return "isolated diagnostic document fixture failed";
        const std::array isolatedDiagnostic = {
            talas::runtime::Diagnostic{
                talas::runtime::DiagnosticCode::ParameterEvaluationError,
                "isolatedExpandedVca",
                {},
                "gain",
                "Isolated audition diagnostic smoke fixture",
                "auditionSound/"
                    + editableDefinition.definitionId
                    + "::"
                    + internalVca->instanceId,
            },
        };
        publishCompileDiagnostics(
            isolatedDiagnostic,
            &*isolatedDiagnosticDocument.document);
        if (auto error = require(
                !graphCanvas_->hasCompileDiagnostic(
                    extractedInstance->instanceId)
                    && secondaryGraphCanvas_->hasCompileDiagnostic(
                        internalVca->instanceId),
                "isolated audition diagnostic did not retain its source document for definition-pane mapping"))
        {
            return error;
        }
        if (auto error = require(
                diagnosticsPanel_->activateFirstTargetForSmoke()
                    && activeGraphContext_
                        == std::optional<std::string>{
                            editableDefinition.definitionId}
                    && selectedNodeId_ == internalVca->instanceId,
                "isolated reusable-Sound compile diagnostic did not navigate from Diagnostics to its current-project definition target"))
        {
            return error;
        }
        publishCompileDiagnostics({});

        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Performance);
        if (auto error = require(
                performancePanel_->controlCount() == 2
                    && performancePanel_
                        ->controlsHaveAccessibleEditors()
                    && talas::app::soundWorkbenchSessionSelection(
                           workspaceSession_,
                           std::nullopt)
                        == extractedInstance->instanceId,
                "Performance workspace did not retain the root Sound-instance identity"))
        {
            return error;
        }
        if (auto error = snapshot("workbench-performance.png"))
            return error;
        performancePanel_->showSnapshotsForSmoke();
        if (auto error = require(
                performancePanel_->snapshotWorkflowIsAccessible()
                    && performancePanel_
                        ->snapshotEmptyStateIsUsableForSmoke()
                    && performancePanel_
                        ->snapshotOffersDefinitionScopeForSmoke()
                    && performancePanel_
                        ->snapshotDefinitionScopeIsSelectableForSmoke()
                    && performancePanel_
                        ->snapshotOffersExactNestedNodeScopeForSmoke(),
                "Performance Snapshots tab lacked an accessible empty state or exact selected-Sound definition scope"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-performance-snapshots-empty.png"))
        {
            return error;
        }
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Patch);
        if (auto error = require(
                selectedNodeId_ == scopedSelection
                    && secondaryGraphContext_
                        == std::optional<std::string>{
                            editableDefinition.definitionId},
                "Structure selection did not restore after Performance view"))
        {
            return error;
        }
        if (auto error = require(
                graphCanvas_->getTitle()
                        == "ROOT Sound graph"
                    && secondaryGraphCanvas_->getTitle()
                        == "SOUND / "
                            + juceString(
                                editableDefinition.definitionId)
                            + " Sound graph"
                    && graphCanvas_->getTitle()
                        != secondaryGraphCanvas_->getTitle()
                    && !graphCanvas_->contextIsActiveForSmoke()
                    && secondaryGraphCanvas_
                        ->contextIsActiveForSmoke()
                    && graphCanvas_->viewScaleForSmoke() >= 0.38F
                    && secondaryGraphCanvas_->viewScaleForSmoke()
                        >= 0.38F
                    && graphCanvas_->graphIsFullyVisibleForSmoke()
                    && secondaryGraphCanvas_
                        ->graphIsFullyVisibleForSmoke(),
                "Structure split lacked distinct active-scope labels or a complete readable fit"))
        {
            return error;
        }
        const auto beforeSplitSessionProject =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        const auto beforeSplitSessionDirty =
            editBoundary_.isDirty(
                projectBridge_.editor().document());
        const auto beforeSplitStageGeneration =
            projectBridge_.latestStageGeneration();
        const auto beforeSplitTransportPlaying = transportPlaying_;
        const auto beforeSplitHierarchy = hierarchyPath_;
        const auto beforeSplitPrimaryDiagnostic =
            graphCanvas_->hasCompileDiagnostic(
                extractedInstance->instanceId);
        const auto beforeSplitSecondaryDiagnostic =
            secondaryGraphCanvas_->hasCompileDiagnostic(
                internalVca->instanceId);
        juce::ApplicationCommandInfo splitCommandInfo(
            commandStructureSplitLeftRight);
        getCommandInfo(
            commandStructureSplitLeftRight,
            splitCommandInfo);
        juce::ApplicationCommandInfo graphViewCommandInfo(
            commandGraphViewCompact);
        getCommandInfo(
            commandGraphViewCompact,
            graphViewCommandInfo);
        if (auto error = require(
                !splitCommandInfo.defaultKeypresses.isEmpty()
                    && !graphViewCommandInfo.defaultKeypresses.isEmpty(),
                "E2 Structure layout and graph presentation commands lacked keyboard bindings"))
        {
            return error;
        }
        const auto defaultSplitTarget = juce::Point<int>{
            graphSplitArea_.getCentreX(),
            graphSplitArea_.getY()
                + static_cast<int>(std::lround(
                    static_cast<double>(
                        graphSplitArea_.getHeight())
                    * 0.7)),
        };
        updateWorkspacePaneSplit(defaultSplitTarget);
        if (auto error = require(
                workspaceSplitHandle_->isVisible()
                    && !graphSplitUsesVerticalDivider_
                    && workspaceSplitHandle_->getHeight() == 8
                    && workspaceSplitHandle_
                           ->getWantsKeyboardFocus()
                    && workspaceSplitHandle_->getTitle()
                           .isNotEmpty()
                    && std::abs(
                           workspaceSession_.paneSplit
                           - 0.7)
                        < 0.01
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == beforeSplitSessionProject
                    && editBoundary_.isDirty(
                           projectBridge_.editor().document())
                        == beforeSplitSessionDirty,
                "Structure pane divider did not remain accessible, local-only, and responsive in the default top/bottom layout"))
        {
            return error;
        }
        const auto pointerAdjustedSplit = workspaceSession_.paneSplit;
        if (auto error = require(
                workspaceSplitHandle_->keyPressed(
                    juce::KeyPress{juce::KeyPress::downKey})
                    && workspaceSession_.paneSplit
                        > pointerAdjustedSplit,
                "Structure divider keyboard adjustment did not update the same bounded workspace split as pointer input"))
        {
            return error;
        }
        workspaceSession_.paneSplit = 0.5;
        setSize(1600, 900);
        resized();
        if (auto error = require(
                workspaceSplitHandle_->isVisible()
                    && graphSplitUsesVerticalDivider_
                    && workspaceSplitHandle_->getWidth() == 8
                    && !graphCanvas_->getBounds().isEmpty()
                    && !secondaryGraphCanvas_
                            ->getBounds()
                            .isEmpty(),
                "Structure pane divider did not adapt to the expanded left/right layout"))
        {
            return error;
        }

        setWorkspaceStructureSplitOrientation(
            talas::project::WorkspaceStructureSplitOrientation::TopBottom);
        if (auto error = require(
                workspaceSplitHandle_->isVisible()
                    && !graphSplitUsesVerticalDivider_
                    && workspaceSplitHandle_->getHeight() == 8,
                "explicit top/bottom Structure split changed at expanded width"))
        {
            return error;
        }
        setWorkspaceStructureSplitOrientation(
            talas::project::WorkspaceStructureSplitOrientation::LeftRight);
        if (auto error = require(
                workspaceSplitHandle_->isVisible()
                    && graphSplitUsesVerticalDivider_
                    && workspaceSplitHandle_->getWidth() == 8,
                "explicit left/right Structure split did not override adaptive layout"))
        {
            return error;
        }

        const auto activeBeforeSwap = activeGraphContext_;
        const auto selectionBeforeSwap = selectedNodeId_;
        swapWorkspaceStructurePanes();
        if (auto error = require(
                !workspaceSession_.primaryPaneFirst
                    && secondaryGraphCanvas_->getX()
                        < graphCanvas_->getX()
                    && activeGraphContext_ == activeBeforeSwap
                    && selectedNodeId_ == selectionBeforeSwap,
                "Structure pane swap changed edit scope/selection or failed to reverse visual order"))
        {
            return error;
        }
        updateWorkspacePaneSplit({
            graphSplitArea_.getX()
                + static_cast<int>(std::lround(
                    static_cast<double>(graphSplitArea_.getWidth())
                    * 0.3)),
            graphSplitArea_.getCentreY(),
        });
        if (auto error = require(
                std::abs(workspaceSession_.paneSplit - 0.7) < 0.01,
                "reversed Structure pane divider did not preserve the primary-pane split authority"))
        {
            return error;
        }

        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::Compact);
        if (auto error = require(
                graphCanvas_->graphViewModeForSmoke()
                        == talas::project::WorkspaceGraphViewMode::Compact
                    && secondaryGraphCanvas_->graphViewModeForSmoke()
                        == talas::project::WorkspaceGraphViewMode::Compact
                    && !graphCanvas_->portsVisibleForSmoke()
                    && !graphCanvas_->wiresVisibleForSmoke()
                    && !graphCanvas_->hasPortAccessibilityForSmoke()
                    && graphCanvas_->getDescription().contains(
                        "without ports or wires"),
                "Compact graph mode retained hidden port/wire interaction or inaccurate accessibility copy"))
        {
            return error;
        }
        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::Ports);
        if (auto error = require(
                graphCanvas_->portsVisibleForSmoke()
                    && !graphCanvas_->wiresVisibleForSmoke()
                    && graphCanvas_->hasPortAccessibilityForSmoke()
                    && secondaryGraphCanvas_->portsVisibleForSmoke()
                    && !secondaryGraphCanvas_->wiresVisibleForSmoke(),
                "Ports graph mode failed to expose typed accessible ports without wires in both panes"))
        {
            return error;
        }
        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::PortsAndWires);
        if (auto error = require(
                graphCanvas_->portsVisibleForSmoke()
                    && graphCanvas_->wiresVisibleForSmoke()
                    && graphCanvas_->hasPortAccessibilityForSmoke()
                    && secondaryGraphCanvas_->portsVisibleForSmoke()
                    && secondaryGraphCanvas_->wiresVisibleForSmoke(),
                "Ports-and-Wires graph mode did not restore the complete two-pane presentation"))
        {
            return error;
        }

        const auto retainedSecondarySelection =
            talas::app::soundWorkbenchSessionSelection(
                workspaceSession_,
                optionalStringView(secondaryGraphContext_));
        toggleSecondaryWorkspacePane();
        if (auto error = require(
                !workspaceSession_.secondaryPaneVisible
                    && secondaryGraphContext_
                        == std::optional<std::string>{
                            editableDefinition.definitionId}
                    && !secondaryGraphCanvas_->isVisible()
                    && graphCanvas_->isVisible()
                    && activeGraphContext_ == primaryGraphContext_
                    && talas::app::soundWorkbenchSessionSelection(
                           workspaceSession_,
                           std::optional<std::string_view>{
                               editableDefinition.definitionId})
                        == retainedSecondarySelection,
                "closing the secondary Structure pane lost its context/selection or left an invisible edit scope"))
        {
            return error;
        }
        toggleSecondaryWorkspacePane();
        if (auto error = require(
                workspaceSession_.secondaryPaneVisible
                    && secondaryGraphCanvas_->isVisible()
                    && workspaceSplitHandle_->isVisible()
                    && talas::app::soundWorkbenchSessionSelection(
                           workspaceSession_,
                           std::optional<std::string_view>{
                               editableDefinition.definitionId})
                        == retainedSecondarySelection,
                "restoring the secondary Structure pane failed to recover its retained local context"))
        {
            return error;
        }
        selectNode(
            secondaryGraphContext_,
            retainedSecondarySelection);

        auto e2RoundTrip = talas::project::parseWorkspaceSessionState(
            talas::project::serializeWorkspaceSessionState(
                workspaceSession_));
        if (auto error = require(
                e2RoundTrip.ok()
                    && e2RoundTrip.state.structureSplitOrientation
                        == talas::project::WorkspaceStructureSplitOrientation::LeftRight
                    && e2RoundTrip.state.secondaryPaneVisible
                    && !e2RoundTrip.state.primaryPaneFirst
                    && e2RoundTrip.state.graphViewMode
                        == talas::project::WorkspaceGraphViewMode::PortsAndWires,
                "E2 Structure layout and graph presentation did not round-trip through local WorkspaceSession v4"))
        {
            return error;
        }
        workspaceSession_ = std::move(e2RoundTrip.state);
        applyWorkspaceSessionProjection(true);
        if (auto error = require(
                graphSplitUsesVerticalDivider_
                    && secondaryGraphCanvas_->getX()
                        < graphCanvas_->getX()
                    && graphCanvas_->wiresVisibleForSmoke()
                    && secondaryGraphCanvas_->wiresVisibleForSmoke()
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == beforeSplitSessionProject
                    && editBoundary_.isDirty(
                           projectBridge_.editor().document())
                        == beforeSplitSessionDirty
                    && projectBridge_.latestStageGeneration()
                        == beforeSplitStageGeneration
                    && transportPlaying_ == beforeSplitTransportPlaying
                    && hierarchyPath_ == beforeSplitHierarchy
                    && graphCanvas_->hasCompileDiagnostic(
                           extractedInstance->instanceId)
                        == beforeSplitPrimaryDiagnostic
                    && secondaryGraphCanvas_->hasCompileDiagnostic(
                           internalVca->instanceId)
                        == beforeSplitSecondaryDiagnostic,
                "pure E2 workspace edits changed project bytes, dirty/runtime/transport state, hierarchy, or restored presentation"))
        {
            return error;
        }
        if (auto error = runAccessibilityScaleSmoke(
                smokeDirectory, extractedInstance->instanceId))
        {
            return error;
        }
        const auto definitionActiveSession = workspaceSession_;
        selectNode(std::nullopt, extractedInstance->instanceId);
        auto rootActiveSplitSession =
            talas::project::parseWorkspaceSessionState(
                talas::project::serializeWorkspaceSessionState(
                    workspaceSession_));
        if (auto error = require(
                rootActiveSplitSession.ok(),
                "root-active split WorkspaceSession did not round-trip"))
        {
            return error;
        }
        workspaceSession_ =
            std::move(rootActiveSplitSession.state);
        applyWorkspaceSessionProjection(true);
        if (auto error = require(
                !activeGraphContext_.has_value()
                    && secondaryGraphContext_
                        == std::optional<std::string>{
                            editableDefinition.definitionId}
                    && hierarchyPath_
                        == std::vector<std::string>{
                            editableDefinition.definitionId},
                "WorkspaceSession reopen lost root-active with a visible secondary definition"))
        {
            return error;
        }
        showRootGraph();
        auto rootOnlySession =
            talas::project::parseWorkspaceSessionState(
                talas::project::serializeWorkspaceSessionState(
                    workspaceSession_));
        if (auto error = require(
                rootOnlySession.ok(),
                "root-only WorkspaceSession did not round-trip"))
        {
            return error;
        }
        workspaceSession_ = std::move(rootOnlySession.state);
        applyWorkspaceSessionProjection(true);
        if (auto error = require(
                !activeGraphContext_.has_value()
                    && !secondaryGraphContext_.has_value()
                    && hierarchyPath_.empty()
                    && talas::project::findWorkspaceGraphSessionState(
                           workspaceSession_,
                           std::optional<std::string_view>{
                               editableDefinition.definitionId})
                        != nullptr,
                "WorkspaceSession reopen resurrected a cached definition after returning to root-only"))
        {
            return error;
        }
        workspaceSession_ = definitionActiveSession;
        applyWorkspaceSessionProjection(true);
        if (auto error = runE4HierarchyNavigationSmoke(
                editableDefinition.definitionId,
                extractedInstance->instanceId,
                scopedSelection,
                definitionActiveSession))
        {
            return error;
        }
        if (auto error = runE8SoundLibraryWorkflowSmoke(
                editableDefinition.definitionId))
        {
            return error;
        }
        if (auto error = snapshot("workbench-structure.png"))
            return error;
        soundDefinitionsPanel_->focusSearch();
        if (auto error = require(
                soundDefinitionsPanel_->searchCanReceiveKeyboardFocus(),
                "reusable Sound search was not keyboard-focusable"))
        {
            return error;
        }

        if (auto error = runSoundLibraryPrivateAuditionSmoke(
                smokeDirectory, editableDefinition.definitionId))
        {
            return error;
        }


        showRootGraph();
        selectNode(extractedInstance->instanceId);
        if (auto error = edit(
                projectBridge_.editor().setParameter(
                    extractedInstance->instanceId,
                    expressionParameter("gainScale", "0.25")),
                "Performance override fixture failed"))
        {
            return error;
        }
        documentChanged();
        selectNode(extractedInstance->instanceId);
        inlineSelectedSound();
        if (auto error = require(
                projectBridge_.editor().document()
                        .soundDefinitions.size()
                    == 1
                    && std::ranges::none_of(
                        projectBridge_.editor().document()
                            .graph.nodes,
                        [](const auto& candidate) {
                            return candidate.soundDefinitionId
                                .has_value();
                        })
                    && projectBridge_.editor().document()
                           .graph.connections.size()
                        == 3,
                "Workbench Sound inline did not restore flat topology"))
        {
            return error;
        }
        const auto inlinedVca = std::ranges::find_if(
            projectBridge_.editor().document().graph.nodes,
            [](const auto& candidate) {
                return candidate.typeId == "talas.vca";
            });
        if (auto error = require(
                inlinedVca
                        != projectBridge_.editor().document().graph.nodes.end()
                    && std::ranges::any_of(
                        inlinedVca->parameters,
                        [](const auto& parameter) {
                            return parameter.parameterId == "gain"
                                && parameter.expression
                                    == std::optional<std::string>{"0.25"};
                        }),
                "Workbench inline did not preserve the stable exposed-parameter override"))
        {
            return error;
        }

        juce::Array<juce::CommandID> workbenchCommands;
        getAllCommands(workbenchCommands);
        if (auto error = require(
                workbenchCommands.contains(commandWorkspacePerformance)
                    && workbenchCommands.contains(commandWorkspaceSamples)
                    && workbenchCommands.contains(commandShowWorkbenchGuide)
                    && workbenchCommands.contains(commandToggleContextHelp),
                "Performance, Samples, Workbench Guide, or Context Help command was not registered"))
        {
            return error;
        }

        const auto sampleOne =
            (smokeDirectory / "sample-map-a.wav").lexically_normal();
        const auto sampleTwo =
            (smokeDirectory / "sample-map-b.wav").lexically_normal();
        const auto sampleCancel =
            (smokeDirectory / "sample-map-cancel.wav").lexically_normal();
        std::vector<double> sampleFrames(4096);
        for (std::size_t index = 0; index < sampleFrames.size(); ++index)
        {
            sampleFrames[index] = 0.35 * std::sin(
                2.0 * std::numbers::pi
                * static_cast<double>(index) / 47.0);
        }
        std::string sampleWriteError;
        const std::array<std::vector<double>, 1> sampleOneChannels = {
            sampleFrames,
        };
        const std::array<std::vector<double>, 1> sampleTwoChannels = {
            std::vector<double>(
                sampleFrames.rbegin(),
                sampleFrames.rend()),
        };
        std::vector<double> cancelFrames(1024 * 1024, 0.125);
        const std::array<std::vector<double>, 1> sampleCancelChannels = {
            std::move(cancelFrames),
        };
        if (auto error = require(
                talas::audio::writePcm16Wav(
                    sampleOne,
                    48000.0,
                    sampleOneChannels,
                    sampleWriteError)
                    && talas::audio::writePcm16Wav(
                        sampleTwo,
                        48000.0,
                        sampleTwoChannels,
                        sampleWriteError)
                    && talas::audio::writePcm16Wav(
                        sampleCancel,
                        48000.0,
                        sampleCancelChannels,
                        sampleWriteError),
                "Samples workspace WAV fixtures failed: "
                    + sampleWriteError))
        {
            return error;
        }

        createSampleMap();
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Samples);
        if (auto error = require(
                workspaceSession_.samples.activeMapId.has_value()
                    && workspaceMode_
                        == talas::app::WorkbenchWorkspaceMode::Samples
                    && workspaceSamplesButton_.getToggleState(),
                "Samples workspace command did not establish its portable and local state boundaries"))
        {
            return error;
        }
        const auto sampleMapForSmoke = [&]()
            -> const talas::project::ProjectSampleMap* {
            return workspaceSession_.samples.activeMapId.has_value()
                ? talas::project::findProjectSampleMap(
                      projectBridge_.editor().document(),
                      *workspaceSession_.samples.activeMapId)
                : nullptr;
        };

        const auto waitForImport = [&]() -> std::optional<std::string> {
            const auto deadline =
                juce::Time::getMillisecondCounterHiRes() + 10000.0;
            while (!sampleImportWorkerFinished_.load(
                       std::memory_order_acquire)
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                std::this_thread::yield();
            }
            if (!sampleImportWorkerFinished_.load(
                    std::memory_order_acquire))
            {
                return "Samples import worker exceeded its bounded smoke deadline";
            }
            finishSampleMapImportIfReady();
            return {};
        };
        startSampleMapImport({sampleCancel});
        if (auto error = require(
                sampleImportPresentation_
                        == SampleMapImportPresentation::Running
                    && sampleImportThread_.joinable(),
                "Samples import did not expose an owned running state"))
        {
            return error;
        }
        cancelSampleMapImport();
        if (auto error = waitForImport())
            return error;
        if (auto error = require(
                sampleImportPresentation_
                        == SampleMapImportPresentation::Cancelled
                    && sampleMapForSmoke() != nullptr
                    && sampleMapForSmoke()->regions.empty(),
                "Cancelled Samples import published a partial project edit"))
        {
            return error;
        }
#if !defined(_WIN32)
        std::error_code cleanupFixtureError;
        const auto cleanupFixtureRoot = std::filesystem::canonical(smokeDirectory);
        const auto cleanupPackage =
            (cleanupFixtureRoot / "sample-cleanup-retry.talaspkg")
                .lexically_normal();
        const auto cleanupParent =
            (cleanupPackage
             / talas::project::projectPackageAssetsDirectoryName)
                .lexically_normal();
        const auto dispositionStaging =
            cleanupParent / "Disposition.wav.tmp-owned-relaunch";
        const auto importStaging =
            cleanupParent / "Import.wav.tmp-owned-relaunch";
        std::filesystem::create_directories(
            cleanupParent,
            cleanupFixtureError);
        {
            std::ofstream output(
                cleanupPackage
                / talas::project::projectPackageManifestFileName,
                std::ios::binary | std::ios::trunc);
            output << talas::project::projectPackageManifestContents;
        }
        const auto cleanupProjectSaved = talas::project::saveProjectFile(
            cleanupPackage
                / talas::project::projectPackageDocumentFileName,
            {}).ok();
        std::filesystem::create_directories(
            dispositionStaging,
            cleanupFixtureError);
        std::filesystem::create_directories(
            importStaging,
            cleanupFixtureError);
        {
            std::ofstream output(dispositionStaging / "Asset");
            output << "owned disposition staging";
        }
        {
            std::ofstream output(importStaging / "Asset");
            output << "owned import staging";
        }
        std::filesystem::permissions(
            cleanupParent,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);

        talas::audio::PreparedProjectAssetDisposition dispositionCleanup;
        dispositionCleanup.cleanupImports.push_back({
            {},
            "retained smoke staging",
            false,
            talas::audio::ProjectPackageImportErrorCode::CleanupFailed,
            dispositionStaging,
        });
        talas::audio::PreparedSampleMapImport importCleanup;
        importCleanup.cleanupImports.push_back({
            {},
            "retained smoke staging",
            false,
            talas::audio::ProjectPackageImportErrorCode::CleanupFailed,
            importStaging,
        });
        const auto firstDispositionCleanup =
            discardOrRetainSampleAssetDispositionCleanup(
                dispositionCleanup);
        const auto firstImportCleanup =
            discardOrRetainSampleMapImportCleanup(importCleanup);
        const auto retainedFirstFailure =
            !cleanupFixtureError
            && cleanupProjectSaved
            && !firstDispositionCleanup.empty()
            && !firstImportCleanup.empty()
            && pendingSampleAssetDispositionCleanup_.size() == 1
            && pendingSampleMapImportCleanup_.size() == 1
            && std::filesystem::is_regular_file(cleanupLedgerPath_)
            && (std::filesystem::status(cleanupLedgerPath_).permissions()
                    & (std::filesystem::perms::group_all
                       | std::filesystem::perms::others_all))
                == std::filesystem::perms::none;
        pendingSampleAssetDispositionCleanup_.clear();
        pendingSampleMapImportCleanup_.clear();
        pendingProjectPackageImportCleanup_.clear();
        pendingOwnedCleanupFiles_.clear();
        auto ledgerBackup = cleanupLedgerPath_;
        ledgerBackup += ".bak";
        std::filesystem::rename(
            cleanupLedgerPath_,
            ledgerBackup,
            cleanupFixtureError);
        const auto backupReloadError = loadPendingCleanupLedger();
        const auto recoveredBackupGap =
            backupReloadError.empty()
            && pendingProjectPackageImportCleanup_.size() == 2;

        pendingProjectPackageImportCleanup_.clear();
        auto ledgerTemporary = cleanupLedgerPath_;
        ledgerTemporary += ".tmp";
        std::filesystem::rename(
            cleanupLedgerPath_,
            ledgerTemporary,
            cleanupFixtureError);
        const auto temporaryReloadError = loadPendingCleanupLedger();
        const auto recoveredTemporaryGap =
            temporaryReloadError.empty()
            && pendingProjectPackageImportCleanup_.size() == 2;

        std::filesystem::permissions(
            cleanupParent,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        retryPendingSampleCleanup();
        if (auto error = require(
                retainedFirstFailure
                    && recoveredBackupGap
                    && recoveredTemporaryGap
                    && !cleanupFixtureError
                    && pendingSampleAssetDispositionCleanup_.empty()
                    && pendingSampleMapImportCleanup_.empty()
                    && pendingProjectPackageImportCleanup_.empty()
                    && !std::filesystem::exists(dispositionStaging)
                    && !std::filesystem::exists(importStaging)
                    && !std::filesystem::exists(cleanupLedgerPath_),
                "Samples cleanup ledger did not retain owner-only shutdown state, reload ownership, and consume it after a successful retry"))
        {
            return error;
        }

        const auto liveMixedStaging =
            cleanupParent / "Live.wav.tmp-mixed-relaunch";
        const auto missingMixedPackage =
            (cleanupFixtureRoot / "cleanup-missing.talaspkg")
                .lexically_normal();
        const auto missingMixedAssets = missingMixedPackage
            / talas::project::projectPackageAssetsDirectoryName;
        const auto missingMixedStaging = missingMixedAssets
            / "Missing.wav.tmp-mixed-relaunch";
        std::filesystem::create_directories(
            liveMixedStaging,
            cleanupFixtureError);
        std::filesystem::create_directories(
            missingMixedStaging,
            cleanupFixtureError);
        {
            std::ofstream output(liveMixedStaging / "Asset");
            output << "live mixed staging";
        }
        {
            std::ofstream output(
                missingMixedPackage
                    / talas::project::projectPackageManifestFileName,
                std::ios::binary | std::ios::trunc);
            output << talas::project::projectPackageManifestContents;
        }
        const auto missingMixedProjectSaved =
            talas::project::saveProjectFile(
                missingMixedPackage
                    / talas::project::projectPackageDocumentFileName,
                {}).ok();
        {
            std::ofstream output(missingMixedStaging / "Asset");
            output << "missing mixed staging";
        }
        std::filesystem::permissions(
            cleanupParent,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        std::filesystem::permissions(
            missingMixedAssets,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        talas::audio::PreparedSampleMapImport mixedCleanup;
        mixedCleanup.cleanupImports.push_back({
            {},
            "retained live mixed staging",
            false,
            talas::audio::ProjectPackageImportErrorCode::CleanupFailed,
            liveMixedStaging,
        });
        mixedCleanup.cleanupImports.push_back({
            {},
            "retained missing mixed staging",
            false,
            talas::audio::ProjectPackageImportErrorCode::CleanupFailed,
            missingMixedStaging,
        });
        const auto mixedCleanupError =
            discardOrRetainSampleMapImportCleanup(mixedCleanup);
        const auto mixedLedgerPersisted =
            missingMixedProjectSaved
            && !mixedCleanupError.empty()
            && pendingSampleMapImportCleanup_.size() == 1
            && std::filesystem::is_regular_file(cleanupLedgerPath_);
        std::filesystem::permissions(
            cleanupParent,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        std::filesystem::permissions(
            missingMixedAssets,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        pendingSampleMapImportCleanup_.clear();
        std::filesystem::remove_all(
            missingMixedPackage,
            cleanupFixtureError);
        const auto mixedReloadError = loadPendingCleanupLedger();
        const auto mixedReloadedLive =
            mixedReloadError.empty()
            && pendingProjectPackageImportCleanup_.size() == 1
            && pendingProjectPackageImportCleanup_.front()
                    .retainedStagingPath
                == liveMixedStaging;
        retryPendingSampleCleanup();
        if (auto error = require(
                mixedLedgerPersisted
                    && mixedReloadedLive
                    && pendingProjectPackageImportCleanup_.empty()
                    && !std::filesystem::exists(liveMixedStaging)
                    && !std::filesystem::exists(missingMixedPackage)
                    && !std::filesystem::exists(cleanupLedgerPath_)
                    && !cleanupFixtureError,
                "Missing cleanup package poisoned recovery of a second live retained artifact"))
        {
            return error;
        }

        const auto adversarialStaging =
            (cleanupFixtureRoot / "not-a-package" / "Assets"
             / "Danger.tmp-owned")
                .lexically_normal();
        std::filesystem::create_directories(
            adversarialStaging,
            cleanupFixtureError);
        auto* adversarialArtifact = new juce::DynamicObject();
        adversarialArtifact->setProperty("kind", "import-staging");
        adversarialArtifact->setProperty(
            "path",
            juceString(talas::project::pathToUtf8(adversarialStaging)));
        juce::Array<juce::var> adversarialArtifacts;
        adversarialArtifacts.add(juce::var(adversarialArtifact));
        auto* adversarialRoot = new juce::DynamicObject();
        adversarialRoot->setProperty(
            "format",
            "TalasCleanupLedger");
        adversarialRoot->setProperty("version", 1);
        adversarialRoot->setProperty(
            "artifacts",
            juce::var(adversarialArtifacts));
        {
            std::ofstream output(
                cleanupLedgerPath_,
                std::ios::binary | std::ios::trunc);
            output << juce::JSON::toString(
                juce::var(adversarialRoot),
                true).toStdString();
        }
        std::filesystem::permissions(
            cleanupLedgerPath_,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        pendingProjectPackageImportCleanup_.clear();
        const auto adversarialError = loadPendingCleanupLedger();
        const auto rejectedAdversarialAuthority =
            !adversarialError.empty()
            && pendingProjectPackageImportCleanup_.empty()
            && std::filesystem::is_directory(adversarialStaging);
        std::filesystem::remove(cleanupLedgerPath_, cleanupFixtureError);
        std::filesystem::remove_all(
            adversarialStaging.parent_path().parent_path(),
            cleanupFixtureError);
        if (auto error = require(
                rejectedAdversarialAuthority
                    && !cleanupFixtureError,
                "Cleanup ledger accepted deletion authority outside a canonical non-symlink project package"))
        {
            return error;
        }

        const auto replacementPackage =
            (cleanupFixtureRoot / "cleanup-replacement.talaspkg")
                .lexically_normal();
        const auto replacementAssets = replacementPackage
            / talas::project::projectPackageAssetsDirectoryName;
        const auto replacementAsset = replacementAssets / "same.bin";
        std::filesystem::create_directories(
            replacementAssets,
            cleanupFixtureError);
        {
            std::ofstream output(
                replacementPackage
                    / talas::project::projectPackageManifestFileName,
                std::ios::binary | std::ios::trunc);
            output << talas::project::projectPackageManifestContents;
        }
        const auto oldProjectSaved = talas::project::saveProjectFile(
            replacementPackage
                / talas::project::projectPackageDocumentFileName,
            {}).ok();
        {
            std::ofstream output(
                replacementAsset,
                std::ios::binary | std::ios::trunc);
            output << "old uncommitted candidate bytes";
        }
        const auto oldAssetHash =
            talas::project::calculateProjectAssetSha256(replacementAsset);
        std::filesystem::permissions(
            replacementAssets,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        talas::audio::PreparedProjectAssetDisposition replacementCleanup;
        replacementCleanup.asset.contentHash = oldAssetHash.contentHash;
        replacementCleanup.createdFiles.push_back(replacementAsset);
        const auto replacementCleanupError =
            discardOrRetainSampleAssetDispositionCleanup(
                replacementCleanup);
        const auto persistedOldOwnership =
            oldProjectSaved
            && oldAssetHash.ok()
            && !replacementCleanupError.empty()
            && pendingOwnedCleanupFiles_.size() == 1
            && std::filesystem::is_regular_file(cleanupLedgerPath_);
        std::filesystem::permissions(
            replacementAssets,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            cleanupFixtureError);
        pendingOwnedCleanupFiles_.clear();
        std::filesystem::remove_all(
            replacementPackage,
            cleanupFixtureError);

        std::filesystem::create_directories(
            replacementAssets,
            cleanupFixtureError);
        {
            std::ofstream output(
                replacementPackage
                    / talas::project::projectPackageManifestFileName,
                std::ios::binary | std::ios::trunc);
            output << talas::project::projectPackageManifestContents;
        }
        {
            std::ofstream output(
                replacementAsset,
                std::ios::binary | std::ios::trunc);
            output << "new legitimate replacement-package bytes";
        }
        talas::project::ProjectAsset replacementManifest;
        replacementManifest.id = "Smoke/ReplacementAsset";
        replacementManifest.kind = talas::project::ProjectAssetKind::Binary;
        replacementManifest.path = "Assets/same.bin";
        const auto replacementInspected =
            talas::project::inspectProjectAsset(
                replacementManifest,
                replacementAsset);
        talas::project::ProjectDocument replacementDocument;
        if (replacementInspected.ok())
        {
            auto replacementRecord = replacementInspected.asset;
            replacementRecord.path = "Assets/same.bin";
            replacementRecord.disposition =
                talas::project::ProjectAssetDisposition::Embedded;
            replacementDocument.assets.push_back(
                std::move(replacementRecord));
        }
        const auto replacementProjectSaved =
            replacementInspected.ok()
            && talas::project::saveProjectFile(
                   replacementPackage
                       / talas::project::projectPackageDocumentFileName,
                   replacementDocument).ok();
        const auto newAssetHash =
            talas::project::calculateProjectAssetSha256(replacementAsset);
        const auto replacementReloadError = loadPendingCleanupLedger();
        retryPendingSampleCleanup();
        const auto replacementHashAfterRetry =
            talas::project::calculateProjectAssetSha256(replacementAsset);
        if (auto error = require(
                persistedOldOwnership
                    && replacementProjectSaved
                    && newAssetHash.ok()
                    && replacementReloadError.empty()
                    && pendingOwnedCleanupFiles_.empty()
                    && replacementHashAfterRetry.ok()
                    && replacementHashAfterRetry.contentHash
                        == newAssetHash.contentHash
                    && std::filesystem::is_regular_file(replacementAsset)
                    && !std::filesystem::exists(cleanupLedgerPath_)
                    && !cleanupFixtureError,
                "Cleanup ledger replacement-package guard failed · persisted "
                    + std::to_string(persistedOldOwnership)
                    + " · project "
                    + std::to_string(replacementProjectSaved)
                    + " · reload " + replacementReloadError
                    + " · queue "
                    + std::to_string(pendingOwnedCleanupFiles_.size())
                    + " · after hash "
                    + std::to_string(replacementHashAfterRetry.ok())
                    + " · file "
                    + std::to_string(std::filesystem::is_regular_file(
                        replacementAsset))
                    + " · ledger "
                    + std::to_string(std::filesystem::exists(
                        cleanupLedgerPath_))
                    + " · fs " + cleanupFixtureError.message()))
        {
            return error;
        }
#endif

        if (auto error = runSampleMapImportPreviewSmoke(
                sampleOne,
                sampleTwo,
                waitForImport,
                snapshot))
        {
            return error;
        }
        const auto* importedMap = sampleMapForSmoke();

        const auto firstRegionId = importedMap->regions.front().regionId;
        const auto secondRegionId = importedMap->regions.back().regionId;
        workspaceSession_.samples.selectedRegionIds.clear();
        refreshSampleMapWorkspace();
        resized();
        if (auto error = require(
                sampleMapPanel_->accessibleRegionCountForSmoke() == 2
                    && sampleMapPanel_->invokeKeyboardRegionSelectionForSmoke()
                    && workspaceSession_.samples.selectedRegionIds
                        == std::vector<std::string>{firstRegionId}
                    && sampleMapPanel_->invokeKeyboardRegionSelectionForSmoke()
                    && workspaceSession_.samples.selectedRegionIds
                        == std::vector<std::string>{secondRegionId},
                "Samples map accessibility did not expose every region or select first/next by keyboard"))
        {
            return error;
        }
        selectSampleRegion(
            firstRegionId,
            talas::app::SampleMapSelectionMode::Replace);
        const auto waitForWaveform = [&]() -> std::optional<std::string> {
            const auto deadline =
                juce::Time::getMillisecondCounterHiRes() + 10000.0;
            while (!sampleWaveformWorkerFinished_.load(
                       std::memory_order_acquire)
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                std::this_thread::yield();
            }
            if (!sampleWaveformWorkerFinished_.load(
                    std::memory_order_acquire))
            {
                return "Waveform worker exceeded its bounded smoke deadline";
            }
            finishSampleWaveformIfReady();
            return {};
        };
        if (auto error = waitForWaveform())
            return error;
        resized();
        if (auto error = require(
                sampleMapPanel_->hasReadySurfaceForSmoke()
                    && sampleMapPanel_->hasAccessibleEditorsForSmoke()
                    && waveformPanel_->hasReadySurfaceForSmoke()
                    && sampleRegionInspector_->hasNumericEditorsForSmoke()
                    && sampleRegionInspector_->hasAssetActionsForSmoke(),
                "Samples ready state omitted a surface · map "
                    + std::to_string(sampleMapPanel_->hasReadySurfaceForSmoke())
                    + " · a11y "
                    + std::to_string(sampleMapPanel_->hasAccessibleEditorsForSmoke())
                    + " · waveform "
                    + std::to_string(waveformPanel_->hasReadySurfaceForSmoke())
                    + " · numeric "
                    + std::to_string(sampleRegionInspector_->hasNumericEditorsForSmoke())
                    + " · asset actions "
                    + std::to_string(sampleRegionInspector_->hasAssetActionsForSmoke())
                    + " · waveform error "
                    + sampleWaveformError_.toStdString()))
        {
            return error;
        }

        importedMap = sampleMapForSmoke();
        const auto beforeKeyboardMove =
            *talas::project::findProjectSampleRegion(
                *importedMap,
                firstRegionId);
        if (auto error = require(
                sampleMapPanel_->invokeKeyboardMoveForSmoke(),
                "Samples map did not accept its keyboard move equivalent"))
        {
            return error;
        }
        importedMap = sampleMapForSmoke();
        const auto afterKeyboardMove =
            *talas::project::findProjectSampleRegion(
                *importedMap,
                firstRegionId);
        undoProject();
        importedMap = sampleMapForSmoke();
        const auto afterMoveUndo =
            *talas::project::findProjectSampleRegion(
                *importedMap,
                firstRegionId);
        redoProject();
        if (auto error = require(
                afterKeyboardMove != beforeKeyboardMove
                    && afterMoveUndo == beforeKeyboardMove,
                "One Samples keyboard gesture did not produce one undoable edit"))
        {
            return error;
        }

        const auto beforeZoom =
            workspaceSession_.samples.waveformViewport.normalizedSpan;
        zoomSampleWaveform(2.0);
        if (auto error = require(
                workspaceSession_.samples.waveformViewport.normalizedSpan
                    < beforeZoom,
                "Waveform zoom did not update workspace-local viewport state"))
        {
            return error;
        }

        importedMap = sampleMapForSmoke();
        auto loopRegion = *talas::project::findProjectSampleRegion(
            *importedMap,
            firstRegionId);
        loopRegion.sustainLoop = talas::project::ProjectSampleLoop{
            talas::project::ProjectSampleLoopMode::Alternate,
            4,
            16,
        };
        applySampleRegion(loopRegion);
        moveSampleLoopMarker(
            false,
            talas::app::WaveformLoopMarker::EndExclusive,
            1);
        importedMap = sampleMapForSmoke();
        const auto* loopEdited = talas::project::findProjectSampleRegion(
            *importedMap,
            firstRegionId);
        if (auto error = require(
                loopEdited != nullptr
                    && loopEdited->sustainLoop.has_value()
                    && loopEdited->sustainLoop->mode
                        == talas::project::ProjectSampleLoopMode::Alternate
                    && loopEdited->sustainLoop->endFrameExclusive == 17,
                "Exact alternate-loop marker edit did not reach portable project state"))
        {
            return error;
        }

        workspaceSession_.samples.selectedRegionIds.clear();
        for (const auto& region : importedMap->regions)
            workspaceSession_.samples.selectedRegionIds.push_back(region.regionId);
        refreshSampleMapWorkspace();
        if (auto error = require(
                sampleRegionInspector_->showsMultiSelectionForSmoke(),
                "Samples Inspector did not expose an honest multi-selection state"))
        {
            return error;
        }
        if (auto error = snapshot("workbench-samples.png"))
            return error;

        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                !sampleMapPanel_->getBounds().isEmpty()
                    && !sampleRegionInspector_->getBounds().isEmpty()
                    && !waveformPanel_->getBounds().isEmpty(),
                "Compact Samples workspace lost a summonable dock or task surface"))
        {
            return error;
        }
        if (auto error = snapshot("workbench-samples-compact.png"))
            return error;
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();

        if (auto error = runSampleMapAuditionLifecycleSmoke(
                firstRegionId,
                smokeDirectory
                    / "workbench-sample-map-output-take.wav"))
            return error;

        talas::project::ProjectSoundDefinition eventAudition;
        eventAudition.definitionId = "eventAudition";
        eventAudition.displayName = "Event Audition Fixture";
        eventAudition.inputs.push_back({
            "trigger",
            talas::core::PortType::Event,
            "scope",
            "trigger",
        });
        eventAudition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            "scope",
            "audioOut",
        });
        eventAudition.graph.nodes.push_back({
            "source",
            "talas.oscillator",
            {},
        });
        eventAudition.graph.nodes.push_back({
            "scope",
            "talas.oscilloscopeDisplay",
            {},
        });
        eventAudition.graph.connections.push_back(
            connection("source", "audioOut", "scope", "audioIn"));
        if (auto error = edit(
                projectBridge_.editor().addSoundDefinition(
                    std::move(eventAudition)),
                "unsupported audition fixture failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().moveNodeInSoundDefinition(
                    "eventAudition",
                    "source",
                    {80, 100}),
                "unsupported audition source layout failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().moveNodeInSoundDefinition(
                    "eventAudition",
                    "scope",
                    {360, 100}),
                "unsupported audition scope layout failed"))
        {
            return error;
        }
        documentChanged();

        const auto auditionAssetPath =
            (smokeDirectory / "uninstantiated-audition-table.txt")
                .lexically_normal();
        if (!juce::File{juceString(auditionAssetPath.string())}
                 .replaceWithText("0 1 0 -1\n"))
        {
            return "uninstantiated audition asset fixture could not be written";
        }
        const auto* resourcePlayerDescriptor =
            registry_.findNodeType(
                "talas.samplesFromDiskSingleStep");
        if (resourcePlayerDescriptor == nullptr)
            return "resource player descriptor unavailable for audition asset fixture";
        const auto resourceFixtureNodeId = nextNodeId(
            std::nullopt,
            *resourcePlayerDescriptor);
        addPrototypeAt(
            "talas.samplesFromDiskSingleStep",
            {80, 520});
        if (auto error = edit(
                projectBridge_.editor()
                    .setResourceParameterAsset(
                        resourceFixtureNodeId,
                        "fileName",
                        {
                            "UninstantiatedAuditionTable",
                            talas::project::ProjectAssetKind::Table,
                            talas::project::pathToUtf8(
                                auditionAssetPath),
                        }),
                "uninstantiated audition resource binding failed"))
        {
            return error;
        }
        const auto* boundResourceNode = findNode(
            projectBridge_.editor().document(),
            resourceFixtureNodeId);
        if (boundResourceNode == nullptr)
            return "bound audition resource node was not retained";
        auto resourceAuditionNode = *boundResourceNode;
        resourceAuditionNode.instanceId = "resourcePlayer";
        if (auto error = edit(
                projectBridge_.editor().removeNode(
                    resourceFixtureNodeId),
                "temporary audition resource node removal failed"))
        {
            return error;
        }
        talas::project::ProjectSoundDefinition resourceAudition;
        resourceAudition.definitionId = "resourceAudition";
        resourceAudition.displayName = "Resource Audition Fixture";
        resourceAudition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            "resourcePlayer",
            "audioOut",
        });
        resourceAudition.graph.nodes.push_back(
            std::move(resourceAuditionNode));
        if (auto error = edit(
                projectBridge_.editor().addSoundDefinition(
                    std::move(resourceAudition)),
                "uninstantiated resource audition definition failed"))
        {
            return error;
        }
        documentChanged();
        const auto reachableAuditionAssets = talas::audio::
            referencedProjectAssetParameters(
                projectBridge_.editor().document(), registry_);
        if (auto error = require(
                reachableAuditionAssets.ok()
                    && std::ranges::none_of(
                        reachableAuditionAssets.references,
                        [](const auto& reference) { return reference.assetId
                            == "UninstantiatedAuditionTable"; }),
                "uninstantiated definition asset unexpectedly became reachable from the project root"))
        {
            return error;
        }
        soundDefinitionsPanel_->selectDefinition("resourceAudition");
        startSoundLibraryAuditionWithProfile(
            "project:resourceAudition",
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire));
        if (auto error = waitForSoundLibraryAuditionSmoke())
            return error;
        std::array<float, 64> resourceAuditionLeft{};
        std::array<float, 64> resourceAuditionRight{};
        std::array<float*, 2> resourceAuditionChannels{
            resourceAuditionLeft.data(),
            resourceAuditionRight.data(),
        };
        if (auto error = require(
                auditionActive_
                    && renderSoundLibraryAudition(
                           0.0,
                           resourceAuditionChannels.data(),
                           resourceAuditionChannels.size(),
                           resourceAuditionLeft.size())
                           .value_or(false)
                    && std::ranges::any_of(
                        resourceAuditionLeft,
                        [](float sample) {
                            return std::isfinite(sample)
                                && std::abs(sample) > 1.0e-6F;
                        }),
                "uninstantiated resource definition audition did not load its isolated document assets: active="
                    + std::string{auditionActive_ ? "true" : "false"}
                    + "; status="
                    + statusLabel_.getText().toStdString()))
        {
            return error;
        }
        if (auto error = runSoundLibraryPendingSwitchPauseSmoke(
                "resourceAudition", editableDefinition.definitionId))
            return error;
        soundLibraryAuditionSmokeUsePreparedProfile_ = true;
        soundDefinitionsPanel_->triggerAuditionActionForSmoke();
        soundLibraryAuditionSmokeUsePreparedProfile_ = false;
        if (auto error = waitForSoundLibraryAuditionSmoke())
            return error;
        if (auto error = require(
                soundLibraryAuditionActive_ != nullptr
                    && soundLibraryAuditionActive_->entry.stableId
                        == "project:" + editableDefinition.definitionId
                    && soundDefinitionsPanel_->hasActiveAuditionState(
                        editableDefinition.definitionId),
                "Sound Library audition switch did not revoke the old private runtime and activate the exact selected entry: selected="
                    + soundDefinitionsPanel_->selectedEntryId()
                    + "; active="
                    + (soundLibraryAuditionActive_ == nullptr
                        ? std::string{"none"}
                        : soundLibraryAuditionActive_->entry.stableId)
                    + "; status="
                    + statusLabel_.getText().toStdString()))
        {
            return error;
        }
        std::array<float, 65> failedAuditionLeft{};
        std::array<float, 65> failedAuditionRight{};
        std::array<float*, 2> failedAuditionChannels{
            failedAuditionLeft.data(), failedAuditionRight.data()};
        (void)renderSoundLibraryAudition(
            0.0,
            failedAuditionChannels.data(),
            failedAuditionChannels.size(),
            failedAuditionLeft.size());
        finishSoundLibraryAuditionIfReady();
        if (auto error = require(
                soundLibraryAuditionActive_ == nullptr
                    && soundDefinitionsPanel_->auditionStateIsIdle(),
                "Sound Library private render failure did not revoke and retire its exact runtime"))
        {
            return error;
        }
        showSoundDefinition("resourceAudition");
        const juce::File auditionAssetFile{
            juceString(auditionAssetPath.string())};
        soundDefinitionsPanel_->selectDefinition("resourceAudition");
        soundLibraryAuditionSmokeHoldNextWorker_ = true;
        startSoundLibraryAuditionWithProfile(
            "project:resourceAudition",
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire));
        if (auto error = waitForSoundLibraryAuditionBarrierSmoke())
            return error;
        if (!auditionAssetFile.deleteFile())
            return "audition asset failure fixture could not be removed";
        soundLibraryAuditionSmokeHoldWorker_.store(
            false, std::memory_order_release);
        if (auto error = waitForSoundLibraryAuditionSmoke())
            return error;
        if (auto error = require(
                soundLibraryAuditionActive_ == nullptr
                    && soundDefinitionsPanel_->auditionStateIsIdle(),
                "Sound Library audition survived a terminal asset-identity change"))
        {
            return error;
        }
        if (!auditionAssetFile.replaceWithText("0 1 0 -1\n"))
            return "audition asset terminal fixture could not be restored";
        if (!auditionAssetFile.deleteFile())
            return "audition asset load-failure fixture could not be removed";
        soundDefinitionsPanel_->selectDefinition("resourceAudition");
        startSoundLibraryAuditionWithProfile(
            "project:resourceAudition",
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire));
        if (auto error = waitForSoundLibraryAuditionSmoke())
            return error;
        if (auto error = require(
                !auditionActive_
                    && secondaryGraphCanvas_->hasCompileDiagnostic(
                        "resourcePlayer")
                    && !latestCompileDiagnostics_.empty()
                    && latestCompileDiagnostics_.front()
                           .structuralSourcePath
                        == "auditionSound/resourceAudition::resourcePlayer",
                "failed reusable Sound asset load did not identify the definition node: status="
                    + statusLabel_.getText().toStdString()))
        {
            return error;
        }
        if (!auditionAssetFile.replaceWithText("0 1 0 -1\n"))
            return "audition asset failure fixture could not be restored";
        publishCompileDiagnostics({});
        showSoundDefinition(editableDefinition.definitionId);
        selectNode(
            std::optional<std::string>{
                editableDefinition.definitionId},
            internalVca->instanceId);
        if (auto error = require(
                !graphCanvas_->getBounds().isEmpty()
                    && !inspectorPanel_->getBounds().isEmpty()
                    && !soundDefinitionsPanel_->getBounds().isEmpty()
                    && !virtualControlSurface_->getBounds().isEmpty()
                    && timelinePanel_->getBounds().isEmpty()
                    && analysisPanel_->getBounds().isEmpty()
                    && !workspacePatchButton_.getBounds().isEmpty()
                    && !workspaceAutomationButton_.getBounds().isEmpty()
                    && !workspaceAnalysisButton_.getBounds().isEmpty()
                    && projectStateLabel_.getText().isNotEmpty()
                    && projectStateLabel_.getY() >= 46
                    && statusLabel_.getY() >= 46
                    && prototypePanel_->getY() >= 76
                    && !playButton_.getBounds().isEmpty(),
                "Patch workspace layout did not produce the expected focused bounds"))
        {
            return error;
        }

        if (auto error = runTimelineScoreShellSmoke(
                smokeDirectory,
                editableDefinition.definitionId))
        {
            return error;
        }

        setSize(960, 640);
        compactDock_ = CompactDock::Library;
        resized();
        if (auto error = require(
                !graphCanvas_->getBounds().isEmpty()
                    && !soundDefinitionsPanel_->getBounds().isEmpty()
                    && inspectorPanel_->getBounds().isEmpty(),
                "compact Library dock did not preserve a usable graph"))
        {
            return error;
        }
        const auto actualFocusOrder = visibleWorkbenchKeyboardFocusOrder();
        const auto traverser = createKeyboardFocusTraverser();
        const auto traversedFocusOrder = traverser->getAllComponents(this);
        std::vector<juce::Component*> expectedToolbarFocus;
        for (auto* component : std::array<juce::Component*, 13>{
                 &playButton_, &pauseButton_, &stopButton_, &undoButton_,
                 &redoButton_, &deleteButton_, &saveButton_, &moreButton_,
                 &workspacePatchButton_, &workspaceAutomationButton_,
                 &workspaceAnalysisButton_, &workspacePerformanceButton_,
                 &workspaceSamplesButton_})
        {
            if (component->isVisible() && component->isEnabled()
                && component->getWantsKeyboardFocus())
            {
                expectedToolbarFocus.push_back(component);
            }
        }
        const auto firstWithin = [&](const juce::Component& root) {
            return std::ranges::find_if(
                actualFocusOrder,
                [&](const auto* component) {
                    return component == &root || root.isParentOf(component);
                });
        };
        const auto libraryFocus = std::ranges::find_if(
            actualFocusOrder,
            [&](const auto* component) {
                return component == prototypePanel_.get()
                    || prototypePanel_->isParentOf(component)
                    || component == soundDefinitionsPanel_.get()
                    || soundDefinitionsPanel_->isParentOf(component);
            });
        const auto graphFocus = firstWithin(*graphCanvas_);
        const auto rightFocus = firstWithin(*inspectorPanel_);
        const auto lowerFocus = firstWithin(*virtualControlSurface_);
        const auto projectStatusFocus = std::ranges::find(
            actualFocusOrder, &projectStateLabel_);
        const auto statusFocus = std::ranges::find(
            actualFocusOrder, &statusLabel_);
        const auto focusIndex = [&](const auto iterator) {
            return iterator == actualFocusOrder.end()
                ? -1
                : static_cast<int>(std::distance(
                    actualFocusOrder.begin(), iterator));
        };
        const auto focusEvidence =
            " count=" + std::to_string(actualFocusOrder.size())
            + " same=" + std::to_string(
                actualFocusOrder == traversedFocusOrder)
            + " front=" + std::to_string(
                !actualFocusOrder.empty() && !expectedToolbarFocus.empty()
                && actualFocusOrder.front()
                    == expectedToolbarFocus.front())
            + " next=" + std::to_string(
                expectedToolbarFocus.size() >= 2
                && traverser->getNextComponent(expectedToolbarFocus.front())
                    == expectedToolbarFocus[1])
            + " previous=" + std::to_string(
                expectedToolbarFocus.size() >= 2
                && traverser->getPreviousComponent(expectedToolbarFocus[1])
                    == expectedToolbarFocus.front())
            + " library=" + std::to_string(focusIndex(libraryFocus))
            + " graph=" + std::to_string(focusIndex(graphFocus))
            + " right=" + std::to_string(focusIndex(rightFocus))
            + " lower=" + std::to_string(focusIndex(lowerFocus))
            + " project-status="
            + std::to_string(focusIndex(projectStatusFocus))
            + " status=" + std::to_string(focusIndex(statusFocus))
            + " status-previous=" + std::to_string(
                traverser->getPreviousComponent(&statusLabel_)
                    == &projectStateLabel_);
        if (auto error = require(
                actualFocusOrder == traversedFocusOrder
                    && expectedToolbarFocus.size() >= 2
                    && actualFocusOrder.size()
                        >= expectedToolbarFocus.size()
                    && std::equal(
                        expectedToolbarFocus.begin(),
                        expectedToolbarFocus.end(),
                        actualFocusOrder.begin())
                    && traverser->getNextComponent(
                           expectedToolbarFocus.front())
                        == expectedToolbarFocus[1]
                    && traverser->getPreviousComponent(
                           expectedToolbarFocus[1])
                        == expectedToolbarFocus.front()
                    && libraryFocus != actualFocusOrder.end()
                    && graphFocus != actualFocusOrder.end()
                    && rightFocus == actualFocusOrder.end()
                    && lowerFocus != actualFocusOrder.end()
                    && projectStatusFocus != actualFocusOrder.end()
                    && statusFocus != actualFocusOrder.end()
                    && libraryFocus < graphFocus
                    && graphFocus < lowerFocus
                    && lowerFocus < projectStatusFocus
                    && projectStatusFocus < statusFocus
                    && traverser->getPreviousComponent(&statusLabel_)
                        == &projectStateLabel_,
                "native Tab and Shift-Tab traversal did not follow the complete visible task topology"
                    + focusEvidence))
        {
            return error;
        }
        focusCoordinator_.capture(WorkbenchFocusIdentity{
            WorkbenchFocusRegion::Library,
            "library.sounds",
        });
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        const auto compactRightFocusOrder =
            visibleWorkbenchKeyboardFocusOrder();
        const auto compactRightLibrary = std::ranges::find_if(
            compactRightFocusOrder,
            [&](const auto* component) {
                return component == soundDefinitionsPanel_.get()
                    || soundDefinitionsPanel_->isParentOf(component);
            });
        const auto compactRightWorkspace = std::ranges::find_if(
            compactRightFocusOrder,
            [&](const auto* component) {
                return component == inspectorPanel_.get()
                    || inspectorPanel_->isParentOf(component);
            });
        if (auto error = require(
                !graphCanvas_->getBounds().isEmpty()
                    && soundDefinitionsPanel_->getBounds().isEmpty()
                    && !inspectorPanel_->getBounds().isEmpty()
                    && compactRightLibrary
                        == compactRightFocusOrder.end()
                    && compactRightWorkspace
                        != compactRightFocusOrder.end()
                    && lastFocusRestorationTarget_
                        == std::optional<WorkbenchFocusIdentity>{
                            {WorkbenchFocusRegion::PrimaryGraph,
                                "graph.primary"}},
                "compact right workspace did not move hidden Library focus to the active graph"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();

        const auto keyboardNodeId = internalVca->instanceId;
        const auto keyboardContext = activeGraphContext_;
        const auto commandModifiers = juce::ModifierKeys{
            juce::ModifierKeys::commandModifier};
        const auto shiftModifiers = juce::ModifierKeys{
            juce::ModifierKeys::shiftModifier};
        const auto duplicateKey = juce::KeyPress{'d', commandModifiers, 'd'};
        const auto selectAllKey = juce::KeyPress{'a', commandModifiers, 'a'};
        const auto addKey = juce::KeyPress{'a'};
        const auto activateKey = juce::KeyPress{juce::KeyPress::spaceKey};
        const auto escapeKey = juce::KeyPress{juce::KeyPress::escapeKey};
        const auto deleteKey = juce::KeyPress{juce::KeyPress::deleteKey};
        const auto rightKey = juce::KeyPress{juce::KeyPress::rightKey};
        const auto fineDownKey = juce::KeyPress{
            juce::KeyPress::downKey, shiftModifiers, 0};
        auto* keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        const auto keyboardBaseline =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        const auto* keyboardGraph = graphForContext(keyboardContext);
        if (keyboardCanvas == nullptr || keyboardGraph == nullptr)
            return "E7 keyboard graph fixture did not resolve its exact active scope";
        const auto keyboardNodeCount = keyboardGraph->nodes.size();
        const auto otherKeyboardNode = std::ranges::find_if(
            keyboardGraph->nodes,
            [&](const auto& node) {
                return node.instanceId != keyboardNodeId;
            });
        const auto otherKeyboardNodeId =
            otherKeyboardNode == keyboardGraph->nodes.end()
            ? std::string{}
            : otherKeyboardNode->instanceId;
        if (!otherKeyboardNodeId.empty())
        {
            selectNode(keyboardContext, keyboardNodeId, false);
            if (!keyboardCanvas->queueNodeKeyForSmoke(
                    keyboardNodeId, duplicateKey))
            {
                return "graph node did not queue the duplicate authority regression";
            }
            selectNode(
                keyboardContext,
                otherKeyboardNodeId,
                false);
            keyboardCanvas->drainKeyboardCommandsForSmoke();
            if (auto error = require(
                    talas::project::serializeProjectDocument(
                        projectBridge_.editor().document())
                        == keyboardBaseline,
                    "queued graph duplicate rebound to a newer same-context selection"))
            {
                return error;
            }
        }
        selectNode(keyboardContext, keyboardNodeId, false);
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, duplicateKey))
        {
            return "graph node did not accept the real duplicate key event";
        }
        const auto duplicatedKeyboardId = keyboardNodeId + "-copy";
        if (auto error = require(
                graphForContext(keyboardContext)->nodes.size()
                        == keyboardNodeCount + 1
                    && nodeForContext(
                           keyboardContext,
                           duplicatedKeyboardId)
                        != nullptr
                    && selectedNodeIds_
                        == std::vector<std::string>{duplicatedKeyboardId},
                "keyboard duplicate did not preserve exact scope, deterministic identity, and selection"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardBaseline,
                "one Undo did not restore exact bytes after keyboard duplicate"))
        {
            return error;
        }

        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        selectNode(keyboardContext, keyboardNodeId, false);
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, selectAllKey))
        {
            return "graph node did not accept the real select-all key event";
        }
        if (auto error = require(
                selectedNodeIds_.size() == keyboardNodeCount,
                "Cmd/Ctrl+A graph command did not select the exact active graph"))
        {
            return error;
        }
        selectNode(keyboardContext, keyboardNodeId, false);
        const auto positionBeforeKeyboardNudge =
            talas::project::findProjectNodePresentation(
                projectBridge_.editor().document(),
                keyboardContext,
                keyboardNodeId)->position;
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, rightKey)
            || !keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, fineDownKey))
        {
            return "graph node rejected the real held-key nudge events";
        }
        keyboardCanvas->finishKeyboardNudgeForSmoke();
        const auto positionAfterKeyboardNudge =
            talas::project::findProjectNodePresentation(
                projectBridge_.editor().document(),
                keyboardContext,
                keyboardNodeId)->position;
        if (auto error = require(
                positionAfterKeyboardNudge
                    == talas::project::CanvasPosition{
                        positionBeforeKeyboardNudge.x + 32,
                        positionBeforeKeyboardNudge.y + 1},
                "held-key keyboard nudge did not apply exact grid and pixel deltas"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardBaseline,
                "one Undo did not coalesce the complete held-key nudge gesture"))
        {
            return error;
        }

        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        selectNode(keyboardContext, keyboardNodeId, false);
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, rightKey)
            || !keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, escapeKey))
        {
            return "graph node rejected the nudge-cancellation key sequence";
        }
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardBaseline,
                "Escape did not restore exact bytes for an active keyboard nudge"))
        {
            return error;
        }

        if (!otherKeyboardNodeId.empty())
        {
            keyboardCanvas =
                keyboardContext == secondaryGraphContext_
                    && secondaryGraphContext_.has_value()
                ? secondaryGraphCanvas_.get()
                : graphCanvas_.get();
            selectNode(keyboardContext, keyboardNodeId, false);
            const auto otherBefore =
                talas::project::findProjectNodePresentation(
                    projectBridge_.editor().document(),
                    keyboardContext,
                    otherKeyboardNodeId)->position;
            if (!keyboardCanvas->pressNodeKeyForSmoke(
                    keyboardNodeId, rightKey))
            {
                return "graph node rejected authority-change nudge fixture";
            }
            const auto unrelatedResult = keyboardContext.has_value()
                ? projectBridge_.editor().moveNodeInSoundDefinition(
                      *keyboardContext,
                      otherKeyboardNodeId,
                      {otherBefore.x, otherBefore.y + 1})
                : projectBridge_.editor().moveNode(
                      otherKeyboardNodeId,
                      {otherBefore.x, otherBefore.y + 1});
            if (!unrelatedResult.ok())
                return "authority-change fixture could not commit unrelated edit";
            keyboardCanvas->finishKeyboardNudgeForSmoke();
            if (auto error = require(
                    talas::project::findProjectNodePresentation(
                        projectBridge_.editor().document(),
                        keyboardContext,
                        keyboardNodeId)->position
                            == positionBeforeKeyboardNudge
                        && talas::project::findProjectNodePresentation(
                            projectBridge_.editor().document(),
                            keyboardContext,
                            otherKeyboardNodeId)->position
                            == talas::project::CanvasPosition{
                                otherBefore.x, otherBefore.y + 1},
                    "keyboard nudge reverted or joined an unrelated edit"))
            {
                return error;
            }
            undoProject();
            if (auto error = require(
                    talas::project::serializeProjectDocument(
                        projectBridge_.editor().document())
                        == keyboardBaseline,
                    "one Undo did not isolate the unrelated nudge-race edit"))
            {
                return error;
            }
        }

        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        selectNode(keyboardContext, keyboardNodeId, false);
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, duplicateKey))
        {
            return "graph node rejected the duplicate key before port testing";
        }
        const auto keyboardDeleteBaseline =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());

        if (!keyboardCanvas->pressPortKeyForSmoke(
                keyboardNodeId, "audioOut", true, activateKey)
            || !keyboardCanvas->pendingKeyboardConnectionForSmoke()
            || !keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, escapeKey)
            || keyboardCanvas->pendingKeyboardConnectionForSmoke()
            || talas::project::serializeProjectDocument(
                   projectBridge_.editor().document())
                != keyboardDeleteBaseline)
        {
            return "Escape did not cancel the exact keyboard connection without history";
        }

        if (!keyboardCanvas->pressPortKeyForSmoke(
                keyboardNodeId, "audioOut", true, activateKey)
            || !keyboardCanvas->pressPortKeyForSmoke(
                keyboardNodeId, "audioIn", false, activateKey)
            || keyboardCanvas->pendingKeyboardConnectionForSmoke()
            || talas::project::serializeProjectDocument(
                   projectBridge_.editor().document())
                != keyboardDeleteBaseline)
        {
            return "invalid keyboard connection changed project bytes or retained pending state";
        }

        if (!keyboardCanvas->pressPortKeyForSmoke(
                keyboardNodeId, "audioOut", true, activateKey)
            || !keyboardCanvas->pressPortKeyForSmoke(
                duplicatedKeyboardId, "audioIn", false, activateKey))
        {
            return "compatible focused ports rejected keyboard connection";
        }
        const auto keyboardConnectionBytes =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        if (auto error = require(
                std::ranges::any_of(
                    graphForContext(keyboardContext)->connections,
                    [&](const auto& connection) {
                        return connection.fromNode == keyboardNodeId
                            && connection.fromPort == "audioOut"
                            && connection.toNode == duplicatedKeyboardId
                            && connection.toPort == "audioIn";
                    }),
                "keyboard port activation did not publish the exact compatible connection"))
        {
            return error;
        }

        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        if (!keyboardCanvas->pressPortKeyForSmoke(
                duplicatedKeyboardId, "audioIn", false, activateKey))
        {
            return "focused connected input did not accept keyboard disconnect";
        }
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardDeleteBaseline,
                "keyboard disconnect did not restore the exact pre-connection bytes"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardConnectionBytes,
                "one Undo did not restore the exact keyboard-disconnected connection"))
        {
            return error;
        }
        redoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardDeleteBaseline,
                "one Redo did not restore the exact keyboard disconnect"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardConnectionBytes,
                "Undo after disconnect Redo did not restore exact connected bytes"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardDeleteBaseline,
                "second Undo did not remove the complete keyboard connection"))
        {
            return error;
        }

        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        selectNode(keyboardContext, duplicatedKeyboardId, false);
        smokeInputChoice_ = 0; // Yes to the question about each Sound removed.
        const auto deleted = keyboardCanvas->pressNodeKeyForSmoke(duplicatedKeyboardId, deleteKey);
        smokeInputChoice_.reset();
        if (!deleted)
            return "graph node rejected the real Delete key event";
        if (auto error = require(
                nodeForContext(keyboardContext, duplicatedKeyboardId) == nullptr
                    && nodeForContext(keyboardContext, keyboardNodeId) != nullptr
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == keyboardBaseline,
                "keyboard Delete did not remove the exact selected set: "
                    + statusLabel_.getText().toStdString()))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardDeleteBaseline,
                "one Undo did not restore keyboard multi-delete topology"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == keyboardBaseline,
                "second Undo did not remove the keyboard duplicate fixture"))
        {
            return error;
        }

        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Samples);
        const auto hiddenCommandBaseline =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        const auto hiddenKeyHandled =
            keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, duplicateKey);
        const auto hiddenPortHandled =
            keyboardCanvas->pressPortKeyForSmoke(
                keyboardNodeId, "audioOut", true, activateKey);
        const auto hiddenNudgeHandled =
            keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, rightKey);
        if (auto error = require(
                !hiddenKeyHandled
                    && !hiddenPortHandled
                    && !hiddenNudgeHandled
                    && talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == hiddenCommandBaseline,
                "hidden graph accepted a stale command, port action, or nudge"))
        {
            return error;
        }
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
        showSoundDefinition(*keyboardContext);
        selectNode(keyboardContext, keyboardNodeId, false);
        keyboardCanvas =
            keyboardContext == secondaryGraphContext_
                && secondaryGraphContext_.has_value()
            ? secondaryGraphCanvas_.get()
            : graphCanvas_.get();
        if (!keyboardCanvas->pressNodeKeyForSmoke(
                keyboardNodeId, addKey))
        {
            return "graph node rejected the real keyboard Add event";
        }
        if (auto error = require(
                !prototypePanel_->getBounds().isEmpty()
                    && prototypePanel_->searchFocusedForSmoke(),
                "keyboard Add did not expose and focus deterministic prototype search"))
        {
            return error;
        }

        talas::project::ProjectAsset missingRecoveryAsset;
        missingRecoveryAsset.id = "Smoke/MissingAsset";
        missingRecoveryAsset.kind =
            talas::project::ProjectAssetKind::Binary;
        missingRecoveryAsset.path =
            "A very long moved project directory/with UTF-8 ž/and enough segments to require visual elision/missing.bin";
        missingRecoveryAsset.contentHash.assign(64, 'a');
        missingRecoveryAsset.generation = 7;
        missingRecoveryAsset.provenance =
            "workbench smoke fixture";
        auto exactRecoveryAsset = missingRecoveryAsset;
        exactRecoveryAsset.id = "Smoke/ExactAsset";
        exactRecoveryAsset.path = "missing/exact.bin";
        auto exactRecoveryCandidate = exactRecoveryAsset;
        exactRecoveryCandidate.path = "found/exact.bin";
        assetRecoveryCoordinator_.replaceItems({
            {
                missingRecoveryAsset,
                talas::app::AssetRecoveryItemState::Missing,
                "root/recoveryPlayer.fileName",
                "Referenced asset is missing",
            },
            {
                exactRecoveryAsset,
                talas::app::AssetRecoveryItemState::ExactMatch,
                "voice/recoveryPlayer.fileName",
                "Exact SHA-256 match ready to apply",
                {{
                    talas::project::ProjectAssetResolutionMode::Relocation,
                    exactRecoveryAsset.generation,
                    exactRecoveryAsset.contentHash,
                    exactRecoveryCandidate,
                }},
                0,
            },
        });
        refreshAssetRecoveryPanel();
        if (auto error = require(
                assetRecoveryPanel_->hasUsableStateForSmoke()
                    && assetRecoveryPanel_->exactApplyVisibleForSmoke()
                    && !graphCanvas_->getBounds().isEmpty()
                    && inspectorPanel_->getBounds().isEmpty(),
                "populated Asset Recovery surface did not preserve graph context, exact apply, bounds, or keyboard access"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-asset-recovery-populated.png"))
        {
            return error;
        }

        const auto recoverySearchGeneration =
            assetRecoveryCoordinator_.beginSearch();
        if (!assetRecoveryCoordinator_.publishSearchProgress(
                recoverySearchGeneration,
                {421, 73, "deep/folder/current-candidate.bin"}))
        {
            return "Asset Recovery smoke progress generation was rejected";
        }
        refreshAssetRecoveryPanel();
        if (auto error = require(
                assetRecoveryPanel_->activeOperationVisibleForSmoke(),
                "scanning Asset Recovery surface did not isolate Cancel from competing actions"))
        {
            return error;
        }
        if (auto error = verifyAssetRecoveryContextHelpForSmoke()) return error;
        if (auto error = snapshot(
                "workbench-asset-recovery-scanning.png"))
        {
            return error;
        }
        talas::project::ProjectAssetSearchResult cancelledRecoverySearch;
        cancelledRecoverySearch.status = talas::project::ProjectAssetSearchStatus::Cancelled;
        if (!assetRecoveryCoordinator_.finishSearch(
                recoverySearchGeneration,
                std::move(cancelledRecoverySearch)))
        {
            return "Asset Recovery smoke cancellation was rejected";
        }
        refreshAssetRecoveryPanel();

        auto replacementCandidate = missingRecoveryAsset;
        replacementCandidate.path = "chosen/replacement.bin";
        replacementCandidate.contentHash.assign(64, 'b');
        assetRecoveryCoordinator_.replaceItems({{
            missingRecoveryAsset,
            talas::app::AssetRecoveryItemState::Changed,
            "root/recoveryPlayer.fileName",
            "Asset changed on disk",
        }});
        if (!assetRecoveryCoordinator_.setCandidate(
                missingRecoveryAsset.id,
                {
                    talas::project::ProjectAssetResolutionMode::Replacement,
                    missingRecoveryAsset.generation,
                    missingRecoveryAsset.contentHash,
                    replacementCandidate,
                })
            || !assetRecoveryCoordinator_
                    .requestReplacementConfirmation(
                        missingRecoveryAsset.id))
        {
            return "Asset Recovery smoke replacement confirmation could not be prepared";
        }
        refreshAssetRecoveryPanel();
        if (auto error = require(
                assetRecoveryPanel_->confirmationVisibleForSmoke(),
                "different-content Asset Recovery state did not expose its in-context old/new hash confirmation"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-asset-recovery-replacement.png"))
        {
            return error;
        }

        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                assetRecoveryPanel_->hasUsableStateForSmoke()
                    && assetRecoveryPanel_->confirmationVisibleForSmoke()
                    && !graphCanvas_->getBounds().isEmpty(),
                "compact Asset Recovery surface clipped the confirmed replacement workflow or graph"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-asset-recovery-compact.png"))
        {
            return error;
        }
        assetRecoveryCoordinator_.replaceItems({});
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        refreshAssetRecoveryPanel();
        if (auto error = require(
                assetRecoveryPanel_->getBounds().isEmpty()
                    && !inspectorPanel_->getBounds().isEmpty(),
                "resolved Asset Recovery surface did not restore the incumbent right workspace"))
        {
            return error;
        }

        showRootGraph();
        const auto analysisTargetId = nodeIdForType("talas.vca");
        if (!analysisTargetId.has_value())
            return "E6 Analysis smoke target was unavailable";
        selectNode(*analysisTargetId);
        pinSelectedAnalysisTarget();
        if (auto error = require(
                workspaceSession_.analysis.traces.size() == 1
                    && workspaceSession_.analysis.traces.front().nodeId
                        == *analysisTargetId
                    && workspaceSession_.analysis.traces.front().portId
                        == std::optional<std::string>{"audioOut"},
                "E6 Analysis pin did not create one exact local workspace trace"))
        {
            return error;
        }
        const auto audioTraceId =
            workspaceSession_.analysis.traces.front().traceId;
        selectNode(*constantId);
        pinSelectedAnalysisTarget();
        if (auto error = require(
                workspaceSession_.analysis.traces.size() == 2
                    && workspaceSession_.analysis.traces.back().nodeId
                        == *constantId
                    && workspaceSession_.analysis.traces.back().kind
                        == talas::project::WorkspaceAnalysisTraceKind::Control,
                "E6 Analysis second pin did not create one exact Control trace"))
        {
            return error;
        }
        const auto controlTraceId =
            workspaceSession_.analysis.traces.back().traceId;
        moveAnalysisTrace(controlTraceId, -1);
        setAnalysisTraceScale(controlTraceId, 2.0);
        setAnalysisHistoryFrames(240);
        setAnalysisRefreshRateHz(60);
        setAnalysisTraceVisible(controlTraceId, false);
        setAnalysisTraceVisible(controlTraceId, true);
        if (auto error = require(
                workspaceSession_.analysis.traces.front().traceId
                        == controlTraceId
                    && workspaceSession_.analysis.traces.front().verticalScale
                        == 2.0
                    && workspaceSession_.analysis.traces.front().visible
                    && workspaceSession_.analysis.traces.back().traceId
                        == audioTraceId
                    && workspaceSession_.analysis.historyFrames == 240
                    && workspaceSession_.analysis.refreshRateHz == 60,
                "E6 Analysis reorder, visibility, scale, history, or refresh state was not preserved"))
        {
            return error;
        }

        const auto analysisAssetFixtureNodeId = nextNodeId(
            std::nullopt,
            *resourcePlayerDescriptor);
        addPrototypeAt(
            "talas.samplesFromDiskSingleStep",
            {80, 600});
        if (auto error = edit(
                projectBridge_.editor().setResourceParameterAsset(
                    analysisAssetFixtureNodeId,
                    "fileName",
                    {
                        "AnalysisAuditionWatch",
                        talas::project::ProjectAssetKind::Table,
                        talas::project::pathToUtf8(sampleOne),
                    }),
                "E6 Analysis asset-race fixture binding failed"))
        {
            return error;
        }
        const auto* analysisMixerDescriptor =
            registry_.findNodeType("talas.mixer");
        if (analysisMixerDescriptor == nullptr)
            return "E6 Analysis asset-race mixer descriptor was unavailable";
        const auto analysisMixerId = nextNodeId(
            std::nullopt,
            *analysisMixerDescriptor);
        addPrototypeAt("talas.mixer", {510, 590});
        const auto currentOutputId = nodeIdForType("talas.audioOutput");
        if (!currentOutputId.has_value())
            return "E6 Analysis asset-race output was unavailable";
        const auto currentOutputConnection = std::ranges::find_if(
            projectBridge_.editor().document().graph.connections,
            [&](const auto& candidate) {
                return candidate.toNode == *currentOutputId
                    && candidate.toPort == "audioIn";
            });
        if (currentOutputConnection
            == projectBridge_.editor().document().graph.connections.end())
        {
            return "E6 Analysis asset-race main output path was unavailable";
        }
        const auto currentOutputSourceNode =
            currentOutputConnection->fromNode;
        const auto currentOutputSourcePort =
            currentOutputConnection->fromPort;
        if (auto error = edit(
                projectBridge_.editor().disconnect(
                    *currentOutputId,
                    "audioIn"),
                "E6 Analysis asset-race fixture output reroute failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(
                        currentOutputSourceNode,
                        currentOutputSourcePort,
                        analysisMixerId,
                        "in1")),
                "E6 Analysis asset-race fixture main path connection failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(
                        analysisAssetFixtureNodeId,
                        "audioOut",
                        analysisMixerId,
                        "in2")),
                "E6 Analysis asset-race fixture connection failed"))
        {
            return error;
        }
        if (auto error = edit(
                projectBridge_.editor().connect(
                    connection(
                        analysisMixerId,
                        "audioOut",
                        *currentOutputId,
                        "audioIn")),
                "E6 Analysis asset-race fixture mixed output connection failed"))
        {
            return error;
        }
        documentChanged();

        talas::runtime::RenderSettings settings;
        settings.sampleRate = 8000.0;
        settings.maxBlockFrames = 128;
        if (!stageProject(std::move(settings), "automated smoke"))
            return "realtime staging failed: " + statusLabel_.getText().toStdString();
        const auto analysisCapture = projectBridge_.latestAnalysisCapture();
        if (auto error = require(
                analysisCapture != nullptr
                    && std::ranges::all_of(
                        workspaceSession_.analysis.traces,
                        [&](const auto& trace) {
                            return analysisCapture->probeIndexForClientId(
                                trace.traceId).has_value();
                        }),
                "E6 Analysis workspace traces did not bind to their exact prepared probes"))
        {
            return error;
        }
        const auto smokeEventId =
            projectBridge_.realtimeEventValueIdForLatestStage(
                "SmokePitch", talas::expr::ValueDomain::Hertz);
        const auto smokeMidiRoute = midiRuntimeRoutes_.map({
            2,
            74,
            64,
            1,
            midiInputSourceGeneration_.load(std::memory_order_acquire)});
        if (auto error = require(
                smokeEventId.has_value()
                    && smokeMidiRoute.has_value()
                    && smokeMidiRoute->eventId == *smokeEventId
                    && smokeMidiRoute->domain
                        == talas::expr::ValueDomain::Hertz
                    && midiRuntimeRouteDiagnostic_.isEmpty(),
                "staged Event Value did not publish one exact complete MIDI route"))
        {
            return error;
        }
        if (auto error = require(
                midiSourceCapacityFixtureIsValidForSmoke(),
                "MIDI source capacity and exact Clock authority did not fail closed and restore exactly"))
        {
            return error;
        }
        setPlaying(true);
        std::array<float, 1> activationLeft{};
        std::array<float, 1> activationRight{};
        std::array<float*, 2> activationChannels{
            activationLeft.data(),
            activationRight.data(),
        };
        if (!projectBridge_.render(
                activationChannels.data(),
                activationChannels.size(),
                activationLeft.size()))
        {
            return "realtime baseline did not activate before snapshot workflow";
        }
        if (auto error = require(
                analysisPanel_->traceKeyboardCursorUsableForSmoke(),
                "E6 Analysis keyboard cursor did not inspect and clear the selected live trace"))
        {
            return error;
        }
        for (std::size_t index = 0;
             index <= analysisCapture->queueCapacity();
             ++index)
        {
            if (!projectBridge_.render(
                    activationChannels.data(),
                    activationChannels.size(),
                    activationLeft.size()))
            {
                return "realtime Analysis backpressure block did not render";
            }
        }
        analysisPanel_->poll();
        if (auto error = require(
                analysisPanel_->traceStatusContainsForSmoke("dropped"),
                "E6 Analysis did not expose bounded capture drops in its visible status"))
        {
            return error;
        }
        const auto sequenceBeforeFreeze =
            analysisPanel_->selectedTraceSequenceForSmoke();
        setAnalysisFrozen(true);
        refreshAnalysisWorkspace();
        if (!projectBridge_.render(
                activationChannels.data(),
                activationChannels.size(),
                activationLeft.size()))
        {
            return "realtime Analysis freeze-drain block did not render";
        }
        analysisPanel_->poll();
        const auto sequenceWhileFrozen =
            analysisPanel_->selectedTraceSequenceForSmoke();
        setAnalysisFrozen(false);
        refreshAnalysisWorkspace();
        if (!projectBridge_.render(
                activationChannels.data(),
                activationChannels.size(),
                activationLeft.size()))
        {
            return "realtime Analysis unfreeze block did not render";
        }
        analysisPanel_->poll();
        const auto sequenceAfterUnfreeze =
            analysisPanel_->selectedTraceSequenceForSmoke();
        if (auto error = require(
                sequenceBeforeFreeze.has_value()
                    && sequenceWhileFrozen == sequenceBeforeFreeze
                    && sequenceAfterUnfreeze.has_value()
                    && *sequenceAfterUnfreeze > *sequenceBeforeFreeze,
                "E6 Analysis Freeze did not drain without replacing the displayed record, then resume current evidence"))
        {
            return error;
        }

        const auto waitForAnalysisAudition = [&]()
            -> std::optional<std::string> {
            const auto deadline =
                juce::Time::getMillisecondCounterHiRes() + 10000.0;
            while (!analysisAuditionWorkerFinished_.load(
                       std::memory_order_acquire)
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                std::this_thread::yield();
            }
            if (!analysisAuditionWorkerFinished_.load(
                    std::memory_order_acquire))
            {
                return "Analysis audition worker exceeded its bounded smoke deadline";
            }
            finishAnalysisAuditionIfReady();
            return {};
        };
        talas::project::WorkspaceAnalysisAuditionState auditionRequest;
        auditionRequest.traceId = audioTraceId;
        auditionRequest.destinationInputPortId = "audioIn";
        auditionRequest.source =
            talas::project::WorkspaceAnalysisTestSourceKind::Sine;
        auditionRequest.sineFrequencyHz = 1000.0;
        selectAnalysisTrace(audioTraceId);
        const auto projectBytesBeforeAudition =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        const auto dirtyBeforeAudition = editBoundary_.isDirty(
            projectBridge_.editor().document());
        const auto auditionDeviceGeneration =
            deviceGeneration_.load(std::memory_order_acquire);
        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            auditionDeviceGeneration);
        if (auto error = waitForAnalysisAudition())
            return error;
        if (auto error = require(
                analysisAuditionActive_ != nullptr
                    && workspaceSession_.analysis.audition.has_value()
                    && analysisAuditionStatus_.contains("Audition active"),
                "E6 Analysis Sine audition did not reach exact active state"))
        {
            return error;
        }
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Analysis);
        if (auto error = require(
                analysisPanel_->auditionControlsUsableForSmoke()
                    && analysisPanel_->outputTakeControlsUsableForSmoke(),
                "E6 Analysis audition or output-take controls were clipped or lacked an accessible status"))
        {
            return error;
        }
        if (auto error = snapshot("workbench-analysis-audition.png"))
            return error;
        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                !analysisPanel_->getBounds().isEmpty()
                    && analysisPanel_->auditionControlsUsableForSmoke()
                    && analysisPanel_->outputTakeControlsUsableForSmoke(),
                "compact E6 Analysis audition or output-take controls were clipped"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-analysis-audition-compact.png"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();
        const auto mainPositionBeforeAudition =
            projectBridge_.transportFramePosition();
        std::array<const float*, 8> auditionInputs{};
        std::array<float, 128> auditionOutput{};
        std::array<float*, 1> analysisAuditionChannels{
            auditionOutput.data()};
        const auto auditionRendered = renderAnalysisAudition(
            auditionInputs,
            0.0,
            analysisAuditionChannels.data(),
            analysisAuditionChannels.size(),
            auditionOutput.size());
        if (auto error = require(
                auditionRendered == std::optional<bool>{true}
                    && std::ranges::any_of(
                        auditionOutput,
                        [](float sample) {
                            return std::isfinite(sample)
                                && std::abs(sample) > 1.0e-6F;
                        })
                    && projectBridge_.transportFramePosition()
                        == mainPositionBeforeAudition
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == projectBytesBeforeAudition
                    && editBoundary_.isDirty(
                           projectBridge_.editor().document())
                        == dirtyBeforeAudition,
                "E6 Analysis audition did not render audibly while retaining exact project bytes, dirty state, and main graph cursor"))
        {
            return error;
        }
        stopAnalysisAudition();
        auditionOutput.fill(0.0F);
        if (auto error = require(
                !renderAnalysisAudition(
                     auditionInputs,
                     0.0,
                     analysisAuditionChannels.data(),
                     analysisAuditionChannels.size(),
                     auditionOutput.size())
                     .has_value()
                    && projectBridge_.render(
                        analysisAuditionChannels.data(),
                        analysisAuditionChannels.size(),
                        auditionOutput.size())
                    && projectBridge_.transportFramePosition()
                        == mainPositionBeforeAudition
                            + auditionOutput.size()
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == projectBytesBeforeAudition,
                "E6 Analysis Stop did not return atomically to the exact retained main graph"))
        {
            return error;
        }
        const auto outputTakePath =
            smokeDirectory / "workbench-analysis-output-take.wav";
        std::string outputTakeError;
        auto outputTake = talas::audio::RealtimeOutputTake::start(
            {
                outputTakePath,
                talas::audio::ExistingAudioFilePolicy::Replace,
                8000.0,
                1,
                128,
                4,
                projectBridge_.activeStageId(),
                analysisOutputTakeAuthorityGeneration_.load(
                    std::memory_order_acquire),
                projectBridge_.editor().documentAuthorityGeneration(),
            },
            outputTakeError);
        if (outputTake == nullptr)
        {
            return "E6 output-take smoke could not start: "
                + outputTakeError;
        }
        analysisOutputTakeLastSnapshot_.reset();
        analysisOutputTake_ = std::move(outputTake);
        analysisOutputTakeCallback_.store(
            analysisOutputTake_.get(),
            std::memory_order_release);
        analysisOutputTakeLifecycle_.store(1, std::memory_order_seq_cst);
        const auto previousOutputMapping =
            projectOutputChannelMap_[0].load(std::memory_order_relaxed);
        projectOutputChannelMap_[0].store(0, std::memory_order_relaxed);
        std::array<float, 128> callbackTakeOutput{};
        std::array<float*, 1> callbackTakeChannels{
            callbackTakeOutput.data()};
        const juce::AudioIODeviceCallbackContext callbackContext{};
        audioDeviceIOCallbackWithContext(
            nullptr,
            0,
            callbackTakeChannels.data(),
            callbackTakeChannels.size(),
            callbackTakeOutput.size(),
            callbackContext);
        projectOutputChannelMap_[0].store(
            previousOutputMapping,
            std::memory_order_relaxed);
        stopAnalysisOutputTake();
        const auto capturedTake = talas::audio::readPcm16Wav(
            outputTakePath);
        const auto capturedSamplesMatch = capturedTake.ok()
            && capturedTake.sampleRate == 8000
            && capturedTake.channels.size() == 1
            && capturedTake.channels.front().size()
                == callbackTakeOutput.size()
            && std::ranges::all_of(
                std::views::iota(
                    std::size_t{0},
                    callbackTakeOutput.size()),
                [&](std::size_t frame) {
                    return std::abs(
                               capturedTake.channels.front()[frame]
                               - std::clamp(
                                   static_cast<double>(
                                       callbackTakeOutput[frame]),
                                   -1.0,
                                   1.0))
                        <= 1.0 / 32767.0 + 1.0e-9;
                });
        if (auto error = require(
                analysisOutputTake_ == nullptr
                    && analysisOutputTakeLastSnapshot_.has_value()
                    && analysisOutputTakeLastSnapshot_->state
                        == talas::audio::RealtimeOutputTakeState::Completed
                    && analysisOutputTakeLastSnapshot_->writtenFrames
                        == callbackTakeOutput.size()
                    && !analysisOutputTakeLastSnapshot_->incomplete
                    && analysisOutputTakeStatus_.contains(
                        "Output take saved")
                    && std::ranges::any_of(
                        callbackTakeOutput,
                        [](float sample) {
                            return std::isfinite(sample)
                                && std::abs(sample) > 1.0e-6F;
                        })
                    && capturedSamplesMatch,
                "E6 output take did not capture, finalize, and publish the exact callback samples"))
        {
            return error;
        }
        const auto installLifecycleOutputTake =
            [&](const std::filesystem::path& destination, bool renderThroughDeviceCallback = true)
            -> std::optional<std::string> {
            if (!adoptPreparedOutputTakeStageForSmoke())
                return "E6 lifecycle output take could not adopt the exact prepared source stage";
            std::string startError;
            auto candidate = talas::audio::RealtimeOutputTake::start(
                {
                    destination,
                    talas::audio::ExistingAudioFilePolicy::Replace,
                    8000.0,
                    1,
                    128,
                    4,
                    projectBridge_.activeStageId(),
                    analysisOutputTakeAuthorityGeneration_.load(
                        std::memory_order_acquire),
                    projectBridge_.editor().documentAuthorityGeneration(),
                },
                startError);
            if (candidate == nullptr)
            {
                return "E6 lifecycle output take could not start: "
                    + startError;
            }
            analysisOutputTakeLastSnapshot_.reset();
            analysisOutputTakeDiskRecorderCapture_.reset();
            analysisOutputTake_ = std::move(candidate);
            analysisOutputTakeCallback_.store(
                analysisOutputTake_.get(),
                std::memory_order_release);
            analysisOutputTakeLifecycle_.store(
                1,
                std::memory_order_seq_cst);
            const auto previousMapping =
                projectOutputChannelMap_[0].load(
                    std::memory_order_relaxed);
            projectOutputChannelMap_[0].store(
                0,
                std::memory_order_relaxed);
            callbackTakeOutput.fill(0.0F);
            if (renderThroughDeviceCallback)
            {
                audioDeviceIOCallbackWithContext(
                    nullptr, 0, callbackTakeChannels.data(),
                    callbackTakeChannels.size(), callbackTakeOutput.size(), callbackContext);
            }
            else
            {
                callbackTakeOutput.fill(0.25F);
                publishAnalysisOutputTakeFromAudio(
                    callbackTakeChannels.data(), callbackTakeOutput.size(), true);
            }
            projectOutputChannelMap_[0].store(
                previousMapping,
                std::memory_order_relaxed);
            return std::nullopt;
        };
        const auto completedLifecycleTake =
            [&](const std::filesystem::path& destination) {
            const auto capture = talas::audio::readPcm16Wav(destination);
            return analysisOutputTake_ == nullptr
                && analysisOutputTakeLastSnapshot_.has_value()
                && analysisOutputTakeLastSnapshot_->state
                    == talas::audio::RealtimeOutputTakeState::Completed
                && analysisOutputTakeLastSnapshot_->writtenFrames
                    == callbackTakeOutput.size()
                && capture.ok()
                && capture.sampleRate == 8000
                && capture.channels.size() == 1
                && capture.channels.front().size()
                    == callbackTakeOutput.size();
        };
        const auto protectedLifecycleTake = smokeDirectory
            / "workbench-analysis-protected-take.wav";
        if (auto error = installLifecycleOutputTake(protectedLifecycleTake))
            return error;
        const auto priorProjectPath = projectPath();
        const auto priorCleanDocument = editBoundary_.cleanDocument();
        editBoundary_.markSaved(
            projectBridge_.editor().document(),
            protectedLifecycleTake);
        stopAnalysisOutputTake();
        const auto protectedTakeCancelled =
            analysisOutputTake_ == nullptr
            && analysisOutputTakeLastSnapshot_.has_value()
            && analysisOutputTakeLastSnapshot_->state
                == talas::audio::RealtimeOutputTakeState::Cancelled
            && !std::filesystem::exists(protectedLifecycleTake);
        editBoundary_.markRecovered(
            priorCleanDocument,
            priorProjectPath);
        if (auto error = require(
                protectedTakeCancelled,
                "output recording published after its destination became the active protected project path"))
        {
            return error;
        }

        const auto documentLifecycleTake = smokeDirectory
            / "workbench-analysis-document-take.wav";
        if (auto error = installLifecycleOutputTake(
                documentLifecycleTake))
        {
            return error;
        }
        prepareForDocumentReplacement();
        if (auto error = require(
                completedLifecycleTake(documentLifecycleTake),
                "document replacement did not revoke, drain, join, and publish the active output take"))
        {
            return error;
        }

        const auto deviceLifecycleTake = smokeDirectory
            / "workbench-analysis-device-take.wav";
        if (auto error = installLifecycleOutputTake(deviceLifecycleTake))
            return error;
        analysisOutputTakeDeviceEpoch_.fetch_add(
            1,
            std::memory_order_acq_rel);
        requestAnalysisOutputTakeStopFromDevice();
        stopAnalysisOutputTakeForLifecycle();
        if (auto error = require(
                completedLifecycleTake(deviceLifecycleTake),
                "device restart did not revoke callback reachability before draining and publishing the output take"))
        {
            return error;
        }

        const auto restartedLifecycleTake = smokeDirectory
            / "workbench-analysis-device-restarted-take.wav";
        if (auto error = installLifecycleOutputTake(
                restartedLifecycleTake))
        {
            return error;
        }
        stopAnalysisOutputTake();
        if (auto error = require(
                completedLifecycleTake(restartedLifecycleTake),
                "output recording did not restart cleanly after device lifecycle revocation"))
        {
            return error;
        }

        const auto invalidatedLifecycleTake = smokeDirectory
            / "workbench-analysis-authority-invalidated-take.wav";
        if (auto error = installLifecycleOutputTake(invalidatedLifecycleTake))
            return error;
        snapshotAuthoringChanged();
        if (analysisOutputTake_ != nullptr)
            analysisOutputTake_->join();
        finishAnalysisOutputTakeIfReady();
        const auto authorityTakeCancelled = analysisOutputTake_ == nullptr
            && analysisOutputTakeLastSnapshot_.has_value()
            && analysisOutputTakeLastSnapshot_->state
                == talas::audio::RealtimeOutputTakeState::Cancelled
            && analysisOutputTakeLastSnapshot_->diagnostic.find(
                   "project authority changed") != std::string::npos
            && !std::filesystem::exists(invalidatedLifecycleTake);
        if (auto error = require(
                authorityTakeCancelled,
                "snapshot authoring change did not revoke and cancel the prior-authority output take"))
            return error;

        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            auditionDeviceGeneration);
        if (auto error = waitForAnalysisAudition())
            return error;
        if (analysisAuditionActive_ == nullptr)
            return "E6 Analysis stage-race fixture did not become active";
        const auto stageBoundOutputTake = smokeDirectory
            / "workbench-analysis-stage-bound-take.wav";
        if (auto error = installLifecycleOutputTake(stageBoundOutputTake))
            return error;
        talas::runtime::RenderSettings replacementSettings;
        replacementSettings.sampleRate = 8000.0;
        replacementSettings.maxBlockFrames = 128;
        if (!stageProject(
                std::move(replacementSettings),
                "automated Analysis stage replacement"))
        {
            return "E6 Analysis stage replacement did not stage";
        }
        if (auto error = require(
                completedLifecycleTake(stageBoundOutputTake),
                "E6 Analysis stage replacement did not revoke and finalize the prior-stage output take before graph adoption"))
        {
            return error;
        }
        const auto pendingStageOutputTake = smokeDirectory
            / "workbench-analysis-pending-stage-take.wav";
        startAnalysisOutputTake(
            pendingStageOutputTake,
            talas::audio::ExistingAudioFilePolicy::Reject);
        if (auto error = require(
                analysisOutputTake_ == nullptr
                    && analysisOutputTakeStatus_.contains(
                        "stage to be adopted")
                    && !std::filesystem::exists(pendingStageOutputTake),
                "E6 output take began between stage acceptance and callback-boundary adoption · active="
                    + std::to_string(
                        analysisOutputTake_ != nullptr)
                    + " · status="
                    + analysisOutputTakeStatus_.toStdString()
                    + " · active stage="
                    + std::to_string(projectBridge_.activeStageId())
                    + " · prepared stage="
                    + std::to_string(
                        projectBridge_.preparedGraphStatus().prepared
                                != nullptr
                            ? projectBridge_.preparedGraphStatus()
                                  .prepared->stageId
                            : 0)))
        {
            return error;
        }
        setPlaying(true);
        auditionOutput.fill(0.0F);
        if (auto error = require(
                analysisAuditionActive_ == nullptr
                    && !workspaceSession_.analysis.audition.has_value()
                    && !renderAnalysisAudition(
                            auditionInputs,
                            0.0,
                            analysisAuditionChannels.data(),
                            analysisAuditionChannels.size(),
                            auditionOutput.size())
                            .has_value()
                    && projectBridge_.render(
                        analysisAuditionChannels.data(),
                        analysisAuditionChannels.size(),
                        auditionOutput.size()),
                "E6 Analysis stage replacement did not revoke the temporary graph before adopting the new main graph"))
        {
            return error;
        }

        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            auditionDeviceGeneration);
        if (auto error = waitForAnalysisAudition())
            return error;
        if (analysisAuditionActive_ == nullptr)
            return "E6 Analysis commit fixture did not become active";
        commitAnalysisAudition();
        if (auto error = require(
                std::ranges::any_of(
                    projectBridge_.editor().document().graph.nodes,
                    [](const auto& node) {
                        return node.instanceId.starts_with(
                            "talas-analysis-test-source");
                    })
                    && projectBridge_.editor().canUndo()
                    && !workspaceSession_.analysis.audition.has_value(),
                "E6 Analysis Commit did not publish one ordinary undoable project edit"))
        {
            return error;
        }
        if (!projectBridge_.editor().undo().ok())
            return "E6 Analysis Commit could not be undone";
        documentChanged();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == projectBytesBeforeAudition,
                "one undo did not restore exact pre-Commit project bytes"))
        {
            return error;
        }
        talas::runtime::RenderSettings postAuditionSettings;
        postAuditionSettings.sampleRate = 8000.0;
        postAuditionSettings.maxBlockFrames = 128;
        if (!stageProject(
                std::move(postAuditionSettings),
                "automated smoke after Analysis audition"))
        {
            return "E6 Analysis post-Commit undo graph did not restage";
        }
        setPlaying(true);
        auditionOutput.fill(0.0F);
        if (!projectBridge_.render(
                analysisAuditionChannels.data(),
                analysisAuditionChannels.size(),
                auditionOutput.size()))
        {
            return "E6 Analysis post-Commit undo graph did not adopt";
        }

        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            auditionDeviceGeneration);
        const auto raceDeadline =
            juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!analysisAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes() < raceDeadline)
        {
            std::this_thread::yield();
        }
        if (!analysisAuditionWorkerFinished_.load(
                std::memory_order_acquire))
        {
            return "E6 Analysis device-race worker exceeded its bounded smoke deadline";
        }
        deviceGeneration_.fetch_add(1, std::memory_order_acq_rel);
        finishAnalysisAuditionIfReady();
        if (auto error = require(
                analysisAuditionActive_ == nullptr
                    && !workspaceSession_.analysis.audition.has_value()
                    && analysisAuditionStatus_.contains("cancelled"),
                "E6 Analysis stale prepared audition survived a device-generation change"))
        {
            return error;
        }

        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            deviceGeneration_.load(std::memory_order_acquire));
        const auto documentRaceDeadline =
            juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!analysisAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes()
                   < documentRaceDeadline)
        {
            std::this_thread::yield();
        }
        if (!analysisAuditionWorkerFinished_.load(
                std::memory_order_acquire))
        {
            return "E6 Analysis document-race worker exceeded its bounded smoke deadline";
        }
        uiDocumentGeneration_.fetch_add(1, std::memory_order_acq_rel);
        finishAnalysisAuditionIfReady();
        if (auto error = require(
                analysisAuditionActive_ == nullptr
                    && !workspaceSession_.analysis.audition.has_value()
                    && analysisAuditionStatus_.contains("cancelled"),
                "E6 Analysis stale prepared audition survived a document-generation change"))
        {
            return error;
        }

        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            deviceGeneration_.load(std::memory_order_acquire));
        const auto transportRaceDeadline =
            juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!analysisAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes()
                   < transportRaceDeadline)
        {
            std::this_thread::yield();
        }
        if (!analysisAuditionWorkerFinished_.load(
                std::memory_order_acquire))
        {
            return "E6 Analysis transport-race worker exceeded its bounded smoke deadline";
        }
        const auto analysisTransportSettings =
            projectBridge_.editor().document().deviceSettings;
        MidiInputSourceRecord analysisClockSource;
        analysisClockSource.identifier = "smoke-analysis-clock";
        analysisClockSource.displayName = "Smoke Analysis Clock";
        analysisClockSource.slot = 1;
        analysisClockSource.generation =
            midiInputSourceGeneration_.load(std::memory_order_acquire);
        midiInputSources_.push_back(std::move(analysisClockSource));
        auto externalAnalysisClockSettings = analysisTransportSettings;
        externalAnalysisClockSettings.configured = true;
        externalAnalysisClockSettings.sampleRate = 8000.0;
        externalAnalysisClockSettings.bufferSize = 128;
        externalAnalysisClockSettings.outputChannels = {1, 2};
        externalAnalysisClockSettings.midiInputDeviceIdentifiers.push_back(
            "smoke-analysis-clock");
        std::ranges::sort(
            externalAnalysisClockSettings.midiInputDeviceIdentifiers);
        const auto uniqueAnalysisMidiInputs = std::ranges::unique(
            externalAnalysisClockSettings.midiInputDeviceIdentifiers).begin();
        externalAnalysisClockSettings.midiInputDeviceIdentifiers.erase(
            uniqueAnalysisMidiInputs,
            externalAnalysisClockSettings.midiInputDeviceIdentifiers.end());
        externalAnalysisClockSettings.transportSync.sourceMode =
            talas::project::TransportSyncSourceMode::MidiClockExactInput;
        externalAnalysisClockSettings.transportSync.sourceIdentifier =
            "smoke-analysis-clock";
        const auto analysisClockConfigured = projectBridge_.editor()
            .setDeviceSettings(externalAnalysisClockSettings).ok();
        if (analysisClockConfigured)
            rebuildTransportSyncAuthority();
        finishAnalysisAuditionIfReady();
        const auto analysisClockRaceRejected =
            analysisClockConfigured
            && projectBridge_.midiClockTelemetry().externalAuthority
            && analysisAuditionActive_ == nullptr
            && !workspaceSession_.analysis.audition.has_value()
            && analysisAuditionStatus_.contains("cancelled");
        const auto analysisClockRestored = projectBridge_.editor()
            .setDeviceSettings(analysisTransportSettings).ok();
        std::erase_if(midiInputSources_, [](const auto& source) {
            return source.identifier == "smoke-analysis-clock";
        });
        rebuildTransportSyncAuthority();
        bool analysisTransportRestaged = false;
        if (analysisClockRestored)
        {
            talas::runtime::RenderSettings restoredTransportSettings;
            restoredTransportSettings.sampleRate = 8000.0;
            restoredTransportSettings.maxBlockFrames = 128;
            analysisTransportRestaged = stageProject(
                std::move(restoredTransportSettings),
                "automated Analysis transport-race restoration");
            if (analysisTransportRestaged)
            {
                setPlaying(true);
                auditionOutput.fill(0.0F);
                analysisTransportRestaged = projectBridge_.render(
                    analysisAuditionChannels.data(),
                    analysisAuditionChannels.size(),
                    auditionOutput.size());
            }
        }
        if (auto error = require(
                analysisClockRaceRejected
                    && analysisClockRestored
                    && analysisTransportRestaged,
                "E6 Analysis audition prepared under Internal transport was published after exact MIDI Clock authority replaced it"))
        {
            return error;
        }

        const auto assetRaceWatch =
            talas::audio::captureProjectAssetWatchSnapshot(
                projectBridge_.editor().document(),
                registry_,
                projectPath());
        const auto assetRaceEntry = std::ranges::find_if(
            assetRaceWatch.entries,
            [&](const auto& entry) {
                return entry.assetId == "AnalysisAuditionWatch";
            });
        if (assetRaceEntry == assetRaceWatch.entries.end())
            return "E6 Analysis asset-race fixture has no exact reachable sample asset";
        startAnalysisAuditionWithProfile(
            auditionRequest,
            8000.0,
            128,
            deviceGeneration_.load(std::memory_order_acquire));
        const auto assetRaceDeadline =
            juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!analysisAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes()
                   < assetRaceDeadline)
        {
            std::this_thread::yield();
        }
        if (!analysisAuditionWorkerFinished_.load(
                std::memory_order_acquire))
        {
            return "E6 Analysis asset-race worker exceeded its bounded smoke deadline";
        }
        auto heldAssetPath = assetRaceEntry->resolvedPath;
        auto heldAssetBackup = heldAssetPath;
        heldAssetBackup += ".e6-audition-hold-"
            + std::to_string(
                analysisAuditionRequestGeneration_.load(
                    std::memory_order_acquire));
        std::error_code assetRaceError;
        std::filesystem::rename(
            heldAssetPath,
            heldAssetBackup,
            assetRaceError);
        if (assetRaceError)
            return "E6 Analysis asset-race fixture could not hide its exact asset";
        finishAnalysisAuditionIfReady();
        const auto assetRaceCancelled =
            analysisAuditionActive_ == nullptr
            && !workspaceSession_.analysis.audition.has_value()
            && analysisAuditionStatus_.contains("cancelled");
        std::filesystem::rename(
            heldAssetBackup,
            heldAssetPath,
            assetRaceError);
        if (assetRaceError)
            return "E6 Analysis asset-race fixture could not restore its exact asset";
        if (auto error = require(
                assetRaceCancelled,
                "E6 Analysis stale prepared audition survived an exact asset change"))
        {
            return error;
        }
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Performance);
        performancePanel_->setSelectedInstanceId(*constantId);
        if (auto error = performancePanel_
                             ->runSnapshotWorkflowForSmoke())
        {
            return *error;
        }
        performanceCanvas_->refreshRuntimeState();
        if (auto error = require(
                performanceCanvas_->firstControlActivityForSmoke()
                    == talas::app::PerformanceActivitySource::Snapshot,
                "Snapshot recall/morph/randomize did not project explicit canvas activity"))
        {
            return error;
        }
        if (auto error = require(
                performancePanel_
                    ->snapshotPopulatedStateIsUsableForSmoke(),
                "Populated Snapshot workflow lacked usable bounds, focus, or accessible controls"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-performance-snapshots-populated.png"))
        {
            return error;
        }
        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                performancePanel_
                    ->snapshotPopulatedStateIsUsableForSmoke(),
                "Compact Performance Snapshot layout clipped its primary workflow"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-performance-snapshots-compact.png"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Patch);

        std::array<float, 128> left{};
        std::array<float, 128> right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        const auto beforeRealtimeRender =
            projectBridge_.transportFramePosition();
        if (!projectBridge_.render(channels.data(), channels.size(), left.size()))
            return "realtime bridge rejected the smoke render";
        const auto finiteAndNonSilent = std::ranges::any_of(
            left,
            [](float sample) {
                return std::isfinite(sample) && std::abs(sample) > 1.0e-6f;
            })
            || std::ranges::any_of(
                right,
                [](float sample) {
                    return std::isfinite(sample)
                        && std::abs(sample) > 1.0e-6f;
                });
        const auto allFinite = std::ranges::all_of(
            left,
            [](float sample) { return std::isfinite(sample); })
            && std::ranges::all_of(
                right,
                [](float sample) { return std::isfinite(sample); });
        if (auto error = require(
                allFinite && finiteAndNonSilent
                    && projectBridge_.transportFramePosition()
                        == beforeRealtimeRender + left.size(),
                "playback did not produce finite non-silent audio and advance transport"))
        {
            return error;
        }

        showRootGraph();
        const auto preparedOutputSourceId =
            nodeIdForType("talas.mixer");
        if (!preparedOutputSourceId.has_value())
            return "prepared output source fixture was unavailable";
        selectNode(*preparedOutputSourceId);
        refreshPreparedGraphStatus();
        if (!realtimeMetricsFixtureIsValidForSmoke())
            return "realtime metrics smoke fixture was inconsistent";
        const auto stablePresentationCount =
            preparedPresentationRefreshCountForSmoke_;
        refreshPreparedGraphStatus();
        if (auto error = require(
                preparedPresentationRefreshCountForSmoke_
                    == stablePresentationCount,
                "unchanged prepared status rebuilt its presentation"))
        {
            return error;
        }
        currentDocumentStaged_ = false;
        refreshPreparedGraphStatus();
        if (auto error = require(
                preparedPresentationRefreshCountForSmoke_
                        == stablePresentationCount + 1
                    && inspectorPanel_
                        ->preparedContractContainsForSmoke(
                            "last compiled project state"),
                "prepared presentation cache ignored document-stage invalidation"))
        {
            return error;
        }
        currentDocumentStaged_ = true;
        refreshPreparedGraphStatus();
        if (auto error = require(
                projectStateLabel_.getText().contains("out"),
                "prepared graph headline did not reach the visible project state: "
                    + projectStateLabel_.getText().toStdString()))
        {
            return error;
        }
        if (auto error = require(
                projectStateLabel_.getTitle().contains(
                    "Prepared graph:"),
                "prepared graph detail did not reach the accessible project-state title: "
                    + projectStateLabel_.getTitle().toStdString()))
        {
            return error;
        }
        if (auto error = require(
                inspectorPanel_->preparedContractContainsForSmoke(
                    "Output"),
                "prepared output contract did not reach Inspector"))
        {
            return error;
        }
        if (auto error = require(
                analysisPanel_->preparedStatusContainsForSmoke(
                    "Prepared spectral contract"),
                "prepared spectral inventory did not reach Analysis"))
        {
            return error;
        }
        if (auto error = require(
                diagnosticsPanel_
                    ->hasKeyboardAccessiblePreparedStateForSmoke(),
                "prepared diagnostics did not expose a keyboard-accessible list or explicit empty state"))
        {
            return error;
        }
        if (auto error = require(
                diagnosticsPanel_->runtimeDiagnosticContainsForSmoke(
                    "CPU ·",
                    "round-robin sampled node time",
                    talas::app::AnalysisNavigationKind::GraphNode,
                    latestPreparedGraphStatus_.activeStageId),
                "prepared per-node timing did not reach Diagnostics with the exact active-stage graph identity"))
        {
            return error;
        }
        showSoundDefinition(editableDefinition.definitionId);
        selectNode(
            std::optional<std::string>{
                editableDefinition.definitionId},
            internalVca->instanceId);
        diagnosticsPanel_->setRuntimeDiagnostics({
            WorkbenchStatusDiagnostic{
                "Prepared status navigation",
                "Press Return to select the prepared output source",
                {*preparedOutputSourceId, *preparedOutputSourceId},
                WorkbenchStatusDiagnosticSeverity::Info,
            },
        });
        if (auto error = require(
                diagnosticsPanel_->activateFirstTargetForSmoke()
                    && !activeGraphContext_.has_value()
                    && selectedNodeId_ == *preparedOutputSourceId,
                "Return did not navigate a prepared diagnostic to its exact root graph target"))
        {
            return error;
        }
        WorkbenchStatusDiagnostic monitorDiagnostic;
        monitorDiagnostic.title = juceString("MIDI monitor · accepted");
        monitorDiagnostic.detail = "Exact source smoke-input";
        monitorDiagnostic.navigation.kind =
            talas::app::AnalysisNavigationKind::MidiMonitor;
        monitorDiagnostic.navigation.stageId =
            latestPreparedGraphStatus_.activeStageId;
        monitorDiagnostic.navigation.authorityGeneration = 17;
        monitorDiagnostic.navigation.monitorSequence = 23;
        monitorDiagnostic.navigation.sourceId = "smoke-input";
        diagnosticsPanel_->setRuntimeDiagnostics({monitorDiagnostic});
        if (auto error = require(
                diagnosticsPanel_->activateFirstTargetForSmoke()
                    && workspaceMode_
                        == talas::app::WorkbenchWorkspaceMode::Analysis
                    && analysisPanel_->monitorFocusContainsForSmoke(
                        "sequence 23")
                    && analysisPanel_->monitorFocusContainsForSmoke(
                        "smoke-input"),
                "Return did not focus exact monitor evidence in Analysis"))
        {
            return error;
        }
        refreshPreparedGraphStatus();
        if (auto error = snapshot(
                "workbench-prepared-status.png"))
        {
            return error;
        }
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Analysis);
        analysisPanel_->poll();
        if (auto error = require(
                !analysisPanel_->getBounds().isEmpty()
                    && analysisPanel_->traceLayoutControlsUsableForSmoke()
                    && analysisPanel_->auditionControlsUsableForSmoke()
                    && analysisPanel_->outputTakeControlsUsableForSmoke()
                    && analysisPanel_
                        ->preparedStatusContainsForSmoke(
                            "Prepared spectral contract"),
                "Analysis workspace did not retain its prepared spectral inventory or accessible trace layout controls"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-prepared-analysis.png"))
        {
            return error;
        }
        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                !analysisPanel_->getBounds().isEmpty()
                    && analysisPanel_->traceLayoutControlsUsableForSmoke()
                    && analysisPanel_->auditionControlsUsableForSmoke()
                    && analysisPanel_->outputTakeControlsUsableForSmoke(),
                "compact Analysis workspace clipped its trace, audition, or output-take controls"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Patch);
        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = snapshot(
                "workbench-prepared-status-compact.png"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();

        const auto beforeRejectedAuditionFrame =
            projectBridge_.transportFramePosition();
        soundDefinitionsPanel_->selectDefinition("eventAudition");
        startSoundLibraryAuditionWithProfile(
            "project:eventAudition",
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire));
        if (auto error = waitForSoundLibraryAuditionSmoke())
            return error;
        left.fill(0.0F);
        right.fill(0.0F);
        if (auto error = require(
                !auditionActive_
                    && statusLabel_.getText().contains("Event")
                    && projectBridge_.render(
                        channels.data(),
                        channels.size(),
                        left.size())
                    && projectBridge_.transportFramePosition()
                        == beforeRejectedAuditionFrame + left.size()
                    && std::ranges::all_of(
                        left,
                        [](float sample) {
                            return std::isfinite(sample);
                        }),
                "failed isolated audition did not keep the active prepared graph"))
        {
            return error;
        }

        const auto preservedComponent = [](
                                            std::string displayName,
                                            std::string bundleId) {
            talas::project::ProjectPreservedComponent component;
            component.typeVersion = "1.2.3";
            component.stateSchemaVersion = 4;
            component.state = {0x00, 0xff, 0x41};
            component.displayName = std::move(displayName);
            component.bundleId = std::move(bundleId);
            const auto monoAudio =
                talas::core::defaultPortSignalContract(
                    talas::core::PortType::Audio);
            component.inputs.push_back({
                "audioIn",
                talas::core::PortType::Audio,
                false,
                monoAudio,
            });
            component.outputs.push_back({
                "audioOut",
                talas::core::PortType::Audio,
                false,
                monoAudio,
            });
            component.parameters.push_back({
                "mode",
                talas::core::ParameterKind::Choice,
                talas::expr::ValueDomain::Scalar,
                {"clean", "colour"},
            });
            component.recoveryHint =
                "Install the matching owner-approved bundle";
            return component;
        };
        const auto addRegisteredPreservedDescriptor = [this](
                                                         std::string typeId,
                                                         const talas::project::ProjectPreservedComponent& component,
                                                         std::function<void(talas::core::NodeTypeDescriptor&)> mutate) {
            auto descriptor =
                talas::project::makeProjectPreservedComponentDescriptor(
                    std::move(typeId),
                    component);
            mutate(descriptor);
            registry_.addNodeType(std::move(descriptor));
        };

        const auto missingComponent = preservedComponent(
            "Missing Smoke Component",
            "org.talas.smoke.missing");
        const auto versionComponent = preservedComponent(
            "Version Smoke Component",
            "org.talas.smoke.version");
        const auto interfaceComponent = preservedComponent(
            "Interface Smoke Component",
            "org.talas.smoke.interface");
        const auto resolvedComponent = preservedComponent(
            "Resolved Smoke Component",
            "org.talas.smoke.resolved");
        addRegisteredPreservedDescriptor(
            "org.talas.smoke.version",
            versionComponent,
            [](auto& descriptor) {
                descriptor.typeVersion = "2.0.0";
            });
        addRegisteredPreservedDescriptor(
            "org.talas.smoke.interface",
            interfaceComponent,
            [](auto& descriptor) {
                descriptor.outputs.front().id = "renamedOut";
            });
        addRegisteredPreservedDescriptor(
            "org.talas.smoke.resolved",
            resolvedComponent,
            [](auto&) {});

        auto missingDocument = projectBridge_.editor().document();
        const auto appendPreservedNode = [](
                                             talas::project::ProjectGraph& graph,
                                             std::string nodeId,
                                             std::string typeId,
                                             talas::project::ProjectPreservedComponent component) {
            talas::project::ProjectNode node;
            node.instanceId = std::move(nodeId);
            node.typeId = std::move(typeId);
            node.preservedComponent = std::move(component);
            graph.nodes.push_back(std::move(node));
        };
        appendPreservedNode(
            missingDocument.graph,
            "missingSmoke",
            "org.talas.smoke.missing",
            missingComponent);
        appendPreservedNode(
            missingDocument.graph,
            "versionSmoke",
            "org.talas.smoke.version",
            versionComponent);
        appendPreservedNode(
            missingDocument.graph,
            "resolvedSmoke",
            "org.talas.smoke.resolved",
            resolvedComponent);
        auto nestedDefinition = std::ranges::find_if(
            missingDocument.soundDefinitions,
            [&](const auto& definition) {
                return definition.definitionId
                    == editableDefinition.definitionId;
            });
        if (nestedDefinition
            == missingDocument.soundDefinitions.end())
        {
            return "missing-node smoke Sound definition was unavailable";
        }
        appendPreservedNode(
            nestedDefinition->graph,
            "nestedMissingSmoke",
            "org.talas.smoke.missing",
            missingComponent);
        appendPreservedNode(
            nestedDefinition->graph,
            "interfaceSmoke",
            "org.talas.smoke.interface",
            interfaceComponent);
        missingDocument.graph.connections.push_back({
            *preparedOutputSourceId,
            "audioOut",
            "missingSmoke",
            "audioIn",
        });
        nestedDefinition->graph.connections.push_back({
            internalVca->instanceId,
            "audioOut",
            "nestedMissingSmoke",
            "audioIn",
        });
        talas::project::reconcileProjectPresentation(
            missingDocument);
        const auto setPreservedPosition = [&](std::optional<std::string_view> scope,
                                               std::string_view nodeId,
                                               talas::project::CanvasPosition position) {
            auto* presentation =
                talas::project::findProjectNodePresentation(
                    missingDocument,
                    scope,
                    nodeId);
            if (presentation != nullptr)
                presentation->position = position;
            return presentation != nullptr;
        };
        if (!setPreservedPosition(
                std::nullopt,
                "missingSmoke",
                {760, 90})
            || !setPreservedPosition(
                std::nullopt,
                "versionSmoke",
                {760, 250})
            || !setPreservedPosition(
                std::nullopt,
                "resolvedSmoke",
                {760, 410})
            || !setPreservedPosition(
                editableDefinition.definitionId,
                "nestedMissingSmoke",
                {700, 90})
            || !setPreservedPosition(
                editableDefinition.definitionId,
                "interfaceSmoke",
                {700, 250}))
        {
            return "missing-node smoke presentation was unavailable";
        }
        prepareForDocumentReplacement();
        if (auto error = edit(
                projectBridge_.editor().replaceDocument(
                    std::move(missingDocument)),
                "missing-node smoke document was rejected"))
        {
            rebuildOscEgress(); return error;
        }
        documentChanged();
        const auto activeCanvas = [&]() -> GraphCanvas* {
            return activeGraphContext_ == primaryGraphContext_
                ? graphCanvas_.get()
                : secondaryGraphCanvas_.get();
        };
        showRootGraph();
        selectNode("missingSmoke");
        activeCanvas()->zoomToFit(false);
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();
        if (auto error = require(
                activeCanvas()->isVisible()
                    && !activeCanvas()->getBounds().isEmpty()
                    && activeCanvas()->hasNodeForSmoke("missingSmoke")
                    && activeCanvas()->preservedStatusContainsForSmoke(
                        "missingSmoke",
                        "Missing")
                    && activeCanvas()->preservedNodeIsAccessibleForSmoke(
                        "missingSmoke",
                        "Missing")
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Missing",
                        false)
                    && inspectorPanel_->preservedActionsAvailableForSmoke(),
                "root missing-node surface omitted its card, text status, keyboard semantics, details, or safe actions"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-missing-node.png"))
        {
            return error;
        }

#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
        if (auto error = require(
                inspectorPanel_
                    ->componentReplacementActionAvailableForSmoke(),
                "root preserved node omitted its explicit replacement action"))
        {
            return error;
        }
#endif
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
        ComponentReplacementPresentation replacementPreview;
        replacementPreview.state =
            ComponentReplacementPresentationState::AwaitingOwnerApproval;
        replacementPreview.headline = "Approve exact candidate";
        replacementPreview.status =
            "Exact session-local owner approval is required";
        replacementPreview.details =
            "Preserved: com.talas.missing-smoke @ 1.0.0\n"
            "Candidate: com.talas.missing-smoke @ 2.0.0\n"
            "Publisher: Talas smoke fixture\n"
            "Content SHA-256: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
            "Signer SHA-256: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
            "Decision: Exact session-local owner approval is required\n\n"
            "Approval is local to this Workbench session; approval does not stage or execute the candidate.";
        replacementPreview.confirmLabel = "Approve candidate";
        replacementPreview.canChoose = true;
        replacementPreview.canConfirm = true;
        replacementPreview.canCancel = true;
        inspectorPanel_->setComponentReplacementPresentation(
            replacementPreview);
        resized();
        if (auto error = require(
                inspectorPanel_->componentReplacementSurfaceReadyForSmoke(
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    true),
                "replacement preview omitted full copyable identity, confirmation, or cancellation semantics"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-component-replacement-preview.png"))
        {
            return error;
        }
        inspectorPanel_->setComponentReplacementPresentation({});
        resized();
#endif

        const auto beforeDisconnect =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        handlePreservedNodeAction(
            std::nullopt,
            "missingSmoke",
            PreservedNodeAction::Disconnect);
        if (auto error = require(
                std::ranges::none_of(
                    projectBridge_.editor().document()
                        .graph.connections,
                    [](const auto& connection) {
                        return connection.fromNode
                                == "missingSmoke"
                            || connection.toNode
                                == "missingSmoke";
                    }),
                "Workbench preserved-node disconnect left an incident edge"))
        {
            return error;
        }
        undoProject();
        if (auto error = require(
                talas::project::serializeProjectDocument(
                    projectBridge_.editor().document())
                    == beforeDisconnect,
                "one Workbench Undo did not restore the complete preserved node and its edges"))
        {
            return error;
        }

        setSize(960, 640);
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (auto error = require(
                !activeCanvas()->getBounds().isEmpty()
                    && !inspectorPanel_->getBounds().isEmpty()
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Missing",
                        false),
                "compact missing-node workspace lost its graph or Inspector state"))
        {
            return error;
        }
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT)
        inspectorPanel_->setComponentReplacementPresentation(
            replacementPreview);
        resized();
        if (auto error = require(
                !activeCanvas()->getBounds().isEmpty()
                    && inspectorPanel_->componentReplacementSurfaceReadyForSmoke(
                        "com.talas.missing-smoke @ 2.0.0",
                        true),
                "compact replacement preview clipped its graph, exact candidate, or confirmation actions"))
        {
            return error;
        }
        if (auto error = snapshot(
                "workbench-component-replacement-preview-compact.png"))
        {
            return error;
        }
        inspectorPanel_->setComponentReplacementPresentation({});
        resized();
#endif
        if (auto error = snapshot(
                "workbench-missing-node-compact.png"))
        {
            return error;
        }
        setSize(1180, 700);
        compactDock_ = CompactDock::None;
        resized();

        selectNode("versionSmoke");
        if (auto error = require(
                activeCanvas()->preservedStatusContainsForSmoke(
                    "versionSmoke",
                    "Incompatible version")
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Incompatible version",
                        false),
                "version mismatch did not retain its distinct Workbench state"))
        {
            return error;
        }
        selectNode("resolvedSmoke");
        if (auto error = require(
                activeCanvas()->preservedStatusContainsForSmoke(
                    "resolvedSmoke",
                    "Resolved")
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Resolved",
                        true),
                "exactly resolved preserved component did not restore parameter editing"))
        {
            return error;
        }

        showSoundDefinition(editableDefinition.definitionId);
        selectNode(
            editableDefinition.definitionId,
            "nestedMissingSmoke");
        activeCanvas()->zoomToFit(false);
        if (auto error = require(
                activeCanvas()->hasNodeForSmoke(
                    "nestedMissingSmoke")
                    && activeCanvas()->preservedStatusContainsForSmoke(
                        "nestedMissingSmoke",
                        "Missing")
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Missing",
                        false),
                "nested missing-node surface omitted its scoped card or Inspector state"))
        {
            return error;
        }
        selectNode(
            editableDefinition.definitionId,
            "interfaceSmoke");
        if (auto error = require(
                activeCanvas()->preservedStatusContainsForSmoke(
                    "interfaceSmoke",
                    "Incompatible interface")
                    && inspectorPanel_->showsPreservedStateForSmoke(
                        "Incompatible interface",
                        false),
                "interface mismatch did not retain its distinct nested Workbench state"))
        {
            return error;
        }

        showRootGraph();
        selectNode("missingSmoke");
        const auto beforeRejectedMissingFrame =
            projectBridge_.transportFramePosition();
        pendingAssetReplacementId_ = missingRecoveryAsset.id;
        talas::runtime::RenderSettings missingSettings;
        missingSettings.sampleRate = 8000.0;
        missingSettings.maxBlockFrames = 128;
        if (auto error = require(
                !stageProject(
                    std::move(missingSettings),
                    "missing-node smoke")
                    && statusLabel_.getText().contains(
                        "Missing component type")
                    && pendingAssetReplacementId_
                        == std::optional<std::string>{
                            missingRecoveryAsset.id}
                    && projectBridge_.render(
                        channels.data(),
                        channels.size(),
                        left.size())
                    && projectBridge_.transportFramePosition()
                        == beforeRejectedMissingFrame
                            + left.size(),
                "failed Workbench Play preparation did not preflight before asset I/O, name the missing component, or preserve the active generation"))
        {
            return error;
        }
        pendingAssetReplacementId_.reset();

        const auto auditionGenerationBeforeMissingSource =
            sampleMapAuditionGeneration_.load(
                std::memory_order_acquire);
        beginSampleMapAudition({1, 60, 96, 0.0, false, true});
        if (auto error = require(
                sampleMapAuditionPresentation_
                        == SampleMapAuditionPresentation::Failed
                    && !sampleMapAuditionActive_
                    && sampleMapAuditionWorkerFinished_.load(
                        std::memory_order_acquire)
                    && sampleMapAuditionGeneration_.load(
                           std::memory_order_acquire)
                        == auditionGenerationBeforeMissingSource
                    && statusLabel_.getText().contains(
                        "Missing component type"),
                "Samples audition did not reject an unresolved source before worker or graph preparation"))
        {
            return error;
        }

        const auto rejectedExportPath =
            (smokeDirectory / "missing-node-export.wav")
                .lexically_normal();
        {
            std::ofstream sentinel(
                rejectedExportPath,
                std::ios::binary | std::ios::trunc);
            sentinel << "preserve";
            if (!sentinel.good())
                return "missing-node export sentinel could not be written";
        }
        talas::audio::ProjectWavExportOptions exportOptions;
        exportOptions.outputPath = rejectedExportPath;
        exportOptions.sampleRate = 8000.0;
        exportOptions.durationSeconds = 0.01;
        exportOptions.maxBlockFrames = 64;
        const auto rejectedExport =
            talas::audio::exportProjectToPcm16Wav(
                projectBridge_.editor().document(),
                registry_,
                std::move(exportOptions));
        std::ifstream preservedExport(
            rejectedExportPath,
            std::ios::binary);
        const std::string preservedExportBytes{
            std::istreambuf_iterator<char>{preservedExport},
            std::istreambuf_iterator<char>{}};
        if (auto error = require(
                !rejectedExport.ok()
                    && !rejectedExport.diagnostics.empty()
                    && preservedExportBytes == "preserve",
                "Workbench export boundary did not fail closed while preserving the destination"))
        {
            return error;
        }

        const auto projectFile =
            (smokeDirectory / "workbench-smoke.talas.json").lexically_normal();
        assetRecoveryCoordinator_.replaceItems({{
            missingRecoveryAsset,
            talas::app::AssetRecoveryItemState::ReplacementCandidate,
            "root/recoveryPlayer.fileName",
            "Different content requires replacement confirmation",
            {{
                talas::project::ProjectAssetResolutionMode::Replacement,
                missingRecoveryAsset.generation,
                missingRecoveryAsset.contentHash,
                replacementCandidate,
            }},
            0,
        }});
        refreshAssetRecoveryPanel();
        if (!saveProjectToPath(projectFile))
            return "project save failed: " + statusLabel_.getText().toStdString();
        const auto expectedDocument =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document());
        if (auto error = require(
                projectPath() == std::optional<std::filesystem::path>{projectFile}
                    && !editBoundary_.isDirty(projectBridge_.editor().document())
                    && assetRecoveryCoordinator_.state().items.empty(),
                "Save As did not establish the clean path boundary and invalidate old-path recovery candidates"))
        {
            return error;
        }

        newProjectNow();
        loadProjectFromPath(projectFile);
        const auto& reopened = projectBridge_.editor().document();
        const auto reopenedControl = std::ranges::find_if(
            reopened.virtualControls,
            [](const auto& control) {
                return control.eventName == "SmokePitch"
                    && control.domain == talas::expr::ValueDomain::Hertz;
            });
        if (auto error = require(
                talas::project::serializeProjectDocument(reopened)
                        == expectedDocument
                    && reopenedControl != reopened.virtualControls.end()
                    && projectPath()
                        == std::optional<std::filesystem::path>{projectFile}
                    && !editBoundary_.isDirty(reopened) && reopened.formatVersion == talas::project::currentProjectFormatVersion && reopened.generatorLibrary.programs.size() == 2,
                "reopened graph, expression, controls, path, or clean state differed"))
        {
            return error;
        }
        if (auto error = runAnalogSequencerExampleSmoke())
            return error;
        if (auto error = runDiskRecorderOutputTakeSmoke(smokeDirectory))
            return error;
        if (auto error = runDiskCacheCaptureSmoke(smokeDirectory)) return error;
        const auto lifecycleSearchRoot =
            smokeDirectory / "asset-recovery-lifecycle";
        const auto lifecycleCandidate =
            lifecycleSearchRoot / "large-candidate.bin";
        std::filesystem::create_directories(
            lifecycleSearchRoot);
        {
            std::ofstream output(
                lifecycleCandidate,
                std::ios::binary | std::ios::trunc);
            std::array<char, 1024 * 1024> block{};
            block.fill(static_cast<char>(0x5a));
            for (int index = 0; index < 16; ++index)
                output.write(block.data(), block.size());
            if (!output.good())
                return "Asset Recovery lifecycle fixture could not be written";
        }
        talas::project::ProjectAsset lifecycleAsset;
        lifecycleAsset.id = "Smoke/LifecycleAsset";
        lifecycleAsset.kind =
            talas::project::ProjectAssetKind::Binary;
        lifecycleAsset.path = "missing/lifecycle.bin";
        lifecycleAsset.contentHash.assign(64, 'a');
        lifecycleAsset.provenance =
            "workbench lifecycle smoke fixture";
        const auto startLifecycleSearch = [&] {
            assetRecoveryCoordinator_.replaceItems({{
                lifecycleAsset,
                talas::app::AssetRecoveryItemState::Missing,
                "root/lifecycle.fileName",
                "Missing",
            }});
            refreshAssetRecoveryPanel();
            startAssetRecoveryDirectorySearch(
                lifecycleSearchRoot);
        };
        startLifecycleSearch();
        if (auto error = require(
                assetRecoveryThread_.joinable()
                    && assetRecoveryCoordinator_.state().searchActive,
                "Asset Recovery lifecycle worker did not enter an owned cancellable generation"))
        {
            return error;
        }
        resetAssetRecovery();
        if (auto error = require(
                !assetRecoveryThread_.joinable()
                    && assetRecoveryCoordinator_.state().items.empty(),
                "project replacement did not join/cancel Asset Recovery or clear its generation"))
        {
            return error;
        }
        startLifecycleSearch();
        if (auto error = require(
                assetRecoveryThread_.joinable()
                    && assetRecoveryCoordinator_.state().searchActive,
                "Asset Recovery destruction fixture did not leave one owned worker for destructor cancellation"))
        {
            return error;
        }
        const auto shutdownLifecycleTake = smokeDirectory
            / "workbench-analysis-shutdown-take.wav";
        std::error_code shutdownTakeRemovalError;
        (void) std::filesystem::remove(
            shutdownLifecycleTake, shutdownTakeRemovalError);
        if (shutdownTakeRemovalError
            || std::filesystem::exists(shutdownLifecycleTake))
        {
            return "E6 shutdown output-take fixture could not remove stale destination authority";
        }
        if (auto error = installLifecycleOutputTake(shutdownLifecycleTake, false))
        {
            return error;
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return "unexpected exception: " + std::string(error.what());
    }
    catch (...)
    {
        return "unexpected non-standard exception";
    }
}
}
