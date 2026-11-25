#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <ranges>

#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/DspStatusModel.h"
#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/SoundInformation.h"
#include "talas/app/VariableValuePrompt.h"
#include "talas/project/ProjectVariables.h"
#include "talas/runtime/ConstantFields.h"

namespace talas::app::workbench
{
// Describe Sound: a
// help window with a description of the Sound's class and its parameters,
// those the virtual control surface can control named in red.
void WorkbenchComponent::describeSound(const talas::core::NodeTypeDescriptor& descriptor)
{
    soundHelpText_ = {};
    std::vector<juce::Range<int>> red;
    for (const auto& span : talas::app::descriptorSoundClassHelpSpans(descriptor, [&descriptor](std::string_view id) {
             return talas::runtime::isConstantField(descriptor.typeId, id);
         }))
    {
        const auto text = juceString(span.text);
        if (span.red)
            red.emplace_back(soundHelpText_.length(), soundHelpText_.length() + text.length());
        soundHelpText_ += text;
    }
    setStatus("Describing " + juceString(descriptor.displayName));
    showHelpWindow("Describe Sound: " + juceString(descriptor.displayName), soundHelpText_, false, red);
}

void WorkbenchComponent::showHelpWindow(
    const juce::String& title, const juce::String& text, bool editable, const std::vector<juce::Range<int>>& red)
{
    if (!isShowing())
        return;
    auto editor = std::make_unique<juce::TextEditor>();
    editor->setMultiLine(true);
    editor->setReadOnly(!editable);
    editor->setScrollbarsShown(true);
    editor->setCaretVisible(editable);
    if (editable)
    {
        editor->setReturnKeyStartsNewLine(true);
        editor->addKeyListener(&evaluateKeys_);
    }
    editor->setFont(WorkbenchType::body());
    editor->setText(text.substring(0, red.empty() ? text.length() : red.front().getStart()), false);
    // The red ranges, and the black text after each.
    for (std::size_t index = 0; index < red.size(); ++index)
    {
        const auto end = index + 1 < red.size() ? red[index + 1].getStart() : text.length();
        editor->setColour(juce::TextEditor::textColourId,
            juce::Colour{talas::app::fieldTextColour(talas::app::FieldTextKind::EventValue)}.withAlpha(1.0F));
        editor->moveCaretToEnd();
        editor->insertTextAtCaret(text.substring(red[index].getStart(), red[index].getEnd()));
        editor->setColour(juce::TextEditor::textColourId, textColour);
        editor->insertTextAtCaret(text.substring(red[index].getEnd(), end));
    }
    editor->moveCaretToTop(false);
    editor->setSize(560, 440);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(editor.release());
    options.dialogTitle = title;
    options.dialogBackgroundColour = panelColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

// Get info: the Sound itself, its parameters'
// values, inputs, Event Values, variables, files and complexity.
void WorkbenchComponent::describeGraphNode(
    const std::optional<std::string>& definitionId, std::string_view nodeId, bool info)
{
    const auto* node = nodeForContext(definitionId, nodeId);
    const auto* graph = graphForContext(definitionId);
    if (node == nullptr || graph == nullptr)
    {
        setStatus("Select one Sound to describe");
        return;
    }
    if (info)
    {
        const auto information =
            talas::app::soundInformation(projectBridge_.editor().document(), *graph, registry_, nodeId);
        if (!information)
        {
            setStatus("There is no information on " + juceString(node->instanceId));
            return;
        }
        soundHelpText_ = juceString(talas::app::soundInformationText(*information));
        // Get Info takes place within the variables
        // environment, each variable with the value it gives.
        if (!information->variables.empty())
        {
            soundHelpText_ << "\nIn the environment:";
            for (const auto& line : talas::app::variableInformationLines(information->variables, wholeVariableEnvironment()))
                soundHelpText_ << "\n  " << juceString(line);
        }
        setStatus("Information on " + juceString(node->instanceId));
        showHelpWindow("Get Info: " + juceString(node->instanceId), soundHelpText_);
        return;
    }
    const auto descriptor = talas::project::describeProjectNode(projectBridge_.editor().document(), *node, registry_);
    if (!descriptor)
    {
        setStatus("There is no description of " + juceString(node->typeId));
        return;
    }
    describeSound(*descriptor);
}

void WorkbenchComponent::showEvaluationDebugLines(const std::vector<std::string>& lines)
{
    evaluationDebugLines_ = lines;
    if (lines.empty() || !isShowing())
        return;
    juce::String text;
    for (const auto& line : lines)
        text << juceString(line) << "\n";
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, "Debug", text.trimEnd(), "OK", this);
}
namespace
{
// A graph of points joined by lines, within axes through 0 where the
// points' ranges hold it.
class XYGraphComponent final : public juce::Component
{
public:
    explicit XYGraphComponent(talas::project::smalltalk::XYGraph graph)
        : graph_(std::move(graph))
    {
        setSize(480, 320);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        if (graph_.empty())
            return;
        auto [minimumX, maximumX] = std::ranges::minmax(graph_ | std::views::keys);
        auto [minimumY, maximumY] = std::ranges::minmax(graph_ | std::views::values);
        if (maximumX == minimumX)
            maximumX = minimumX + 1.0;
        if (maximumY == minimumY)
            maximumY = minimumY + 1.0;
        const auto area = getLocalBounds().reduced(24).toFloat();
        const auto at = [&](double x, double y) {
            return juce::Point<float>{area.getX() + static_cast<float>((x - minimumX) / (maximumX - minimumX)) * area.getWidth(),
                area.getBottom() - static_cast<float>((y - minimumY) / (maximumY - minimumY)) * area.getHeight()};
        };
        graphics.setColour(secondaryTextColour);
        if (minimumY <= 0.0 && maximumY >= 0.0)
            graphics.drawLine({at(minimumX, 0.0), at(maximumX, 0.0)});
        if (minimumX <= 0.0 && maximumX >= 0.0)
            graphics.drawLine({at(0.0, minimumY), at(0.0, maximumY)});
        juce::Path line;
        line.startNewSubPath(at(graph_.front().first, graph_.front().second));
        for (const auto& [x, y] : graph_)
            line.lineTo(at(x, y));
        graphics.setColour(dataColour);
        graphics.strokePath(line, juce::PathStrokeType{1.5F});
    }

private:
    talas::project::smalltalk::XYGraph graph_;
};
}

// A question an evaluation asks, OK and Cancel (Yes
// and No for confirmYesNo:) with a field for a typed answer; the evaluation
// runs again with the answer.
void WorkbenchComponent::askEvaluationQuestion(juce::TextEditor& editor, const talas::project::smalltalk::Prompt& prompt,
    std::vector<talas::project::smalltalk::PromptAnswer> answers, std::vector<std::uint32_t> seeds)
{
    using talas::project::smalltalk::PromptKind;
    if (!smokePromptAnswers_.empty())
    {
        answers.push_back(smokePromptAnswers_.front());
        smokePromptAnswers_.pop_front();
        (void)evaluateSelectionIn(editor, std::move(answers), std::move(seeds));
        return;
    }
    // A file, from a file dialog of the types given.
    if (prompt.kind == PromptKind::OpenFile || prompt.kind == PromptKind::SaveFile)
    {
        juce::StringArray patterns;
        for (const auto& extension : prompt.fileExtensions)
            patterns.add("*." + juceString(extension));
        const auto open = prompt.kind == PromptKind::OpenFile;
        evaluationFileChooser_ = std::make_unique<juce::FileChooser>(juceString(prompt.question),
            prompt.initialAnswer.empty() ? juce::File{} : juce::File::getCurrentWorkingDirectory().getChildFile(juceString(prompt.initialAnswer)),
            patterns.joinIntoString(";"));
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        juce::Component::SafePointer<juce::TextEditor> safeEditor(&editor);
        evaluationFileChooser_->launchAsync(
            (open ? juce::FileBrowserComponent::openMode : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting)
                | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, safeEditor, answers = std::move(answers), seeds = std::move(seeds)](const juce::FileChooser& chooser) mutable {
                if (safeThis == nullptr || safeEditor == nullptr)
                    return;
                const auto chosen = chooser.getResult();
                answers.push_back({chosen != juce::File{}, chosen == juce::File{} ? std::string{} : chosen.getFullPathName().toStdString()});
                (void)safeThis->evaluateSelectionIn(*safeEditor, std::move(answers), std::move(seeds));
            });
        return;
    }
    const auto typed = prompt.kind == PromptKind::Value || prompt.kind == PromptKind::Request;
    auto* window = new juce::AlertWindow("Question", juceString(prompt.question), juce::MessageBoxIconType::QuestionIcon, this);
    if (typed)
        window->addTextEditor("answer", juceString(prompt.initialAnswer));
    const auto yesNo = prompt.kind == PromptKind::ConfirmYesNo;
    window->addButton(yesNo ? "Yes" : "OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton(yesNo ? "No" : "Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::Component::SafePointer<juce::TextEditor> safeEditor(&editor);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safeThis, safeEditor, window, typed, answers = std::move(answers), seeds = std::move(seeds)](int result) mutable {
                if (safeThis == nullptr || safeEditor == nullptr)
                    return;
                answers.push_back({result == 1, typed ? window->getTextEditorContents("answer").toStdString() : std::string{}});
                (void)safeThis->evaluateSelectionIn(*safeEditor, std::move(answers), std::move(seeds));
            }),
        true);
}

