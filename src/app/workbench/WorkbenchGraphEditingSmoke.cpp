#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchProjectLookup.h"
#include "talas/app/PartialTrackEditorModel.h"
#include "talas/project/PartialTrackAsset.h"

#include <fstream>

namespace talas::app::workbench
{
std::optional<std::string> WorkbenchComponent::runAuthoringToolSmokes(
    const std::filesystem::path& smokeDirectory)
{
    if (auto error = runGraphEditingSmoke())
        return error;
    if (auto error = runFakeKeyboardSmoke())
        return error;
    if (auto error = runTapeRecorderSmoke(smokeDirectory))
        return error;
    if (auto error = runLiveGridEditorSmoke())
        return error;
    if (auto error = runAnalysisToolsSmoke(smokeDirectory))
        return error;
    if (auto error = runSpectralAnalysisWizardSmoke(smokeDirectory))
        return error;
    if (auto error = runSynchronizingSpectraSmoke(smokeDirectory))
        return error;
    if (auto error = runHotValueCompletionSmoke())
        return error;
    if (auto error = runDiskSplicerFieldSmoke())
        return error;
    if (auto error = runDiskButtonSmoke())
        return error;
    if (auto error = runKeyEquivalentsSmoke())
        return error;
    if (auto error = runCdpProcessToolSmoke(smokeDirectory))
        return error;
    if (auto error = runTimelineAutomationSmoke())
        return error;
    if (auto error = runTimelineInfoSmoke())
        return error;
    if (auto error = runTimelineInputSmoke())
        return error;
    if (auto error = runTimelinePointsSmoke())
        return error;
    if (auto error = runTimelineGridSmoke())
        return error;
    if (auto error = runSoundBrowserSmoke(smokeDirectory))
        return error;
    if (auto error = runVcsPresetsSmoke())
        return error;
    if (auto error = runSoundDragSmoke())
        return error;
    if (auto error = runSoundFileSmoke())
        return error;
    if (auto error = runToolEditorSmoke())
        return error;
    if (auto error = runPresetCaptureSmoke())
        return error;
    if (auto error = runGraphArrangementSmoke())
        return error;
    if (auto error = runParameterFieldEditorSmoke())
        return error;
    if (auto error = runSampleEditorSmoke(smokeDirectory))
        return error;
    if (auto error = runVcsWidgetSmoke())
        return error;
    if (auto error = runGlobalMapSmoke())
        return error;
    if (auto error = runTimelineEditingSmoke(smokeDirectory))
        return error;
    if (auto error = runClassEditorSmoke())
        return error;
    if (auto error = runAlternateTuningsSmoke(smokeDirectory))
        return error;
    if (auto error = runStructureAsTextSmoke())
        return error;
    if (auto error = runVariableEnvironmentSmoke())
        return error;
    if (auto error = runDspStatusSmoke())
        return error;
    if (auto error = runPluginExportSmoke(smokeDirectory))
        return error;
    if (auto error = runFileArchivistSmoke(smokeDirectory))
        return error;
    if (auto error = runInfoDisplaySmoke())
        return error;
    if (auto error = runFindReplaceSmoke())
        return error;
    if (auto error = runCompileAndLoadSmoke())
        return error;
    if (auto error = runRevertSmoke())
        return error;
    if (auto error = runPasteHotSmoke())
        return error;
    if (auto error = runPasteSpecialSmoke())
        return error;
    if (auto error = runInputFieldSmoke())
        return error;
    if (auto error = runFilesSmoke(smokeDirectory))
        return error;
    if (auto error = runSampleEditorPlaySmoke(smokeDirectory))
        return error;
    return std::nullopt;
}

// A prototype dropped on a line is
// inserted, asking which input when it has several (as Delete does),
// dropped on a Sound replaces it, alone or with its inputs; Cut, Copy and
// Paste of Sounds, Paste over one replacing it; the canvas's context menu
// and the Edit menu carry the graph's edit actions, and Evaluate;
// Escape in a value field learns an Event Value from MIDI.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runGraphEditingSmoke()
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
        isolatedGraphClipboard_.clear();
        documentChanged();
        setWorkspaceGraphViewMode(previousGraphViewMode);
        if (!restored.ok() && error.empty())
            error = "graph editing smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto document = [this]() -> const talas::project::ProjectDocument& {
        return projectBridge_.editor().document();
    };
    const auto hasConnection = [&](const talas::core::Connection& wanted) {
        return std::ranges::find(document().graph.connections, wanted)
            != document().graph.connections.end();
    };
    const auto nodeOfType = [&](std::string_view typeId) -> std::string {
        const auto found = std::ranges::find(
            document().graph.nodes, typeId, &talas::project::ProjectNode::typeId);
        return found == document().graph.nodes.end()
            ? std::string{}
            : found->instanceId;
    };

    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {
        {"noise", "talas.noise", {}},
        {"out", "talas.audioOutput", {}},
    };
    fixture.graph.connections = {{"noise", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "noise")
        ->position = {40, 80};
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "out")
        ->position = {640, 80};
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("graph editing fixture was rejected: " + replaced.message);
    }
    documentChanged();
    showRootGraph();
    setWorkspaceGraphViewMode(talas::project::WorkspaceGraphViewMode::PortsAndWires);
    graphCanvas_->refreshModel();
    auto* canvas = graphCanvas_.get();
    const auto baseline = talas::project::serializeProjectDocument(document());
    const talas::core::Connection wire{"noise", "audioOut", "out", "audioIn"};

    // A prototype dropped on the line goes between noise and out.
    const auto midpoint = canvas->connectionMidpointForSmoke(wire);
    if (!midpoint.has_value()
        || canvas->dropTargetForSmoke(*midpoint)
            != GraphDropTarget{wire, {}})
        return finish("the line between noise and out was not a drop target");
    if (!canvas->dropPrototypeForSmoke("talas.filter", *midpoint))
        return finish("the canvas refused a prototype dropped on a line");
    const auto filterId = nodeOfType("talas.filter");
    if (filterId.empty()
        || !hasConnection({"noise", "audioOut", filterId, "audioIn"})
        || !hasConnection({filterId, "audioOut", "out", "audioIn"})
        || hasConnection(wire)
        || selectedNodeId_ != filterId)
        return finish("a prototype dropped on a line was not inserted between its Sounds");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back a drop on a line");

    // A prototype dropped on the line's plus is
    // mixed with noise, a Mixer taking the line.
    graphCanvas_->refreshModel();
    const auto plus = canvas->connectionPlusForSmoke(wire);
    if (!plus.has_value() || !canvas->dropTargetForSmoke(*plus).mix || !canvas->dropPrototypeForSmoke("talas.oscillator", *plus))
        return finish("the line's plus was not a drop target that mixes");
    const auto mixerId = nodeOfType("talas.mixerV2");
    const auto mixedId = nodeOfType("talas.oscillator");
    if (mixerId.empty() || mixedId.empty() || !hasConnection({"noise", "audioOut", mixerId, "inputs"})
        || !hasConnection({mixedId, "audioOut", mixerId, "inputs"}) || hasConnection(wire))
        return finish("a prototype dropped on the line's plus was not mixed with noise");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back the mix");

    // A Sound of several inputs asks which takes the line,
    // here the Crossfade's second; Delete asks which input takes a deleted
    // Sound's place, here the Oscillator in its first.
    graphCanvas_->refreshModel();
    smokeInputChoice_ = 1;
    const auto crossfaded = canvas->dropPrototypeForSmoke("talas.crossfade", *midpoint);
    smokeInputChoice_.reset();
    const auto crossfadeId = nodeOfType("talas.crossfade");
    if (!crossfaded || crossfadeId.empty() || !hasConnection({"noise", "audioOut", crossfadeId, "snd2"})
        || !hasConnection({crossfadeId, "leftOut", "out", "audioIn"}))
        return finish("a Crossfade dropped on a line did not take it into the input chosen");
    if (!projectBridge_.editor().addNode({"tone", "talas.oscillator", {}}, {40, 200}).ok()
        || !projectBridge_.editor().connect({"tone", "audioOut", crossfadeId, "snd1"}).ok())
        return finish("the Crossfade's first input could not be fed");
    documentChanged();
    smokeInputChoice_ = 1;
    performDeleteNodes(std::nullopt, std::array{crossfadeId});
    smokeInputChoice_.reset();
    if (!nodeOfType("talas.crossfade").empty() || !hasConnection({"tone", "audioOut", "out", "audioIn"})
        || hasConnection(wire))
        return finish("deleting the Crossfade did not give its place to the input chosen");
    undoProject();
    undoProject();
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("four Undos did not take back the Crossfade, its input and its deletion");

    // A Sound dropped on one with inputs asks Replace or
    // Replace all; Replace all loses the replaced Sound's input, and the
    // replacement takes its default input, a Noise of its own.
    graphCanvas_->refreshModel();
    if (!canvas->dropPrototypeForSmoke("talas.filter", *canvas->connectionMidpointForSmoke(wire)))
        return finish("the Filter was not inserted for Replace all");
    graphCanvas_->refreshModel();
    const auto insertedFilter = nodeOfType("talas.filter");
    const auto filterCentre = canvas->nodeCentreForSmoke(insertedFilter);
    smokeInputChoice_ = 1;
    const auto replacedAll = filterCentre.has_value() && canvas->dropPrototypeForSmoke("talas.harmonicResonator", *filterCentre);
    smokeInputChoice_.reset();
    const auto resonatorId = nodeOfType("talas.harmonicResonator");
    const auto defaultInput = nodeOfType("talas.noise");
    if (!replacedAll || resonatorId.empty() || defaultInput.empty() || !nodeOfType("talas.filter").empty()
        || !hasConnection({defaultInput, "audioOut", resonatorId, "audioIn"})
        || !hasConnection({resonatorId, "audioOut", "out", "audioIn"}))
        return finish("Replace all did not replace the Filter and its input with the resonator and its default input");
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("two Undos did not take back the Filter and Replace all");

    // A prototype dropped on a Sound replaces it.
    graphCanvas_->refreshModel();
    const auto noiseCentre = canvas->nodeCentreForSmoke("noise");
    if (!noiseCentre.has_value()
        || canvas->dropTargetForSmoke(*noiseCentre).nodeId != "noise")
        return finish("the noise Sound was not a drop target");
    // One source replacing another asks to confirm; Cancel
    // leaves it, Replace replaces it.
    smokeInputChoice_ = 1;
    (void) canvas->dropPrototypeForSmoke("talas.oscillator", *noiseCentre);
    smokeInputChoice_.reset();
    if (nodeOfType("talas.noise").empty() || !nodeOfType("talas.oscillator").empty())
        return finish("Cancel did not leave the source a source replacing it would have replaced");
    smokeInputChoice_ = 0;
    const auto replacedSource = canvas->dropPrototypeForSmoke("talas.oscillator", *noiseCentre);
    smokeInputChoice_.reset();
    if (!replacedSource)
        return finish("the canvas refused a prototype dropped on a Sound");
    const auto oscillatorId = nodeOfType("talas.oscillator");
    if (oscillatorId.empty() || !nodeOfType("talas.noise").empty()
        || !hasConnection({oscillatorId, "audioOut", "out", "audioIn"}))
        return finish("a prototype dropped on a Sound did not replace it");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back a replacement");

    // Empty canvas still adds the prototype alone.
    graphCanvas_->refreshModel();
    const auto emptyPoint = juce::Point<int>{
        canvas->nodeCentreForSmoke("noise")->x,
        canvas->nodeCentreForSmoke("noise")->y + 260};
    if (canvas->dropTargetForSmoke(emptyPoint) != GraphDropTarget{})
        return finish("empty canvas below the Sounds was a drop target");

    // The context menu: Copy the Sound under the pointer, Paste at another.
    const auto menu = canvas->contextMenuForSmoke(*noiseCentre);
    std::vector<juce::String> names;
    for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
    {
        if (!item.getItem().isSeparator)
            names.push_back(item.getItem().text);
    }
    if (names != std::vector<juce::String>{
            "Cut", "Copy", "Paste", "Duplicate", "Delete",
            "Hide Inputs", "Show All Inputs", "Rename...", "Describe Sound...", "Get Info...",
            "Set Replaceable Input", "Clean Up", "Lines",
            "Select All", "Find Prototype..."}
        || selectedNodeIds_ != std::vector<std::string>{"noise"})
        return finish("the graph context menu did not offer the Edit actions on the Sound under the pointer");
    if (!canvas->chooseContextMenuItemForSmoke(*noiseCentre, 2)
        || !isolatedGraphClipboard_.contains("talasGraphFragment"))
        return finish("the context menu's Copy did not put the Sound on the clipboard");
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("Copy changed the project");
    if (!canvas->chooseContextMenuItemForSmoke(emptyPoint, 3))
        return finish("the context menu's Paste was refused");
    const auto* pasted = talas::project::findProjectNodePresentation(
        document(), std::nullopt, "noise-copy");
    if (pasted == nullptr
        || selectedNodeIds_ != std::vector<std::string>{"noise-copy"})
        return finish("the context menu's Paste did not add a selected copy");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back a Paste");

    // Paste with one Sound selected replaces it with the
    // copied Sound.
    if (!projectBridge_.editor().addNode({"tone", "talas.oscillator", {}}, {40, 400}).ok())
        return finish("the Sound to paste over could not be added");
    documentChanged();
    publishGraphSelection(std::nullopt, {"tone"}, "tone");
    smokeInputChoice_ = 0; // Replace, asked as one source takes another's place.
    const auto pastedOver = commandManager_.invokeDirectly(commandPasteGraphNodes, false);
    smokeInputChoice_.reset();
    if (!pastedOver || !nodeOfType("talas.oscillator").empty()
        || std::ranges::count(document().graph.nodes, std::string{"talas.noise"}, &talas::project::ProjectNode::typeId) != 2)
        return finish("Paste with one Sound selected did not replace it with the copied Sound");
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("two Undos did not take back the replacing Paste");

    // Cut and Paste from the keyboard keep the Sounds and their line.
    graphCanvas_->refreshModel();
    publishGraphSelection(std::nullopt, {"noise", "out"}, "noise");
    const auto command = juce::ModifierKeys{juce::ModifierKeys::commandModifier};
    if (!canvas->pressNodeKeyForSmoke("noise", juce::KeyPress{'x', command, 'x'})
        || !document().graph.nodes.empty())
        return finish("Cmd/Ctrl+X did not cut the selected Sounds");
    if (!commandManager_.invokeDirectly(commandPasteGraphNodes, false))
        return finish("the Paste command was refused");
    if (nodeOfType("talas.noise") != "noise" || nodeOfType("talas.audioOutput") != "out"
        || !hasConnection(wire))
        return finish("pasting cut Sounds did not restore them with their line");
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("two Undos did not take back a Cut and its Paste");

    // The Edit menu carries the graph's edit actions; Find prototype and
    // Evaluate are in the Action menu.
    std::vector<int> commands;
    for (const auto* menuName : {"Edit", "Action"})
    {
        const auto menu = getMenuForIndex(getMenuBarNames().indexOf(menuName), menuName);
        for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
        {
            if (item.getItem().itemID != 0)
                commands.push_back(item.getItem().itemID);
        }
    }
    for (const auto wanted : {
             commandCutSelectedNodes,
             commandCopySelectedNodes,
             commandPasteGraphNodes,
             commandDuplicateSelectedNodes,
             commandDeleteSelectedNode,
             commandSelectAllGraphNodes,
             commandFocusPrototypeSearch,
             commandEvaluateSelection,
         })
    {
        if (std::ranges::find(commands, static_cast<int>(wanted)) == commands.end())
            return finish("the Edit and Action menus lack a graph edit action");
    }

    // Evaluate puts what a text field's
    // selection prints just after it, selected; Ctrl+Y does it.
    juce::TextEditor field;
    field.setText("5 c hz", false);
    field.setHighlightedRegion({0, 6});
    if (!evaluateSelectionIn(field) || !field.getText().startsWith("5 c hz 523.251")
        || field.getHighlightedText() != field.getText().substring(7))
        return finish("Evaluate did not put 5 c hz's frequency after it, selected: " + field.getText().toStdString());
    // An error goes into the text, selected, the field flashing.
    field.setText("3 +", false);
    field.setHighlightedRegion({0, 3});
    if (evaluateSelectionIn(field) || field.getText().length() <= 3 || field.getHighlightedText().isEmpty()
        || !field.getText().contains(field.getHighlightedText()))
        return finish("Evaluate did not put its error into the text: " + field.getText().toStdString());
    field.setText("3 + 4", false);
    field.setHighlightedRegion({0, 5});
    if (!evaluateKeys_.keyPressed(juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0), &field)
        || field.getText() != "3 + 4 7")
        return finish("Ctrl+Y did not evaluate the field's selection: " + field.getText().toStdString());
    // What debug: shows as it runs comes up in a
    // dialog, in order.
    // Each value pauses with OK and Cancel, answered here.
    field.setText("0 to: 1 do: [:i | self debugWithLabel: 'i' value: i]. self debug: 2 + 3", false);
    field.setHighlightedRegion({0, field.getTotalNumChars()});
    smokePromptAnswers_ = {{true, {}}, {true, {}}, {true, {}}};
    if (!evaluateSelectionIn(field) || evaluationDebugLines_ != std::vector<std::string>{"i = 0", "i = 1", "5"})
        return finish("Evaluate did not show what debug: showed");
    field.setText("0 to: 1 do: [:i | self debugWithLabel: 'i' value: i]. 7", false);
    field.setHighlightedRegion({0, field.getTotalNumChars()});
    smokePromptAnswers_ = {{true, {}}, {false, {}}};
    (void) evaluateSelectionIn(field);
    if (evaluationDebugLines_ != std::vector<std::string>{"i = 0", "i = 1"} || !field.getText().contains("Stopped at i = 1"))
        return finish("Cancel at a debug: value did not stop the evaluation: " + field.getText().toStdString());
    // XYGraphView opens a graph of what it is given.
    field.setText("XYGraphView openOnValues: #(1 4 2)", false);
    field.setHighlightedRegion({0, field.getTotalNumChars()});
    if (!evaluateSelectionIn(field) || evaluationGraphs_.size() != 1 || evaluationGraphs_.front().size() != 3)
        return finish("Evaluate did not open XYGraphView's graph");
    // A question for the user, the evaluation run again
    // with the answer.
    smokePromptAnswers_ = {{true, "3 + 4"}};
    field.setText("(self getValueFromUser: 'How many?') * 2", false);
    field.setHighlightedRegion({0, field.getTotalNumChars()});
    if (!evaluateSelectionIn(field) || !smokePromptAnswers_.empty() || !field.getText().endsWith(" 14"))
        return finish("Evaluate did not ask getValueFromUser:'s question and use its answer: " + field.getText().toStdString());

    // Escape in a parameter's value field,
    // then two keys at once, types !KeyDown; a controller types its name.
    juce::TextEditor valueField;
    valueField.setComponentID("inspector.parameter.value");
    if (!evaluateKeys_.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey), &valueField)
        || valueField.getMouseCursor() != juce::MouseCursor{juce::MouseCursor::UpDownResizeCursor})
        return finish("Escape in a parameter's value field did not start learning with a fader cursor");
    learnFromMidi(juce::MidiMessage::noteOn(1, 60, 0.5F));
    learnFromMidi(juce::MidiMessage::noteOn(1, 64, 0.5F));
    learnFromMidi(juce::MidiMessage::noteOff(1, 60));
    learnFromMidi(juce::MidiMessage::noteOff(1, 64));
    if (valueField.getText() != "!KeyDown" || valueField.getMouseCursor() != juce::MouseCursor{juce::MouseCursor::IBeamCursor})
        return finish("Escape and two keys did not type !KeyDown: " + valueField.getText().toStdString());
    valueField.clear();
    (void)evaluateKeys_.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey), &valueField);
    learnFromMidi(juce::MidiMessage::controllerEvent(1, 7, 64));
    learnFromMidi(juce::MidiMessage::controllerEvent(1, 8, 64));
    if (valueField.getText() != "!Volume")
        return finish("Escape and controller 7 did not type !Volume: " + valueField.getText().toStdString());
    return finish();
}

