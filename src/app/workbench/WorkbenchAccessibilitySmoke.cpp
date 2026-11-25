#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::verifyAssetRecoveryContextHelpForSmoke()
{
    const auto wasVisible =
        workspaceSession_.accessibility.contextHelpVisible;
    workspaceSession_.accessibility.contextHelpVisible = true;
    applyAccessibilityPreferences();
    refreshContextHelp(assetRecoveryPanel_.get());
    const auto exact = contextHelpPanel_->contentContainsForSmoke(
        "workspace.asset-recovery", "Loading")
        && contextHelpPanel_->contentContainsForSmoke(
            "workspace.asset-recovery", "Activate Cancel");
    const auto summary = contextHelpPanel_->contentSummaryForSmoke();
    workspaceSession_.accessibility.contextHelpVisible = wasVisible;
    applyAccessibilityPreferences();
    if (exact)
        return std::nullopt;
    return "Context Help did not consume the exact Asset Recovery loading/cancel authority: "
        + summary;
}

std::optional<std::string> WorkbenchComponent::runAccessibilityScaleSmoke(
    const std::filesystem::path& smokeDirectory,
    std::string_view nodeId)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::nullopt
                         : std::optional<std::string>{std::move(message)};
    };
    const auto snapshot = [&](std::string_view fileName)
        -> std::optional<std::string> {
        const auto image = createComponentSnapshot(
            getLocalBounds(), true, 1.0F);
        if (!image.isValid())
            return "Workbench accessibility snapshot was invalid";
        const auto path = (smokeDirectory / fileName).lexically_normal();
        auto stream = juce::File{juceString(path.string())}
                          .createOutputStream();
        if (stream == nullptr || !stream->openedOk()
            || !stream->setPosition(0)
            || stream->truncate().failed()
            || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        {
            return "Workbench accessibility snapshot could not be written";
        }
        stream->flush();
        return std::nullopt;
    };

    const auto baselineProject = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto* presentation = talas::project::findProjectNodePresentation(
        projectBridge_.editor().document(), std::nullopt, nodeId);
    if (presentation == nullptr)
        return "accessibility scale smoke node presentation was unavailable";
    const auto baselinePosition = presentation->position;
    const auto baselineStatusFontHeight = statusLabel_.getFont().getHeight();
    const auto baselinePreferences = workspaceSession_.accessibility;

    for (const auto [scalePreset, expectedScale] : std::array{
             std::pair{talas::project::WorkspaceInterfaceScale::Percent100,
                       1.0F},
             std::pair{talas::project::WorkspaceInterfaceScale::Percent115,
                       1.15F},
             std::pair{talas::project::WorkspaceInterfaceScale::Percent130,
                       1.30F},
             std::pair{talas::project::WorkspaceInterfaceScale::Percent150,
                       1.50F},
         })
    {
        workspaceSession_.accessibility.interfaceScale = scalePreset;
        applyAccessibilityPreferences();
        for (const auto size : std::array{
                 juce::Point<int>{960, 640},
                 juce::Point<int>{1180, 700},
                 juce::Point<int>{1600, 900},
             })
        {
            setSize(size.x, size.y);
            resized();
            const auto componentBounds = getLocalBounds();
            const auto controlIsContained = [&](juce::Component& control) {
                return !control.isVisible()
                    || (!control.getBounds().isEmpty()
                        && componentBounds.contains(control.getBounds()));
            };
            const auto layoutPolicy = talas::app::workbenchLayoutPolicyForSize(
                size.x, size.y, accessibilityProjection_);
            const auto* currentPresentation =
                talas::project::findProjectNodePresentation(
                    projectBridge_.editor().document(),
                    std::nullopt,
                    nodeId);
            if (auto error = require(
                    layoutPolicy.has_value()
                        && currentPresentation != nullptr
                        && WorkbenchType::interfaceScale() == expectedScale
                        && controlIsContained(playButton_)
                        && controlIsContained(pauseButton_)
                        && controlIsContained(stopButton_)
                        && controlIsContained(saveButton_)
                        && controlIsContained(moreButton_)
                        && controlIsContained(workspacePatchButton_)
                        && controlIsContained(workspaceAutomationButton_)
                        && controlIsContained(workspaceAnalysisButton_)
                        && controlIsContained(workspacePerformanceButton_)
                        && controlIsContained(workspaceSamplesButton_)
                        && controlIsContained(transportLabel_)
                        && workspacePatchButton_.isVisible()
                            == layoutPolicy->showWorkspaceCommandsInToolbar
                        && currentPresentation->position == baselinePosition,
                    "closed interface scale/window matrix clipped toolbar controls or changed project coordinates"))
            {
                return error;
            }
        }
    }

    workspaceSession_.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent150;
    workspaceSession_.accessibility.enhancedContrast = true;
    workspaceSession_.accessibility.reduceMotion = true;
    applyAccessibilityPreferences();
    setSize(960, 640);
    resized();
    const auto scaledSession = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(workspaceSession_));
    const auto* scaledPresentation = talas::project::findProjectNodePresentation(
        projectBridge_.editor().document(), std::nullopt, nodeId);
    auto graphTraverser = graphCanvas_->createFocusTraverser();
    const auto graphTraversedItems = graphTraverser == nullptr
        ? std::vector<juce::Component*>{}
        : graphTraverser->getAllComponents(graphCanvas_.get());
    if (!graphCanvas_->isFocusContainer()
        || graphTraversedItems.size()
            != graphCanvas_->accessibilityItemCountForSmoke()
        || !std::ranges::all_of(
            graphTraversedItems,
            [](auto* item) {
                return item != nullptr && item->isAccessible();
            }))
    {
        return "Graph accessibility traverser omitted or hid graph items";
    }
    if (auto error = require(
            scaledSession.ok()
                && scaledPresentation != nullptr
                && scaledSession.state.accessibility
                    == workspaceSession_.accessibility
                && WorkbenchType::interfaceScale() == 1.5F
                && accessibilityProjection_.enhancedContrast()
                && accessibilityProjection_.reduceMotion()
                && statusLabel_.getFont().getHeight()
                    > baselineStatusFontHeight
                && playButton_.getHeight()
                    >= accessibilityProjection_.compactControlHeight()
                && playButton_.getBounds().getRight()
                    <= getLocalBounds().getRight()
                && moreButton_.isVisible()
                && moreButton_.getBounds().getRight()
                    <= getLocalBounds().getRight()
                && !workspacePatchButton_.isVisible()
                && !workspaceAutomationButton_.isVisible()
                && !workspaceAnalysisButton_.isVisible()
                && !workspacePerformanceButton_.isVisible()
                && !workspaceSamplesButton_.isVisible()
                && !transportLabel_.getBounds().isEmpty()
                && transportLabel_.getBounds().getRight()
                    <= getLocalBounds().getRight()
                && virtualControlSurface_->scaledRowsScrollableForSmoke()
                && lookAndFeel_.findColour(
                       juce::ComboBox::outlineColourId)
                    == juce::Colour{0xffd4e2ef}
                && panelBorderColour == workbenchTheme.interactiveOutline
                && WorkbenchType::focusOutlinePixels() >= 3.0F
                && scaledPresentation->position == baselinePosition
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == baselineProject,
            "150% accessibility projection did not scale type/geometry, expose the complete graph tree, overflow secondary actions, persist locally, or preserve project coordinates"))
    {
        return error;
    }
    if (auto error = snapshot("workbench-accessibility-150.png"))
        return error;
    setSize(1180, 700);
    resized();
    if (auto error = snapshot("workbench-accessibility-150-default.png"))
        return error;

    const auto beforeContextHelpProject =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    const auto beforeContextHelpDirty = editBoundary_.isDirty(
        projectBridge_.editor().document());
    const auto beforeContextHelpCanUndo =
        projectBridge_.editor().canUndo();
    const auto beforeContextHelpCanRedo =
        projectBridge_.editor().canRedo();
    workspaceSession_.accessibility.contextHelpVisible = true;
    applyAccessibilityPreferences();
    setSize(960, 640);
    resized();
    const auto announcementBaseline =
        contextHelpPanel_->announcementCountForSmoke();
    const auto loadingHelp = talas::app::workbenchSurfaceState({
        talas::app::WorkbenchSurfaceStateKind::Loading,
        "Loading exact help fixture",
        "The fixture is loading and is not empty.",
        talas::app::WorkbenchSurfaceActionEvidence{
            "context-help.cancel", "Cancel", true},
        false,
    });
    contextHelpPanel_->setContent({
        "context-help.loading-fixture",
        "Loading exact help fixture",
        "Status",
        "Loading",
        "Press Escape to cancel",
        "Loading never appears as an empty surface.",
        "Wait for completion or cancel",
        loadingHelp,
    });
    contextHelpPanel_->setContent({
        "context-help.loading-fixture",
        "Loading exact help fixture",
        "Status",
        "Loading",
        "Press Escape to cancel",
        "Loading never appears as an empty surface.",
        "Wait for completion or cancel",
        loadingHelp,
    });
    if (saveButton_.isShowing())
        saveButton_.grabKeyboardFocus();
    refreshContextHelp(&saveButton_);
    if (auto error = require(
            contextHelpPanel_->isVisible()
                && !contextHelpPanel_->getBounds().isEmpty()
                && getLocalBounds().contains(
                    contextHelpPanel_->getBounds())
                && graphCanvas_->getHeight() >= 220
                && !virtualControlSurface_->isVisible()
                && !diagnosticsPanel_->isVisible()
                && contextHelpPanel_->announcementCountForSmoke()
                    == announcementBaseline + 1,
            "Context Help did not adapt lower surfaces or preserve the primary graph at 150%/960px"))
    {
        return error;
    }
    if (auto error = require(
            contextHelpPanel_->contentContainsForSmoke(
                "toolbar.save", "Activate Save"),
            "Context Help did not follow the exact focused Save command authority: "
                + contextHelpPanel_->contentSummaryForSmoke()))
    {
        return error;
    }
    if (auto error = require(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
                    == beforeContextHelpProject
                && editBoundary_.isDirty(
                       projectBridge_.editor().document())
                    == beforeContextHelpDirty
                && projectBridge_.editor().canUndo()
                    == beforeContextHelpCanUndo
                && projectBridge_.editor().canRedo()
                    == beforeContextHelpCanRedo,
            "Context Help changed project bytes, dirty state, or history"))
    {
        return error;
    }
    if (auto error = snapshot("workbench-context-help-150.png"))
        return error;

    const auto baselineWorkspace = workspaceMode_;
    for (const auto [mode, fileName] : std::array{
             std::pair{talas::app::WorkbenchWorkspaceMode::Patch,
                       std::string_view{
                           "workbench-accessibility-150-patch.png"}},
             std::pair{talas::app::WorkbenchWorkspaceMode::Automation,
                       std::string_view{
                           "workbench-accessibility-150-automation.png"}},
             std::pair{talas::app::WorkbenchWorkspaceMode::Analysis,
                       std::string_view{
                           "workbench-accessibility-150-analysis.png"}},
             std::pair{talas::app::WorkbenchWorkspaceMode::Performance,
                       std::string_view{
                           "workbench-accessibility-150-performance.png"}},
             std::pair{talas::app::WorkbenchWorkspaceMode::Samples,
                       std::string_view{
                           "workbench-accessibility-150-samples.png"}},
         })
    {
        activateWorkspaceMode(mode);
        if (mode == talas::app::WorkbenchWorkspaceMode::Automation)
        {
            auto* score = timelinePanel_->findChildWithID("timeline.score");
            auto* ruler = score != nullptr
                ? score->findChildWithID("timeline.score.ruler")
                : nullptr;
            auto* zoom = score != nullptr
                ? score->findChildWithID("timeline.score.zoom")
                : nullptr;
            if (auto error = require(
                    score != nullptr && ruler != nullptr && zoom != nullptr
                        && score->isVisible()
                        && score->getLocalBounds().contains(ruler->getBounds())
                        && score->getLocalBounds().contains(zoom->getBounds())
                        && ruler->getHeight() >= 32
                        && zoom->getHeight() >= 32,
                    "150% Automation workspace clipped the Score ruler or zoom controls"))
            {
                return error;
            }
        }
        if (auto error = snapshot(fileName))
            return error;
    }
    activateWorkspaceMode(baselineWorkspace);
    workspaceSession_.accessibility = baselinePreferences;
    applyAccessibilityPreferences();
    if (auto error = require(
            WorkbenchType::interfaceScale() == 1.0F
                && std::abs(
                       statusLabel_.getFont().getHeight()
                       - baselineStatusFontHeight)
                    < 0.01F
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == baselineProject,
            "restoring 100% accessibility preferences drifted type or project state"))
    {
        return error;
    }
    setSize(1180, 700);
    resized();
    return std::nullopt;
}
}
