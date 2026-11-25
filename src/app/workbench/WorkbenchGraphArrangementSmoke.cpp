#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/GraphBranchLayout.h"
#include "talas/runtime/ConstantFields.h"

namespace talas::app::workbench
{
// The dot where a Sound's lines meet it hides its
// branch and the ellipsis shows it again; renaming a Sound;
// Edit > Clean up of the signal flow; shared inputs
// and the thick numbered line of an input used more than once, and radial
// and right angle lines; Describe Sound and Get info.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runGraphArrangementSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousGraphViewMode = workspaceSession_.graphViewMode;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        setWorkspaceGraphViewMode(previousGraphViewMode);
        if (!restored.ok() && error.empty())
            error = "graph arrangement smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto document = [this]() -> const talas::project::ProjectDocument& {
        return projectBridge_.editor().document();
    };

    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {
        {"left", "talas.noise", {}},
        {"right", "talas.noise", {}},
        {"mix", "talas.mixerV2", {}},
        {"out", "talas.audioOutput", {}},
    };
    fixture.graph.connections = {
        {"left", "audioOut", "mix", "inputs"},
        {"right", "audioOut", "mix", "inputs"},
        {"mix", "leftOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(fixture);
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "left")->position = {40, 300};
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "right")->position = {60, 40};
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "mix")->position = {500, 500};
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "out")->position = {900, 80};
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("graph arrangement fixture was rejected: " + replaced.message);
    }
    documentChanged();
    showRootGraph();
    setWorkspaceGraphViewMode(talas::project::WorkspaceGraphViewMode::PortsAndWires);
    graphCanvas_->refreshModel();
    auto* canvas = graphCanvas_.get();
    const auto baseline = talas::project::serializeProjectDocument(document());
    const std::vector<std::string> everything{"left", "mix", "out", "right"};

    // Hide and show the mixer's branch.
    if (canvas->shownNodeIdsForSmoke() != everything)
        return finish("every Sound should be shown at first");
    if (canvas->clickBranchMarkerForSmoke("left", false))
        return finish("a Sound without inputs offered a branch to hide");
    if (!canvas->clickBranchMarkerForSmoke("mix", false)
        || canvas->shownNodeIdsForSmoke() != std::vector<std::string>{"mix", "out"})
        return finish("clicking the mixer's dot did not hide the Sounds to its left");
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("hiding a branch changed the project");
    if (!canvas->clickBranchMarkerForSmoke("mix", false)
        || canvas->shownNodeIdsForSmoke() != everything)
        return finish("clicking the ellipsis did not show the mixer's inputs again");
    if (!canvas->clickBranchMarkerForSmoke("mix", false)
        || !canvas->clickBranchMarkerForSmoke("out", false)
        || canvas->shownNodeIdsForSmoke() != std::vector<std::string>{"out"})
        return finish("hiding the output's branch did not hide everything to its left");
    if (!canvas->clickBranchMarkerForSmoke("out", true)
        || canvas->shownNodeIdsForSmoke() != everything)
        return finish("a double-click on the ellipsis did not open the whole branch");

    // The context menu offers the branch, Rename and Clean Up.
    const auto mixCentre = canvas->nodeCentreForSmoke("mix");
    if (!mixCentre.has_value())
        return finish("the mixer has no place on the canvas");
    std::map<juce::String, int> items;
    const auto menu = canvas->contextMenuForSmoke(*mixCentre);
    for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
    {
        if (!item.getItem().isSeparator && item.getItem().isEnabled)
            items[item.getItem().text] = item.getItem().itemID;
    }
    if (!items.contains("Hide Inputs") || !items.contains("Show All Inputs")
        || !items.contains("Rename...") || !items.contains("Clean Up"))
        return finish("the mixer's context menu lacks Hide Inputs, Show All Inputs, Rename or Clean Up");
    if (!canvas->chooseContextMenuItemForSmoke(*mixCentre, items["Hide Inputs"])
        || canvas->shownNodeIdsForSmoke() != std::vector<std::string>{"mix", "out"})
        return finish("the context menu's Hide Inputs did not hide the mixer's branch");
    if (!canvas->chooseContextMenuItemForSmoke(*mixCentre, items["Show All Inputs"])
        || canvas->shownNodeIdsForSmoke() != everything)
        return finish("the context menu's Show All Inputs did not show the mixer's branch");

    // Rename a Sound; its line follows.
    if (!renameGraphNode(std::nullopt, "left", "tense anticipation")
        || std::ranges::find(document().graph.connections,
               talas::core::Connection{"tense anticipation", "audioOut", "mix", "inputs"})
            == document().graph.connections.end()
        || selectedNodeIds_ != std::vector<std::string>{"tense anticipation"})
        return finish("renaming a Sound did not keep its line and selection");
    if (renameGraphNode(std::nullopt, "right", "mix"))
        return finish("a Sound took the name of another");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back a rename");

    // Clean up from the context menu.
    graphCanvas_->refreshModel();
    if (!canvas->chooseContextMenuItemForSmoke(*mixCentre, items["Clean Up"]))
        return finish("the context menu's Clean Up was refused");
    const auto at = [&](std::string_view id) {
        return talas::project::findProjectNodePresentation(document(), std::nullopt, id)->position;
    };
    if (at("out") != talas::project::CanvasPosition{40 + 2 * 230, 40}
        || at("mix") != talas::project::CanvasPosition{40 + 230, 40}
        || at("left") != talas::project::CanvasPosition{40, 40}
        || at("right") != talas::project::CanvasPosition{40, 150})
        return finish("Clean Up did not lay the signal flow out in columns");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back Clean Up");

    // The Edit menu carries Rename Sound and Clean Up.
    const auto editMenu = getMenuForIndex(1, "Edit");
    std::vector<int> commands;
    for (juce::PopupMenu::MenuItemIterator item{editMenu}; item.next();)
    {
        if (item.getItem().itemID != 0)
            commands.push_back(item.getItem().itemID);
    }
    if (std::ranges::find(commands, static_cast<int>(commandRenameGraphNode)) == commands.end()
        || std::ranges::find(commands, static_cast<int>(commandCleanUpGraph)) == commands.end())
        return finish("the Edit menu lacks Rename Sound or Clean Up");
    if (!commandManager_.invokeDirectly(commandCleanUpGraph, false)
        || at("out") != talas::project::CanvasPosition{40 + 2 * 230, 40})
        return finish("the Edit menu's Clean Up did not lay the signal flow out");
    undoProject();

    // Left, shared by the mixer and a Crossfade, has a
    // line to each; used twice within the Crossfade, it joins it with one
    // thick line labelled 2.
    if (!projectBridge_.editor().addNode({"fade", "talas.crossfade", {}}, {500, 200}).ok()
        || !projectBridge_.editor().connect({"left", "audioOut", "fade", "snd1"}).ok()
        || !projectBridge_.editor().connect({"left", "audioOut", "fade", "snd2"}).ok())
        return finish("left could not feed both of the Crossfade's inputs");
    documentChanged();
    graphCanvas_->refreshModel();
    const talas::core::Connection first{"left", "audioOut", "fade", "snd1"};
    const talas::core::Connection second{"left", "audioOut", "fade", "snd2"};
    const talas::core::Connection shared{"left", "audioOut", "mix", "inputs"};
    if (talas::app::graphNodesFedBy(document().graph, "left") != talas::app::GraphNodeSet{"fade", "mix"}
        || !canvas->connectionMidpointForSmoke(shared) || !canvas->connectionMidpointForSmoke(first))
        return finish("the shared input did not have a line to each Sound it feeds");
    if (canvas->connectionUseCountForSmoke(first) != std::optional<std::size_t>{2}
        || canvas->connectionUseCountForSmoke(second) != std::optional<std::size_t>{0}
        || canvas->connectionUseCountForSmoke(shared) != std::optional<std::size_t>{1})
        return finish("an input used twice was not one line labelled 2");

    // The context menu's Lines draws them radial (straight)
    // or at right angles; a drop still finds the line it draws.
    const auto contourItem = [&](const juce::String& name) -> std::optional<std::pair<int, bool>> {
        const auto lines = canvas->contextMenuForSmoke(*mixCentre);
        for (juce::PopupMenu::MenuItemIterator item{lines, true}; item.next();)
            if (item.getItem().text == name)
                return std::pair{item.getItem().itemID, item.getItem().isTicked};
        return std::nullopt;
    };
    const auto chooseContour = [&](const juce::String& name) {
        const auto item = contourItem(name);
        return item && canvas->chooseContextMenuItemForSmoke(*mixCentre, item->first)
            && contourItem(name)->second;
    };
    const talas::core::Connection output{"mix", "leftOut", "out", "audioIn"};
    const auto along = [&](float t) { return canvas->connectionPointForSmoke(output, t).value_or(juce::Point<int>{}); };
    if (!contourItem("Curved") || !contourItem("Curved")->second || !chooseContour("Right angle"))
        return finish("the context menu's Lines did not offer Curved, Radial and Right angle");
    if (std::abs(along(0.05F).y - along(0.0F).y) > 1 || along(0.05F).x <= along(0.0F).x
        || std::abs(along(0.95F).y - along(1.0F).y) > 1
        || std::abs(along(0.5F).x - (along(0.0F).x + along(1.0F).x) / 2) > 1
        || canvas->dropTargetForSmoke(along(0.05F)) != GraphDropTarget{output, {}})
        return finish("a right angle line did not run level from the output and the input");
    if (!chooseContour("Radial"))
        return finish("the context menu's Lines did not choose Radial");
    const auto start = along(0.0F);
    const auto end = along(1.0F);
    const auto quarter = along(0.25F);
    if (std::abs(quarter.x - (start.x + (end.x - start.x) / 4)) > 1
        || std::abs(quarter.y - (start.y + (end.y - start.y) / 4)) > 1
        || canvas->dropTargetForSmoke(quarter) != GraphDropTarget{output, {}})
        return finish("a radial line was not straight from the output to the input");
    if (!chooseContour("Curved") || talas::project::serializeProjectDocument(document()) == baseline)
        return finish("the lines did not go back to curves, or the Crossfade's lines were lost");

    // Describe Sound in the context menu
    // opens a help window on the class and its parameters; in the
    // inspector the pointer over the class name and the parameter's name
    // tells what they are, and a click on the class name opens the help.
    const auto leftCentre = canvas->nodeCentreForSmoke("left");
    const auto* noise = registry_.findNodeType("talas.noise");
    if (!leftCentre || noise == nullptr)
        return finish("the noise has no place on the canvas");
    int describe = 0;
    const auto leftMenu = canvas->contextMenuForSmoke(*leftCentre);
    for (juce::PopupMenu::MenuItemIterator item{leftMenu}; item.next();)
        if (item.getItem().text == "Describe Sound..." && item.getItem().isEnabled)
            describe = item.getItem().itemID;
    soundHelpText_.clear();
    if (describe == 0 || !canvas->chooseContextMenuItemForSmoke(*leftCentre, describe)
        || !soundHelpText_.startsWith(juceString(noise->displayName + " (" + noise->category + ")"))
        || !soundHelpText_.contains(juceString(talas::app::descriptorParameterInfoHint(noise->parameters.front()))))
        return finish("Describe Sound did not describe the Noise and its parameters");
    const auto [classTip, parameterTip] = inspectorPanel_->helpTooltipsForSmoke();
    soundHelpText_.clear();
    inspectorPanel_->clickClassNameForSmoke();
    if (!soundHelpText_.startsWith(juceString(noise->displayName)) || !classTip.startsWith(soundHelpText_)
        || !parameterTip.contains(":"))
        return finish("the inspector's class name and parameter did not give their help");
    // The class name at the parameters' lower
    // right, a click on it its help too.
    const auto [cornerName, inCorner] = inspectorPanel_->classNameCornerForSmoke();
    soundHelpText_.clear();
    inspectorPanel_->clickClassNameCornerForSmoke();
    if (cornerName != juceString(noise->displayName) || !inCorner || !soundHelpText_.startsWith(juceString(noise->displayName)))
        return finish("the class name did not show at the parameters' lower right, or a click did not give its help");
    // Dragging the line between the signal flow and
    // the parameters gives them more room; a double-click
    // in their background gives them the whole area, another gives it back.
    if (parametersSplitHandle_->isVisible() && inspectorPanel_->isVisible())
    {
        const auto before = inspectorPanel_->getWidth();
        dragParametersLine({parametersSplitArea_.getRight() - before - 100, parametersSplitArea_.getCentreY()});
        const auto dragged = inspectorPanel_->getWidth();
        toggleParametersTakeOver();
        const auto tookOver = inspectorPanel_->getWidth() == parametersSplitArea_.getWidth();
        toggleParametersTakeOver();
        const auto gaveBack = inspectorPanel_->getWidth() == dragged;
        parametersWidth_.reset();
        resized();
        if (dragged != before + 100 || !tookOver || !gaveBack)
            return finish("the line between the signal flow and the parameters did not move, or they did not take over");
    }
    // A hot parameter's name is in italics,
    // a constant one's is not.
    auto hotNames = 0;
    for (const auto& parameter : noise->parameters)
    {
        const auto hot = parameter.kind == talas::core::ParameterKind::Expression
            && !talas::runtime::isConstantField(noise->typeId, parameter.id);
        inspectorPanel_->focusParameter(parameter.id);
        if (inspectorPanel_->parameterNameItalicForSmoke() != hot)
            return finish("the Noise's " + parameter.id + " was not named in italics as and only as a hot field");
        hotNames += hot ? 1 : 0;
    }
    inspectorPanel_->focusParameter({});
    if (hotNames == 0)
        return finish("the Noise has no hot field to show in italics");

    // Get info on the mixer itself: its inputs, its
    // parameters' values and its complexity, three Sounds.
    int getInfo = 0;
    const auto mixMenu = canvas->contextMenuForSmoke(*mixCentre);
    for (juce::PopupMenu::MenuItemIterator item{mixMenu}; item.next();)
        if (item.getItem().text == "Get Info..." && item.getItem().isEnabled)
            getInfo = item.getItem().itemID;
    soundHelpText_.clear();
    if (getInfo == 0 || !canvas->chooseContextMenuItemForSmoke(*mixCentre, getInfo)
        || !soundHelpText_.startsWith("mix (") || !soundHelpText_.contains("\ninputs: left, right")
        || !soundHelpText_.contains("Complexity: 3 Sounds"))
        return finish("Get Info did not give the mixer's inputs and complexity: " + soundHelpText_.toStdString());
    // And Info > Get Info (Ctrl+I) on the Sound selected.
    soundHelpText_.clear();
    if (!commandManager_.invokeDirectly(commandGetInfo, false) || !soundHelpText_.startsWith("mix ("))
        return finish("Info > Get Info did not give the selected mixer's information: " + soundHelpText_.toStdString());
    // Zoomed in, the flow is wider than the window and the
    // scroll bar at the bottom scrolls it left and right.
    for (auto step = 0; step < 6; ++step)
        canvas->zoomStep(true);
    const auto scroll = canvas->horizontalScroll();
    const auto barShown = graphScroll_.isVisible();
    if (scroll.has_value())
        graphScroll_.setCurrentRangeStart(scroll->start, juce::sendNotificationSync);
    const auto scrolled = canvas->horizontalScroll();
    for (auto step = 0; step < 6; ++step)
        canvas->zoomStep(false);
    if (!scroll.has_value() || !barShown || !scrolled.has_value() || std::abs(scrolled->viewStart - scroll->start) > 1e-6)
        return finish("zoomed in, the signal flow had no scroll bar to scroll it");
    return finish();
}
}