void WorkbenchComponent::showEvaluationGraphs(const std::vector<talas::project::smalltalk::XYGraph>& graphs)
{
    evaluationGraphs_ = graphs;
    if (!isShowing())
        return;
    for (const auto& graph : graphs)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(new XYGraphComponent(graph));
        options.dialogTitle = "XYGraphView";
        options.dialogBackgroundColour = panelColour;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
    }
}

void WorkbenchComponent::describeFrontSound()
{
    if (const auto typeId = prototypePanel_->focusedPrototype())
    {
        if (const auto* descriptor = registry_.findNodeType(*typeId))
        {
            describeSound(*descriptor);
            return;
        }
    }
    if (selectedNodeIds_.size() == 1)
    {
        describeGraphNode(activeGraphContext_, selectedNodeIds_.front());
        return;
    }
    setStatus("Select one Sound or a prototype to describe it");
}

// A Sound compiled may still warn of a Script
// variable assigned a value never used; with Notify immediately in
// a dialog to close, else in the status line once the Sound is ready.
void WorkbenchComponent::publishCompiledSound()
{
    publishCompileDiagnostics({});
    const auto warnings = talas::project::scriptCompilerWarnings(projectBridge_.editor().document());
    if (warnings.empty())
        return;
    const auto message = juceString(warnings.front().message);
    if (preferences_.notifyCompilerWarnings && isShowing())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Sound compiler", message, {}, this);
        return;
    }
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<WorkbenchComponent>(this), message] {
        if (safeThis != nullptr)
            safeThis->setStatus("Sound compiler warning: " + message);
    });
}

