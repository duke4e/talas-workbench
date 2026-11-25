#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
void WorkbenchComponent::resized()
{
    applyAccessibilityPreferences();
    const auto scaledPolicy = talas::app::workbenchLayoutPolicyForSize(
        getWidth(), getHeight(), accessibilityProjection_);
    const auto policy = scaledPolicy.value_or(
        talas::app::workbenchLayoutPolicyForSize(getWidth(), getHeight()));
    const auto scaled = [&](int pixels) {
        return talas::app::workbenchScaledLogicalPixels(
            pixels, accessibilityProjection_.interfaceScalePreset());
    };
    if (lastLayoutWidthClass_ != policy.widthClass)
    {
        compactDock_ = CompactDock::None;
        lastLayoutWidthClass_ = policy.widthClass;
    }
    graphCanvas_->setCompactGraphProtection(
        policy.useSummonableSideWorkspaces);
    secondaryGraphCanvas_->setCompactGraphProtection(
        policy.useSummonableSideWorkspaces);
    const auto workspace = talas::app::workbenchWorkspaceVisibility(
        workspaceMode_);
    const auto assetRecoveryVisible =
        !assetRecoveryCoordinator_.state().items.empty();
    const auto showLibrary = workspace.library
        && (policy.useSummonableSideWorkspaces
                ? compactDock_ == CompactDock::Library
                : libraryVisible_);
    const auto showRightWorkspace =
        (assetRecoveryVisible || workspace.inspector || workspace.timeline
         || workspace.analysis || workspace.performance || workspace.samples)
        && (policy.useSummonableSideWorkspaces
                ? compactDock_ == CompactDock::RightWorkspace
                : rightWorkspaceVisible_);
    auto bounds = getLocalBounds();
    auto toolbarArea = bounds.removeFromTop(
        accessibilityProjection_.toolbarHeight()
        + accessibilityProjection_.statusHeight());
    auto toolbar = toolbarArea.removeFromTop(
        accessibilityProjection_.toolbarHeight())
        .reduced(scaled(10), scaled(7));
    auto statusBar = toolbarArea.reduced(scaled(10), 0);

    const auto hideToolbarButton = [](juce::Button& button) {
        button.setVisible(false);
        button.setBounds({});
    };
    const auto placeToolbarButton = [&toolbar, &scaled](
                                            juce::Button& button,
                                            int width) {
        button.setVisible(true);
        button.setBounds(toolbar.removeFromLeft(width));
        toolbar.removeFromLeft(scaled(6));
    };
    placeToolbarButton(playButton_, scaled(64));
    placeToolbarButton(pauseButton_, scaled(64));
    placeToolbarButton(stopButton_, scaled(64));
    if (policy.showEditCommandsInToolbar)
    {
        placeToolbarButton(undoButton_, scaled(54));
        placeToolbarButton(redoButton_, scaled(54));
        placeToolbarButton(deleteButton_, scaled(84));
    }
    else
    {
        hideToolbarButton(undoButton_);
        hideToolbarButton(redoButton_);
        hideToolbarButton(deleteButton_);
    }
    placeToolbarButton(saveButton_, scaled(60));
    placeToolbarButton(moreButton_, scaled(76));
    if (policy.showWorkspaceCommandsInToolbar)
    {
        placeToolbarButton(workspacePatchButton_, scaled(58));
        placeToolbarButton(workspaceAutomationButton_, scaled(84));
        placeToolbarButton(workspaceAnalysisButton_, scaled(68));
        placeToolbarButton(workspacePerformanceButton_, scaled(90));
        placeToolbarButton(workspaceSamplesButton_, scaled(70));
    }
    else
    {
        hideToolbarButton(workspacePatchButton_);
        hideToolbarButton(workspaceAutomationButton_);
        hideToolbarButton(workspaceAnalysisButton_);
        hideToolbarButton(workspacePerformanceButton_);
        hideToolbarButton(workspaceSamplesButton_);
    }
    transportLabel_.setBounds(toolbar.removeFromLeft(scaled(82)));
    projectStateLabel_.setBounds(
        statusBar.removeFromLeft(
            std::min(scaled(300), statusBar.getWidth() * 42 / 100)));
    statusBar.removeFromLeft(scaled(12));
    outputMeterLabel_.setBounds(
        statusBar.removeFromLeft(
            std::min(scaled(330), statusBar.getWidth() * 55 / 100)));
    statusBar.removeFromLeft(scaled(12));
    statusLabel_.setBounds(statusBar);

    auto content = bounds.reduced(scaled(10));
    const auto showContextHelp =
        accessibilityProjection_.contextHelpVisible();
    const auto contextHelpOwnsLowerWorkspace = showContextHelp
        && content.getHeight() < scaled(520);
    contextHelpPanel_->setVisible(showContextHelp);
    if (showContextHelp)
    {
        contextHelpPanel_->setBounds(
            content.removeFromBottom(scaled(82)));
        content.removeFromBottom(scaled(8));
        refreshContextHelp();
    }
    else
    {
        contextHelpPanel_->setBounds({});
    }
    prototypePanel_->setVisible(showLibrary);
    soundDefinitionsPanel_->setVisible(showLibrary);
    if (showLibrary)
    {
        // Edit > View... may put the
        // prototypes in a strip across the top.
        const auto parts = talas::app::workbenchLibraryParts(content.getWidth(), content.getHeight(),
            prototypeStripAtTop_, policy.libraryWidth, policy.soundDefinitionsHeight, scaled(168), 8);
        const auto area = [origin = content.getPosition()](const talas::app::WorkbenchEditorArea& part) {
            return juce::Rectangle<int>{origin.x + part.x, origin.y + part.y, part.width, part.height};
        };
        prototypePanel_->setBounds(area(parts.prototypes));
        soundDefinitionsPanel_->setBounds(area(parts.soundDefinitions));
        content = area(parts.rest);
    }
    else
    {
        prototypePanel_->setBounds({});
        soundDefinitionsPanel_->setBounds({});
    }

    assetRecoveryPanel_->setVisible(
        showRightWorkspace && assetRecoveryVisible);
    inspectorPanel_->setVisible(
        showRightWorkspace && !assetRecoveryVisible
        && workspace.inspector);
    timelinePanel_->setVisible(
        showRightWorkspace && !assetRecoveryVisible
        && workspace.timeline);
    analysisPanel_->setVisible(
        showRightWorkspace && !assetRecoveryVisible
        && workspace.analysis);
    performancePanel_->setVisible(
        showRightWorkspace && !assetRecoveryVisible
        && workspace.performance);
    sampleRegionInspector_->setVisible(
        showRightWorkspace && !assetRecoveryVisible
        && workspace.samples);
    // The line between the signal flow and
    // the parameters is dragged; the parameters may take
    // the whole area over.
    const auto adjustable = showRightWorkspace && !policy.useSummonableSideWorkspaces;
    if (parametersSplitHandle_ != nullptr)
        parametersSplitHandle_->setVisible(adjustable && !parametersTakeOver_);
    if (showRightWorkspace)
    {
        parametersSplitArea_ = content;
        const auto rightWidth = adjustable
            ? talas::app::workbenchParametersWidth(parametersWidth_, policy.inspectorWidth, parametersTakeOver_, content.getWidth(), 200)
            : policy.inspectorWidth;
        // Edit > View... may put the
        // parameters below the signal flow.
        const auto below = parametersBelow_ && workspace.inspector && !assetRecoveryVisible && !parametersTakeOver_;
        const auto parts = talas::app::workbenchEditorParts(content.getWidth(), content.getHeight(), below, rightWidth, 8, 160);
        const auto area = [origin = content.getPosition()](const talas::app::WorkbenchEditorArea& part) {
            return juce::Rectangle<int>{origin.x + part.x, origin.y + part.y, part.width, part.height};
        };
        auto inspectorColumn = area(parts.parameters);
        assetRecoveryPanel_->setBounds(
            assetRecoveryVisible
                ? inspectorColumn
                : juce::Rectangle<int>{});
        inspectorPanel_->setBounds(
            !assetRecoveryVisible && workspace.inspector
                ? inspectorColumn
                : juce::Rectangle<int>{});
        timelinePanel_->setBounds(
            !assetRecoveryVisible && workspace.timeline
                ? inspectorColumn
                : juce::Rectangle<int>{});
        analysisPanel_->setBounds(
            !assetRecoveryVisible && workspace.analysis
                ? inspectorColumn
                : juce::Rectangle<int>{});
        performancePanel_->setBounds(
            !assetRecoveryVisible && workspace.performance
                ? inspectorColumn
                : juce::Rectangle<int>{});
        sampleRegionInspector_->setBounds(
            !assetRecoveryVisible && workspace.samples
                ? inspectorColumn
                : juce::Rectangle<int>{});
        const auto signalFlow = area(parts.signalFlow);
        const auto gap = below ? juce::Rectangle<int>{}
                               : juce::Rectangle<int>{signalFlow.getRight(), content.getY(),
                                     std::max(0, inspectorColumn.getX() - signalFlow.getRight()), content.getHeight()};
        content = signalFlow;
        if (parametersSplitHandle_ != nullptr)
        {
            parametersSplitHandle_->setVisible(adjustable && !parametersTakeOver_ && !below);
            parametersSplitHandle_->setBounds(adjustable && !parametersTakeOver_ && !below ? gap : juce::Rectangle<int>{});
        }
    }
    else
    {
        if (parametersSplitHandle_ != nullptr)
            parametersSplitHandle_->setBounds({});
        assetRecoveryPanel_->setBounds({});
        inspectorPanel_->setBounds({});
        timelinePanel_->setBounds({});
        analysisPanel_->setBounds({});
        performancePanel_->setBounds({});
        sampleRegionInspector_->setBounds({});
    }

    sampleMapPanel_->setVisible(workspace.samples);
    waveformPanel_->setVisible(workspace.samples);
    performanceCanvas_->setVisible(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
    partialTrackEditorPanel_->setVisible(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis);
    if (workspace.samples)
    {
        virtualControlSurface_->setVisible(false);
        virtualControlSurface_->setBounds({});
        diagnosticsPanel_->setVisible(false);
        diagnosticsPanel_->setBounds({});
        graphCanvas_->setVisible(false);
        secondaryGraphCanvas_->setVisible(false);
        workspaceSplitHandle_->setVisible(false);
        graphCanvas_->setBounds({});
        secondaryGraphCanvas_->setBounds({});
        workspaceSplitHandle_->setBounds({});
        const auto split = std::clamp(
            workspaceSession_.samples.mapWaveformSplit,
            0.35,
            0.72);
        auto mapBounds = content.removeFromTop(
            static_cast<int>(std::lround(
                static_cast<double>(content.getHeight() - 8)
                * split)));
        content.removeFromTop(8);
        sampleMapPanel_->setBounds(mapBounds);
        waveformPanel_->setBounds(content);
        refreshSampleMapWorkspace();
        refreshGraphContextPresentation();
        restoreWorkbenchFocusAfterLayout();
        return;
    }
    sampleMapPanel_->setBounds({});
    waveformPanel_->setBounds({});
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance)
    {
        virtualControlSurface_->setVisible(false);
        virtualControlSurface_->setBounds({});
        diagnosticsPanel_->setVisible(false);
        diagnosticsPanel_->setBounds({});
        graphCanvas_->setVisible(false);
        secondaryGraphCanvas_->setVisible(false);
        workspaceSplitHandle_->setVisible(false);
        graphCanvas_->setBounds({});
        secondaryGraphCanvas_->setBounds({});
        workspaceSplitHandle_->setBounds({});
        performanceCanvas_->setBounds(content);
        refreshGraphContextPresentation();
        restoreWorkbenchFocusAfterLayout();
        return;
    }
    performanceCanvas_->setBounds({});
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis)
    {
        virtualControlSurface_->setVisible(false);
        virtualControlSurface_->setBounds({});
        graphCanvas_->setVisible(false);
        secondaryGraphCanvas_->setVisible(false);
        workspaceSplitHandle_->setVisible(false);
        graphCanvas_->setBounds({});
        secondaryGraphCanvas_->setBounds({});
        workspaceSplitHandle_->setBounds({});
        partialTrackEditorPanel_->setBounds(content);
        refreshGraphContextPresentation();
        restoreWorkbenchFocusAfterLayout();
        return;
    }
    partialTrackEditorPanel_->setBounds({});
    graphCanvas_->setVisible(true);
    diagnosticsPanel_->setVisible(true);

    virtualControlSurface_->setVisible(
        workspace.virtualControls && !contextHelpOwnsLowerWorkspace);
    if (workspace.virtualControls && !contextHelpOwnsLowerWorkspace)
    {
        virtualControlSurface_->setBounds(
            content.removeFromBottom(policy.controlSurfaceHeight));
        content.removeFromBottom(8);
    }
    else
    {
        virtualControlSurface_->setBounds({});
    }
    diagnosticsPanel_->setVisible(!contextHelpOwnsLowerWorkspace);
    if (contextHelpOwnsLowerWorkspace)
    {
        diagnosticsPanel_->setBounds({});
    }
    else
    {
        diagnosticsPanel_->setBounds(
            content.removeFromBottom(policy.diagnosticsHeight));
        content.removeFromBottom(8);
    }
    const auto showSecondaryGraph = secondaryGraphContext_.has_value()
        && workspaceSession_.secondaryPaneVisible
        && (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
            || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
    const auto splitGraphPanes = showSecondaryGraph
        && content.getWidth() >= 380
        && content.getHeight() >= 260;
    graphCanvas_->setVisible(
        !showSecondaryGraph || splitGraphPanes
        || activeGraphContext_ != secondaryGraphContext_);
    secondaryGraphCanvas_->setVisible(
        showSecondaryGraph
        && (splitGraphPanes
            || activeGraphContext_ == secondaryGraphContext_));
    workspaceSplitHandle_->setVisible(splitGraphPanes);
    if (splitGraphPanes)
    {
        graphSplitArea_ = content;
        const auto layout =
            talas::app::workbenchStructureLayoutProjection(
                workspaceSession_.structureSplitOrientation,
                workspaceSession_.primaryPaneFirst,
                content.getWidth(),
                content.getHeight());
        graphSplitUsesVerticalDivider_ =
            layout.orientation
            == talas::app::WorkbenchStructurePaneOrientation::LeftRight;
        workspaceSplitHandle_->setVerticalDivider(
            graphSplitUsesVerticalDivider_);
        const auto split = std::clamp(
            workspaceSession_.paneSplit,
            0.2,
            0.8);
        if (graphSplitUsesVerticalDivider_)
        {
            const auto available = content.getWidth() - 8;
            const auto primaryExtent = static_cast<int>(std::lround(
                static_cast<double>(available) * split));
            const auto secondaryExtent = available - primaryExtent;
            if (layout.primaryPaneFirst)
            {
                graphCanvas_->setBounds(
                    content.removeFromLeft(primaryExtent));
                workspaceSplitHandle_->setBounds(
                    content.removeFromLeft(8));
                secondaryGraphCanvas_->setBounds(content);
            }
            else
            {
                secondaryGraphCanvas_->setBounds(
                    content.removeFromLeft(secondaryExtent));
                workspaceSplitHandle_->setBounds(
                    content.removeFromLeft(8));
                graphCanvas_->setBounds(content);
            }
        }
        else
        {
            const auto available = content.getHeight() - 8;
            const auto primaryExtent = static_cast<int>(std::lround(
                static_cast<double>(available) * split));
            const auto secondaryExtent = available - primaryExtent;
            if (layout.primaryPaneFirst)
            {
                graphCanvas_->setBounds(
                    content.removeFromTop(primaryExtent));
                workspaceSplitHandle_->setBounds(
                    content.removeFromTop(8));
                secondaryGraphCanvas_->setBounds(content);
            }
            else
            {
                secondaryGraphCanvas_->setBounds(
                    content.removeFromTop(secondaryExtent));
                workspaceSplitHandle_->setBounds(
                    content.removeFromTop(8));
                graphCanvas_->setBounds(content);
            }
        }
    }
    else if (showSecondaryGraph)
    {
        graphSplitArea_ = {};
        workspaceSplitHandle_->setBounds({});
        if (activeGraphContext_ == secondaryGraphContext_)
        {
            graphCanvas_->setBounds({});
            secondaryGraphCanvas_->setBounds(content);
        }
        else
        {
            graphCanvas_->setBounds(content);
            secondaryGraphCanvas_->setBounds({});
        }
    }
    else
    {
        graphSplitArea_ = {};
        workspaceSplitHandle_->setBounds({});
        graphCanvas_->setBounds(content);
        secondaryGraphCanvas_->setBounds({});
    }
    refreshGraphContextPresentation();
    restoreWorkbenchFocusAfterLayout();
    updateGraphScroll();
}
}
