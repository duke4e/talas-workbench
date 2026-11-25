#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/SoundFileWindowModel.h"

#include <filesystem>
#include <fstream>
#include "app/workbench/WorkbenchParameterFieldEditor.h"


namespace talas::app::workbench
{
// A Preset's Set to Current Event Values writes the Virtual
// Control Surface's current values of its Input's Event Values into its
// EventValues field.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runPresetCaptureSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Preset smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectParameterExpression level;
    level.parameterId = "value";
    level.expression = "!Level";
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {
        {"level", "talas.constant", {level}},
        {"held", "talas.preset", {}},
        {"out", "talas.audioOutput", {}},
    };
    fixture.graph.connections = {
        {"level", "value", "held", "audioIn"},
        {"held", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(fixture);
    (void)talas::project::synchronizeProjectEventValueControls(fixture, registry_);
    const auto control = std::ranges::find(
        fixture.virtualControls, "Level", &talas::project::VirtualControl::eventName);
    if (control == fixture.virtualControls.end())
        return finish("the Preset fixture has no Level control");
    control->value = 0.375;
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("Preset fixture was rejected: " + replaced.message);
    }
    documentChanged();
    showRootGraph();
    selectNode("level");
    if (inspectorPanel_->capturePresetEventValuesForSmoke())
        return finish("Set to Current Event Values was offered for a Sound that is not a Preset");
    selectNode("held");
    if (!inspectorPanel_->capturePresetEventValuesForSmoke())
        return finish("the inspector did not offer Set to Current Event Values for a Preset");
    const auto& document = projectBridge_.editor().document();
    const auto preset = std::ranges::find(
        document.graph.nodes, "held", &talas::project::ProjectNode::instanceId);
    const auto saved = preset == document.graph.nodes.end()
        ? nullptr
        : findParameter(*preset, "eventValues");
    if (saved == nullptr || saved->text
            != std::optional<std::string>{"!Level channel: 1; initialValue: 0.375."})
        return finish("Set to Current Event Values did not write the Level control's value");
    undoProject();
    if (talas::project::serializeProjectDocument(projectBridge_.editor().document())
        != talas::project::serializeProjectDocument(fixture))
        return finish("one Undo did not take back Set to Current Event Values");
    return finish();
}

// The envelope breakpoint and array field editors;
// An expression dragged from the Expressions
// Library into a parameter field.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runParameterFieldEditorSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "parameter field editor smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    fixture.graph.nodes = {
        {"env", "talas.graphicalEnvelope", {}},
        {"noise", "talas.noise", {{"frequency", "!Pitch * 0.5"}}},
        {"fir", "talas.firFilter", {}},
        {"osc", "talas.oscillator", {}},
        {"out", "talas.audioOutput", {}},
    };
    fixture.graph.connections = {
        {"noise", "audioOut", "fir", "audioIn"},
        {"fir", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("parameter field fixture was rejected: " + replaced.message);
    }
    documentChanged();
    showRootGraph();
    const auto text = [&](std::string_view nodeId, std::string_view parameterId) -> std::string {
        const auto* node = findNode(projectBridge_.editor().document(), nodeId);
        const auto* parameter = node == nullptr ? nullptr : findParameter(*node, parameterId);
        if (parameter == nullptr)
            return {};
        return parameter->text.value_or(parameter->expression.value_or(std::string{}));
    };

    // The envelope breakpoint field.
    selectNode("env");
    if (!inspectorPanel_->selectParameterForSmoke("envelope"))
        return finish("the Graphical Envelope's Envelope field could not be selected");
    auto* envelope = inspectorPanel_->fieldEditorForSmoke();
    if (envelope == nullptr || envelope->mode() != ParameterFieldEditor::Mode::Envelope)
        return finish("the Envelope field did not show the breakpoint editor");
    if (!envelope->addEnvelopePoint(0.5, 0.5)
        || text("env", "envelope") != "{0 s} {0} {0.1 s} {1} {0.5 s} {0.5} {1 s} {0}"
        || inspectorPanel_->parameterValueTextForSmoke() != "{0 s} {0} {0.1 s} {1} {0.5 s} {0.5} {1 s} {0}")
        return finish("shift-click in the envelope did not add a breakpoint to the field");
    envelope = inspectorPanel_->fieldEditorForSmoke();
    if (envelope == nullptr || !envelope->selectEnvelopePoint(1) || !envelope->toggleSelectedLoopStart()
        || !envelope->selectEnvelopePoint(2) || !envelope->toggleSelectedLoopEnd()
        || text("env", "envelope") != "{0 s} {0} {0.1 s} {1} loopStart {0.5 s} {0.5} loopEnd {1 s} {0}")
        return finish("the loop buttons did not mark the envelope's loop");
    envelope = inspectorPanel_->fieldEditorForSmoke();
    if (envelope == nullptr || !envelope->selectEnvelopePoint(2) || !envelope->moveSelectedEnvelopePoint(0.75, 0.25)
        || text("env", "envelope") != "{0 s} {0} {0.1 s} {1} loopStart {0.75 s} {0.25} loopEnd {1 s} {0}")
        return finish("dragging a breakpoint did not move it in the field");
    envelope = inspectorPanel_->fieldEditorForSmoke();
    if (envelope == nullptr || !envelope->selectEnvelopePoint(2) || !envelope->removeSelectedEnvelopePoint()
        || text("env", "envelope") != "{0 s} {0} {0.1 s} {1} {1 s} {0}")
        return finish("Delete did not remove the selected breakpoint and its loop mark");
    undoProject();
    if (text("env", "envelope") != "{0 s} {0} {0.1 s} {1} loopStart {0.75 s} {0.25} loopEnd {1 s} {0}")
        return finish("one Undo did not take back a breakpoint's removal");

    // The array field.
    selectNode("fir");
    if (!inspectorPanel_->selectParameterForSmoke("coefficients"))
        return finish("the FIR Filter's coefficients could not be selected");
    auto* array = inspectorPanel_->fieldEditorForSmoke();
    if (array == nullptr || array->mode() != ParameterFieldEditor::Mode::Array)
        return finish("an array of numbers did not show the array editor");
    if (!array->setArrayValue(1, 0.75) || text("fir", "coefficients") != "0.25 0.75 0.25")
        return finish("dragging an array bar did not set its value");
    array = inspectorPanel_->fieldEditorForSmoke();
    if (array == nullptr || !array->appendArrayValue() || text("fir", "coefficients") != "0.25 0.75 0.25 0.25")
        return finish("the array editor's + did not add a value");

    // The Expressions Library.
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowExpressionsLibrary;
    if (!listed || !commandManager_.invokeDirectly(commandShowExpressionsLibrary, false)
        || expressionsLibraryPanel_ == nullptr)
        return finish("the Tools menu did not open the Expressions Library");
    expressionsLibraryPanel_->setQueryForSmoke("pitch interval");
    const auto description = expressionsLibraryPanel_->dragDescriptionForSmoke("Pitch plus an interval");
    if (expressionsLibraryPanel_->visibleEntryCountForSmoke() != 1 || description.isEmpty())
        return finish("finding pitch interval did not list Pitch plus an interval");
    // An expressions file of one's own in a frequently used
    // folder joins the library.
    const auto ownFolder = std::filesystem::temp_directory_path() / "talas-own-expressions-smoke";
    std::filesystem::create_directories(ownFolder);
    {
        std::ofstream file(ownFolder / "mine.txt", std::ios::binary);
        file << "EXPR\n\"Halved fader\nHalf of the fader, for a quieter level.\"\n!Fader * 0.5\n";
    }
    const auto previousFolders = preferences_.frequentlyUsedFolders;
    preferences_.frequentlyUsedFolders = {talas::project::pathToUtf8(ownFolder)};
    expressionsLibraryPanel_->setQueryForSmoke("halved fader");
    const auto own = expressionsLibraryPanel_->dragDescriptionForSmoke("Halved fader");
    preferences_.frequentlyUsedFolders = previousFolders;
    std::filesystem::remove_all(ownFolder);
    if (expressionsLibraryPanel_->visibleEntryCountForSmoke() != 1 || own.isEmpty())
        return finish("an expressions file of one's own did not join the Expressions Library");
    expressionsLibraryPanel_->setQueryForSmoke("pitch interval");
    selectNode("osc");
    if (!inspectorPanel_->selectParameterForSmoke("frequency"))
        return finish("the oscillator's Frequency could not be selected");
    if (inspectorPanel_->fieldEditorForSmoke() != nullptr)
        return finish("an expression field showed a graphical editor");
    if (inspectorPanel_->dropExpressionForSmoke("talas.oscillator"))
        return finish("a prototype was taken as an expression");
    if (!inspectorPanel_->dropExpressionForSmoke(description) || text("osc", "frequency") != "!KeyPitch + !Interval nn")
        return finish("dropping Pitch plus an interval did not set the Frequency field");
    undoProject();
    if (text("osc", "frequency") == "!KeyPitch + !Interval nn")
        return finish("one Undo did not take back the dropped expression");
    // A Sound dragged into a field gives it its own value for
    // the parameter of that name; a Sound without one is refused.
    const auto before = text("osc", "frequency");
    if (inspectorPanel_->dropSoundForSmoke(juceString(prototypeDragPrefix) + "talas.constant") || text("osc", "frequency") != before)
        return finish("a Sound without a Frequency was not refused");
    const auto* oscillatorPrototype = registry_.findNodeType("talas.oscillator");
    const auto prototypeFrequency = oscillatorPrototype == nullptr ? std::nullopt
                                                                   : talas::app::parameterValueOfSound(*oscillatorPrototype, "frequency");
    if (!prototypeFrequency.has_value() || !inspectorPanel_->dropSoundForSmoke(juceString(prototypeDragPrefix) + "talas.oscillator")
        || text("osc", "frequency") != *prototypeFrequency)
        return finish("an Oscillator dragged into the Frequency field did not give it its Frequency");
    if (before != *prototypeFrequency)
        undoProject();
    // A Sound of the signal flow gives its own value.
    const auto noiseDrag = juceString(talas::app::signalFlowSoundDragDescription(std::nullopt, "noise"));
    if (!inspectorPanel_->dropSoundForSmoke(noiseDrag) || text("osc", "frequency") != "!Pitch * 0.5")
        return finish("the Noise dragged into the Frequency field did not give it the Noise's Frequency");
    undoProject();

    // Tab goes on through the fields, Shift+Tab back, the edit in a field
    // taken as Tab leaves it; an expression refused keeps its field.
    selectNode("noise");
    const auto* noise = registry_.findNodeType("talas.noise");
    if (noise == nullptr || noise->parameters.size() < 3
        || !inspectorPanel_->selectParameterForSmoke(noise->parameters[0].id))
        return finish("the noise's first field could not be selected");
    const auto& fields = noise->parameters;
    if (inspectorPanel_->tabFromValueForSmoke("0.25", true).first != fields[1].id || text("noise", fields[0].id) != "0.25")
        return finish("Tab did not take the edit and go on to the next field");
    const auto onward = inspectorPanel_->tabFromValueForSmoke({}, true);
    if (onward.first != fields[2].id || !onward.second)
        return finish("Tab did not go on to the next field with its text selected");
    if (inspectorPanel_->tabFromValueForSmoke("1 +", true).first != fields[2].id)
        return finish("Tab left a field whose expression was refused");
    if (!inspectorPanel_->selectParameterForSmoke(fields[2].id)
        || inspectorPanel_->tabFromValueForSmoke({}, false).first != fields[1].id)
        return finish("Shift+Tab did not go back to the field before");

    // The Edit menu's Select All selects the active field's text, not the
    // Sounds.
    selectNode("osc");
    auto* field = dynamic_cast<juce::TextEditor*>(inspectorPanel_->expressionValueComponentForSmoke());
    if (field == nullptr || !inspectorPanel_->selectParameterForSmoke("frequency"))
        return finish("the oscillator's Frequency field could not be made active");
    smokeActiveField_ = field;
    field->setHighlightedRegion({});
    const auto selectedAll = commandManager_.invokeDirectly(commandSelectAllGraphNodes, false)
        && field->getHighlightedRegion().getLength() == field->getTotalNumChars() && field->getTotalNumChars() > 0
        && selectedNodeIds_ == std::vector<std::string>{"osc"};
    smokeActiveField_ = nullptr;
    if (!selectedAll)
        return finish("the Edit menu's Select All did not select the active field's text");
    return finish();
}
}