// The Tools menu's Structure as Text shows the
// Sound in view as text, each Sound with its inputs.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runStructureAsTextSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Structure as Text smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"noise", "talas.noise", {}}, {"filter", "talas.filter", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"noise", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Structure as Text fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectedNodeIds_.clear();
    const auto names = getMenuBarNames();
    const auto infoMenu = getMenuForIndex(names.indexOf("Info"), "Info");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{infoMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowStructureAsText;
    if (!listed || !commandManager_.invokeDirectly(commandShowStructureAsText, false) || structureText_ == nullptr)
        return finish("the Info menu did not show the structure as text");
    // The text cannot be edited.
    if (structureText_->getText() != "filter (noise)\n\t.noise\n" || !structureText_->isReadOnly())
        return finish("Structure as Text showed: " + structureText_->getText().toStdString());
    // A prototype selected among the prototypes reads as its Sound.
    const auto prototypeRead = prototypePanel_->selectForSmoke("talas.oscillator")
        && commandManager_.invokeDirectly(commandShowStructureAsText, false) && structureText_->getText() == "oscillator\n";
    (void) prototypePanel_->selectForSmoke({});
    if (!prototypeRead)
        return finish("Structure as Text did not read the selected prototype: " + structureText_->getText().toStdString());
    if (structureWindow_ != nullptr)
        structureWindow_->setVisible(false);
    return finish();
}