talas::runtime::ProjectStageResult WorkbenchComponent::compileShowingWatch(
    const talas::project::ProjectDocument* document, talas::runtime::RenderSettings settings)
{
    refreshActivityCursor(true);
    // DSP > Configure MIDI's Default Input Channel, unless
    // the stage names its own.
    if (settings.defaultMidiChannel == talas::runtime::RenderSettings{}.defaultMidiChannel)
        settings.defaultMidiChannel = static_cast<std::uint8_t>(preferences_.defaultMidiInputChannel);
    auto result = document != nullptr ? projectBridge_.compileAndStageDocument(*document, std::move(settings))
                                      : projectBridge_.compileAndStage(std::move(settings));
    refreshActivityCursor();
    return result;
}

// A watch while compiling, a hollow arrow
// while playing, partly filled or a floppy disk while the Sound uses the
// disk, the arrow after.
void WorkbenchComponent::refreshActivityCursor(bool compiling, bool loadingSamples)
{
    auto usingDisk = false;
    if (transportPlaying_)
    {
        const auto generation = projectBridge_.editor().documentAuthorityGeneration();
        if (!diskUseOfDocument_.has_value() || diskUseOfDocument_->first != generation)
            diskUseOfDocument_ = std::pair{generation, talas::app::loadedSoundInputs(projectBridge_.editor().document()).usesDisk};
        usingDisk = diskUseOfDocument_->second;
    }
    const auto cursor = talas::app::workbenchCursor(
        {false, compiling, transportPlaying_, usingDisk, preferences_.updateDisplayDuringDiskOperations, loadingSamples});
    if (cursor == shownActivityCursor_)
        return;
    shownActivityCursor_ = cursor;
    lookAndFeel_.setActivityCursor(workbenchActivityMouseCursor(cursor));
    juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
}
}
