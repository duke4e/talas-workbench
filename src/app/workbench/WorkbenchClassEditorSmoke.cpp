#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/SoundClassIcon.h"

namespace talas::app::workbench
{
// The Class Editor sets a Sound's parameter
// ranges, fields and comments and its Class Description; an instance's
// field shows the comment and range, and a value outside the range is
// refused; and its icon.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runClassEditorSmoke()
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
        frontWindowName_.clear();
        updateHistoryWindow();
        if (!restored.ok() && error.empty())
            error = "Class Editor smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    voice.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "freq";
    frequency.displayName = "Frequency";
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue.parameterId = "freq";
    frequency.defaultValue.expression = "440 hz";
    frequency.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, "osc", "frequency", {},
        talas::expr::ValueDomain::Hertz});
    voice.exposedParameters.push_back(frequency);
    talas::project::ProjectSoundExposedParameter interpolation;
    interpolation.parameterId = "interpolation";
    interpolation.displayName = "Interpolation";
    interpolation.kind = talas::core::ParameterKind::Choice;
    interpolation.choices = {"linear", "none"};
    interpolation.defaultValue.parameterId = "interpolation";
    interpolation.defaultValue.choice = "linear";
    interpolation.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, "osc", "interpolation", {},
        talas::expr::ValueDomain::Scalar});
    voice.exposedParameters.push_back(interpolation);
    fixture.soundDefinitions.push_back(voice);
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    fixture.graph.nodes.push_back(instance);
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Class Editor fixture was rejected: " + replaced.message);
    documentChanged();
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowClassEditor;
    if (!listed || !commandManager_.invokeDirectly(commandShowClassEditor, false) || classEditorPanel_ == nullptr)
        return finish("the Tools menu did not open the Class Editor");
    // Action > Edit Class opens the Class
    // Editor on the selected Sound's class.
    showRootGraph();
    selectNode(std::nullopt, "voice1");
    if (!commandManager_.invokeDirectly(commandEditClass, false) || classEditorPanel_->selectedDefinition() != "voice")
        return finish("Edit Class did not open the Class Editor on the selected Sound's class");
    auto& editor = *classEditorPanel_;
    if (!editor.selectDefinitionForSmoke("voice") || !editor.selectParameterForSmoke("freq")
        || editor.fieldForSmoke("name") != "Frequency" || editor.fieldForSmoke("type") != "hertz")
        return finish("the Class Editor did not show the Voice's Frequency");
    if (!editor.setFieldForSmoke("minimum", "20") || !editor.setFieldForSmoke("maximum", "2000")
        || !editor.setFieldForSmoke("field", "0 @ 0 corner: 1 @ 0.5")
        || !editor.setFieldForSmoke("comment", "The partial's frequency.")
        || !editor.setFieldForSmoke("description", "One partial.") || !editor.applyForSmoke())
        return finish("the Class Editor's fields were refused");
    const auto& kept = projectBridge_.editor().document().soundDefinitions.front();
    if (kept.description != "One partial." || kept.exposedParameters.front().minimum != std::optional<double>{20.0}
        || kept.exposedParameters.front().comment != "The partial's frequency."
        || !kept.exposedParameters.front().field.has_value())
        return finish("the Class Editor did not keep the Sound's fields");
    // The Field Layout draws the field, gray
    // while its parameter is selected.
    if (editor.fieldForSmoke("fieldLayout") != "Frequency*")
        return finish("the Field Layout did not highlight the Frequency's field: " + editor.fieldForSmoke("fieldLayout").toStdString());
    // The Edit menu edits the class description, its field
    // in the Class Editor's own window.
    {
        auto& description = editor.descriptionFieldForSmoke();
        description.grabKeyboardFocus();
        const auto focused = juce::Component::getCurrentlyFocusedComponent() == &description;
        smokeActiveField_ = focused ? nullptr : &description;
        description.setHighlightedRegion({});
        const auto selectedAll = (!focused || activeTextField() == &description)
            && commandManager_.invokeDirectly(commandSelectAllGraphNodes, false)
            && description.getHighlightedRegion().getLength() == description.getTotalNumChars()
            && description.getTotalNumChars() > 0;
        smokeActiveField_ = nullptr;
        juce::Component::unfocusAllComponents();
        if (!selectedAll || (isShowing() && !isInToolWindow(description)))
            return finish("the Edit menu's Select All did not select the class description's text");
    }
    // Expand, checked as a class is made, unchecked lets a
    // ParameterTransformer transform its parameters before it expands.
    if (editor.fieldForSmoke("expand") != "true" || !editor.setFieldForSmoke("expand", "false") || !editor.applyForSmoke()
        || projectBridge_.editor().document().soundDefinitions.front().expandsBeforeTransformation
        || !editor.setFieldForSmoke("expand", "true") || !editor.applyForSmoke()
        || !projectBridge_.editor().document().soundDefinitions.front().expandsBeforeTransformation)
        return finish("the Class Editor's Expand box was not kept");
    // Each Apply is one Undo step; take both Expand changes back.
    undoProject();
    if (projectBridge_.editor().document().soundDefinitions.front().expandsBeforeTransformation)
        return finish("Undo did not take back the Class Editor's checked Expand box");
    undoProject();
    if (!projectBridge_.editor().document().soundDefinitions.front().expandsBeforeTransformation
        || projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().minimum
            != std::optional<double>{20.0})
        return finish("Undo did not take back the Class Editor's unchecked Expand box");
    if (!editor.setFieldForSmoke("maximum", "10") || editor.applyForSmoke())
        return finish("a maximum below the minimum was applied");
    // A parameter type fills the type and range, which Apply
    // keeps; a frequency parameter takes the frequency type.
    if (!editor.setFieldForSmoke("talasType", "positiveFrequency") || editor.fieldForSmoke("type") != "hertz"
        || editor.fieldForSmoke("minimum") != "0.001" || editor.fieldForSmoke("maximum") != "25000"
        || !editor.applyForSmoke()
        || projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().maximum
            != std::optional<double>{25000.0})
    {
        return finish("the parameter type positiveFrequency did not set the Frequency's range");
    }
    // The type selects its default field, here a
    // value field, which takes no Event Values.
    {
        talas::project::ProjectParameterExpression played;
        played.parameterId = "freq";
        played.expression = "440 hz * (1 + !Bend)";
        const auto refused = projectBridge_.editor().setParameter("voice1", played);
        if (editor.fieldForSmoke("fieldKind") != "value"
            || projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().fieldKind
                != talas::project::ProjectSoundFieldKind::Value
            || refused.ok() || refused.message.find("value field") == std::string::npos)
            return finish("the positiveFrequency type did not give the Frequency a value field refusing Event Values: "
                + refused.message);
    }
    undoProject();
    // Closing the editor with changes
    // not kept asks whether to save them; dropped, the class stays as it
    // was, saved, it takes them.
    editor.refresh();
    (void) editor.selectParameterForSmoke("freq");
    const auto comment = [this] {
        return projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().comment;
    };
    const auto commentKept = comment();
    if (editor.hasChangesNotKept() || !editor.setFieldForSmoke("comment", "Dropped on closing.") || !editor.hasChangesNotKept()
        || !closeClassEditor(false) || editor.hasChangesNotKept() || comment() != commentKept)
        return finish("closing the Class Editor did not drop the changes not kept");
    (void) editor.selectParameterForSmoke("freq");
    if (!editor.setFieldForSmoke("comment", "Saved on closing.") || !closeClassEditor(true) || comment() != "Saved on closing.")
        return finish("closing the Class Editor did not save the changes when asked to");
    undoProject();
    // The instance's field shows the comment and range, and refuses 10 hz.
    showRootGraph();
    selectNode("voice1");
    if (!inspectorPanel_->selectParameterForSmoke("freq")
        || !inspectorPanel_->parameterHintContainsForSmoke("The partial's frequency.")
        || !inspectorPanel_->parameterHintContainsForSmoke("Range 20 to 2000"))
        return finish("the instance's Frequency did not show its comment and range");
    talas::project::ProjectParameterExpression low;
    low.parameterId = "freq";
    low.expression = "10 hz";
    if (projectBridge_.editor().setParameter("voice1", low).ok())
        return finish("a Frequency below the range was accepted");
    undoProject();
    if (projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().minimum.has_value())
        return finish("one Undo did not take back the Class Editor's change");

    // A radioButtonsTwoColumns field shows
    // a choice's values as radio buttons in two columns; a click chooses.
    if (!editor.selectParameterForSmoke("interpolation") || editor.fieldForSmoke("fieldKind") != "symbolList"
        || !editor.setFieldForSmoke("fieldKind", "radioButtonsTwoColumns") || !editor.applyForSmoke())
        return finish("the Class Editor did not give the Interpolation a radioButtonsTwoColumns field");
    showRootGraph();
    selectNode("voice1");
    if (!inspectorPanel_->selectParameterForSmoke("interpolation")
        || inspectorPanel_->radioChoicesForSmoke() != "linear@0,0* none@1,0"
        || !inspectorPanel_->clickRadioChoiceForSmoke("none"))
        return finish("the Interpolation's field did not show two columns of radio buttons: "
            + inspectorPanel_->radioChoicesForSmoke().toStdString());
    const auto& radioNode = projectBridge_.editor().document().graph.nodes.front();
    const auto chosen = std::ranges::find(radioNode.parameters, std::string{"interpolation"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (chosen == radioNode.parameters.end() || chosen->choice != std::optional<std::string>{"none"}
        || inspectorPanel_->radioChoicesForSmoke() != "linear@0,0 none@1,0*")
        return finish("a click on a radio button did not choose none");
    undoProject();
    undoProject();

    // Retrieve Example from Class puts the Sound the
    // class was made from among the project's Sounds.
    {
        const auto before = projectBridge_.editor().document().soundDefinitions.size();
        if (!commandManager_.invokeDirectly(commandRetrieveExampleFromClass, false)
            || projectBridge_.editor().document().soundDefinitions.size() != before + 1
            || projectBridge_.editor().document().soundDefinitions.back().displayName != "Voice example")
            return finish("Retrieve Example from Class did not save the class's example: " + statusLabel_.getText().toStdString());
        undoProject();
    }

    // Another Sound dragged onto the
    // Parameter list gives each parameter its namesake's type there.
    {
        auto withTone = projectBridge_.editor().document();
        auto tone = withTone.soundDefinitions.front();
        tone.definitionId = "tone";
        tone.displayName = "Tone";
        tone.exposedParameters.front().minimum = 20.0;
        tone.exposedParameters.front().maximum = 2000.0;
        withTone.soundDefinitions.push_back(std::move(tone));
        talas::project::reconcileProjectPresentation(withTone);
        if (const auto added = projectBridge_.editor().applyDocumentEdit(std::move(withTone)); !added.ok())
            return finish("the second Sound class was refused: " + added.message);
        documentChanged();
        if (!editor.selectDefinitionForSmoke("voice") || !editor.dropOnParametersForSmoke(juceString(soundDragPrefix) + "tone")
            || projectBridge_.editor().document().soundDefinitions.front().exposedParameters.front().maximum
                != std::optional<double>{2000.0})
            return finish("a Sound class dragged onto the Parameter list did not give the Frequency its type");
        undoProject();
        undoProject();
        if (projectBridge_.editor().document().soundDefinitions.size() != 1)
            return finish("two Undos did not take back the typed parameters and the second class");
    }

    // The icon editor draws a
    // stroke of dots on the large icon and erases one on the small, takes
    // the icon of a prototype dragged onto it, and keeps the PNG icon box;
    // Apply saves the icon with the class.
    (void)editor.selectDefinitionForSmoke("voice");
    editor.strokeIconForSmoke(true, 2, 2, 12, 7);
    editor.strokeIconForSmoke(false, 0, 0, 3, 0);
    editor.strokeIconForSmoke(false, 1, 0, 1, 0);
    const auto drawn = editor.iconForSmoke();
    if (!talas::app::soundIconDot(drawn, true, {2, 2}) || !talas::app::soundIconDot(drawn, true, {12, 7})
        || !talas::app::soundIconDot(drawn, false, {3, 0}) || talas::app::soundIconDot(drawn, false, {1, 0}))
        return finish("the icon editor did not draw and erase dots");
    editor.setPngIconForSmoke(true);
    if (!editor.applyForSmoke() || projectBridge_.editor().document().soundDefinitions.front().icon
            != std::optional<talas::project::ProjectSoundIcon>{editor.iconForSmoke()}
        || !editor.iconForSmoke().png)
        return finish("Apply did not keep the class's drawn icon and PNG icon box");
    if (!editor.dropOnIconForSmoke(juceString(prototypeDragPrefix) + "talas.absoluteValue") || !editor.applyForSmoke()
        || projectBridge_.editor().document().soundDefinitions.front().icon->sameAs != "talas.absoluteValue"
        || !projectBridge_.editor().document().soundDefinitions.front().icon->large.empty())
        return finish("an AbsoluteValue dragged into the icon editor did not give the class its icon");
    // The input and output types, side effects
    // and numbers of channels.
    editor.setIconTypesForSmoke("audio", "audio", true);
    editor.setIconChannelsForSmoke(1, 2);
    if (!editor.applyForSmoke() || projectBridge_.editor().document().soundDefinitions.front().icon->inputType != "audio"
        || projectBridge_.editor().document().soundDefinitions.front().icon->outputType != "audio"
        || !projectBridge_.editor().document().soundDefinitions.front().icon->sideEffects
        || projectBridge_.editor().document().soundDefinitions.front().icon->inputChannels != 1
        || projectBridge_.editor().document().soundDefinitions.front().icon->outputChannels != 2)
        return finish("Apply did not keep the class's input and output types, side effects and channels");
    undoProject();
    undoProject();
    undoProject();
    if (projectBridge_.editor().document().soundDefinitions.front().icon.has_value())
        return finish("Undo did not take back the class's icon");
    // New Class from Example applies
    // only in a Sound file window: dimmed with the Sound selected in the
    // signal flow, it makes the Sound selected in the Sound file window a
    // class of its own and opens it in the Class Editor.
    const auto classesBefore = projectBridge_.editor().document().soundDefinitions.size();
    showRootGraph();
    selectNode(std::nullopt, "voice1");
    frontWindowName_.clear();
    if (commandManager_.invokeDirectly(commandNewClassFromExample, false)
        || projectBridge_.editor().document().soundDefinitions.size() != classesBefore)
        return finish("New Class from Example applied with the Sound in the signal flow, not in a Sound file window");
    showSoundFileWindow();
    if (soundFilePanel_ == nullptr)
        return finish("the Sound file window did not open for New Class from Example");
    soundFilePanel_->selectSounds({"voice"});
    const auto beforeClass = talas::project::serializeProjectDocument(projectBridge_.editor().document());
    if (!commandManager_.invokeDirectly(commandNewClassFromExample, false)
        || projectBridge_.editor().document().soundDefinitions.size() != classesBefore + 1
        || classEditorPanel_->selectedDefinition() != projectBridge_.editor().document().soundDefinitions.back().definitionId)
        return finish("New Class from Example did not make a class of the Sound selected in the Sound file window and edit it: "
            + statusLabel_.getText().toStdString());
    // The first close asks whether to save an
    // instance of the new class; No starts over, Yes keeps it, saving an
    // instance in the Sound file window, and the question is not asked
    // again.
    if (!newClassStart_.has_value() || !keepNewClass(false)
        || talas::project::serializeProjectDocument(projectBridge_.editor().document()) != beforeClass)
        return finish("No to saving an instance of the new class did not start over");
    showSoundFileWindow();
    soundFilePanel_->selectSounds({"voice"});
    if (!commandManager_.invokeDirectly(commandNewClassFromExample, false) || !keepNewClass(true)
        || projectBridge_.editor().document().soundDefinitions.size() != classesBefore + 2 || !closeClassEditor(true))
        return finish("Yes to saving an instance of the new class did not keep it, with its instance, and close the editor");
    // The example's free variables are asked for
    // first, and the values given become the class's defaults.
    auto withVariable = projectBridge_.editor().document();
    withVariable.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::ProjectSoundDefinition level;
    level.definitionId = "level";
    level.displayName = "Level";
    level.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{.parameterId = "amplitude", .expression = std::string{"?left"}}}, {}, {}});
    level.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    withVariable.soundDefinitions.push_back(level);
    talas::project::reconcileProjectPresentation(withVariable);
    if (const auto replaced = projectBridge_.editor().replaceDocument(withVariable); !replaced.ok())
        return finish("the example with a free variable was refused: " + replaced.message);
    documentChanged();
    variableEnvironment_.clear();
    smokeVariableValue_ = 0.75;
    showSoundFileWindow();
    soundFilePanel_->selectSounds({"level"});
    const auto asked = commandManager_.invokeDirectly(commandNewClassFromExample, false);
    smokeVariableValue_.reset();
    const auto made = projectBridge_.editor().document().soundDefinitions.back();
    const auto left = std::ranges::find(made.exposedParameters, std::string{"left"},
        &talas::project::ProjectSoundExposedParameter::parameterId);
    if (!asked || left == made.exposedParameters.end()
        || left->defaultValue.expression != std::optional<std::string>{"0.75"} || !keepNewClass(true))
    {
        std::string seen = std::string{asked ? "asked" : "not asked"} + " " + made.definitionId + ":";
        for (const auto& parameter : made.exposedParameters)
            seen += " " + parameter.parameterId + "=" + parameter.defaultValue.expression.value_or("-");
        return finish("the value given the example's ?left did not become the new class's default: " + seen + " "
            + statusLabel_.getText().toStdString());
    }
    variableEnvironment_.clear();
    return finish();
}
}