// Playing a Sound with ?level asks for
// its value, the environment keeps it for the next play and Info shows it,
// and Reset environment makes the next play ask again.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runVariableEnvironmentSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousEnvironment = variableEnvironment_;
    const auto previousSoundEnvironment = soundVariableEnvironment_;
    const auto previousDefaultSound = defaultSound_;
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        smokeVariableValue_.reset();
        variableEnvironment_ = previousEnvironment;
        soundVariableEnvironment_ = previousSoundEnvironment;
        defaultSound_ = previousDefaultSound;
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{8000.0, 128};
            if ((!stageProject(std::move(restoreSettings), "Environment smoke restore") || !adoptPreparedOutputTakeStageForSmoke())
                && error.empty())
                error = "Environment smoke could not restore the prior stage";
        }
        if (!restored.ok() && error.empty())
            error = "Environment smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    fixture.graph.nodes = {{"osc", "talas.oscillator", {{"amplitude", "?level"}}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Environment fixture was rejected: " + replaced.message);
    documentChanged();
    variableEnvironment_.clear();
    smokeVariableValue_ = 0.25;
    const auto requests = variableValueRequests_;
    if (askForVariableValues() || variableValueRequests_ != requests + 1 || variableEnvironment_.size() != 1
        || variableEnvironment_.front() != std::pair<std::string, double>{"level", 0.25})
        return finish("playing ?level did not ask for its value");
    talas::runtime::RenderSettings settings{8000.0, 128};
    if (!stageProject(std::move(settings), "Environment smoke") || !adoptPreparedOutputTakeStageForSmoke())
        return finish("a Sound with ?level did not play from the environment: " + statusLabel_.getText().toStdString());
    if (askForVariableValues() || variableValueRequests_ != requests + 1)
        return finish("the environment did not keep ?level for the next play");
    if (!commandManager_.invokeDirectly(commandShowEnvironment, false)
        || !statusLabel_.getText().contains("?level -> 0.25"))
        return finish("Environment did not show ?level: " + statusLabel_.getText().toStdString());
    if (!commandManager_.invokeDirectly(commandResetEnvironment, false) || !variableEnvironment_.empty()
        || askForVariableValues() || variableValueRequests_ != requests + 2)
        return finish("Reset environment did not make the next play ask again");

    // Set default Sound takes the Sound
    // selected, and a Variable no declaration names takes the default Sound
    // when the Sound plays, which then plays it.
    talas::project::ProjectDocument lifted;
    lifted.graph.nodes = {{"noise", "talas.noise", {}}, {"lead", "talas.variable", {}}, {"out", "talas.audioOutput", {}}};
    talas::project::ProjectParameterExpression variable;
    variable.parameterId = "variableId";
    variable.text = "Variable";
    lifted.graph.nodes[1].parameters.push_back(variable);
    lifted.graph.connections = {{"lead", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(lifted);
    if (const auto replaced = projectBridge_.editor().replaceDocument(lifted); !replaced.ok())
        return finish("Default Sound fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectNode(std::nullopt, "noise");
    if (!commandManager_.invokeDirectly(commandSetDefaultSound, false) || !defaultSound_.has_value()
        || defaultSound_->graph.nodes.size() != 1 || defaultSound_->graph.nodes.front().typeId != "talas.noise")
        return finish("Set default Sound did not take the selected Sound");
    soundVariableEnvironment_.clear();
    if (askForVariableValues() || variableValueRequests_ != requests + 3 || soundVariableEnvironment_.size() != 1
        || soundVariableEnvironment_.front().first != "Variable"
        || soundVariableEnvironment_.front().second.front().graph.nodes.front().typeId != "talas.noise")
        return finish("playing ?Variable did not give it the default Sound");
    talas::runtime::RenderSettings liftedSettings{8000.0, 128};
    if (!stageProject(std::move(liftedSettings), "Default Sound smoke") || !adoptPreparedOutputTakeStageForSmoke())
        return finish("a Variable did not play the default Sound: " + statusLabel_.getText().toStdString());
    return finish();
}

// DSP > DSP Status shows the time since
// the Sound started as hours:minutes:seconds and a peak indicator that shows
// >0< after a full-scale peak until it is clicked.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runDspStatusSmoke()
{
    const auto previousClock = dspStatusClock_;
    const auto previousPeak = outputMeterPeaks_[0].load(std::memory_order_relaxed);
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        dspStatusClock_ = previousClock;
        outputMeterPeaks_[0].store(previousPeak, std::memory_order_relaxed);
        if (dspStatusWindow_ != nullptr)
            dspStatusWindow_->setVisible(false);
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto names = getMenuBarNames();
    const auto dspMenu = getMenuForIndex(names.indexOf("DSP"), "DSP");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{dspMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowDspStatus;
    if (!listed || !commandManager_.invokeDirectly(commandShowDspStatus, false) || dspStatusPanel_ == nullptr)
        return finish("the DSP menu did not show the DSP status");
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    dspStatusClock_.start(now - 3725.0);
    dspStatusClock_.stop(now);
    outputMeterPeaks_[0].store(1.0F, std::memory_order_relaxed);
    dspStatusPanel_->refresh();
    if (dspStatusPanel_->elapsedText() != "1:02:05" || dspStatusPanel_->peakText(true) != ">0<")
        return finish("DSP status showed " + dspStatusPanel_->elapsedText() + " and " + dspStatusPanel_->peakText(true));
    outputMeterPeaks_[0].store(0.0F, std::memory_order_relaxed);
    const auto eventTime = juce::Time::getCurrentTime();
    const juce::MouseEvent click{juce::Desktop::getInstance().getMainMouseSource(), {20.0F, 20.0F}, {}, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, dspStatusPanel_.get(), dspStatusPanel_.get(), eventTime, {20.0F, 20.0F}, eventTime, 1, false};
    dspStatusPanel_->mouseDown(click);
    if (dspStatusPanel_->peakText(true) == ">0<")
        return finish("clicking the DSP status did not reset its peak indicator");
    // The Sound last loaded's global map, A/D
    // input, MIDI channels and Event Values.
    const auto previousLines = loadedSoundInputLines_;
    talas::project::ProjectDocument loaded;
    loaded.graph.nodes.push_back({"voice", "talas.midiVoice", {{"channel", "1"}}});
    loaded.virtualControls.push_back({"Rate", talas::expr::ValueDomain::Scalar});
    loadedSoundInputLines_ = talas::app::loadedSoundInputLines(talas::app::loadedSoundInputs(loaded));
    dspStatusPanel_->refresh();
    const auto lines = dspStatusPanel_->soundInputLines();
    loadedSoundInputLines_ = previousLines;
    if (lines.size() != 4 || lines[2] != "MIDI keyboard on channel 1" || lines[3] != "Event Values: !Rate")
        return finish("the DSP status did not tell of the Sound last loaded's MIDI channel and Event Values");
    // The memory bar, the share of the computer's memory in use,
    // and the File menu's Status.
    if (!(dspStatusPanel_->memoryFraction() > 0.0) || dspStatusPanel_->memoryFraction() > 1.0)
        return finish("the DSP status did not show the memory in use");
    const auto fileMenu = getMenuForIndex(names.indexOf("File"), "File");
    auto statusInFile = false;
    for (juce::PopupMenu::MenuItemIterator item{fileMenu}; item.next();)
        statusInFile = statusInFile || item.getItem().itemID == commandShowDspStatus;
    if (!statusInFile)
        return finish("the File menu did not have the Status window");
    // Show Eight Channels in DSP Status.
    const auto previousEight = preferences_.showEightChannels;
    preferences_.showEightChannels = true;
    dspStatusPanel_->refresh();
    const auto eight = dspStatusPanel_->meterCount();
    preferences_.showEightChannels = previousEight;
    dspStatusPanel_->refresh();
    if (eight != 8 || dspStatusPanel_->meterCount() != (previousEight ? 8U : 2U))
        return finish("the DSP status did not show eight meters a side when the preferences asked");
    return finish();
}

// Edit > Find selects the next occurrence after the
// selection, Find again the one after it, and Replace all replaces each.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runFindReplaceSmoke()
{
    const auto names = getMenuBarNames();
    const auto editMenu = getMenuForIndex(names.indexOf("Edit"), "Edit");
    std::vector<int> commands;
    for (juce::PopupMenu::MenuItemIterator item{editMenu}; item.next();)
        commands.push_back(item.getItem().itemID);
    for (const auto wanted : {commandFindText, commandFindTextAgain, commandReplaceText, commandReplaceTextAgain})
    {
        if (std::ranges::find(commands, static_cast<int>(wanted)) == commands.end())
            return "the Edit menu lacks Find and Replace";
    }
    juce::TextEditor field;
    field.setText("!Pitch + !Pitch smooth: !Rate", false);
    field.setHighlightedRegion({0, 0});
    if (!findInEditor(field, "!Pitch") || field.getHighlightedRegion() != juce::Range<int>{0, 6}
        || !findInEditor(field, "!Pitch") || field.getHighlightedRegion() != juce::Range<int>{9, 15}
        || findInEditor(field, "!Pitch"))
        return "Find and Find again did not select each !Pitch after the selection in turn";
    if (!replaceInEditor(field, "!Pitch", "!KeyPitch", true) || field.getText() != "!KeyPitch + !KeyPitch smooth: !Rate"
        || replaceInEditor(field, "!Volume", "!Gain", true))
        return "Replace all did not replace each !Pitch: " + field.getText().toStdString();
    return std::nullopt;
}

// Compile & Load loads the project without
// starting it, and Restart starts it, the DSP status time from zero.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runCompileAndLoadSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        setPlaying(false);
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{8000.0, 128};
            if ((!stageProject(std::move(restoreSettings), "Compile & Load smoke restore")
                    || !adoptPreparedOutputTakeStageForSmoke())
                && error.empty())
                error = "Compile & Load smoke could not restore the prior stage";
        }
        setPlaying(previousPlaying);
        if (!restored.ok() && error.empty())
            error = "Compile & Load smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    // Compile & load in the Action menu,
    // Restart in the DSP menu.
    const auto names = getMenuBarNames();
    const auto commandsIn = [&](const char* menuName) {
        const auto menu = getMenuForIndex(names.indexOf(menuName), menuName);
        std::vector<int> commands;
        for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
            commands.push_back(item.getItem().itemID);
        return commands;
    };
    const auto action = commandsIn("Action");
    const auto dsp = commandsIn("DSP");
    if (std::ranges::find(action, static_cast<int>(commandCompileAndLoad)) == action.end()
        || std::ranges::find(dsp, static_cast<int>(commandRestart)) == dsp.end())
        return finish("the Action menu lacks Compile & Load or the DSP menu Restart");
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"noise", "talas.noise", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"noise", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Compile & Load fixture was rejected: " + replaced.message);
    documentChanged();
    setPlaying(false);
    if (!compileAndLoadCurrentProject(talas::runtime::RenderSettings{8000.0, 128}) || !adoptPreparedOutputTakeStageForSmoke()
        || !currentDocumentStaged_ || transportPlaying_)
        return finish("Compile & Load did not load the project without starting it: " + statusLabel_.getText().toStdString());
    restartCurrentProject();
    if (!transportPlaying_ || !dspStatusClock_.running()
        || dspStatusClock_.elapsed(juce::Time::getMillisecondCounterHiRes() / 1000.0) > 5.0)
        return finish("Restart did not start the loaded project from the beginning");
    return finish();
}

// Action > Revert drops the changes to the
// selected Sound's parameters since it was opened.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runRevertSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Revert smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"osc", "talas.oscillator", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Revert fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectNode(std::nullopt, "osc");
    if (!projectBridge_.editor().setParameter("osc", {"amplitude", "0.3"}).ok())
        return finish("the Revert smoke could not change the amplitude");
    documentChanged();
    const auto names = getMenuBarNames();
    const auto actionMenu = getMenuForIndex(names.indexOf("Action"), "Action");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{actionMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandRevertSound;
    if (!listed || !commandManager_.invokeDirectly(commandRevertSound, false))
        return finish("the Action menu lacks Revert");
    const auto& nodes = projectBridge_.editor().document().graph.nodes;
    const auto osc = std::ranges::find(nodes, std::string_view{"osc"}, &talas::project::ProjectNode::instanceId);
    if (osc == nodes.end() || !osc->parameters.empty())
        return finish("Revert did not drop the amplitude changed since the Sound was opened");
    return finish();
}

// Edit > Paste hot pastes what typing
// part of a name finds, here half the sampling rate, at the caret.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runPasteHotSmoke()
{
    const auto names = getMenuBarNames();
    const auto editMenu = getMenuForIndex(names.indexOf("Edit"), "Edit");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{editMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandPasteHot;
    if (!listed)
        return "the Edit menu lacks Paste hot";
    juce::TextEditor field;
    field.setText("1 * ", false);
    field.setCaretPosition(4);
    if (!pasteHotInto(field, "half", -1) || field.getText() != "1 * SignalProcessor sampleRate * 0.5"
        || pasteHotInto(field, "zzz", -1))
        return "Paste hot did not paste half the sampling rate: " + field.getText().toStdString();
    return std::nullopt;
}

// Copy one Sound, select another, and Edit >
// Paste special makes the copied Sound itself take its place.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runPasteSpecialSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousClipboard = graphClipboardText();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        setGraphClipboardText(previousClipboard);
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Paste special smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"noise1", "talas.noise", {}}, {"noise2", "talas.noise", {}}, {"low", "talas.filter", {}},
        {"high", "talas.filter", {}}, {"mix", "talas.mixerV2", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"noise1", "audioOut", "low", "audioIn"}, {"noise2", "audioOut", "high", "audioIn"},
        {"low", "audioOut", "mix", "inputs"}, {"high", "audioOut", "mix", "inputs"}, {"mix", "leftOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Paste special fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectNode(std::nullopt, "noise1");
    if (!copySelectedNodes())
        return finish("the Paste special smoke could not copy noise1");
    selectNode(std::nullopt, "noise2");
    if (!commandManager_.invokeDirectly(commandPasteSpecial, false))
        return finish("the Edit menu lacks Paste special");
    const auto& graph = projectBridge_.editor().document().graph;
    const auto feeds = [&](std::string_view to) {
        return std::ranges::any_of(graph.connections, [&](const auto& line) { return line.fromNode == "noise1" && line.toNode == to; });
    };
    if (!feeds("low") || !feeds("high")
        || std::ranges::any_of(graph.nodes, [](const auto& node) { return node.instanceId == "noise2"; }))
        return finish("Paste special did not make both filters share noise1: " + statusLabel_.getText().toStdString());
    return finish();
}

// The inspector's Inputs field lists a
// Mixer's Sounds; deleting one from it removes that input.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runInputFieldSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Inputs field smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"disk", "talas.noise", {}}, {"sample", "talas.noise", {}}, {"mix", "talas.mixerV2", {}},
        {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"disk", "audioOut", "mix", "inputs"}, {"sample", "audioOut", "mix", "inputs"},
        {"mix", "leftOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Inputs field fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectNode(std::nullopt, "mix");
    const auto listed = inspectorPanel_->inputFieldTextForSmoke();
    if (listed != "disk\nsample")
        return finish("the Inputs field did not list the Mixer's Sounds: " + listed.toStdString());
    if (!inspectorPanel_->applyInputFieldForSmoke("sample")
        || projectBridge_.editor().inputsOf(std::nullopt, "mix", "inputs") != std::vector<std::string>{"sample"})
        return finish("deleting disk from the Inputs field did not remove that input");
    // A Sample dragged from the prototypes into the Inputs
    // field becomes one of the Mixer's inputs, added to its Sounds as the
    // menu's first item chooses, or in place of one, as the others do.
    smokeInputDropChoice_ = 1;
    const auto added = inspectorPanel_->dropSoundForSmoke(juceString(std::string{prototypeDragPrefix} + "talas.sample"));
    smokeInputDropChoice_.reset();
    if (!added || projectBridge_.editor().inputsOf(std::nullopt, "mix", "inputs").size() != 2
        || !inspectorPanel_->inputFieldTextForSmoke().startsWith("sample\n"))
        return finish("a Sample dropped into the Inputs field did not become one of the Mixer's inputs: "
            + inspectorPanel_->inputFieldTextForSmoke().toStdString());
    smokeInputDropChoice_ = 2;
    const auto replaced = inspectorPanel_->dropSoundForSmoke(juceString(std::string{prototypeDragPrefix} + "talas.noise"));
    smokeInputDropChoice_.reset();
    const auto afterReplace = projectBridge_.editor().inputsOf(std::nullopt, "mix", "inputs");
    if (!replaced || afterReplace.size() != 2 || afterReplace.front() == "sample"
        || std::ranges::any_of(projectBridge_.editor().document().graph.nodes,
            [](const talas::project::ProjectNode& node) { return node.instanceId == "sample"; }))
        return finish("a Sound dropped to replace the Mixer's first input did not take its place: "
            + inspectorPanel_->inputFieldTextForSmoke().toStdString());
    return finish();
}

// File > File Organizer lists a
// frequently used folder's files, a double-click opens the folder, a
// file shows its information and opens with the editor for its kind or the
// external one the preferences name; a MIDI file dropped in becomes a Mixer
// of MIDIVoices.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runFilesSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousPreferences = preferences_;
    const auto folder = smokeDirectory / "talas-files";
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        smokeRecordsFileOpenings_ = false;
        preferences_ = previousPreferences;
        applyAppearancePreferences();
        if (fileOrganizerWindow_ != nullptr)
            fileOrganizerWindow_->setVisible(false);
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        if (!restored.ok() && error.empty())
            error = "Files smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    std::filesystem::create_directories(folder);
    {
        const std::vector<std::vector<double>> silence{std::vector<double>(800, 0.0)};
        std::string error;
        if (!talas::audio::writePcm16Wav(folder / "kick.wav", 8000.0, silence, error))
            return finish("the files smoke could not write its sample: " + error);
        const std::vector<std::uint8_t> midi{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 20,
            0, 0x90, 60, 100, 96, 0x80, 60, 0, 0, 0x92, 64, 100, 96, 0x82, 64, 0, 0, 0xFF, 0x2F, 0};
        std::ofstream output(folder / "tune.mid", std::ios::binary);
        output.write(reinterpret_cast<const char*>(midi.data()), static_cast<std::streamsize>(midi.size()));
    }
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"noise", "talas.noise", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"noise", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Files fixture was rejected: " + replaced.message);
    documentChanged();
    preferences_.frequentlyUsedFolders = {folder.string()};
    preferences_.fileEditors.clear();
    const auto names = getMenuBarNames();
    const auto fileMenu = getMenuForIndex(names.indexOf("File"), "File");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{fileMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowFileOrganizer;
    if (!listed || !commandManager_.invokeDirectly(commandShowFileOrganizer, false) || fileOrganizerPanel_ == nullptr)
        return finish("the File menu did not show the File Organizer");
    if (fileOrganizerPanel_->rowsForSmoke().empty() || !fileOrganizerPanel_->activateRowForSmoke(0))
        return finish("the File Organizer did not list the frequently used folder");
    const auto rows = fileOrganizerPanel_->rowsForSmoke();
    // A click selects a file, the arrows move the
    // selection up and down, the page keys a page at a time.
    if (rows.size() < 2
        || (fileOrganizerPanel_->selectThenPressForSmoke(0, juce::KeyPress{juce::KeyPress::downKey}) != 1
            || fileOrganizerPanel_->selectThenPressForSmoke(1, juce::KeyPress{juce::KeyPress::upKey}) != 0
            || fileOrganizerPanel_->selectThenPressForSmoke(0, juce::KeyPress{juce::KeyPress::pageDownKey}) <= 0
            || fileOrganizerPanel_->selectThenPressForSmoke(rows.size() - 1, juce::KeyPress{juce::KeyPress::pageUpKey})
                >= static_cast<int>(rows.size()) - 1))
        return finish("the File Organizer's arrows and page keys did not move the selection");
    const auto kick = std::ranges::find_if(rows, [](const auto& row) { return row.path.filename() == "kick.wav"; });
    if (kick == rows.end() || kick->kind != talas::app::TalasFileKind::Sample)
        return finish("opening the folder did not list its sample");
    const auto row = static_cast<std::size_t>(std::distance(rows.begin(), kick));
    if (!fileOrganizerPanel_->informationForSmoke(row).contains("1 channel · 8000 Hz · 800 frames"))
        return finish("the File Organizer did not show the sample's information");
    smokeRecordsFileOpenings_ = true;
    if (openTalasFile(folder / "kick.wav") != talas::app::TalasFileOpening::SampleEditor)
        return finish("a sample did not open in the Sample Editor");
    preferences_.fileEditors[talas::app::TalasFileKind::Sample] = "/Applications/Wave Editor.app";
    if (openTalasFile(folder / "kick.wav") != talas::app::TalasFileOpening::ExternalEditor)
        return finish("a sample did not open in the external editor the preferences name");
    smokeRecordsFileOpenings_ = false;
    const auto definitions = projectBridge_.editor().document().soundDefinitions.size();
    if (!isInterestedInFileDrag({jucePathString(folder / "tune.mid")}))
        return finish("the Workbench did not take a MIDI file dragged in");
    filesDropped({jucePathString(folder / "tune.mid")}, 0, 0);
    const auto& document = projectBridge_.editor().document();
    if (document.soundDefinitions.size() != definitions + 1
        || std::ranges::count(document.soundDefinitions.back().graph.nodes, std::string_view{"talas.midiVoice"},
               &talas::project::ProjectNode::typeId)
            != 2)
        return finish("a MIDI file dropped in did not become a Mixer of a MIDIVoice per channel: " + statusLabel_.getText().toStdString());

    // Open... on a spectrum file opens
    // the spectrum editor on it.
    {
        const talas::project::PartialTrackAsset spectrum{48000.0, 4800, {{1, 0, {{1, 0.5, 440.0, 8.0}}}, {1, 2400, {{1, 0.4, 445.0, 9.0}}}}};
        const auto encoded = talas::project::encodePartialTrackAsset(spectrum);
        std::ofstream output(folder / "voice.talaspartial", std::ios::binary);
        output.write(reinterpret_cast<const char*>(encoded.bytes.data()), static_cast<std::streamsize>(encoded.bytes.size()));
    }
    // The smoke has no audio device to stage the spectrum on, so it checks
    // the asset the editor is given and the workspace it shows.
    const auto spectrumOpened = openSpectrumEditor(folder / "voice.talaspartial");
    const auto* edited = talas::app::editedSpectrumAsset(projectBridge_.editor().document(), spectrumEditorAssetId_);
    const auto editedPath = edited == nullptr || edited->id != spectrumEditorAssetId_ ? std::string{} : edited->path;
    const auto analysisShown = workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis;
    spectrumEditorAssetId_.reset();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    if (!spectrumOpened || !analysisShown || !editedPath.ends_with("voice.talaspartial"))
        return finish("Open... on a spectrum file did not show it in the spectrum editor: " + statusLabel_.getText().toStdString());

    // The Frequently Used Folders window opens a folder, adds
    // the one selected inside it and removes one.
    {
        const auto savedPreferences = preferences_;
        std::filesystem::create_directories(folder / "library" / "drums");
        showFrequentlyUsedFolders();
        foldersPanel_->open(folder);
        const auto inside = foldersPanel_->upperFolders();
        const auto library = std::ranges::find(inside, folder / "library");
        const auto row = static_cast<int>(library - inside.begin());
        foldersPanel_->selectUpper(row);
        foldersPanel_->addSelected();
        const auto added = std::ranges::count(preferences_.frequentlyUsedFolders,
                               talas::project::pathToUtf8((folder / "library").lexically_normal()))
            == 1;
        foldersPanel_->selectUpper(row);
        foldersPanel_->openSelected();
        const auto opened = foldersPanel_->upperFolders() == std::vector<std::filesystem::path>{folder / "library" / "drums"};
        foldersPanel_->selectLower(static_cast<int>(preferences_.frequentlyUsedFolders.size()) - 1);
        foldersPanel_->removeSelected();
        const auto removed = preferences_.frequentlyUsedFolders == savedPreferences.frequentlyUsedFolders;
        foldersWindow_.reset();
        foldersPanel_.reset();
        preferences_ = savedPreferences;
        if (library == inside.end() || !added || !opened || !removed)
            return finish("the Frequently Used Folders window did not open, add and remove folders");
    }

    // A file's name typed into a Sample's
    // field, its old path dropped, is found in a frequently used folder's
    // subfolder and bound to the field.
    std::error_code copied;
    std::filesystem::create_directories(folder / "Drums", copied);
    std::filesystem::copy_file(folder / "kick.wav", folder / "Drums" / "hat.wav",
        std::filesystem::copy_options::overwrite_existing, copied);
    fileLocatorMemory_ = {};
    if (copied || !projectBridge_.editor().addNode({"player", "talas.sample", {}}, {40, 200}).ok())
        return finish("the Files smoke could not add a Sample to name a file");
    documentChanged();
    inspectorPanel_->commitExpressionForSmoke("player", "sample", "/Volumes/Gone/hat.wav");
    const auto& located = projectBridge_.editor().document();
    const auto player = std::ranges::find(located.graph.nodes, std::string_view{"player"}, &talas::project::ProjectNode::instanceId);
    const auto* sample = player == located.graph.nodes.end() ? nullptr : findParameter(*player, "sample");
    const auto asset = sample == nullptr || !sample->resource.has_value()
        ? located.assets.end()
        : std::ranges::find(located.assets, *sample->resource, &talas::project::ProjectAsset::id);
    if (asset == located.assets.end() || talas::project::pathFromUtf8(asset->path).filename() != "hat.wav"
        || fileLocatorMemory_.recentFolders.empty() || fileLocatorMemory_.recentFolders.front() != folder / "Drums")
        return finish("a file's name typed into the Sample's field was not located and bound: " + statusLabel_.getText().toStdString());
    // The Status window's Recycle forgets the files found.
    recycleMemory();
    if (!fileLocatorMemory_.recentFolders.empty() || !fileLocatorMemory_.recentlyFound.empty())
        return finish("Recycle did not forget the files and folders found recently");

    // The Sample field shows its file's
    // name alone, or its whole path with Show Full Path Names.
    preferences_.showFullPathNames = false;
    applyAppearancePreferences();
    const auto nameAlone = inspectorPanel_->parameterValueTextForSmoke();
    preferences_.showFullPathNames = true;
    applyAppearancePreferences();
    const auto wholePath = inspectorPanel_->parameterValueTextForSmoke();
    if (nameAlone != "hat.wav" || !wholePath.endsWith("/Drums/hat.wav") || wholePath.length() <= 14)
        return finish("Show Full Path Names did not show the Sample's file whole or by name: " + nameAlone.toStdString()
            + " " + wholePath.toStdString());
    // A name no folder holds asks to keep looking, use a
    // file list or cancel.
    smokeFileSearchAnswers_ = talas::app::FileSearchAnswer::Cancel;
    inspectorPanel_->commitExpressionForSmoke("player", "sample", "/Volumes/Gone/nowhere-at-all.wav");
    if (smokeFileSearchAnswers_.has_value())
        return finish("a file no folder holds did not ask to keep looking or use a file dialog");

    // Ctrl+Space in the File Organizer plays the selected
    // file, a MIDI file through a minimal orchestra.
    fileOrganizerPanel_->refresh();
    const auto organizerRows = fileOrganizerPanel_->rowsForSmoke();
    const auto tune = std::ranges::find_if(organizerRows, [](const auto& row) { return row.path.filename() == "tune.mid"; });
    const auto prepared = std::exchange(soundBrowserUsesPreparedProfileForSmoke_, true);
    const auto played = tune != organizerRows.end()
        && fileOrganizerPanel_->playRowForSmoke(static_cast<std::size_t>(std::distance(organizerRows.begin(), tune)));
    stopSoundBrowser();
    soundBrowserUsesPreparedProfileForSmoke_ = prepared;
    if (!played)
        return finish("Ctrl+Space in the File Organizer did not play the MIDI file: " + statusLabel_.getText().toStdString());

    // A file's name dragged from the organizer into a
    // field: the Sample's file field takes the file, a SetDuration's
    // Duration the sample's length, and the Sample's Frequency, which the
    // file means nothing to, stays as it was.
    preferences_.showFullPathNames = false;
    applyAppearancePreferences();
    if (!inspectorPanel_->selectParameterForSmoke("sample") || !inspectorPanel_->dropFileForSmoke(folder / "kick.wav")
        || inspectorPanel_->parameterValueTextForSmoke() != "kick.wav")
        return finish("a sample dragged into the Sample field was not taken: "
            + inspectorPanel_->parameterValueTextForSmoke().toStdString());
    if (!projectBridge_.editor().addNode({"length", "talas.setDuration", {}}, {40, 320}).ok())
        return finish("the Files smoke could not add a SetDuration");
    documentChanged();
    selectNode("length");
    if (!inspectorPanel_->selectParameterForSmoke("duration") || !inspectorPanel_->dropFileForSmoke(folder / "kick.wav")
        || inspectorPanel_->parameterValueTextForSmoke() != "0.1 s")
        return finish("a sample dragged into SetDuration's Duration did not give its length: "
            + inspectorPanel_->parameterValueTextForSmoke().toStdString());
    selectNode("player");
    const auto frequencyBefore = inspectorPanel_->selectParameterForSmoke("frequency")
        ? inspectorPanel_->parameterValueTextForSmoke() : juce::String{};
    if (frequencyBefore.isEmpty() || inspectorPanel_->dropFileForSmoke(folder / "kick.wav")
        || inspectorPanel_->parameterValueTextForSmoke() != frequencyBefore)
        return finish("a sample dragged into the Frequency field was not refused");
    return finish();
}

// The Sample Editor plays its
// selection, reads the selection in seconds or samples and the peak in dB or
// linear, pastes a file over the selection and shows Lissajous figures;
// Info > Full Waveform records the selected Sound with its extremes.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSampleEditorPlaySmoke(const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto pasted = smokeDirectory / "sample-editor-paste.wav";
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSampleBuffer();
        if (sampleEditorWindow_ != nullptr)
            sampleEditorWindow_->setVisible(false);
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        std::error_code ignored;
        std::filesystem::remove(pasted, ignored);
        if (!restored.ok() && error.empty())
            error = "Sample editor play smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    showSampleEditor();
    if (sampleEditorPanel_ == nullptr)
        return finish("the Sample Editor did not open");
    sampleEditorPanel_->newFile(8000.0, 1, 0.0);
    auto& model = sampleEditorPanel_->model();
    if (!model.insertSegment(std::vector<double>(800, 0.5)))
        return finish("the Sample Editor smoke could not fill its file");
    model.select(0, 400);
    if (!sampleEditorPanel_->performForSmoke("Play") || sampleAudition_.load(std::memory_order_acquire) == nullptr)
        return finish("Play did not start the selection");
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> outputs{left.data(), right.data()};
    const juce::AudioIODeviceCallbackContext context{};
    audioDeviceIOCallbackWithContext(nullptr, 0, outputs.data(), 2, 64, context);
    if (std::ranges::none_of(left, [](float sample) { return std::abs(sample) > 0.1F; }))
        return finish("Play did not sound the selection");
    // The System Output Level Pad takes 6 dB
    // off what the outputs play.
    const auto peak = [&left] { return std::ranges::max(left, {}, [](float sample) { return std::abs(sample); }); };
    const auto unpadded = std::abs(peak());
    talas::app::WorkbenchPreferences padded;
    padded.padSystemOutput = true;
    systemOutputGain_.store(talas::app::systemOutputGain(padded), std::memory_order_relaxed);
    audioDeviceIOCallbackWithContext(nullptr, 0, outputs.data(), 2, 64, context);
    applyAppearancePreferences();
    if (std::abs(std::abs(peak()) - unpadded * 0.501187F) > 0.01F)
        return finish("the -6 dB System Output Level Pad did not halve the outputs");
    stopSampleBuffer();
    const auto readout = sampleEditorPanel_->readoutForSmoke();
    sampleEditorPanel_->switchReadoutUnitsForSmoke();
    const auto switched = sampleEditorPanel_->readoutForSmoke();
    if (!readout.contains("0.0500 s (0.0500 s)") || !readout.contains("-6.0 dB") || !switched.contains("0 - 400 (400 samples)")
        || !switched.contains("0.5000"))
        return finish("the readout did not switch units: " + readout.toStdString() + " / " + switched.toStdString());
    std::string error;
    if (!talas::audio::writePcm16Wav(pasted, 8000.0, std::vector<std::vector<double>>{std::vector<double>(100, -0.25)}, error))
        return finish("the smoke could not write the file to paste: " + error);
    if (!sampleEditorPanel_->pasteFileForSmoke(pasted) || model.frames() != 500)
        return finish("the disk button did not paste the file over the selection");
    if (sampleEditorPanel_->lissajousPointsForSmoke(4) == 0)
        return finish("Display Lissajous drew nothing");

    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"osc", "talas.oscillator", {}}, {"gain", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"osc", "audioOut", "gain", "audioIn"}, {"gain", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    auto across = 0;
    for (auto& placed : fixture.presentation.structureLayouts.front().nodes)
    {
        placed.position.x = across;
        across += 260;
    }
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Full waveform fixture was rejected: " + replaced.message);
    documentChanged();
    showRootGraph();
    selectNode(std::nullopt, "osc");
    if (!recordFullWaveform(0.05) || !lastFullWaveformExtremes_.has_value() || !(lastFullWaveformExtremes_->minimum < -0.1)
        || !(lastFullWaveformExtremes_->maximum > 0.1))
        return finish("Full Waveform did not record the selected Sound with its extremes: " + statusLabel_.getText().toStdString());
    // A double-click on a Sound opens its
    // parameters below, its icon patterned; one on an empty area closes them.
    if (!graphCanvas_->doubleClickNodeForSmoke("gain") || selectedNodeId_ != "gain")
        return finish("a double-click on a Sound did not open its parameters");
    graphCanvas_->doubleClickEmptyForSmoke();
    if (!selectedNodeId_.empty())
        return finish("a double-click on an empty area did not close the parameters");
    return finish();
}
}
