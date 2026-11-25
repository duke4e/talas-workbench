#include "app/workbench/WorkbenchInspectorPanel.h"

#include "app/workbench/WorkbenchProjectLookup.h"
#include "app/workbench/WorkbenchProjectVariablesPanel.h"
#include "app/workbench/WorkbenchGeneratorPanel.h"
#include "app/workbench/WorkbenchParameterFieldEditor.h"
#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/DiskButton.h"
#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/ClassEditorModel.h"
#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/TalasFiles.h"
#include "talas/app/ExpressionsLibraryModel.h"
#include "talas/runtime/ConstantFields.h"
#include "talas/app/MissingNodeWorkbenchModel.h"
#include "talas/app/PresetEventValuesModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/VariableValuePrompt.h"
#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectParameterTransformer.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace talas::app::workbench
{
namespace
{
constexpr std::size_t maximumDefinitionParameterEditorBytes =
    talas::expr::ExpressionLimits::maximumDefinitionParameters
        * (talas::expr::ExpressionLimits::maximumDefinitionNameBytes
           + 1U + 10U)
    + (talas::expr::ExpressionLimits::maximumDefinitionParameters - 1U) * 2U;

// An input field's Sounds as icons, small or large, each with its name
// below, in their order.
class SoundFieldIcons final : public juce::Component
{
public:
    void setSounds(std::vector<std::string> names)
    {
        names_ = std::move(names);
        repaint();
    }

    void setLarge(bool large)
    {
        large_ = large;
        repaint();
    }

    [[nodiscard]] int preferredHeight(int width) const
    {
        const auto places = talas::app::soundFieldIconPlaces(names_.size(), width, large_);
        return places.empty() ? 0 : places.back().y + places.back().height;
    }

    // A Sound dragged out of the field is the Sound itself, as one dragged
    // from the signal flow (dropped with Control or Option on another Sound,
    // it takes that one's place, shared).
    std::function<juce::String(const std::string&)> dragDescription;

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragging_ || !dragDescription || event.getDistanceFromDragStart() < 4)
            return;
        const auto places = talas::app::soundFieldIconPlaces(names_.size(), getWidth(), large_);
        for (std::size_t index = 0; index < places.size(); ++index)
        {
            const auto& place = places[index];
            if (!juce::Rectangle<int>{place.x, place.y, place.width, place.height}.contains(event.getMouseDownPosition()))
                continue;
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                dragging_ = true;
                container->startDragging(dragDescription(names_[index]), this);
            }
            return;
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        dragging_ = false;
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto places = talas::app::soundFieldIconPlaces(names_.size(), getWidth(), large_);
        const auto side = static_cast<float>(talas::app::soundFieldIconSize(large_));
        graphics.setFont(WorkbenchType::micro());
        for (std::size_t index = 0; index < names_.size(); ++index)
        {
            const auto& place = places[index];
            const auto cell = juce::Rectangle<int>{place.x, place.y, place.width, place.height}.toFloat();
            const auto icon = juce::Rectangle<float>{side, side}.withCentre({cell.getCentreX(), cell.getY() + 1.0F + side / 2.0F});
            graphics.setColour(audioColour.withAlpha(0.25F));
            graphics.fillRoundedRectangle(icon, side / 5.0F);
            graphics.setColour(audioColour);
            graphics.drawRoundedRectangle(icon.reduced(0.5F), side / 5.0F, 1.0F);
            graphics.setColour(textColour);
            graphics.drawFittedText(juce::String::fromUTF8(names_[index].c_str()),
                cell.withTop(icon.getBottom() + 1.0F).toNearestInt(), juce::Justification::centredTop, 1);
        }
    }

private:
    std::vector<std::string> names_;
    bool large_ = false;
    bool dragging_ = false;
};

// The value field, where a double-click just inside punctuation selects
// what it encloses and one before or after the text all of it.
class ParameterValueEditor final : public juce::TextEditor
{
public:
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (event.getNumberOfClicks() != 2)
        {
            juce::TextEditor::mouseDoubleClick(event);
            return;
        }
        selectAsDoubleClickAt(getTextIndexAt(event.getPosition()));
    }

    // The name of the Sound copied in the graph, pasted into the field as
    // a control signal, or none for a text.
    std::function<std::optional<std::string>()> soundToPaste;
    // Once a Sound or a text is pasted: the field's value is taken.
    std::function<void()> onPasted;
    // The Sounds of the structure, whose names the field boxes.
    std::function<std::vector<std::string>()> soundNames;
    // Paste special into an input field: the Sound
    // copied shares the field, true when it did; a value field's takes a
    // text cut or copied before.
    std::function<bool()> pasteSpecialSound;
    talas::app::PasteHistory history;

    // A Sound used as a control signal shows as its name in a box.
    void paintOverChildren(juce::Graphics& graphics) override
    {
        juce::TextEditor::paintOverChildren(graphics);
        if (!soundNames)
            return;
        const auto names = soundNames();
        if (names.empty())
            return;
        const auto utf8 = getText().toStdString();
        const auto characters = [&utf8](std::size_t bytes) {
            return juce::String::fromUTF8(utf8.data(), static_cast<int>(bytes)).length();
        };
        graphics.setColour(findColour(juce::TextEditor::textColourId).withAlpha(0.6F));
        for (const auto& span : talas::app::fieldSoundSpans(utf8, names))
        {
            for (const auto& box : getTextBounds({characters(span.start), characters(span.end)}))
                graphics.drawRoundedRectangle(box.toFloat().expanded(1.5F, 0.5F), 3.0F, 1.0F);
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto command = key.getModifiers().isCommandDown();
        const auto code = key.getKeyCode();
        if (command && (code == 'c' || code == 'C' || code == 'x' || code == 'X'))
            history.remember(getHighlightedText().toStdString());
        if (command && (code == 'v' || code == 'V') && key.getModifiers().isShiftDown())
        {
            if (!pasteSpecialSound || !pasteSpecialSound())
                showPasteSpecial();
            return true;
        }
        // The right arrow jumps to the end of a
        // completion's guess, to type on after it.
        if (key == juce::KeyPress::rightKey && !getHighlightedRegion().isEmpty())
        {
            setCaretPosition(getHighlightedRegion().getEnd());
            return true;
        }
        if (command && (code == 'v' || code == 'V') && soundToPaste)
        {
            if (const auto sound = soundToPaste())
            {
                insertTextAtCaret(juce::String{talas::app::pastedSoundText(*sound)});
                if (onPasted)
                    onPasted();
                return true;
            }
        }
        return juce::TextEditor::keyPressed(key);
    }

    // Paste special...: the last texts cut or copied.
    void showPasteSpecial()
    {
        juce::PopupMenu menu;
        const auto& texts = history.texts();
        for (std::size_t index = 0; index < texts.size(); ++index)
            menu.addItem(static_cast<int>(index) + 1, juce::String{texts[index]}.substring(0, 60));
        if (texts.empty())
            menu.addItem(-1, "Nothing cut or copied yet", false);
        juce::Component::SafePointer<ParameterValueEditor> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this), [safeThis](int chosen) {
            if (safeThis != nullptr && chosen > 0)
                safeThis->pasteSpecial(static_cast<std::size_t>(chosen - 1));
        });
    }

    bool pasteSpecial(std::size_t index)
    {
        if (index >= history.texts().size())
            return false;
        insertTextAtCaret(juce::String{history.texts()[index]});
        return true;
    }

    // The selection a double-click at a character makes.
    void selectAsDoubleClickAt(int index)
    {
        const auto text = getText();
        const auto bytesBefore = [&](int characters) {
            return static_cast<std::size_t>(text.substring(0, characters).getNumBytesAsUTF8());
        };
        const auto utf8 = text.toStdString();
        const auto [start, end] = talas::app::doubleClickSelection(utf8, bytesBefore(index));
        const auto charactersIn = [&](std::size_t bytes) {
            return juce::String::fromUTF8(utf8.data(), static_cast<int>(bytes)).length();
        };
        setHighlightedRegion({charactersIn(start), charactersIn(end)});
    }
};

class InspectorPanelImpl final : public InspectorPanel,
                                 public juce::DragAndDropTarget
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using BrowseResourceCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        std::string)>;
    using PreservedNodeActionCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        PreservedNodeAction)>;
    using ExpressionLanguageUpgradeCallback = std::function<void()>;

    InspectorPanelImpl(talas::runtime::ProjectRuntimeBridge& bridge,
                   const talas::core::NodeRegistry& registry,
                   DocumentChangeCallback onDocumentChange,
                   ProjectVariablesPanel::LiveMutationCallback
                       onVariableLiveMutation,
                   StatusCallback onStatus,
                   BrowseResourceCallback onBrowseResource,
                   PreservedNodeActionCallback onPreservedNodeAction,
                   ExpressionLanguageUpgradeCallback
                       onExpressionLanguageUpgrade,
                   ExpressionLanguageUpgradeCallback
                       onTimedExpressionLanguageUpgrade,
                   DiskCacheCaptureProjectionCallback
                       diskCacheCaptureProjection,
                   DiskCacheCaptureActionCallback
                       onDiskCacheCaptureAction)
        : bridge_(bridge)
        , registry_(registry)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , onBrowseResource_(std::move(onBrowseResource))
        , onPreservedNodeAction_(
              std::move(onPreservedNodeAction))
        , onExpressionLanguageUpgrade_(
              std::move(onExpressionLanguageUpgrade))
        , onTimedExpressionLanguageUpgrade_(
              std::move(onTimedExpressionLanguageUpgrade))
        , diskCacheCaptureProjection_(
              std::move(diskCacheCaptureProjection))
        , onDiskCacheCaptureAction_(
              std::move(onDiskCacheCaptureAction))
        , nodeModeButton_("Node")
        , variablesModeButton_("Variables")
        , generatorsModeButton_("Generators")
        , upgradeExpressionsButton_("Upgrade to Session v1")
        , upgradeTimedExpressionsButton_("Upgrade to Variables v3")
        , newDefinitionButton_("New")
        , applyDefinitionButton_("Save")
        , deleteDefinitionButton_("Delete")
        , applyButton_("Apply")
        , browseButton_("Browse...")
        , retryButton_("Retry")
        , replacePreservedButton_("Replace...")
        , disconnectButton_("Disconnect")
        , deletePreservedButton_("Delete")
        , componentReplacementPanel_(makeComponentReplacementPanel({
              [this] {
                  invokePreservedAction(PreservedNodeAction::Replace);
              },
              [this] {
                  invokePreservedAction(
                      PreservedNodeAction::ConfirmReplacement);
              },
              [this] {
                  invokePreservedAction(
                      PreservedNodeAction::CancelReplacement);
              },
          }))
        , variablesPanel_(makeProjectVariablesPanel(
              bridge_,
              [this] { onDocumentChange_(); },
              std::move(onVariableLiveMutation),
              [this](const juce::String& status) { onStatus_(status); }))
        , generatorPanel_(makeGeneratorPanel(
              bridge_,
              registry_,
              [this] { onDocumentChange_(); },
              [this](const juce::String& status) { onStatus_(status); }))
        , diskCacheCaptureButton_("Capture Disk Cache Loop")
        , presetCaptureButton_("Set to Current Event Values")
        , fieldEditor_(std::make_unique<ParameterFieldEditor>(
              [this](std::string text) { commitFieldEditorText(std::move(text)); }))
        , splicerField_(std::make_unique<DiskSplicerField>(
              [this](std::string text) { commitFieldEditorText(std::move(text)); }))
    {
        describeControl(
            *this,
            "Inspector",
            "Inspect and edit parameters for the selected graph node.");
        setFocusContainerType(
            juce::Component::FocusContainerType::none);
        addAndMakeVisible(nodeLabel_);
        addAndMakeVisible(nodeModeButton_);
        addAndMakeVisible(variablesModeButton_);
        addAndMakeVisible(generatorsModeButton_);
        addAndMakeVisible(expressionLanguageLabel_);
        addAndMakeVisible(upgradeExpressionsButton_);
        addAndMakeVisible(upgradeTimedExpressionsButton_);
        addAndMakeVisible(definitionTitle_);
        addAndMakeVisible(definitionBox_);
        addAndMakeVisible(newDefinitionButton_);
        addAndMakeVisible(definitionName_);
        addAndMakeVisible(definitionParameters_);
        addAndMakeVisible(definitionResultDomain_);
        addAndMakeVisible(definitionSource_);
        addAndMakeVisible(applyDefinitionButton_);
        addAndMakeVisible(deleteDefinitionButton_);
        addAndMakeVisible(structureFocusLabel_);
        addAndMakeVisible(parameterBox_);
        addAndMakeVisible(parameterHint_);
        addAndMakeVisible(textValue_);
        addChildComponent(soundIcons_);
        addAndMakeVisible(choiceValue_);
        addAndMakeVisible(booleanValue_);
        addAndMakeVisible(applyButton_);
        addAndMakeVisible(browseButton_);
        addChildComponent(fromMemoryWriterBox_);
        addAndMakeVisible(preservedStatus_);
        addAndMakeVisible(preservedDetails_);
        addAndMakeVisible(retryButton_);
        addAndMakeVisible(replacePreservedButton_);
        addAndMakeVisible(disconnectButton_);
        addAndMakeVisible(deletePreservedButton_);
        addChildComponent(*componentReplacementPanel_);
        addChildComponent(*variablesPanel_);
        addChildComponent(*generatorPanel_);
        addAndMakeVisible(preparedTitle_);
        addAndMakeVisible(preparedContract_);
        addChildComponent(diskCacheCaptureButton_);
        addChildComponent(presetCaptureButton_);
        addChildComponent(*fieldEditor_);
        addChildComponent(*splicerField_);

        nodeLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
        nodeLabel_.addMouseListener(this, false);
        className_.setColour(juce::Label::textColourId, secondaryTextColour);
        className_.setFont(WorkbenchType::label());
        className_.setJustificationType(juce::Justification::centredRight);
        className_.addMouseListener(this, false);
        addChildComponent(className_);
        nodeLabel_.setFont(WorkbenchType::body());
        nodeLabel_.setJustificationType(juce::Justification::topLeft);
        describeControl(
            nodeLabel_,
            "Selected node",
            "Display name and stable identifier of the selected graph node.");
        nodeModeButton_.setComponentID("inspector.mode.node");
        variablesModeButton_.setComponentID("inspector.mode.variables");
        generatorsModeButton_.setComponentID("inspector.mode.generators");
        describeControl(
            nodeModeButton_,
            "Node inspector",
            "Show selected-node parameters, expression definitions, and the prepared contract.");
        describeControl(
            variablesModeButton_,
            "Variables inspector",
            "Show bounded project and exact Sound-instance variable declarations and live values.");
        describeControl(
            generatorsModeButton_,
            "Generators inspector",
            "Author bounded deterministic symbol generators, preview symbols, and apply exact Timeline or Score output as one Undo step.");
        nodeModeButton_.setClickingTogglesState(true);
        variablesModeButton_.setClickingTogglesState(true);
        nodeModeButton_.setRadioGroupId(4101);
        variablesModeButton_.setRadioGroupId(4101);
        generatorsModeButton_.setClickingTogglesState(true);
        generatorsModeButton_.setRadioGroupId(4101);
        expressionLanguageLabel_.setComponentID(
            "inspector.expression-language");
        expressionLanguageLabel_.setWantsKeyboardFocus(true);
        expressionLanguageLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        expressionLanguageLabel_.setFont(WorkbenchType::microStrong());
        describeControl(
            expressionLanguageLabel_,
            "Project expression language",
            "Exact project-wide parser and evaluator authority for every expression.");
        upgradeExpressionsButton_.setComponentID(
            "inspector.upgrade-expressions");
        describeControl(
            upgradeExpressionsButton_,
            "Upgrade expressions to Session state v1",
            "Dry-runs every persisted expression and effective descriptor default. Commits only when all sources are compatible, as one Undo step; runtime state remains session-only.");
        upgradeTimedExpressionsButton_.setComponentID(
            "inspector.upgrade-timed-expressions");
        describeControl(
            upgradeTimedExpressionsButton_,
            "Upgrade expressions to Variables v3",
            "Enables millisecond-clock ramp: expressions after a compatibility audit. One Undo restores Variables v2.");
        definitionTitle_.setText(
            "Project expression definitions",
            juce::dontSendNotification);
        definitionTitle_.setFont(WorkbenchType::microStrong());
        definitionTitle_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        definitionTitle_.setInterceptsMouseClicks(false, false);
        definitionBox_.setComponentID("inspector.expression-definitions");
        describeControl(
            definitionBox_,
            "Expression definition",
            "Select one of at most 32 project-owned Pure v4 or Session v1 definitions to inspect or edit.");
        newDefinitionButton_.setComponentID(
            "inspector.expression-definition.new");
        describeControl(
            newDefinitionButton_,
            "Create expression definition",
            "Clears the bounded editor for one new project-owned pure definition.");
        definitionName_.setComponentID(
            "inspector.expression-definition:new:name");
        definitionName_.setTextToShowWhenEmpty(
            "name",
            secondaryTextColour);
        definitionName_.setInputRestrictions(
            static_cast<int>(talas::expr::ExpressionLimits::maximumDefinitionNameBytes));
        describeControl(
            definitionName_,
            "Expression definition name",
            "ASCII identifier. Existing definitions cannot be renamed; create a new name, update callers, then delete the old definition.");
        definitionParameters_.setComponentID(
            "inspector.expression-definition:new:parameters");
        definitionParameters_.setTextToShowWhenEmpty(
            "parameters, e.g. value:scalar, amount:scalar",
            secondaryTextColour);
        definitionParameters_.setInputRestrictions(
            static_cast<int>(maximumDefinitionParameterEditorBytes));
        describeControl(
            definitionParameters_,
            "Expression definition parameters",
            "At most four comma-separated name:domain pairs. Domains are scalar, hertz, seconds, noteNumber, or samples.");
        definitionResultDomain_.setComponentID(
            "inspector.expression-definition:new:result-domain");
        definitionResultDomain_.addItem("scalar", 1);
        definitionResultDomain_.addItem("hertz", 2);
        definitionResultDomain_.addItem("seconds", 3);
        definitionResultDomain_.addItem("noteNumber", 4);
        definitionResultDomain_.addItem("samples", 5);
        definitionResultDomain_.setSelectedId(1, juce::dontSendNotification);
        describeControl(
            definitionResultDomain_,
            "Expression definition result domain",
            "Declared result domain checked against the compiled definition body.");
        definitionSource_.setComponentID(
            "inspector.expression-definition:new:source");
        definitionSource_.setMultiLine(true);
        definitionSource_.setReturnKeyStartsNewLine(true);
        definitionSource_.setScrollbarsShown(true);
        definitionSource_.setTextToShowWhenEmpty(
            "pure expression body",
            secondaryTextColour);
        definitionSource_.setInputRestrictions(
            static_cast<int>(talas::expr::ExpressionLimits::maximumDefinitionSourceBytes));
        describeControl(
            definitionSource_,
            "Expression definition source",
            "Pure v4 or Session v1 body: at most 4096 bytes, 32768 aggregate bytes, eight expansion levels, and 32 fixed locals. Definitions perform no I/O and compile before staging; Session v1 state belongs to each expanded call site.");
        applyDefinitionButton_.setComponentID(
            "inspector.expression-definition.apply");
        describeControl(
            applyDefinitionButton_,
            "Save expression definition",
            "Validates the complete project and commits one failure-atomic Undo step.");
        deleteDefinitionButton_.setComponentID(
            "inspector.expression-definition.delete");
        describeControl(
            deleteDefinitionButton_,
            "Delete expression definition",
            "Deletes the selected definition only when no project expression or other definition depends on it.");
        structureFocusLabel_.setColour(juce::Label::textColourId,
            workbenchTheme.fullSupport);
        structureFocusLabel_.setFont(WorkbenchType::labelStrong());
        structureFocusLabel_.setWantsKeyboardFocus(true);
        structureFocusLabel_.setVisible(false);
        describeControl(parameterBox_,
            "Parameter",
            "Parameter to inspect and edit on the selected node.");
        parameterHint_.setMultiLine(true);
        parameterHint_.setReadOnly(true);
        parameterHint_.setScrollbarsShown(true);
        parameterHint_.setCaretVisible(false);
        parameterHint_.setFont(WorkbenchType::micro());
        parameterHint_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        parameterHint_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        parameterHint_.setColour(
            juce::TextEditor::textColourId,
            secondaryTextColour);
        describeControl(
            parameterHint_,
            "Selected parameter Info Hint",
            "Descriptor-derived parameter kind, domain or unit, "
            "default, choices, and selected-node capability summary.");
        textValue_.setComponentID("inspector.parameter.value");
        describeControl(textValue_,
            "Parameter value",
            "Expression, resource, or text value for the selected parameter.");
        describeControl(
            choiceValue_,
            "Parameter choice",
            "Choice value for the selected parameter.");
        describeControl(
            booleanValue_,
            "Parameter toggle",
            "Boolean value for the selected parameter.");
        describeControl(
            applyButton_,
            "Apply parameter value",
            "Commits the edited parameter value to the selected node.");
        describeControl(
            browseButton_,
            "Browse for resource",
            "Chooses a project resource file for the selected parameter; held down with Command or Control, opens an editor on the file, and with Shift, a window of information on it.");
        preservedStatus_.setColour(
            juce::Label::textColourId,
            errorColour);
        preservedStatus_.setFont(WorkbenchType::labelStrong());
        describeControl(
            preservedStatus_,
            "Preserved component availability",
            "Text status for an unavailable, incompatible, or exactly "
            "resolved preserved component.");
        preservedDetails_.setMultiLine(true);
        preservedDetails_.setReadOnly(true);
        preservedDetails_.setScrollbarsShown(true);
        preservedDetails_.setFont(WorkbenchType::label());
        preservedDetails_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        preservedDetails_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        preservedDetails_.setColour(
            juce::TextEditor::textColourId,
            secondaryTextColour);
        describeControl(
            preservedDetails_,
            "Preserved component details",
            "Copyable original identity, state size, recovery hint, "
            "frozen ports, and exact compatibility diagnostic.");
        describeControl(retryButton_,
            "Retry exact component repair",
            "Checks the current trusted registry again without loading "
            "or discovering code.");
        describeControl(replacePreservedButton_,
            "Replace preserved component",
            "Inspect one explicitly selected local component bundle for this "
            "root "
            "preserved node. No sibling directories are scanned.");
        describeControl(disconnectButton_,
            "Disconnect preserved node",
            "Removes every existing connection to and from this node "
            "as one undoable edit.");
        describeControl(deletePreservedButton_,
            "Delete preserved node",
            "Requests one confirmation before deleting the node and "
            "its existing connections.");
        preparedTitle_.setText("Prepared contract", juce::dontSendNotification);
        preparedTitle_.setColour(juce::Label::textColourId, textColour);
        preparedTitle_.setFont(WorkbenchType::labelStrong());
        preparedTitle_.setInterceptsMouseClicks(false, false);
        describeControl(
            preparedTitle_,
            "Prepared contract",
            "Exact prepared output, latency, spectral, resource, and "
            "stream details for the selected node.");
        preparedContract_.setMultiLine(true);
        preparedContract_.setReadOnly(true);
        preparedContract_.setScrollbarsShown(true);
        preparedContract_.setCaretVisible(false);
        preparedContract_.setFont(WorkbenchType::label());
        preparedContract_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        preparedContract_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        preparedContract_.setColour(
            juce::TextEditor::textColourId,
            secondaryTextColour);
        describeControl(
            preparedContract_,
            "Selected node prepared contract",
            "Read-only prepared graph details. Long paths and "
            "limitations scroll inside this region.");
        expressionLanguageLabel_.setExplicitFocusOrder(1);
        upgradeExpressionsButton_.setExplicitFocusOrder(2);
        upgradeTimedExpressionsButton_.setExplicitFocusOrder(3);
        definitionBox_.setExplicitFocusOrder(4);
        newDefinitionButton_.setExplicitFocusOrder(5);
        definitionName_.setExplicitFocusOrder(6);
        definitionParameters_.setExplicitFocusOrder(7);
        definitionResultDomain_.setExplicitFocusOrder(8);
        definitionSource_.setExplicitFocusOrder(9);
        applyDefinitionButton_.setExplicitFocusOrder(10);
        deleteDefinitionButton_.setExplicitFocusOrder(11);
        parameterBox_.setExplicitFocusOrder(12);
        textValue_.setExplicitFocusOrder(13);
        choiceValue_.setExplicitFocusOrder(13);
        booleanValue_.setExplicitFocusOrder(13);
        browseButton_.setExplicitFocusOrder(14);
        applyButton_.setExplicitFocusOrder(15);
        preservedDetails_.setExplicitFocusOrder(1);
        retryButton_.setExplicitFocusOrder(2);
        replacePreservedButton_.setExplicitFocusOrder(3);
        disconnectButton_.setExplicitFocusOrder(4);
        deletePreservedButton_.setExplicitFocusOrder(5);
        preparedContract_.setExplicitFocusOrder(6);
        diskCacheCaptureButton_.setComponentID(
            "inspector.disk-cache.capture");
        diskCacheCaptureButton_.setExplicitFocusOrder(15);
        diskCacheCaptureButton_.onClick = [this] {
            if (onDiskCacheCaptureAction_)
                onDiskCacheCaptureAction_(
                    soundDefinitionContext_, selectedNodeId_);
        };
        presetCaptureButton_.setComponentID("inspector.preset.capture");
        presetCaptureButton_.setExplicitFocusOrder(15);
        describeControl(
            presetCaptureButton_,
            "Set to Current Event Values",
            "Writes the Virtual Control Surface's current values of the "
            "Input's Event Values into the Preset's EventValues");
        presetCaptureButton_.onClick = [this] { capturePresetEventValues(); };
        textValue_.setFont(WorkbenchType::body());
        parameterBox_.onChange = [this] { refreshParameterValue(); };
        textValue_.onReturnKey = [this] {
            if (!textValue_.isMultiLine())
                commitParameter();
        };
        textValue_.onPasted = [this] {
            if (!textValue_.isMultiLine())
                commitParameter();
        };
        soundIcons_.dragDescription = [this](const std::string& nodeId) {
            return juceString(talas::app::signalFlowSoundDragDescription(soundDefinitionContext_, nodeId));
        };
        textValue_.soundNames = [this] {
            std::vector<std::string> names;
            const auto& document = bridge_.editor().document();
            const auto* graph = &document.graph;
            if (soundDefinitionContext_.has_value())
            {
                const auto definition = std::ranges::find(
                    document.soundDefinitions, *soundDefinitionContext_, &talas::project::ProjectSoundDefinition::definitionId);
                if (definition == document.soundDefinitions.end())
                    return names;
                graph = &definition->graph;
            }
            for (const auto& node : graph->nodes)
                names.push_back(node.instanceId);
            return names;
        };
        // A hot value typed into the field
        // is completed, the rest selected, so typing goes on over it and the
        // right arrow takes it.
        typedKeys_.owner = this;
        textValue_.addKeyListener(&typedKeys_);
        textValue_.onTextChange = [this] {
            valueEdited_ = textValue_.getText() != shownValueText_;
            completeHotValue();
        };
        tabKeys_.owner = this;
        for (auto* control : std::initializer_list<juce::Component*>{&textValue_, &choiceValue_, &booleanValue_})
            control->addKeyListener(&tabKeys_);
        choiceValue_.onChange = [this] { commitParameter(); };
        booleanValue_.onClick = [this] { commitParameter(); };
        applyButton_.onClick = [this] { commitParameter(); };
        fromMemoryWriterBox_.onClick = [this] { commitFromMemoryWriter(); };
        browseButton_.onClick = [this] {
            const auto* descriptor = selectedParameterDescriptor();
            if (descriptor != nullptr && descriptor->kind == talas::core::ParameterKind::Resource)
                onBrowseResource_(
                    soundDefinitionContext_,
                    selectedNodeId_,
                    descriptor->id);
        };
        retryButton_.onClick = [this] {
            invokePreservedAction(PreservedNodeAction::Retry);
        };
        replacePreservedButton_.onClick = [this] {
            invokePreservedAction(PreservedNodeAction::Replace);
        };
        disconnectButton_.onClick = [this] {
            invokePreservedAction(PreservedNodeAction::Disconnect);
        };
        deletePreservedButton_.onClick = [this] {
            invokePreservedAction(PreservedNodeAction::Delete);
        };
        upgradeExpressionsButton_.onClick = [this] {
            if (onExpressionLanguageUpgrade_)
                onExpressionLanguageUpgrade_();
        };
        upgradeTimedExpressionsButton_.onClick = [this] {
            if (onTimedExpressionLanguageUpgrade_)
                onTimedExpressionLanguageUpgrade_();
        };
        definitionBox_.onChange = [this] {
            const auto index = definitionBox_.getSelectedItemIndex();
            const auto& definitions =
                bridge_.editor().document().expressionDefinitions.definitions;
            if (index < 0
                || static_cast<std::size_t>(index) >= definitions.size())
            {
                beginNewDefinition();
                return;
            }
            loadDefinition(definitions[static_cast<std::size_t>(index)]);
        };
        newDefinitionButton_.onClick = [this] {
            beginNewDefinition();
            definitionName_.grabKeyboardFocus();
        };
        applyDefinitionButton_.onClick = [this] { commitDefinition(); };
        deleteDefinitionButton_.onClick = [this] { deleteDefinition(); };
        nodeModeButton_.onClick = [this] { setMode(false, false); };
        variablesModeButton_.onClick = [this] { setMode(true, false); };
        generatorsModeButton_.onClick = [this] { setMode(false, true); };
        refresh();
        setPreparedContract(
            "No prepared graph. Press Play to compile and stage "
            "current project state.");
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    void setSelectedNodeId(std::string nodeId) override
    {
        if (selectedNodeId_ != nodeId)
        {
            focusedParameterId_.reset();
            structureFocusLabel_.setVisible(false);
        }
        selectedNodeId_ = std::move(nodeId);
        refresh();
    }

    void setSoundDefinitionContext(
        std::optional<std::string> definitionId) override
    {
        if (soundDefinitionContext_ != definitionId)
        {
            focusedParameterId_.reset();
            structureFocusLabel_.setVisible(false);
        }
        soundDefinitionContext_ = std::move(definitionId);
        refresh();
    }

    void focusParameter(std::string parameterId) override
    {
        focusedParameterId_ = std::move(parameterId);
        if (focusedParameterId_->empty())
        {
            focusedParameterId_.reset();
            structureFocusLabel_.setVisible(false);
            resized();
            return;
        }
        structureFocusLabel_.setText("Focused parameter / " +
                                         juceString(*focusedParameterId_),
            juce::dontSendNotification);
        describeControl(structureFocusLabel_,
            "Focused Structure parameter " + juceString(*focusedParameterId_),
            "Exact parameter selected from a Performance control "
            "without changing project topology.");
        structureFocusLabel_.setVisible(true);
        const auto parameterIndex = [&]
        {
            for (int index = 0; index < parameterBox_.getNumItems(); ++index)
            {
                if (parameterBox_.getItemText(index) ==
                    juceString(*focusedParameterId_))
                {
                    return index;
                }
            }
            return -1;
        }();
        if (parameterIndex >= 0)
        {
            parameterBox_.setSelectedItemIndex(parameterIndex,
                juce::sendNotificationSync);
            if (parameterBox_.isShowing())
                parameterBox_.grabKeyboardFocus();
        }
        else if (structureFocusLabel_.isShowing())
        {
            structureFocusLabel_.grabKeyboardFocus();
        }
        resized();
    }

    juce::String inputFieldTextForSmoke() override
    {
        if (inputPorts_.empty())
            return {};
        parameterBox_.setSelectedId(inputFieldItemBase, juce::sendNotificationSync);
        return textValue_.getText();
    }

    bool applyInputFieldForSmoke(const juce::String& text) override
    {
        const auto port = selectedInputPort();
        if (!port.has_value())
            return false;
        textValue_.setText(text, juce::dontSendNotification);
        commitParameter();
        return true;
    }

    [[nodiscard]] bool focusedParameterForSmoke(
        std::string_view parameterId) const override
    {
        return focusedParameterId_ == std::optional<std::string>{parameterId} &&
               structureFocusLabel_.isVisible() &&
               structureFocusLabel_.getTitle().contains(
                   juceString(parameterId));
    }

    void setPreparedContract(juce::String contract) override
    {
        if (preparedContract_.getText() == contract)
            return;
        preparedContract_.setText(contract, false);
        preparedContract_.setCaretPosition(0);
        preparedContract_.setTitle(
            "Selected node prepared contract: "
            + contract.replaceCharacters("\n\r", "  "));
    }

    void setComponentReplacementEnabled(bool enabled) override
    {
        componentReplacementEnabled_ = enabled;
        updatePreservedControls();
    }

    void setComponentReplacementPresentation(
        const ComponentReplacementPresentation& presentation) override
    {
        componentReplacementPresentation_ = presentation;
        componentReplacementPanel_->setPresentation(presentation);
        refresh();
    }

    [[nodiscard]] bool preparedContractContainsForSmoke(
        const juce::String& text) const override
    {
        return preparedContract_.getText().contains(text)
            && preparedContract_.isReadOnly()
            && preparedContract_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool showsPreservedStateForSmoke(
        const juce::String& status,
        bool parameterEditingExpected) const override
    {
        return preservedProjection_.has_value()
            && preservedStatus_.isVisible()
            && preservedStatus_.getText().contains(status)
            && preservedDetails_.isVisible()
            && preservedDetails_.isReadOnly()
            && preservedDetails_.getTitle().isNotEmpty()
            && preservedProjection_->parameterEditingEnabled
                == parameterEditingExpected
            && parameterBox_.isVisible()
                == parameterEditingExpected;
    }

    [[nodiscard]] bool preservedActionsAvailableForSmoke()
        const override
    {
        return preservedProjection_.has_value()
            && deletePreservedButton_.isVisible()
            && deletePreservedButton_.isEnabled()
            && retryButton_.isVisible()
            && retryButton_.isEnabled()
            && disconnectButton_.isVisible()
            && disconnectButton_.isEnabled();
    }

    [[nodiscard]] bool componentReplacementActionAvailableForSmoke()
        const override
    {
        return componentReplacementEnabled_
            && !soundDefinitionContext_.has_value()
            && preservedProjection_.has_value()
            && replacePreservedButton_.isVisible()
            && replacePreservedButton_.isEnabled();
    }

    [[nodiscard]] bool componentReplacementSurfaceReadyForSmoke(
        const juce::String& expectedText,
        bool confirmationExpected) const override
    {
        return componentReplacementPanel_->showsAccessiblePreviewForSmoke(
            expectedText,
            confirmationExpected);
    }

    [[nodiscard]] juce::String radioChoicesForSmoke() const override
    {
        juce::StringArray shown;
        for (std::size_t index = 0; index < radioChoices_.size(); ++index)
        {
            const auto& button = *radioChoices_[index];
            if (!button.isVisible() || button.getBounds().isEmpty())
                continue;
            const auto place = talas::app::radioButtonPlaces(radioChoices_.size(), radioColumns_)[index];
            shown.add(button.getButtonText() + "@" + juce::String{place.column} + "," + juce::String{place.row}
                + (button.getToggleState() ? "*" : ""));
        }
        return shown.joinIntoString(" ");
    }

    [[nodiscard]] bool clickRadioChoiceForSmoke(const juce::String& choice) override
    {
        for (const auto& button : radioChoices_)
        {
            if (button->isVisible() && button->getButtonText() == choice)
            {
                // As a click does: the button chosen, then the choice kept.
                button->setToggleState(true, juce::dontSendNotification);
                button->onClick();
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool parameterHintContainsForSmoke(
        const juce::String& expected) const override
    {
        return parameterHint_.isVisible()
            && parameterHint_.isReadOnly()
            && !parameterHint_.getBounds().isEmpty()
            && parameterHint_.getText().contains(expected)
            && parameterHint_.getTitle().contains(expected);
    }

    [[nodiscard]] bool expressionLanguageReadyForSmoke(
        const juce::String& expected,
        bool upgradeVisible,
        bool upgradeEnabled,
        const juce::String& expectedUpgradeDetail) const override
    {
        return expressionLanguageLabel_.getText().contains(expected)
            && expressionLanguageLabel_.getTitle().contains(expected)
            && upgradeExpressionsButton_.isVisible() == upgradeVisible
            && upgradeExpressionsButton_.isEnabled() == upgradeEnabled
            && (expectedUpgradeDetail.isEmpty()
                || upgradeExpressionsButton_.getDescription().contains(
                    expectedUpgradeDetail));
    }

    [[nodiscard]] bool timedExpressionUpgradeReadyForSmoke(
        bool visible, bool enabled) const override
    {
        return upgradeTimedExpressionsButton_.isVisible() == visible
            && upgradeTimedExpressionsButton_.isEnabled() == enabled
            && (!visible
                || (!upgradeTimedExpressionsButton_.getBounds().isEmpty()
                    && upgradeTimedExpressionsButton_.getTitle().contains(
                        "Variables v3")));
    }

    void activateExpressionUpgradeForSmoke() override
    {
        if (upgradeExpressionsButton_.isVisible()
            && upgradeExpressionsButton_.isEnabled()
            && upgradeExpressionsButton_.onClick)
        {
            upgradeExpressionsButton_.onClick();
        }
    }

    void commitExpressionForSmoke(
        std::string nodeId,
        std::string parameterId,
        std::string source) override
    {
        setSoundDefinitionContext(std::nullopt);
        setSelectedNodeId(std::move(nodeId));
        const auto wanted = juceString(parameterId);
        for (int index = 0; index < parameterBox_.getNumItems(); ++index)
        {
            if (parameterBox_.getItemText(index) == wanted)
            {
                parameterBox_.setSelectedItemIndex(
                    index,
                    juce::sendNotificationSync);
                break;
            }
        }
        textValue_.setText(juceString(source), false);
        commitParameter();
    }

    void commitExpressionDefinitionForSmoke(
        std::string name,
        std::string parameters,
        std::string source) override
    {
        beginNewDefinition();
        definitionName_.setText(juceString(name), false);
        definitionParameters_.setText(juceString(parameters), false);
        definitionResultDomain_.setSelectedId(1, juce::dontSendNotification);
        definitionSource_.setText(juceString(source), false);
        commitDefinition();
    }

    [[nodiscard]] bool expressionDefinitionReadyForSmoke(
        std::string_view name,
        std::string_view source,
        bool deleteEnabled) const override
    {
        return selectedDefinitionName_
                == std::optional<std::string>{name}
            && definitionName_.getText() == juceString(name)
            && definitionSource_.getText() == juceString(source)
            && definitionName_.getComponentID().contains(juceString(name))
            && definitionSource_.getComponentID().contains(juceString(name))
            && deleteDefinitionButton_.isEnabled() == deleteEnabled;
    }

    void deleteExpressionDefinitionForSmoke() override
    {
        deleteDefinition();
    }

    [[nodiscard]] juce::Component*
    expressionLanguageComponentForSmoke() noexcept override
    {
        return &expressionLanguageLabel_;
    }

    [[nodiscard]] juce::Component*
    expressionValueComponentForSmoke() noexcept override
    {
        return &textValue_;
    }

    void focusExpressionLanguage() override
    {
        setVariablesMode(false);
        if (expressionLanguageLabel_.isShowing())
            expressionLanguageLabel_.grabKeyboardFocus();
    }

    void showNode() override
    {
        setMode(false, false);
    }

    void showVariables() override
    {
        setVariablesMode(true);
        variablesPanel_->focusVariables();
    }

    void showGenerators() override
    {
        setMode(false, true);
        generatorPanel_->focusGenerators();
    }

    void createGeneratorForSmoke(
        talas::project::ProjectGeneratorProgram program) override
    {
        setMode(false, true);
        generatorPanel_->createGeneratorForSmoke(std::move(program));
    }

    bool selectGeneratorForSmoke(
        std::string_view generatorId) override
    {
        setMode(false, true);
        return generatorPanel_->selectGeneratorForSmoke(generatorId);
    }

    bool generateGeneratorForSmoke() override
    {
        return generatorPanel_->generateForSmoke();
    }

    bool generateGeneratorTimelineForSmoke() override
    {
        return generatorPanel_->generateTimelineForSmoke();
    }

    bool generateGeneratorScoreForSmoke() override
    {
        return generatorPanel_->generateScoreForSmoke();
    }

    bool generateGeneratorSoundForSmoke() override
    {
        return generatorPanel_->generateSoundForSmoke();
    }

    bool generatorProjectionFinishedForSmoke() override
    {
        return generatorPanel_->projectionFinishedForSmoke();
    }

    bool configureGeneratorProjectionForSmoke(
        std::uint64_t timelineBaseMicroseconds,
        std::uint64_t scoreBaseTick,
        std::string_view outputNamespace) override
    {
        return generatorPanel_->configureProjectionForSmoke(
            timelineBaseMicroseconds,
            scoreBaseTick,
            outputNamespace);
    }

    bool configureGeneratorSoundForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view outputNamespace) override
    {
        return generatorPanel_->configureSoundProjectionForSmoke(
            definitionId,
            displayName,
            outputNamespace);
    }

    bool generatorProjectionActionsEnabledForSmoke(
        bool timelineExpected,
        bool scoreExpected,
        bool soundExpected) override
    {
        return generatorPanel_->projectionActionsEnabledForSmoke(
            timelineExpected,
            scoreExpected,
            soundExpected);
    }

    bool requestGeneratorDeleteForSmoke() override
    {
        return generatorPanel_->requestDeleteForSmoke();
    }

    bool acceptGeneratorDeleteForSmoke() override
    {
        return generatorPanel_->acceptDeleteForSmoke();
    }

    bool generatorSurfaceReadyForSmoke(
        std::string_view generatorId,
        std::string_view expectedPreview) const override
    {
        return generatorsMode_
            && generatorPanel_->surfaceReadyForSmoke(
                generatorId, expectedPreview);
    }

    bool generatorSoundSurfaceReadyForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view expectedPreview) override
    {
        return generatorsMode_
            && generatorPanel_->soundSurfaceReadyForSmoke(
                definitionId,
                displayName,
                expectedPreview);
    }

    bool generatorGraphNodePickerReadyForSmoke() override
    {
        return generatorsMode_
            && generatorPanel_->graphNodePickerReadyForSmoke();
    }

    void refreshVariableValues() override
    {
        variablesPanel_->refreshLiveValues();
    }

    [[nodiscard]] bool variableSurfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view expectedStatus) const override
    {
        return variablesMode_
            && variablesPanel_->surfaceReadyForSmoke(
                variableId, expectedStatus);
    }

    [[nodiscard]] bool selectVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) override
    {
        setVariablesMode(true);
        return variablesPanel_->selectVariableForSmoke(owner, variableId);
    }

    [[nodiscard]] bool selectPreparedVariableForSmoke(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t voiceGeneration) override
    {
        return variablesPanel_->selectPreparedValueForSmoke(
            identity, voiceGeneration);
    }

    [[nodiscard]] bool preparedVariableReadyForSmoke(
        std::string_view expectedInstance,
        std::string_view expectedStatus,
        bool assignmentEnabled,
        bool captureEnabled) const override
    {
        return variablesPanel_->preparedValueReadyForSmoke(
            expectedInstance,
            expectedStatus,
            assignmentEnabled,
            captureEnabled);
    }

    [[nodiscard]] std::string preparedVariableDescriptionForSmoke()
        const override
    {
        return variablesPanel_->preparedValueDescriptionForSmoke();
    }

    void createVariableForSmoke(
        talas::project::ProjectVariableDeclaration declaration) override
    {
        setVariablesMode(true);
        variablesPanel_->createVariableForSmoke(std::move(declaration));
    }

    [[nodiscard]] bool assignSelectedVariableForSmoke(
        double value) override
    {
        return variablesPanel_->assignSelectedForSmoke(value);
    }

    [[nodiscard]] bool resetSelectedVariableForSmoke() override
    {
        return variablesPanel_->resetSelectedForSmoke();
    }

    [[nodiscard]] bool captureSelectedVariableForSmoke() override
    {
        return variablesPanel_->captureSelectedForSmoke();
    }

    [[nodiscard]] bool variableNumbersAreStrictForSmoke() const override
    {
        return variablesPanel_->rejectsMalformedNumbersForSmoke();
    }

    void showSoundState() override
    {
        setVariablesMode(true);
        variablesPanel_->showSoundState();
    }

    bool selectSoundVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) override
    {
        setVariablesMode(true);
        return variablesPanel_->selectSoundVariableForSmoke(
            owner,
            variableId);
    }

    bool selectPreparedSoundValueForSmoke(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t voiceGeneration) override
    {
        return variablesPanel_->selectPreparedSoundValueForSmoke(
            identity,
            voiceGeneration);
    }

    void createSoundVariableForSmoke(
        talas::project::ProjectSoundVariableDeclaration declaration) override
    {
        setVariablesMode(true);
        variablesPanel_->createSoundVariableForSmoke(
            std::move(declaration));
    }

    bool assignSelectedSoundForSmoke(
        std::string_view definitionId) override
    {
        return variablesPanel_->assignSelectedSoundForSmoke(definitionId);
    }

    bool resetSelectedSoundForSmoke() override
    {
        return variablesPanel_->resetSelectedSoundForSmoke();
    }

    bool captureSelectedSoundForSmoke() override
    {
        return variablesPanel_->captureSelectedSoundForSmoke();
    }

    bool soundStateReadyForSmoke(
        std::string_view variableId,
        std::string_view selectedDefinitionId,
        bool assignmentEnabled) const override
    {
        return variablesMode_
            && variablesPanel_->soundStateReadyForSmoke(
                variableId,
                selectedDefinitionId,
                assignmentEnabled);
    }

    void refresh() override
    {
        // A reload takes the field's value from the project, an illegal
        // one typed there gone with it.
        illegalValueHeld_ = false;
        refreshExpressionLanguageControls();
        nodeModeButton_.setToggleState(
            !variablesMode_ && !generatorsMode_,
            juce::dontSendNotification);
        variablesModeButton_.setToggleState(
            variablesMode_, juce::dontSendNotification);
        generatorsModeButton_.setToggleState(
            generatorsMode_, juce::dontSendNotification);
        variablesPanel_->setVisible(variablesMode_);
        generatorPanel_->setVisible(generatorsMode_);
        if (generatorsMode_)
        {
            setNodeSurfaceVisible(false);
            variablesPanel_->setVisible(false);
            generatorPanel_->refresh();
            resized();
            repaint();
            return;
        }
        if (variablesMode_)
        {
            setNodeSurfaceVisible(false);
            variablesPanel_->refresh();
            resized();
            repaint();
            return;
        }
        nodeLabel_.setVisible(true);
        const auto replacementVisible =
            componentReplacementPresentation_.visible();
        preparedTitle_.setVisible(!replacementVisible);
        preparedContract_.setVisible(!replacementVisible);
        const auto* selectedNode = currentSelectedNode();
        const auto diskCache = diskCacheCaptureProjection_
            ? diskCacheCaptureProjection_(
                  soundDefinitionContext_, selectedNodeId_)
            : DiskCacheCapturePresentation{};
        diskCacheCaptureButton_.setVisible(diskCache.visible);
        presetCaptureButton_.setVisible(
            selectedNode != nullptr && selectedNode->typeId == "talas.preset");
        diskCacheCaptureButton_.setEnabled(diskCache.enabled);
        diskCacheCaptureButton_.setButtonText(diskCache.label);
        describeControl(
            diskCacheCaptureButton_,
            diskCache.label,
            diskCache.detail);
        const auto scope = soundDefinitionContext_.has_value()
            ? std::optional<std::string_view>{
                  *soundDefinitionContext_}
            : std::nullopt;
        preservedProjection_ = selectedNode == nullptr
            ? std::optional<
                  talas::app::MissingNodeWorkbenchProjection>{}
            : talas::app::missingNodeWorkbenchProjection(
                  bridge_.editor().document(),
                  scope,
                  selectedNodeId_,
                  registry_);
        updatePreservedControls();
        selectedDescriptor_ = selectedNode == nullptr
                || (preservedProjection_.has_value()
                    && !preservedProjection_
                            ->parameterEditingEnabled)
            ? std::optional<talas::core::NodeTypeDescriptor>{}
            : talas::project::describeProjectNode(
                  bridge_.editor().document(),
                  *selectedNode,
                  registry_);
        if (selectedNode == nullptr)
        {
            nodeLabel_.setText("No node selected", juce::dontSendNotification);
            className_.setVisible(false);
            parameterBox_.clear(juce::dontSendNotification);
            parameterBox_.setVisible(false);
            parameterHint_.setVisible(false);
            setValueControlsVisible(false);
            resized();
            repaint();
            return;
        }

        if (preservedProjection_.has_value())
        {
            nodeLabel_.setText(
                juceString(preservedProjection_->displayName)
                    + "\n"
                    + juceString(selectedNode->instanceId),
                juce::dontSendNotification);
            if (replacementVisible)
            {
                parameterBox_.clear(juce::dontSendNotification);
                parameterBox_.setVisible(false);
                parameterHint_.setVisible(false);
                setValueControlsVisible(false);
                resized();
                repaint();
                return;
            }
            if (!preservedProjection_->parameterEditingEnabled)
            {
                parameterBox_.clear(juce::dontSendNotification);
                parameterBox_.setVisible(false);
                parameterHint_.setVisible(false);
                setValueControlsVisible(false);
                resized();
                repaint();
                return;
            }
        }

        if (!selectedDescriptor_.has_value())
        {
            nodeLabel_.setText(
                "Unavailable node\n" + juceString(selectedNode->instanceId),
                juce::dontSendNotification);
            parameterBox_.clear(juce::dontSendNotification);
            parameterBox_.setVisible(false);
            parameterHint_.setVisible(false);
            setValueControlsVisible(false);
            resized();
            repaint();
            return;
        }

        const auto& descriptor = *selectedDescriptor_;
        if (!preservedProjection_.has_value())
        {
            nodeLabel_.setText(juceString(descriptor.displayName) + "\n" + juceString(selectedNode->instanceId),
                               juce::dontSendNotification);
        }
        className_.setText(juceString(descriptor.displayName), juce::dontSendNotification);
        className_.setTooltip("Click for the help on " + juceString(descriptor.displayName));
        className_.setVisible(true);
        // The pointer over the class name describes
        // the Sound and all its parameters; a click opens it in a help window.
        nodeLabel_.setTooltip(juceString(talas::app::descriptorSoundClassHelp(descriptor))
            + "\n\nClick to open this in a help window.");
        const auto capability = juceString(
            talas::app::descriptorNodeCapabilitySummary(descriptor));
        describeControl(
            nodeLabel_,
            "Selected node: " + juceString(descriptor.displayName),
            capability.replaceCharacters("\n\r", "  "));
        const auto retainedParameterId = parameterBox_.getText().toStdString();
        parameterBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < descriptor.parameters.size(); ++index)
            parameterBox_.addItem(juceString(descriptor.parameters[index].id), static_cast<int>(index + 1));
        // A field of many Sounds, such as a Mixer's Inputs, lists them too
        //.
        inputPorts_.clear();
        for (const auto& port : descriptor.inputs)
        {
            if (!port.acceptsMany)
                continue;
            parameterBox_.addItem(inputFieldLabel(port.id), inputFieldItemBase + static_cast<int>(inputPorts_.size()));
            inputPorts_.push_back(port.id);
        }
        if (descriptor.parameters.empty() && inputPorts_.empty())
        {
            parameterBox_.setVisible(false);
            parameterHint_.setVisible(false);
            setValueControlsVisible(false);
            resized();
            repaint();
            return;
        }

        parameterBox_.setVisible(true);
        parameterHint_.setVisible(true);

        const auto retained = std::ranges::find_if(descriptor.parameters, [&](const talas::core::ParameterDescriptor& parameter) {
            return parameter.id == retainedParameterId;
        });
        const auto retainedInput = std::ranges::find_if(inputPorts_, [&](const std::string& port) {
            return inputFieldLabel(port).toStdString() == retainedParameterId;
        });
        const auto selectedIndex = retained != descriptor.parameters.end()
            ? static_cast<int>(std::distance(descriptor.parameters.begin(), retained) + 1)
            : retainedInput != inputPorts_.end()
            ? inputFieldItemBase + static_cast<int>(std::distance(inputPorts_.begin(), retainedInput))
            : descriptor.parameters.empty() ? inputFieldItemBase : 1;
        parameterBox_.setSelectedId(selectedIndex, juce::dontSendNotification);
        refreshParameterValue();
        resized();
        repaint();
    }

    [[nodiscard]] bool diskCacheCaptureReadyForSmoke(
        const juce::String& label,
        bool enabled,
        const juce::String& detail) const override
    {
        return diskCacheCaptureButton_.isVisible()
            && diskCacheCaptureButton_.getButtonText() == label
            && diskCacheCaptureButton_.isEnabled() == enabled
            && diskCacheCaptureButton_.getDescription()
                .contains(detail);
    }

    [[nodiscard]] bool capturePresetEventValuesForSmoke() override
    {
        if (!presetCaptureButton_.isVisible() || !presetCaptureButton_.onClick)
            return false;
        presetCaptureButton_.onClick();
        return true;
    }

    void activateDiskCacheCaptureForSmoke() override
    {
        if (diskCacheCaptureButton_.isEnabled()
            && diskCacheCaptureButton_.onClick)
        {
            diskCacheCaptureButton_.onClick();
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
        WorkbenchStyle::paintPanelTitle(graphics, getLocalBounds().reduced(14).removeFromTop(24), "Inspector");
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(10).withTrimmedTop(28);
        auto modeRow = content.removeFromTop(24);
        const auto modeWidth = std::max(1, (modeRow.getWidth() - 8) / 3);
        nodeModeButton_.setBounds(modeRow.removeFromLeft(modeWidth));
        modeRow.removeFromLeft(4);
        variablesModeButton_.setBounds(modeRow.removeFromLeft(modeWidth));
        modeRow.removeFromLeft(4);
        generatorsModeButton_.setBounds(modeRow);
        content.removeFromTop(4);
        if (generatorsMode_)
        {
            upgradeTimedExpressionsButton_.setBounds({});
            variablesPanel_->setBounds({});
            generatorPanel_->setBounds(content);
            return;
        }
        generatorPanel_->setBounds({});
        if (variablesMode_)
        {
            auto languageRow = content.removeFromTop(24);
            if (upgradeExpressionsButton_.isVisible())
            {
                upgradeExpressionsButton_.setBounds(
                    languageRow.removeFromRight(
                        std::min(142, languageRow.getWidth() / 2)));
                languageRow.removeFromRight(4);
            }
            else
            {
                upgradeExpressionsButton_.setBounds({});
            }
            expressionLanguageLabel_.setBounds(languageRow);
            content.removeFromTop(4);
            if (upgradeTimedExpressionsButton_.isVisible())
            {
                auto timedRow = content.removeFromTop(24);
                upgradeTimedExpressionsButton_.setBounds(
                    timedRow.removeFromRight(
                        std::min(190, timedRow.getWidth())));
                content.removeFromTop(4);
            }
            else
            {
                upgradeTimedExpressionsButton_.setBounds({});
            }
            variablesPanel_->setBounds(content);
            return;
        }
        variablesPanel_->setBounds({});
        className_.setBounds(content.removeFromBottom(18).removeFromRight(std::min(220, content.getWidth())));
        nodeLabel_.setBounds(content.removeFromTop(34));
        content.removeFromTop(3);
        auto languageRow = content.removeFromTop(24);
        if (upgradeExpressionsButton_.isVisible())
        {
            upgradeExpressionsButton_.setBounds(
                languageRow.removeFromRight(
                    std::min(142, languageRow.getWidth() / 2)));
            languageRow.removeFromRight(4);
        }
        else
        {
            upgradeExpressionsButton_.setBounds({});
        }
        expressionLanguageLabel_.setBounds(languageRow);
        content.removeFromTop(3);
        if (upgradeTimedExpressionsButton_.isVisible())
        {
            auto timedRow = content.removeFromTop(24);
            upgradeTimedExpressionsButton_.setBounds(
                timedRow.removeFromRight(
                    std::min(190, timedRow.getWidth())));
            content.removeFromTop(3);
        }
        else
        {
            upgradeTimedExpressionsButton_.setBounds({});
        }
        if (definitionTitle_.isVisible())
        {
            definitionTitle_.setBounds(content.removeFromTop(18));
            content.removeFromTop(2);
            auto selectionRow = content.removeFromTop(24);
            newDefinitionButton_.setBounds(
                selectionRow.removeFromRight(50));
            selectionRow.removeFromRight(4);
            definitionBox_.setBounds(selectionRow);
            content.removeFromTop(2);
            auto identityRow = content.removeFromTop(24);
            definitionResultDomain_.setBounds(
                identityRow.removeFromRight(
                    std::min(96, identityRow.getWidth() / 3)));
            identityRow.removeFromRight(4);
            definitionName_.setBounds(identityRow);
            content.removeFromTop(2);
            definitionParameters_.setBounds(content.removeFromTop(24));
            content.removeFromTop(2);
            auto sourceRow = content.removeFromTop(48);
            auto actionColumn = sourceRow.removeFromRight(58);
            applyDefinitionButton_.setBounds(
                actionColumn.removeFromTop(22));
            actionColumn.removeFromTop(4);
            deleteDefinitionButton_.setBounds(actionColumn);
            sourceRow.removeFromRight(4);
            definitionSource_.setBounds(sourceRow);
            content.removeFromTop(4);
        }
        else
        {
            definitionTitle_.setBounds({});
            definitionBox_.setBounds({});
            newDefinitionButton_.setBounds({});
            definitionName_.setBounds({});
            definitionParameters_.setBounds({});
            definitionResultDomain_.setBounds({});
            definitionSource_.setBounds({});
            applyDefinitionButton_.setBounds({});
            deleteDefinitionButton_.setBounds({});
        }
        if (structureFocusLabel_.isVisible())
        {
            structureFocusLabel_.setBounds(content.removeFromTop(20));
            content.removeFromTop(3);
        }
        else
        {
            structureFocusLabel_.setBounds({});
        }
        if (componentReplacementPresentation_.visible())
        {
            componentReplacementPanel_->setBounds(content);
            preservedStatus_.setBounds({});
            preservedDetails_.setBounds({});
            retryButton_.setBounds({});
            replacePreservedButton_.setBounds({});
            disconnectButton_.setBounds({});
            deletePreservedButton_.setBounds({});
            parameterBox_.setBounds({});
            parameterHint_.setBounds({});
            textValue_.setBounds({});
            choiceValue_.setBounds({});
            booleanValue_.setBounds({});
            applyButton_.setBounds({});
            browseButton_.setBounds({});
            preparedTitle_.setBounds({});
            preparedContract_.setBounds({});
            return;
        }
        componentReplacementPanel_->setBounds({});
        if (preservedProjection_.has_value())
        {
            preservedStatus_.setBounds(
                content.removeFromTop(20));
            content.removeFromTop(3);
            const auto detailsHeight = std::clamp(
                content.getHeight() / 3,
                92,
                148);
            preservedDetails_.setBounds(
                content.removeFromTop(detailsHeight));
            content.removeFromTop(5);
            const auto compact =
                talas::app::missingNodeLayoutMode(getWidth())
                == talas::app::MissingNodeLayoutMode::Compact;
            auto actionRow = content.removeFromTop(24);
            if (compact)
            {
                retryButton_.setBounds(
                    actionRow.removeFromLeft(
                        (actionRow.getWidth() - 4) / 2));
                actionRow.removeFromLeft(4);
                replacePreservedButton_.setBounds(actionRow);
                content.removeFromTop(3);
                auto secondRow = content.removeFromTop(24);
                disconnectButton_.setBounds(
                    secondRow.removeFromLeft(
                        (secondRow.getWidth() - 4) / 2));
                secondRow.removeFromLeft(4);
                deletePreservedButton_.setBounds(secondRow);
            }
            else
            {
                const auto actionWidth =
                    (actionRow.getWidth() - 12) / 4;
                retryButton_.setBounds(
                    actionRow.removeFromLeft(actionWidth));
                actionRow.removeFromLeft(4);
                replacePreservedButton_.setBounds(
                    actionRow.removeFromLeft(actionWidth));
                actionRow.removeFromLeft(4);
                disconnectButton_.setBounds(
                    actionRow.removeFromLeft(actionWidth));
                actionRow.removeFromLeft(4);
                deletePreservedButton_.setBounds(actionRow);
            }
            content.removeFromTop(8);
        }
        else
        {
            preservedStatus_.setBounds({});
            preservedDetails_.setBounds({});
            retryButton_.setBounds({});
            replacePreservedButton_.setBounds({});
            disconnectButton_.setBounds({});
            deletePreservedButton_.setBounds({});
        }

        const auto radioRows = radioChoicesShown()
            ? static_cast<int>((radioChoices_.size() + radioColumns_ - 1) / std::max<std::size_t>(radioColumns_, 1))
            : 0;
        // The parameter's name above its field, its help and
        // an input field's Sounds (as icons above its list of names) between.
        parameterBox_.setBounds({});
        parameterHint_.setBounds({});
        soundIcons_.setBounds({});
        juce::Rectangle<int> valueRow;
        for (const auto row : talas::app::parameterFieldRows(parameterBox_.isVisible(), parameterHint_.isVisible(), soundIcons_.isVisible()))
        {
            switch (row)
            {
            case talas::app::ParameterFieldRow::Name:
                parameterBox_.setBounds(content.removeFromTop(24));
                content.removeFromTop(3);
                break;
            case talas::app::ParameterFieldRow::Help:
                parameterHint_.setBounds(content.removeFromTop(
                    std::clamp(content.getHeight() / 5, 54, 78)));
                content.removeFromTop(3);
                break;
            case talas::app::ParameterFieldRow::InputSounds:
                soundIcons_.setBounds(content.removeFromTop(
                    std::min(soundIcons_.preferredHeight(content.getWidth()), std::max(0, content.getHeight() / 3))));
                content.removeFromTop(3);
                break;
            case talas::app::ParameterFieldRow::Field:
                valueRow = content.removeFromTop(radioRows > 0 ? radioRows * 22
                        : textValue_.isMultiLine()
                        ? std::clamp(content.getHeight() / 3, 72, 108)
                        : 24);
                break;
            }
        }
        if (applyButton_.isVisible() && !valueRow.isEmpty())
        {
            auto applyColumn = valueRow.removeFromRight(54);
            applyButton_.setBounds(textValue_.isMultiLine()
                ? applyColumn.removeFromTop(24)
                : applyColumn);
            valueRow.removeFromRight(4);
        }
        else
        {
            applyButton_.setBounds({});
        }
        if (browseButton_.isVisible())
        {
            browseButton_.setBounds(valueRow.removeFromRight(70));
            valueRow.removeFromRight(4);
        }
        else
        {
            browseButton_.setBounds({});
        }
        // The FromMemoryWriter check box just below the
        // file name field.
        fromMemoryWriterBox_.setBounds(fromMemoryWriterBox_.isVisible() ? content.removeFromTop(22) : juce::Rectangle<int>{});
        textValue_.setBounds(valueRow);
        choiceValue_.setBounds(valueRow);
        booleanValue_.setBounds(valueRow);
        if (radioRows > 0)
        {
            const auto places = talas::app::radioButtonPlaces(radioChoices_.size(), radioColumns_);
            const auto width = valueRow.getWidth() / static_cast<int>(std::max<std::size_t>(radioColumns_, 1));
            for (std::size_t index = 0; index < radioChoices_.size(); ++index)
            {
                radioChoices_[index]->setBounds(valueRow.getX() + static_cast<int>(places[index].column) * width,
                    valueRow.getY() + static_cast<int>(places[index].row) * 22, width, 22);
            }
        }
        if (fieldEditor_->isVisible())
        {
            content.removeFromTop(4);
            fieldEditor_->setBounds(content.removeFromTop(
                std::min(fieldEditor_->preferredHeight(), std::max(0, content.getHeight() - 60))));
        }
        else
        {
            fieldEditor_->setBounds({});
        }
        if (splicerField_->isVisible())
        {
            content.removeFromTop(4);
            splicerField_->setBounds(content.removeFromTop(
                std::min(splicerField_->preferredHeight(), std::max(0, content.getHeight() - 60))));
        }
        else
        {
            splicerField_->setBounds({});
        }
        content.removeFromTop(8);
        preparedTitle_.setBounds(content.removeFromTop(18));
        content.removeFromTop(3);
        if (diskCacheCaptureButton_.isVisible())
        {
            diskCacheCaptureButton_.setBounds(content.removeFromTop(26));
            content.removeFromTop(5);
        }
        else
        {
            diskCacheCaptureButton_.setBounds({});
        }
        if (presetCaptureButton_.isVisible())
        {
            presetCaptureButton_.setBounds(content.removeFromTop(26));
            content.removeFromTop(5);
        }
        else
        {
            presetCaptureButton_.setBounds({});
        }
        preparedContract_.setBounds(content);
    }

private:
    void setVariablesMode(bool enabled)
    {
        setMode(enabled, false);
    }

    void setMode(bool variables, bool generators)
    {
        if (variablesMode_ == variables
            && generatorsMode_ == generators)
        {
            if (variables)
                variablesPanel_->refreshLiveValues();
            else if (generators)
                generatorPanel_->refresh();
            return;
        }
        if (!variables && !generators
            && (variablesMode_ || generatorsMode_)
            && nodeModeButton_.isShowing())
            nodeModeButton_.grabKeyboardFocus();
        variablesMode_ = variables;
        generatorsMode_ = generators;
        refresh();
    }

    void setNodeSurfaceVisible(bool visible)
    {
        for (auto* component : std::initializer_list<juce::Component*>{
                 &nodeLabel_, &definitionTitle_, &definitionBox_,
                 &newDefinitionButton_, &definitionName_,
                 &definitionParameters_, &definitionResultDomain_,
                 &definitionSource_, &applyDefinitionButton_,
                 &deleteDefinitionButton_, &structureFocusLabel_,
                 &parameterBox_, &parameterHint_, &textValue_,
                 &choiceValue_, &booleanValue_, &applyButton_,
                 &browseButton_, &preservedStatus_, &preservedDetails_,
                 &retryButton_, &replacePreservedButton_,
                 &disconnectButton_, &deletePreservedButton_,
                 componentReplacementPanel_.get(), &preparedTitle_,
                 &diskCacheCaptureButton_, &presetCaptureButton_,
                 &preparedContract_})
        {
            component->setVisible(visible);
        }
        if (!visible)
            soundIcons_.setVisible(false);
    }

    [[nodiscard]] static juce::String domainText(
        talas::expr::ValueDomain domain)
    {
        switch (domain)
        {
        case talas::expr::ValueDomain::Scalar: return "scalar";
        case talas::expr::ValueDomain::Hertz: return "hertz";
        case talas::expr::ValueDomain::Seconds: return "seconds";
        case talas::expr::ValueDomain::NoteNumber: return "noteNumber";
        case talas::expr::ValueDomain::Samples: return "samples";
        }
        return "scalar";
    }

    [[nodiscard]] static std::optional<talas::expr::ValueDomain>
    domainFromText(juce::String text)
    {
        text = text.trim();
        if (text == "scalar") return talas::expr::ValueDomain::Scalar;
        if (text == "hertz") return talas::expr::ValueDomain::Hertz;
        if (text == "seconds") return talas::expr::ValueDomain::Seconds;
        if (text == "noteNumber") return talas::expr::ValueDomain::NoteNumber;
        if (text == "samples") return talas::expr::ValueDomain::Samples;
        return std::nullopt;
    }

    void setDefinitionControlsVisible(bool visible)
    {
        definitionTitle_.setVisible(visible);
        definitionBox_.setVisible(visible);
        newDefinitionButton_.setVisible(visible);
        definitionName_.setVisible(visible);
        definitionParameters_.setVisible(visible);
        definitionResultDomain_.setVisible(visible);
        definitionSource_.setVisible(visible);
        applyDefinitionButton_.setVisible(visible);
        deleteDefinitionButton_.setVisible(visible);
    }

    void updateDefinitionComponentIds(std::string_view name)
    {
        const auto identity = name.empty() ? std::string{"new"}
                                           : std::string{name};
        const auto prefix = juce::String{
            "inspector.expression-definition:" + identity + ":"};
        definitionName_.setComponentID(prefix + "name");
        definitionParameters_.setComponentID(prefix + "parameters");
        definitionResultDomain_.setComponentID(prefix + "result-domain");
        definitionSource_.setComponentID(prefix + "source");
        definitionName_.setTitle(
            "Expression definition " + juceString(identity) + " name");
        definitionParameters_.setTitle(
            "Expression definition " + juceString(identity)
            + " parameters");
        definitionResultDomain_.setTitle(
            "Expression definition " + juceString(identity)
            + " result domain");
        definitionSource_.setTitle(
            "Expression definition " + juceString(identity) + " source");
    }

    void beginNewDefinition()
    {
        selectedDefinitionName_.reset();
        definitionBox_.setSelectedId(0, juce::dontSendNotification);
        definitionName_.setReadOnly(false);
        definitionName_.clear();
        definitionParameters_.clear();
        definitionResultDomain_.setSelectedId(1, juce::dontSendNotification);
        definitionSource_.clear();
        deleteDefinitionButton_.setEnabled(false);
        updateDefinitionComponentIds({});
    }

    void loadDefinition(const talas::expr::ExpressionDefinition& definition)
    {
        selectedDefinitionName_ = definition.name;
        definitionName_.setReadOnly(true);
        definitionName_.setText(juceString(definition.name), false);
        juce::String parameters;
        for (std::size_t index = 0; index < definition.parameters.size(); ++index)
        {
            if (index != 0)
                parameters += ", ";
            parameters += juceString(definition.parameters[index].name)
                + ":" + domainText(definition.parameters[index].domain);
        }
        definitionParameters_.setText(parameters, false);
        definitionResultDomain_.setText(
            domainText(definition.resultDomain),
            juce::dontSendNotification);
        definitionSource_.setText(juceString(definition.source), false);
        deleteDefinitionButton_.setEnabled(true);
        updateDefinitionComponentIds(definition.name);
    }

    void refreshDefinitionControls()
    {
        const auto language =
            bridge_.editor().document().expressionLanguage;
        const auto supportsDefinitions =
            talas::expr::supportsExpressionDefinitions(language);
        setDefinitionControlsVisible(supportsDefinitions);
        if (!supportsDefinitions)
        {
            selectedDefinitionName_.reset();
            return;
        }
        const auto retained = selectedDefinitionName_;
        const auto& definitions =
            bridge_.editor().document().expressionDefinitions.definitions;
        newDefinitionButton_.setEnabled(
            definitions.size()
            < talas::expr::ExpressionLimits::maximumDefinitions);
        applyDefinitionButton_.setEnabled(true);
        definitionBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < definitions.size(); ++index)
        {
            definitionBox_.addItem(
                juceString(definitions[index].name),
                static_cast<int>(index + 1));
        }
        if (retained.has_value())
        {
            const auto found = std::ranges::find(
                definitions,
                *retained,
                &talas::expr::ExpressionDefinition::name);
            if (found != definitions.end())
            {
                definitionBox_.setSelectedItemIndex(
                    static_cast<int>(std::distance(definitions.begin(), found)),
                    juce::dontSendNotification);
                loadDefinition(*found);
                return;
            }
        }
        if (!definitions.empty())
        {
            definitionBox_.setSelectedItemIndex(0, juce::dontSendNotification);
            loadDefinition(definitions.front());
        }
        else
        {
            beginNewDefinition();
        }
    }

    void commitDefinition()
    {
        std::optional<PreparedDefinitionCommit> prepared;
        try
        {
            prepared = prepareDefinitionCommit();
        }
        catch (const std::exception&)
        {
            onStatus_(
                "Expression definition rejected: bounded editor resources are unavailable");
        }
        catch (...)
        {
            onStatus_(
                "Expression definition rejected: unexpected bounded editor failure");
        }
        if (!prepared.has_value())
            return;

        const auto result = bridge_.editor().upsertExpressionDefinition(
            std::move(prepared->definition));
        if (!result.ok())
        {
            onStatus_("Expression definition rejected: "
                + juceString(result.message));
            return;
        }
        selectedDefinitionName_ = std::move(prepared->savedName);
        onDocumentChange_();
        onStatus_(
            juceString("Expression definition saved as one Undo step · fresh staging required"));
    }

    struct PreparedDefinitionCommit
    {
        talas::expr::ExpressionDefinition definition;
        std::string savedName;
    };

    [[nodiscard]] std::optional<PreparedDefinitionCommit>
    prepareDefinitionCommit()
    {
        const auto nameText = definitionName_.getText().trim();
        const auto sourceText = definitionSource_.getText();
        const auto parameterText = definitionParameters_.getText();
        if (static_cast<std::size_t>(nameText.getNumBytesAsUTF8())
                > talas::expr::ExpressionLimits::maximumDefinitionNameBytes
            || static_cast<std::size_t>(sourceText.getNumBytesAsUTF8())
                > talas::expr::ExpressionLimits::maximumDefinitionSourceBytes
            || static_cast<std::size_t>(parameterText.getNumBytesAsUTF8())
                > maximumDefinitionParameterEditorBytes)
        {
            onStatus_(
                "Expression definition exceeds a bounded name, parameter, or source editor limit");
            return std::nullopt;
        }
        talas::expr::ExpressionDefinition definition;
        definition.name = nameText.toStdString();
        definition.source = sourceText.toStdString();
        const auto resultDomain =
            domainFromText(definitionResultDomain_.getText());
        if (!resultDomain.has_value())
        {
            onStatus_("Expression definition result domain is invalid");
            definitionResultDomain_.grabKeyboardFocus();
            return std::nullopt;
        }
        definition.resultDomain = *resultDomain;
        juce::StringArray parameterTokens;
        parameterTokens.addTokens(parameterText, ",", {});
        parameterTokens.trim();
        parameterTokens.removeEmptyStrings();
        if (static_cast<std::size_t>(parameterTokens.size())
            > talas::expr::ExpressionLimits::maximumDefinitionParameters)
        {
            onStatus_("Expression definition exceeds the four-parameter limit");
            definitionParameters_.grabKeyboardFocus();
            return std::nullopt;
        }
        for (const auto& token : parameterTokens)
        {
            const auto separator = token.indexOfChar(':');
            if (separator <= 0 || token.indexOfChar(separator + 1, ':') >= 0)
            {
                onStatus_(
                    "Expression definition parameters must use name:domain pairs");
                definitionParameters_.grabKeyboardFocus();
                return std::nullopt;
            }
            const auto parameterDomain =
                domainFromText(token.substring(separator + 1));
            if (!parameterDomain.has_value())
            {
                onStatus_(
                    "Expression definition parameter has an unsupported domain");
                definitionParameters_.grabKeyboardFocus();
                return std::nullopt;
            }
            const auto parameterName = token.substring(0, separator).trim();
            if (static_cast<std::size_t>(
                    parameterName.getNumBytesAsUTF8())
                > talas::expr::ExpressionLimits::maximumDefinitionNameBytes)
            {
                onStatus_(
                    "Expression definition parameter name exceeds its byte limit");
                definitionParameters_.grabKeyboardFocus();
                return std::nullopt;
            }
            definition.parameters.push_back({
                parameterName.toStdString(),
                *parameterDomain,
            });
        }
        auto savedName = definition.name;
        return PreparedDefinitionCommit{
            std::move(definition),
            std::move(savedName)};
    }

    void deleteDefinition()
    {
        if (!selectedDefinitionName_.has_value())
            return;
        std::optional<std::string> nearestSurvivor;
        const auto& definitions =
            bridge_.editor().document().expressionDefinitions.definitions;
        const auto selected = std::ranges::find(
            definitions,
            *selectedDefinitionName_,
            &talas::expr::ExpressionDefinition::name);
        if (selected != definitions.end())
        {
            const auto next = std::next(selected);
            if (next != definitions.end())
                nearestSurvivor = next->name;
            else if (selected != definitions.begin())
                nearestSurvivor = std::prev(selected)->name;
        }
        const auto result = bridge_.editor().removeExpressionDefinition(
            *selectedDefinitionName_);
        if (!result.ok())
        {
            onStatus_("Expression definition delete blocked: "
                + juceString(result.message));
            return;
        }
        selectedDefinitionName_ = std::move(nearestSurvivor);
        onDocumentChange_();
        onStatus_(
            juceString("Expression definition deleted as one Undo step · fresh staging required"));
    }

    void refreshExpressionLanguageControls()
    {
        const auto language = bridge_.editor().document().expressionLanguage;
        const auto known = language
                == talas::expr::ExpressionLanguageVersion::LegacyV1
            || talas::expr::isCompiledExpressionLanguage(language);
        const auto latest = language
                == talas::expr::ExpressionLanguageVersion::VoiceV1
            || language
                == talas::expr::ExpressionLanguageVersion::ValV1
            || language
                == talas::expr::ExpressionLanguageVersion::PureV5;
        const auto target = language
                == talas::expr::ExpressionLanguageVersion::VariablesV4
            ? talas::expr::ExpressionLanguageVersion::ValV1
            : language
                == talas::expr::ExpressionLanguageVersion::VariablesV3
            ? talas::expr::ExpressionLanguageVersion::VariablesV4
            : language == talas::expr::ExpressionLanguageVersion::VariablesV2
            ? talas::expr::ExpressionLanguageVersion::VoiceV1
            : language == talas::expr::ExpressionLanguageVersion::VariablesV1
            ? talas::expr::ExpressionLanguageVersion::VariablesV2
            : language == talas::expr::ExpressionLanguageVersion::SessionV1
            ? talas::expr::ExpressionLanguageVersion::VariablesV1
            : talas::expr::ExpressionLanguageVersion::SessionV1;
        const auto label = language
                == talas::expr::ExpressionLanguageVersion::LegacyV1
            ? juce::String{"Legacy expression"}
            : language == talas::expr::ExpressionLanguageVersion::PureV2
            ? juce::String{"Pure expression v2"}
            : language == talas::expr::ExpressionLanguageVersion::PureV3
            ? juce::String{"Pure expression v3"}
            : language == talas::expr::ExpressionLanguageVersion::PureV4
            ? juce::String{"Pure expression v4"}
            : language == talas::expr::ExpressionLanguageVersion::PureV5
            ? juce::String{"Pure expression v5"}
            : language == talas::expr::ExpressionLanguageVersion::SessionV1
            ? juce::String{"Session state v1"}
            : language == talas::expr::ExpressionLanguageVersion::VariablesV1
            ? juce::String{"Variables v1"}
            : language == talas::expr::ExpressionLanguageVersion::VariablesV2
            ? juce::String{"Variables v2"}
            : language == talas::expr::ExpressionLanguageVersion::VoiceV1
            ? juce::String{"Voice v1"}
            : language == talas::expr::ExpressionLanguageVersion::VariablesV3
            ? juce::String{"Variables v3"}
            : language == talas::expr::ExpressionLanguageVersion::VariablesV4
            ? juce::String{"Variables v4"}
            : language == talas::expr::ExpressionLanguageVersion::ValV1
            ? juce::String{"Val v1"}
            : juce::String{"Unknown expression language"};
        expressionLanguageLabel_.setText(
            label,
            juce::dontSendNotification);
        expressionLanguageLabel_.setTitle(
            "Project expression language: " + label);
        expressionLanguageLabel_.setDescription(
            language == talas::expr::ExpressionLanguageVersion::LegacyV1
                ? "Legacy parser authority. Use the adjacent audited action to upgrade without rewriting expression text."
                : language
                        == talas::expr::ExpressionLanguageVersion::PureV2
                ? "Pure v2 uses fixed parser and evaluation budgets, pure functions, comparisons, lazy if, and bounded list indexing."
                : language == talas::expr::ExpressionLanguageVersion::PureV3
                ? "Pure v3 retains fixed budgets and adds constant-time stateless series indexing with at most 64 conceptual elements."
                : language == talas::expr::ExpressionLanguageVersion::PureV4
                ? "Pure v4 retains fixed budgets and adds bounded project-owned pure definitions with fixed local slots and no runtime lookup."
                : language == talas::expr::ExpressionLanguageVersion::PureV5
                ? "Pure v5 adds bounded intoXArray mapping over changing Event Values. Its native interpolation curve is not yet verified."
                : language == talas::expr::ExpressionLanguageVersion::SessionV1
                ? "Session v1 adds bounded transactional state to live Event Value expressions: at most 32 slots, advanced once per accepted update. State survives stop/start, resets on seek, graph replacement, device restart, or voice reuse, and is never stored in projects or snapshots."
                : language == talas::expr::ExpressionLanguageVersion::VariablesV1
                ? "Variables v1 retains Session state and adds bounded @variableId references with project or reusable-Sound ownership, exact instance cells, transactional assignment, snapshot eligibility, compatible restaging, explicit reset, and declared initial values."
                : language == talas::expr::ExpressionLanguageVersion::VariablesV2
                ? "Variables v2 retains Variables v1 exactly and adds checked Scalar rem: expressions with truncation-toward-zero remainder semantics."
                : language == talas::expr::ExpressionLanguageVersion::VoiceV1
                ? "Voice v1 retains Variables v2 and adds ?VoiceNumber: the voice slot, starting at one for each MIDI Voice instrument. Each slot keeps its number when notes change, allowing distinct static seeds and other per-voice parameters."
                : language == talas::expr::ExpressionLanguageVersion::VariablesV3
                ? "Variables v3 retains Variables v2 and adds millisecond-clock ramp: expressions. Prepared runtime support currently covers oscillator amplitude without Event Value or variable references."
                : language == talas::expr::ExpressionLanguageVersion::VariablesV4
                ? "Variables v4 retains timed expressions and adds units after scalar expressions: !Interval nn, (!Length * 2) ms, and !Level db. Units use the same conversions as numeric literals."
                : language == talas::expr::ExpressionLanguageVersion::ValV1
                ? "Val v1 reads fields with unary messages such as 3 twoExp and 4 c hz first, then binary operators from left to right, then keyword messages such as 3 vmin: 2. A plain number takes the field's unit."
                : "Unknown project expression-language authority.");
        auto upgradeAvailable = false;
        auto upgradeDetail = juce::String{
            "Dry-runs every persisted expression and effective descriptor default. Commits only when all sources are compatible, as one Undo step."};
        if (known && !latest)
        {
            try
            {
                const auto audit = target
                        == talas::expr::ExpressionLanguageVersion::ValV1
                    ? talas::project::auditProjectExpressionsKeepingMeaning(
                        bridge_.editor().document(),
                        registry_,
                        target,
                        language)
                    : talas::project::auditProjectExpressions(
                        bridge_.editor().document(),
                        registry_,
                        target);
                upgradeAvailable = audit.ok();
                if (!upgradeAvailable)
                {
                    if (!audit.diagnostics.empty())
                    {
                        const auto& diagnostic = audit.diagnostics.front();
                        upgradeDetail = "Unavailable: "
                            + juceString(diagnostic.path)
                            + " bytes "
                            + juce::String(static_cast<juce::int64>(
                                diagnostic.range.offset))
                            + "-"
                            + juce::String(static_cast<juce::int64>(
                                diagnostic.range.offset
                                + diagnostic.range.length))
                            + ": " + juceString(diagnostic.message);
                    }
                    else
                    {
                        upgradeDetail =
                            "Unavailable: target compatibility audit exceeded the diagnostic limit.";
                    }
                }
            }
            catch (const std::bad_alloc&)
            {
                upgradeDetail =
                    "Unavailable: target compatibility audit allocation failed.";
            }
            catch (const std::length_error&)
            {
                upgradeDetail =
                    "Unavailable: target compatibility audit exceeded supported capacity.";
            }
        }
        const auto targetName = target
                == talas::expr::ExpressionLanguageVersion::ValV1
            ? juce::String{"Val v1"}
            : target == talas::expr::ExpressionLanguageVersion::VariablesV4
            ? juce::String{"Variables v4"}
            : target == talas::expr::ExpressionLanguageVersion::VoiceV1
            ? juce::String{"Voice v1"}
            : target == talas::expr::ExpressionLanguageVersion::VariablesV2
            ? juce::String{"Variables v2"}
            : target == talas::expr::ExpressionLanguageVersion::VariablesV1
            ? juce::String{"Variables v1"}
            : juce::String{"Session state v1"};
        upgradeExpressionsButton_.setButtonText("Upgrade to " + targetName);
        upgradeExpressionsButton_.setTitle(
            "Upgrade expressions to " + targetName + ". " + upgradeDetail);
        upgradeExpressionsButton_.setDescription(upgradeDetail);
        upgradeExpressionsButton_.setVisible(known && !latest);
        upgradeExpressionsButton_.setEnabled(
            known && !latest && upgradeAvailable);
        const auto timedVisible = language
            == talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto timedEnabled = false;
        auto timedDetail = juce::String{
            "Dry-runs existing expressions and defaults. One Undo restores Variables v2; restage before rendering."};
        if (timedVisible)
        {
            try
            {
                const auto audit = talas::project::auditProjectExpressions(
                    bridge_.editor().document(),
                    registry_,
                    talas::expr::ExpressionLanguageVersion::VariablesV3);
                timedEnabled = audit.ok();
                if (!timedEnabled)
                {
                    timedDetail = audit.diagnostics.empty()
                        ? juce::String{"Unavailable: target compatibility audit exceeded the diagnostic limit."}
                        : "Unavailable: "
                            + juceString(audit.diagnostics.front().path)
                            + " bytes "
                            + juce::String(static_cast<juce::int64>(
                                audit.diagnostics.front().range.offset))
                            + "-"
                            + juce::String(static_cast<juce::int64>(
                                audit.diagnostics.front().range.offset
                                + audit.diagnostics.front().range.length))
                            + ": "
                            + juceString(audit.diagnostics.front().message);
                }
            }
            catch (const std::exception&)
            {
                timedDetail = "Unavailable: target compatibility audit failed.";
            }
        }
        upgradeTimedExpressionsButton_.setTitle(
            "Upgrade expressions to Variables v3. " + timedDetail);
        upgradeTimedExpressionsButton_.setDescription(timedDetail);
        upgradeTimedExpressionsButton_.setVisible(timedVisible);
        upgradeTimedExpressionsButton_.setEnabled(timedVisible && timedEnabled);
        refreshDefinitionControls();
    }

    void updatePreservedControls()
    {
        const auto visible = preservedProjection_.has_value();
        const auto replacementVisible =
            componentReplacementPresentation_.visible();
        const auto ordinaryVisible = visible && !replacementVisible;
        preservedStatus_.setVisible(ordinaryVisible);
        preservedDetails_.setVisible(ordinaryVisible);
        retryButton_.setVisible(
            ordinaryVisible && preservedProjection_->canRetry);
        replacePreservedButton_.setVisible(
            ordinaryVisible && componentReplacementEnabled_
            && !soundDefinitionContext_.has_value());
        disconnectButton_.setVisible(
            ordinaryVisible && preservedProjection_->canDisconnect);
        deletePreservedButton_.setVisible(
            ordinaryVisible && preservedProjection_->canDelete);
        if (!visible)
        {
            resized();
            return;
        }

        const auto resolved =
            preservedProjection_->parameterEditingEnabled;
        preservedStatus_.setColour(
            juce::Label::textColourId,
            resolved ? workbenchTheme.fullSupport : errorColour);
        preservedStatus_.setText(
            juceString(preservedProjection_->statusLabel),
            juce::dontSendNotification);
        auto detail = juceString(
            preservedProjection_->typeVersionText)
            + "\n"
            + juceString(preservedProjection_->stateText)
            + "\nBundle: "
            + juceString(preservedProjection_->bundleText)
            + "\nRecovery: "
            + juceString(preservedProjection_->recoveryText)
            + "\nReason: "
            + juceString(preservedProjection_->diagnosticText)
            + "\nFrozen ports:";
        for (const auto& port : preservedProjection_->ports)
        {
            detail += "\n  ";
            detail += port.direction
                    == talas::app::MissingNodePortDirection::Input
                ? "In "
                : "Out ";
            detail += juceString(port.portId)
                + juceString(" · ") + juceString(port.typeLabel)
                + (port.connected ? juceString(" · connected") : "");
        }
        preservedDetails_.setText(detail, false);
        preservedDetails_.setCaretPosition(0);
        preservedDetails_.setTitle(
            "Preserved component details: "
            + detail.replaceCharacters("\n\r", "  "));
        retryButton_.setEnabled(
            preservedProjection_->canRetry);
        replacePreservedButton_.setEnabled(
            componentReplacementEnabled_
            && !soundDefinitionContext_.has_value());
        disconnectButton_.setEnabled(
            preservedProjection_->canDisconnect);
        deletePreservedButton_.setEnabled(
            preservedProjection_->canDelete);
        resized();
    }

    void invokePreservedAction(PreservedNodeAction action)
    {
        if (!preservedProjection_.has_value()
            || selectedNodeId_.empty()
            || !onPreservedNodeAction_)
        {
            return;
        }
        onPreservedNodeAction_(
            soundDefinitionContext_,
            selectedNodeId_,
            action);
    }

    [[nodiscard]] const talas::project::ProjectNode* currentSelectedNode() const
    {
        const auto& document = bridge_.editor().document();
        if (!soundDefinitionContext_.has_value())
            return findNode(document, selectedNodeId_);
        const auto definition = std::ranges::find_if(
            document.soundDefinitions,
            [&](const auto& candidate) {
                return candidate.definitionId
                    == *soundDefinitionContext_;
            });
        if (definition == document.soundDefinitions.end())
            return nullptr;
        const auto node = std::ranges::find_if(
            definition->graph.nodes,
            [&](const auto& candidate) {
                return candidate.instanceId == selectedNodeId_;
            });
        return node == definition->graph.nodes.end()
            ? nullptr
            : &*node;
    }

    struct TypedKeys final : juce::KeyListener
    {
        InspectorPanelImpl* owner = nullptr;
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            const auto character = key.getTextCharacter();
            owner->characterTyped_ = character >= ' ' && character != 127 && !key.getModifiers().isCommandDown()
                && !key.getModifiers().isCtrlDown();
            return false;
        }
    };

    // Tab and Shift+Tab: the Sound's next or previous field, the edit in
    // this one taken first; past the last or before the first, the signal
    // flow diagram.
    struct TabKeys final : juce::KeyListener
    {
        InspectorPanelImpl* owner = nullptr;
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            const auto modifiers = key.getModifiers();
            if (key.getKeyCode() != juce::KeyPress::tabKey || modifiers.isCommandDown() || modifiers.isCtrlDown()
                || modifiers.isAltDown())
                return false;
            return owner->tabToField(!modifiers.isShiftDown());
        }
    };

    bool tabToField(bool forward)
    {
        if (!parameterBox_.isVisible() || textValue_.isMultiLine())
            return false;
        if (valueEdited_ && textValue_.isVisible() && !textValue_.isReadOnly())
        {
            commitParameter();
            // An edit refused keeps the field active, its error said.
            if (valueEdited_)
                return true;
        }
        const auto count = parameterBox_.getNumItems();
        const auto current = parameterBox_.getSelectedItemIndex();
        if (count <= 0 || current < 0)
            return false;
        const auto next = talas::app::tabbedParameterField(
            static_cast<std::size_t>(count), static_cast<std::size_t>(current), forward);
        if (!next)
        {
            if (leaveFields_)
                leaveFields_();
            return true;
        }
        parameterBox_.setSelectedItemIndex(static_cast<int>(*next), juce::sendNotificationSync);
        if (textValue_.isVisible())
        {
            if (textValue_.isShowing())
                textValue_.grabKeyboardFocus();
            textValue_.selectAll();
        }
        else if (choiceValue_.isVisible() && choiceValue_.isShowing())
            choiceValue_.grabKeyboardFocus();
        else if (booleanValue_.isVisible() && booleanValue_.isShowing())
            booleanValue_.grabKeyboardFocus();
        return true;
    }

    // Event Values red, variables green and Event Sources blue, each in
    // bold.
    void colourValueText()
    {
        shownValueText_ = textValue_.getText();
        const auto spans = talas::app::fieldTextSpans(shownValueText_.toStdString());
        if (std::ranges::none_of(spans, [](const auto& span) { return span.kind != talas::app::FieldTextKind::Plain; }))
            return;
        const auto plainColour = textValue_.findColour(juce::TextEditor::textColourId);
        const auto plainFont = textValue_.getFont();
        textValue_.clear();
        for (const auto& span : spans)
        {
            const auto marked = span.kind != talas::app::FieldTextKind::Plain;
            textValue_.setColour(juce::TextEditor::textColourId,
                marked ? juce::Colour{talas::app::fieldTextColour(span.kind)} : plainColour);
            textValue_.setFont(marked ? plainFont.boldened() : plainFont);
            textValue_.insertTextAtCaret(juceString(span.text));
        }
        textValue_.setColour(juce::TextEditor::textColourId, plainColour);
        textValue_.setFont(plainFont);
    }

    void completeHotValue()
    {
        const auto typed = std::exchange(characterTyped_, false);
        if (!typed || completingHotValue_ || textValue_.isReadOnly())
            return;
        const auto* descriptor = selectedParameterDescriptor();
        if (descriptor == nullptr)
            return;
        const auto text = textValue_.getText();
        const auto caret = textValue_.getCaretPosition();
        const auto before = text.substring(0, caret).toStdString();
        const auto name = talas::app::hotValueBeingTyped(text.toStdString(), before.size());
        if (!name)
            return;
        std::vector<std::string> names;
        for (const auto& control : bridge_.editor().document().virtualControls)
            names.push_back(control.eventName);
        const auto completed = talas::app::hotValueCompletion(*name, descriptor->id, names);
        if (!completed || completed->size() <= name->size())
            return;
        const auto rest = juceString(completed->substr(name->size()));
        completingHotValue_ = true;
        textValue_.insertTextAtCaret(rest);
        textValue_.setHighlightedRegion({caret, caret + rest.length()});
        completingHotValue_ = false;
    }

    [[nodiscard]] std::pair<std::string, std::string> typeIntoValueForSmoke(
        std::string_view characters, bool overAll) override
    {
        if (overAll)
            textValue_.selectAll();
        // The field reports its changes asynchronously; the smoke completes
        // each character at once.
        for (const auto character : characters)
        {
            textValue_.insertTextAtCaret(juce::String::charToString(static_cast<juce::juce_wchar>(character)));
            characterTyped_ = true;
            completeHotValue();
        }
        return {textValue_.getText().toStdString(), textValue_.getHighlightedText().toStdString()};
    }

    [[nodiscard]] std::pair<std::string, std::string> pressValueKeyForSmoke(const juce::KeyPress& key) override
    {
        (void)textValue_.keyPressed(key);
        return {textValue_.getText().toStdString(), textValue_.getHighlightedText().toStdString()};
    }

    const talas::core::ParameterDescriptor* selectedParameterDescriptor() const
    {
        const auto* selectedNode = currentSelectedNode();
        if (selectedNode == nullptr)
            return nullptr;
        if (!selectedDescriptor_.has_value())
            return nullptr;
        return findDescriptorParameter(
            *selectedDescriptor_,
            parameterBox_.getText().toStdString());
    }

    static constexpr int inputFieldItemBase = 10000;

    [[nodiscard]] static juce::String inputFieldLabel(const std::string& portId)
    {
        return portId == "inputs" ? juce::String{"Inputs"} : juceString(portId) + " Sounds";
    }

    [[nodiscard]] std::optional<std::string> selectedInputPort() const
    {
        const auto id = parameterBox_.getSelectedId();
        if (id < inputFieldItemBase || static_cast<std::size_t>(id - inputFieldItemBase) >= inputPorts_.size())
            return std::nullopt;
        return inputPorts_[static_cast<std::size_t>(id - inputFieldItemBase)];
    }

    // The Sounds in an input field, one a line in their order: deleting a
    // line removes that input, moving lines reorders them, and an existing
    // Sound's name adds it.
    void showInputField(const std::string& portId)
    {
        const auto scope = soundDefinitionContext_.has_value() ? std::optional<std::string_view>{*soundDefinitionContext_}
                                                               : std::nullopt;
        juce::String lines;
        const auto sources = bridge_.editor().inputsOf(scope, selectedNodeId_, portId);
        for (const auto& source : sources)
            lines << juceString(source) << "\n";
        soundIcons_.setSounds(sources);
        soundIcons_.setVisible(!sources.empty());
        const auto hint = juceString("The Sounds in " + portId + ", in order, one a line. Delete a line to remove that input, move lines to reorder them, type a Sound's name to add it; Apply.");
        parameterHint_.setText(hint, false);
        parameterHint_.setVisible(true);
        fieldEditor_->hideField();
        splicerField_->setVisible(false);
        choiceValue_.setVisible(false);
        showRadioChoices(false);
        booleanValue_.setVisible(false);
        browseButton_.setVisible(false);
        textValue_.setMultiLine(true, false);
        textValue_.setSelectAllWhenFocused(talas::app::fieldSelectsAllWhenFocused(true));
        textValue_.setReturnKeyStartsNewLine(true);
        textValue_.setReadOnly(false);
        textValue_.setTitle("Input field · " + hint);
        textValue_.setText(lines.trimEnd(), juce::dontSendNotification);
        textValue_.setVisible(true);
        applyButton_.setVisible(true);
        resized();
    }

    void commitInputField(const std::string& portId)
    {
        std::vector<std::string> sources;
        for (const auto& line : juce::StringArray::fromLines(textValue_.getText()))
        {
            if (line.trim().isNotEmpty())
                sources.push_back(line.trim().toStdString());
        }
        const auto scope = soundDefinitionContext_.has_value() ? std::optional<std::string_view>{*soundDefinitionContext_}
                                                               : std::nullopt;
        const auto result = bridge_.editor().setInputs(scope, selectedNodeId_, portId, std::move(sources));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            showInputField(portId);
            return;
        }
        onDocumentChange_();
        valueEdited_ = false;
        onStatus_("Set the Sounds in " + juceString(portId));
    }

    void refreshParameterValue()
    {
        valueEdited_ = false;
        illegalValueHeld_ = false;
        shownValueText_ = {};
        shownResource_.reset();
        soundIcons_.setVisible(false);
        if (const auto port = selectedInputPort())
        {
            showInputField(*port);
            return;
        }
        const auto* descriptorParameter = selectedParameterDescriptor();
        const auto* selectedNode = currentSelectedNode();
        if (descriptorParameter == nullptr || selectedNode == nullptr)
        {
            markHotParameterName(false);
            parameterHint_.setVisible(false);
            setValueControlsVisible(false);
            return;
        }
        const auto* override =
            findParameter(*selectedNode, descriptorParameter->id);
        textValue_.setReadOnly(!talas::app::parameterTakesTyping(selectedNode->typeId, descriptorParameter->id));

        const auto hint = juceString(
            talas::app::descriptorParameterInfoHint(*descriptorParameter));
        auto completeHint = hint;
        // The Class Editor's comment and range of an instance's parameter
        //.
        if (selectedNode->soundDefinitionId.has_value())
        {
            const auto& definitions = bridge_.editor().document().soundDefinitions;
            const auto definition = std::ranges::find(definitions, *selectedNode->soundDefinitionId,
                &talas::project::ProjectSoundDefinition::definitionId);
            if (definition != definitions.end())
            {
                const auto exposed = std::ranges::find(definition->exposedParameters, descriptorParameter->id,
                    &talas::project::ProjectSoundExposedParameter::parameterId);
                if (exposed != definition->exposedParameters.end())
                {
                    const auto help = talas::app::soundParameterHelp(*exposed);
                    if (!help.empty())
                        completeHint += "\n" + juceString(help);
                }
            }
        }
        if (const auto help = talas::app::prototypeParameterHelp(selectedNode->typeId, descriptorParameter->id); !help.empty())
            completeHint += "\n" + juceString(help);
        const auto parameterTransformation =
            selectedNode->typeId == "talas.parameterTransformer"
            && descriptorParameter->id == "transformation";
        const auto midiMapperMap =
            selectedNode->typeId == "talas.midiMapper"
            && descriptorParameter->id == "map";
        const auto midiNoteScript =
            (selectedNode->typeId == "talas.midiMapper"
                || selectedNode->typeId == "talas.midiVoice")
            && descriptorParameter->id == "script";
        const auto multilineText =
            parameterTransformation || midiMapperMap || midiNoteScript;
        const auto* mapperModel = selectedNode->typeId == "talas.midiMapper"
            ? findParameter(*selectedNode, "voiceModel") : nullptr;
        const auto legacyMapper = mapperModel != nullptr
            && mapperModel->choice == "legacyPassthrough";
        if (parameterTransformation)
        {
            completeHint +=
                "\nClosed compile-time syntax: set <parameter-id> = <expression> or scale <parameter-id> by <finite-scalar>; one rule per line; exclusive upstream branch only; MIDI Voice polyphony is structural and cannot be rewritten.";
        }
        if (midiMapperMap)
        {
            completeHint += legacyMapper
                ? "\nLegacy Mapper: shared Input, no per-note copies. Blank/local Maps do not require MIDI; gate/velocity modes do. One !EventValue is: `Source association per line, up to 16 lines and 512 ASCII bytes. Unlisted Event Values retain their global bindings. Select Apply to commit."
                : "\nPolyphonic Input: MIDI notes create independent Input copies up to Polyphony. Blank/local Maps are supported; gate/velocity tokens require legacyPassthrough. One !EventValue is: `Source association per line, up to 16 lines and 512 ASCII bytes. Unlisted Event Values retain their global bindings. Select Apply to commit.";
        }
        if (midiNoteScript)
        {
            completeHint +=
                "\nLiteral self keyDownAt: notes can be written one per line. Return adds a line; Apply saves the Script. General Smalltalk programs are not yet supported.";
        }
        const auto constantField = selectedDescriptor_.has_value()
            && talas::runtime::isConstantField(selectedDescriptor_->typeId, descriptorParameter->id);
        markHotParameterName(descriptorParameter->kind == talas::core::ParameterKind::Expression && !constantField);
        if (descriptorParameter->kind
            == talas::core::ParameterKind::Expression)
        {
            completeHint += constantField
                ? "\nConstant field: plays the value it has as the Sound starts"
                : "\nHot field: follows Event Values and time while the Sound plays";
            completeHint += "\nProject language: ";
            const auto language =
                bridge_.editor().document().expressionLanguage;
            completeHint += language
                    == talas::expr::ExpressionLanguageVersion::VariablesV4
                ? juceString("Variables v4 · timed expressions · units after scalar expressions, such as !Interval nn")
                : language == talas::expr::ExpressionLanguageVersion::SessionV1
                ? juceString("Session state v1 · fixed budgets · bounded definitions · transactional session state")
                : language == talas::expr::ExpressionLanguageVersion::PureV4
                ? juceString("Pure expression v4 · fixed budgets · bounded project definitions · fixed locals · no runtime lookup")
                : language == talas::expr::ExpressionLanguageVersion::PureV5
                ? juceString("Pure expression v5 · bounded intoXArray mapping · changing Event Values · fixed locals")
                : language == talas::expr::ExpressionLanguageVersion::PureV3
                ? juceString("Pure expression v3 · fixed budgets · bounded list indexing · constant-time series(first, step, count)[index] · 64 element limit")
                : language == talas::expr::ExpressionLanguageVersion::PureV2
                ? juceString("Pure expression v2 · fixed budgets · abs/sqrt/log/exp/clamp/map · comparisons · lazy if · bounded list indexing")
                : juceString("Legacy expression · upgrade is available after an exact compatibility audit");
            if (language == talas::expr::ExpressionLanguageVersion::SessionV1)
            {
                const auto& source = override != nullptr
                        && override->expression.has_value()
                    ? *override->expression
                    : descriptorParameter->defaultExpression;
                const auto parsed = talas::expr::parse(
                    source,
                    language,
                    bridge_.editor().document().expressionDefinitions);
                if (parsed.ok())
                {
                    completeHint += "\nState slots: ";
                    completeHint += juce::String(
                        static_cast<juce::int64>(
                            parsed.expression->stateSlotCount()));
                    completeHint += "/";
                    completeHint += juce::String(
                        static_cast<juce::int64>(
                            talas::expr::ExpressionLimits::maximumStateSlots));
                    completeHint +=
                        juceString(" · clock: one accepted live Event Value update · all affected expressions commit as one transaction");
                }
                completeHint +=
                    "\nLifecycle: stop/start retains state; operator reset is explicit; seek, graph replacement, device restart, and voice reuse reset it. Session state is not project or snapshot state.";
            }
        }
        if (selectedDescriptor_.has_value())
        {
            completeHint += "\n";
            completeHint += juceString(
                talas::app::descriptorNodeCapabilitySummary(
                    *selectedDescriptor_));
        }
        parameterHint_.setText(completeHint, false);
        // The pointer over the parameter's name: its nature and the values
        // it expects.
        parameterBox_.setTooltip(completeHint);
        parameterHint_.setCaretPosition(0);
        parameterHint_.setTitle(
            "Selected parameter Info Hint: "
            + completeHint.replaceCharacters("\n\r", "  "));
        parameterHint_.setVisible(true);
        textValue_.setTitle("Parameter value");
        textValue_.setDescription(
            "Expression, resource, or text value for the selected parameter.");
        textValue_.setMultiLine(multilineText, false);
        // Clicked or tabbed into, a field's contents are selected, so what
        // is typed replaces them.
        textValue_.setSelectAllWhenFocused(talas::app::fieldSelectsAllWhenFocused(false));
        textValue_.setReturnKeyStartsNewLine(multilineText);
        textValue_.setScrollbarsShown(multilineText);
        textValue_.setInputRestrictions(
            parameterTransformation
                ? static_cast<int>(talas::project::maximumParameterTransformationSourceBytes)
                : 0);
        textValue_.setTextToShowWhenEmpty(
            midiMapperMap
                ? "!GainA is: `AltA\n!GainB is: `AltB"
                : midiNoteScript
                ? "self keyDownAt: 0 s duration: 1 s frequency: 440 hz velocity: 1."
                : parameterTransformation
                ? "set frequency = 400 hz\nscale frequency by 2"
                : juce::String{},
            secondaryTextColour);

        textValue_.setVisible(false);
        choiceValue_.setVisible(false);
        showRadioChoices(false);
        booleanValue_.setVisible(false);
        applyButton_.setVisible(false);
        browseButton_.setVisible(false);
        fromMemoryWriterBox_.setVisible(false);
        fromMemoryWriterParameter_.reset();

        fieldEditor_->hideField();
        splicerField_->setVisible(false);
        switch (descriptorParameter->kind)
        {
        case talas::core::ParameterKind::Expression:
            // A hot field has a cyan background and a constant one a plain
            // one.
            textValue_.setColour(juce::TextEditor::backgroundColourId,
                constantField ? workbenchTheme.controlTile
                              : workbenchTheme.controlTile.interpolatedWith(workbenchTheme.audio, 0.22F));
            if (flashing_)
                fieldColour_ = textValue_.findColour(juce::TextEditor::backgroundColourId);
            textValue_.setTitle(
                juceString("Expression parameter value · ")
                + completeHint.replaceCharacters("\n\r", "  "));
            textValue_.setDescription(completeHint);
            textValue_.setText(juceString(override != nullptr && override->expression.has_value()
                ? *override->expression
                : descriptorParameter->defaultExpression), juce::dontSendNotification);
            colourValueText();
            textValue_.setVisible(true);
            applyButton_.setVisible(true);
            break;
        case talas::core::ParameterKind::Resource:
        {
            // A file the field names shows as the preferences say, with
            // its whole path or its name alone.
            const auto id = override != nullptr && override->resource.has_value()
                ? *override->resource
                : descriptorParameter->defaultResource;
            const auto shown = fileNameDisplay_ ? fileNameDisplay_(id) : std::optional<std::string>{};
            shownResource_ = shown ? std::optional<std::pair<std::string, std::string>>{{*shown, id}} : std::nullopt;
            textValue_.setText(juceString(shown.value_or(id)), juce::dontSendNotification);
        }
            textValue_.setTextToShowWhenEmpty(
                "A project resource, or one of the built-in wavetables such as Sine, Hann or FullRamp",
                secondaryTextColour);
            textValue_.setVisible(true);
            applyButton_.setVisible(true);
            browseButton_.setVisible(true);
            // Beside the file it may stand in for, the
            // check box reading a MemoryWriter's recording instead.
            if (selectedDescriptor_.has_value())
                fromMemoryWriterParameter_ = talas::app::fromMemoryWriterCheckBox(*selectedDescriptor_, descriptorParameter->id);
            if (fromMemoryWriterParameter_.has_value())
            {
                const auto* set = findParameter(*selectedNode, *fromMemoryWriterParameter_);
                const auto declared = std::ranges::find(selectedDescriptor_->parameters, *fromMemoryWriterParameter_,
                    &talas::core::ParameterDescriptor::id);
                fromMemoryWriterBox_.setToggleState(set != nullptr && set->boolean.has_value()
                        ? *set->boolean
                        : declared != selectedDescriptor_->parameters.end() && declared->defaultBoolean.value_or(false),
                    juce::dontSendNotification);
                fromMemoryWriterBox_.setVisible(true);
            }
            break;
        case talas::core::ParameterKind::Text:
            if (midiMapperMap)
            {
                textValue_.setTitle(
                    juceString("MIDI Mapper local Map · one association per line"));
                textValue_.setDescription(
                    legacyMapper
                        ? "Enter up to 16 local Event Value associations. Return adds a line; Apply saves the Map. Unlisted values use global bindings. Legacy mode does not create MIDI-triggered Input copies."
                        : "Enter up to 16 local Event Value associations. Return adds a line; Apply saves the Map. Unlisted values use global bindings inside every MIDI-triggered Input copy.");
            }
            else if (midiNoteScript)
            {
                textValue_.setTitle(
                    juceString("MIDI Script · one literal note per line"));
                textValue_.setDescription(
                    "Enter bounded self keyDownAt: notes. Return adds a line; Apply saves the Script. General Smalltalk programs are not yet supported.");
            }
            else if (parameterTransformation)
            {
                textValue_.setTitle(
                    juceString("Parameter transformation · bounded compile-time set/scale rules"));
                textValue_.setDescription(
                    "One closed rule per line. Rules rewrite matching Expression parameters only across this transformer's exclusive upstream audio closure before compilation.");
            }
            textValue_.setText(juceString(override != nullptr && override->text.has_value()
                ? *override->text
                : descriptorParameter->defaultText), juce::dontSendNotification);
            textValue_.setVisible(true);
            applyButton_.setVisible(true);
            // An envelope breakpoint field and an array
            // field also edit graphically.
            // A DiskSplicer's Segments field edits the
            // spliced files graphically.
            if (selectedNode->typeId == "talas.diskSplicer" && descriptorParameter->id == "segments")
            {
                splicerField_->showSegments(override != nullptr && override->text.has_value()
                        ? *override->text
                        : descriptorParameter->defaultText);
            }
            else if (!multilineText && descriptorParameter->id != "eventValues")
            {
                const auto envelope = selectedNode->typeId == "talas.graphicalEnvelope" && descriptorParameter->id == "envelope";
                const auto* rate = envelope ? findParameter(*selectedNode, "rate") : nullptr;
                fieldEditor_->showField(
                    override != nullptr && override->text.has_value()
                        ? *override->text
                        : descriptorParameter->defaultText,
                    envelope,
                    talas::app::envelopeFieldRate(rate != nullptr ? rate->expression.value_or("1") : "1"));
            }
            break;
        case talas::core::ParameterKind::Choice:
        {
            choiceValue_.clear(juce::dontSendNotification);
            for (std::size_t index = 0; index < descriptorParameter->choices.size(); ++index)
                choiceValue_.addItem(juceString(descriptorParameter->choices[index]), static_cast<int>(index + 1));
            const auto selected = override != nullptr && override->choice.has_value()
                ? *override->choice
                : descriptorParameter->defaultChoice;
            choiceValue_.setText(juceString(selected), juce::dontSendNotification);
            // A class's radioButtons field: the choices as
            // radio buttons in one column or two, the pop-up hidden.
            const auto* classParameter = selectedClassParameter(*selectedNode, descriptorParameter->id);
            radioColumns_ = classParameter != nullptr ? talas::app::radioButtonColumns(*classParameter) : 0;
            if (radioColumns_ == 0)
            {
                choiceValue_.setVisible(true);
                break;
            }
            const auto sameChoices = radioChoices_.size() == descriptorParameter->choices.size()
                && std::ranges::equal(radioChoices_, descriptorParameter->choices, {},
                    [](const auto& button) { return button->getButtonText().toStdString(); });
            if (!sameChoices)
            {
                radioChoices_.clear();
                for (const auto& choice : descriptorParameter->choices)
                {
                    auto button = std::make_unique<juce::ToggleButton>(juceString(choice));
                    button->setRadioGroupId(radioChoiceGroup);
                    button->onClick = [this, choice] {
                        choiceValue_.setText(juceString(choice), juce::dontSendNotification);
                        commitParameter();
                    };
                    addChildComponent(*button);
                    radioChoices_.push_back(std::move(button));
                }
            }
            for (const auto& button : radioChoices_)
                button->setToggleState(button->getButtonText() == juceString(selected), juce::dontSendNotification);
            showRadioChoices(true);
            break;
        }
        case talas::core::ParameterKind::Boolean:
            booleanValue_.setButtonText(descriptorParameter->id == "" ? "Enabled" : juceString(descriptorParameter->id));
            booleanValue_.setToggleState(override != nullptr && override->boolean.has_value()
                ? *override->boolean
                : descriptorParameter->defaultBoolean.value_or(false), juce::dontSendNotification);
            booleanValue_.setVisible(true);
            break;
        }
        resized();
    }

    void showRadioChoices(bool visible)
    {
        for (const auto& button : radioChoices_)
            button->setVisible(visible);
    }

    [[nodiscard]] bool radioChoicesShown() const
    {
        return !radioChoices_.empty() && radioChoices_.front()->isVisible();
    }

    // The selected instance's parameter as its class defines it.
    [[nodiscard]] const talas::project::ProjectSoundExposedParameter* selectedClassParameter(
        const talas::project::ProjectNode& node, std::string_view parameterId) const
    {
        if (!node.soundDefinitionId.has_value())
            return nullptr;
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto definition = std::ranges::find(definitions, *node.soundDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == definitions.end())
            return nullptr;
        const auto exposed = std::ranges::find(definition->exposedParameters, parameterId,
            &talas::project::ProjectSoundExposedParameter::parameterId);
        return exposed == definition->exposedParameters.end() ? nullptr : &*exposed;
    }

    void setValueControlsVisible(bool visible)
    {
        fieldEditor_->hideField();
        splicerField_->setVisible(false);
        showRadioChoices(false);
        textValue_.setVisible(visible);
        choiceValue_.setVisible(visible);
        booleanValue_.setVisible(visible);
        applyButton_.setVisible(visible);
        browseButton_.setVisible(false);
        fromMemoryWriterBox_.setVisible(false);
        resized();
    }

    // A graphical field editor's new text for the selected parameter.
    void commitFieldEditorText(std::string text)
    {
        const auto* descriptorParameter = selectedParameterDescriptor();
        if (descriptorParameter == nullptr || selectedNodeId_.empty()
            || descriptorParameter->kind != talas::core::ParameterKind::Text)
            return;
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = descriptorParameter->id;
        parameter.text = std::move(text);
        const auto result = soundDefinitionContext_.has_value()
            ? bridge_.editor().setParameterInSoundDefinition(
                  *soundDefinitionContext_, selectedNodeId_, std::move(parameter))
            : bridge_.editor().setParameter(selectedNodeId_, std::move(parameter));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refreshParameterValue();
            return;
        }
        onDocumentChange_();
        refreshParameterValue();
        onStatus_("Parameter updated");
    }

    // An expression dragged from the
    // Expressions Library or a Sound Browser into a value field takes the
    // place of the text selected there, as a paste does, or of all of it.
    bool acceptExpressionDrop(const juce::String& description)
    {
        const auto expression = talas::app::expressionFromLibraryDrag(description.toStdString());
        const auto* descriptorParameter = selectedParameterDescriptor();
        if (!expression.has_value() || descriptorParameter == nullptr
            || descriptorParameter->kind != talas::core::ParameterKind::Expression
            || !textValue_.isVisible())
            return false;
        const auto text = textValue_.getText().toStdString();
        const auto selected = textValue_.getHighlightedRegion();
        const auto prefix = textValue_.getText().substring(0, selected.getStart()).toStdString();
        const auto chosen = textValue_.getText().substring(selected.getStart(), selected.getEnd()).toStdString();
        textValue_.setText(juceString(talas::app::fieldTextAfterExpressionDrop(
                               text, prefix.size(), prefix.size() + chosen.size(), *expression)),
            juce::dontSendNotification);
        commitParameter();
        return true;
    }

    // A file's name dragged from the File Organizer: a file name
    // field takes the file; another field the file's
    // value where it means one, a duration its length; else it is refused
    // and the field left as it was.
    bool acceptFileDrop(const juce::String& description)
    {
        const auto file = talas::app::talasFileFromDrag(description.toStdString());
        const auto* descriptorParameter = selectedParameterDescriptor();
        if (!file.has_value() || descriptorParameter == nullptr || selectedNodeId_.empty())
            return false;
        if (descriptorParameter->kind == talas::core::ParameterKind::Resource)
        {
            if (!locateResource_
                || !locateResource_(soundDefinitionContext_, selectedNodeId_, descriptorParameter->id, file->string()))
                return false;
            refreshParameterValue();
            return true;
        }
        const auto value = descriptorParameter->kind == talas::core::ParameterKind::Expression
            ? talas::app::talasFileFieldValue(*file, descriptorParameter->domain)
            : std::optional<std::string>{};
        if (!value.has_value() || !textValue_.isVisible())
            return false;
        textValue_.setText(juceString(*value), juce::dontSendNotification);
        commitParameter();
        return true;
    }

    // A Sound dragged into a field: the field
    // takes the Sound's value for its parameter of that name, or refuses it.
    // A prototype gives its own; a Sound of a signal flow its set value.
    bool acceptSoundDrop(const juce::String& description)
    {
        const auto* descriptorParameter = selectedParameterDescriptor();
        const auto fromSignalFlow = talas::app::signalFlowSoundDrag(description.toStdString()).has_value();
        // Into an input field, the Sound joins that input; so
        // does a Sound of the Sound file window.
        if (const auto port = selectedInputPort();
            port.has_value()
            && (fromSignalFlow || description.startsWith(juceString(prototypeDragPrefix))
                || description.startsWith(juceString(soundDragPrefix))))
        {
            const auto added = soundDroppedIntoInput_ && soundDroppedIntoInput_(description.toStdString(), selectedNodeId_, *port);
            if (added)
                showInputField(*port);
            return added;
        }
        if ((!description.startsWith(juceString(prototypeDragPrefix)) && !fromSignalFlow) || descriptorParameter == nullptr
            || !textValue_.isVisible())
            return false;
        std::optional<std::string> value;
        if (fromSignalFlow)
        {
            if (draggedSoundValue_)
                value = draggedSoundValue_(description.toStdString(), descriptorParameter->id);
        }
        else if (const auto* sound = registry_.findNodeType(
                     description.fromFirstOccurrenceOf(juceString(prototypeDragPrefix), false, false).toStdString()))
        {
            value = talas::app::parameterValueOfSound(*sound, descriptorParameter->id);
        }
        if (!value.has_value())
        {
            onStatus_("The Sound has no " + juceString(descriptorParameter->id) + " to give; the field is left as it was");
            return false;
        }
        textValue_.setText(juceString(*value), juce::dontSendNotification);
        commitParameter();
        return true;
    }

public:
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        const auto description = details.description.toString().toStdString();
        return talas::app::expressionFromLibraryDrag(description).has_value() || talas::app::talasFileFromDrag(description).has_value()
            || description.starts_with(prototypeDragPrefix) || talas::app::signalFlowSoundDrag(description).has_value()
            || (description.starts_with(soundDragPrefix) && selectedInputPort().has_value());
    }

    [[nodiscard]] bool dropSoundForSmoke(const juce::String& description) override
    {
        return acceptSoundDrop(description);
    }

    void itemDropped(const SourceDetails& details) override
    {
        const auto description = details.description.toString();
        if (description.startsWith(juceString(prototypeDragPrefix))
            || talas::app::signalFlowSoundDrag(description.toStdString()).has_value()
            || description.startsWith(juceString(soundDragPrefix)))
        {
            (void)acceptSoundDrop(description);
            return;
        }
        if (talas::app::talasFileFromDrag(description.toStdString()).has_value())
        {
            if (!acceptFileDrop(description))
                onStatus_("The file means nothing to this field; it is left as it was");
            return;
        }
        if (!acceptExpressionDrop(description))
            onStatus_("Drop an expression on the value field of an expression parameter");
    }

    [[nodiscard]] bool dropFileForSmoke(const std::filesystem::path& path) override
    {
        return acceptFileDrop(juceString(talas::app::talasFileDragDescription(path)));
    }

    [[nodiscard]] bool dropExpressionForSmoke(const juce::String& description) override
    {
        return acceptExpressionDrop(description);
    }

    [[nodiscard]] ParameterFieldEditor* fieldEditorForSmoke() noexcept override
    {
        return fieldEditor_->isVisible() ? fieldEditor_.get() : nullptr;
    }

    void setSoundPasteHandler(std::function<std::optional<std::string>()> handler) override
    {
        textValue_.soundToPaste = std::move(handler);
    }

    [[nodiscard]] bool holdsIllegalValue() const override
    {
        return illegalValueHeld_;
    }

    bool testTypedValue() override
    {
        if (valueEdited_ && !illegalValueHeld_)
            commitParameter();
        return !illegalValueHeld_;
    }

    void dropIllegalValue() override
    {
        illegalValueHeld_ = false;
        refreshParameterValue();
    }

    void setLargeFieldIcons(bool large) override
    {
        soundIcons_.setLarge(large);
        resized();
    }

    void setInputPasteSpecialHandler(std::function<bool(const std::string&, const std::string&)> handler) override
    {
        textValue_.pasteSpecialSound = [this, handler = std::move(handler)] {
            const auto port = selectedInputPort();
            if (!port.has_value() || !handler || !handler(selectedNodeId_, *port))
                return false;
            showInputField(*port);
            return true;
        };
    }

    bool pasteSpecialIntoFocusedInput() override
    {
        return textValue_.hasKeyboardFocus(false) && textValue_.pasteSpecialSound && textValue_.pasteSpecialSound();
    }

    void setDescribeSoundHandler(std::function<void(const talas::core::NodeTypeDescriptor&)> handler) override
    {
        describeSound_ = std::move(handler);
    }

    void setResourceLocator(ResourceLocator locator) override
    {
        locateResource_ = std::move(locator);
    }

    void setSoundDroppedIntoInput(SoundDroppedIntoInput dropped) override { soundDroppedIntoInput_ = std::move(dropped); }

    void setDraggedSoundValue(DraggedSoundValue value) override
    {
        draggedSoundValue_ = std::move(value);
    }

    void setLeaveFieldsHandler(std::function<void()> handler) override
    {
        leaveFields_ = std::move(handler);
    }

    void setFileNameDisplay(FileNameDisplay display) override
    {
        fileNameDisplay_ = std::move(display);
        refreshParameterValue();
    }

    [[nodiscard]] std::pair<std::string, bool> tabFromValueForSmoke(std::string_view typed, bool forward) override
    {
        if (!typed.empty())
        {
            textValue_.setText(juceString(std::string{typed}), juce::sendNotificationSync);
            valueEdited_ = true;
        }
        if (!tabKeys_.keyPressed(juce::KeyPress{juce::KeyPress::tabKey,
                                     forward ? juce::ModifierKeys{} : juce::ModifierKeys{juce::ModifierKeys::shiftModifier}, 0},
                &textValue_))
            return {};
        const auto all = textValue_.getHighlightedRegion().getLength() == textValue_.getTotalNumChars();
        return {parameterBox_.getText().toStdString(), all};
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (event.eventComponent == &nodeLabel_ || event.eventComponent == &className_)
            describeSelectedSound();
    }

    void describeSelectedSound()
    {
        if (selectedDescriptor_.has_value() && describeSound_)
            describeSound_(*selectedDescriptor_);
    }

    [[nodiscard]] std::pair<juce::String, bool> classNameCornerForSmoke() override
    {
        const auto bounds = className_.getBounds();
        return {className_.isVisible() ? className_.getText() : juce::String{},
            bounds.getCentreX() > getWidth() / 2 && bounds.getCentreY() > getHeight() / 2};
    }

    void clickClassNameCornerForSmoke() override
    {
        describeSelectedSound();
    }

    [[nodiscard]] std::pair<juce::String, juce::String> helpTooltipsForSmoke() override
    {
        return {nodeLabel_.getTooltip(), parameterBox_.getTooltip()};
    }

    void setBackgroundDoubleClick(std::function<void()> handler) override
    {
        backgroundDoubleClick_ = std::move(handler);
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (event.eventComponent == this && backgroundDoubleClick_)
            backgroundDoubleClick_();
    }

    [[nodiscard]] bool parameterNameItalicForSmoke() override
    {
        return parameterBox_.getLookAndFeel().getComboBoxFont(parameterBox_).isItalic();
    }

    [[nodiscard]] std::size_t commitValueForSmoke(std::string_view text) override
    {
        textValue_.setText(juceString(std::string{text}), juce::dontSendNotification);
        commitParameter();
        return illegalValueSignals_;
    }

    // An illegal value: the field flashes and the
    // computer beeps twice.
    void signalIllegalValue()
    {
        ++illegalValueSignals_;
        if (!flashing_)
            fieldColour_ = textValue_.findColour(juce::TextEditor::backgroundColourId);
        flashing_ = true;
        const auto signal = talas::app::illegalFieldSignal();
        for (const auto at : signal.beepMilliseconds)
            juce::Timer::callAfterDelay(std::max(1, at), [] { juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound(); });
        juce::Component::SafePointer<InspectorPanelImpl> safeThis(this);
        for (std::size_t index = 0; index < signal.flashMilliseconds.size(); ++index)
        {
            const auto last = index + 1 == signal.flashMilliseconds.size();
            juce::Timer::callAfterDelay(std::max(1, signal.flashMilliseconds[index]), [safeThis, last, flashed = index % 2 == 0] {
                if (safeThis == nullptr)
                    return;
                safeThis->textValue_.setColour(juce::TextEditor::backgroundColourId,
                    flashed ? errorColour.withAlpha(0.55F) : safeThis->fieldColour_);
                safeThis->textValue_.repaint();
                if (last)
                    safeThis->flashing_ = false;
            });
        }
    }

    // The parameter box's name in italics for a hot field.
    void markHotParameterName(bool hot)
    {
        if (static_cast<bool>(parameterBox_.getProperties()["hotField"]) == hot)
            return;
        parameterBox_.getProperties().set("hotField", hot);
        parameterBox_.resized();
        parameterBox_.repaint();
    }

    void clickClassNameForSmoke() override
    {
        describeSelectedSound();
    }

    [[nodiscard]] std::string pasteIntoValueForSmoke(std::string_view text) override
    {
        textValue_.setText(juceString(std::string{text}), juce::dontSendNotification);
        textValue_.selectAll();
        (void) textValue_.keyPressed(juce::KeyPress{'v', juce::ModifierKeys::commandModifier, 0});
        return textValue_.getText().toStdString();
    }

    [[nodiscard]] std::string pasteSpecialForSmoke(std::string_view copied, std::string_view into) override
    {
        textValue_.setText(juceString(std::string{copied}), juce::dontSendNotification);
        textValue_.selectAll();
        (void) textValue_.keyPressed(juce::KeyPress{'c', juce::ModifierKeys::commandModifier, 0});
        textValue_.setText(juceString(std::string{into}), juce::dontSendNotification);
        textValue_.moveCaretToEnd();
        (void) textValue_.pasteSpecial(0);
        return textValue_.getText().toStdString();
    }

    [[nodiscard]] std::string doubleClickValueForSmoke(std::string_view text, int index) override
    {
        textValue_.setText(juceString(std::string{text}), juce::dontSendNotification);
        textValue_.selectAsDoubleClickAt(index);
        return textValue_.getHighlightedText().toStdString();
    }

    bool zoomActiveField(bool in) override
    {
        const auto focused = [](juce::Component& editor) {
            return editor.isShowing() && (editor.hasKeyboardFocus(true));
        };
        if (fieldEditor_->mode() == ParameterFieldEditor::Mode::Envelope && focused(*fieldEditor_))
        {
            fieldEditor_->zoom(in);
            return true;
        }
        if (focused(*splicerField_))
            return splicerField_->perform(in ? "Time In" : "Time Out");
        return false;
    }

    void setDiskSplicerHost(DiskSplicerField::Host host) override
    {
        splicerField_->setHost(std::move(host));
    }

    [[nodiscard]] DiskSplicerField* diskSplicerFieldForSmoke() noexcept override
    {
        return splicerField_->isVisible() ? splicerField_.get() : nullptr;
    }

    [[nodiscard]] juce::String parameterValueTextForSmoke() const override
    {
        return textValue_.getText();
    }

    [[nodiscard]] bool selectParameterForSmoke(std::string_view parameterId) override
    {
        for (int index = 0; index < parameterBox_.getNumItems(); ++index)
        {
            if (parameterBox_.getItemText(index) == juceString(std::string{parameterId}))
            {
                parameterBox_.setSelectedItemIndex(index, juce::sendNotificationSync);
                refreshParameterValue();
                return true;
            }
        }
        return false;
    }

private:
    // The Preset's EventValues become the surface's current
    // values of its Input's Event Values.
    void capturePresetEventValues()
    {
        const auto entries = talas::app::presetCurrentEventValues(
            bridge_.editor().document(),
            soundDefinitionContext_,
            selectedNodeId_);
        if (entries.empty())
        {
            onStatus_("The Preset's Input uses no Event Value of the "
                      "Virtual Control Surface");
            return;
        }
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = "eventValues";
        parameter.text = entries;
        const auto result = soundDefinitionContext_.has_value()
            ? bridge_.editor().setParameterInSoundDefinition(
                  *soundDefinitionContext_,
                  selectedNodeId_,
                  std::move(parameter))
            : bridge_.editor().setParameter(
                  selectedNodeId_,
                  std::move(parameter));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        onDocumentChange_();
        refreshParameterValue();
        onStatus_("Preset EventValues set to the current Event Values");
    }

    void commitParameter()
    {
        if (const auto port = selectedInputPort())
        {
            commitInputField(*port);
            return;
        }
        const auto* descriptorParameter = selectedParameterDescriptor();
        if (descriptorParameter == nullptr || selectedNodeId_.empty())
            return;

        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = descriptorParameter->id;
        switch (descriptorParameter->kind)
        {
        case talas::core::ParameterKind::Expression:
            parameter.expression = textValue_.getText().toStdString();
            if (const auto parsed = talas::expr::parse(
                    *parameter.expression,
                    bridge_.editor().document().expressionLanguage,
                    bridge_.editor().document().expressionDefinitions);
                !parsed.ok())
            {
                const auto& diagnostic = parsed.diagnostics.front();
                onStatus_(
                    "Expression rejected at bytes "
                    + juce::String(
                        static_cast<juce::int64>(diagnostic.range.offset))
                    + "-"
                    + juce::String(static_cast<juce::int64>(
                        diagnostic.range.offset
                        + diagnostic.range.length))
                    + ": " + juceString(diagnostic.message));
                signalIllegalValue();
                illegalValueHeld_ = true;
                if (textValue_.isShowing())
                    textValue_.grabKeyboardFocus();
                return;
            }
            else if (parsed.expression->eventReferences().empty()
                     && parsed.expression->identifiers().empty()
                     && !parsed.expression->isStateful())
            {
                const auto evaluated = talas::expr::evaluate(
                    *parsed.expression,
                    {},
                    {descriptorParameter->domain});
                if (!evaluated.ok())
                {
                    const auto& diagnostic =
                        evaluated.diagnostics.front();
                    onStatus_(
                        "Expression rejected at bytes "
                        + juce::String(static_cast<juce::int64>(
                            diagnostic.range.offset))
                        + "-"
                        + juce::String(static_cast<juce::int64>(
                            diagnostic.range.offset
                            + diagnostic.range.length))
                        + ": " + juceString(diagnostic.message));
                    signalIllegalValue();
                    illegalValueHeld_ = true;
                    if (textValue_.isShowing())
                        textValue_.grabKeyboardFocus();
                    return;
                }
            }
            break;
        case talas::core::ParameterKind::Choice:
            parameter.choice = choiceValue_.getText().toStdString();
            break;
        case talas::core::ParameterKind::Boolean:
            parameter.boolean = booleanValue_.getToggleState();
            break;
        case talas::core::ParameterKind::Resource:
            // The file as shown, unchanged.
            if (shownResource_ && textValue_.getText().toStdString() == shownResource_->first)
            {
                refreshParameterValue();
                return;
            }
            // A file's name typed into the field, located among the
            // frequently used folders.
            if (locateResource_
                && locateResource_(soundDefinitionContext_, selectedNodeId_, parameter.parameterId,
                    textValue_.getText().toStdString()))
            {
                refreshParameterValue();
                return;
            }
            parameter.resource = textValue_.getText().toStdString();
            break;
        case talas::core::ParameterKind::Text:
            parameter.text = textValue_.getText().toStdString();
            // An array parameter set to a variable
            // asks whether it is an array of that one variable or the
            // variable stands for the whole array.
            if (const auto variable = talas::app::arrayParameterVariableToAsk(*descriptorParameter, *parameter.text);
                variable.has_value() && !arrayVariableAnswered_)
            {
                const auto* node = currentSelectedNode();
                const auto* stored = node == nullptr ? nullptr : findParameter(*node, descriptorParameter->id);
                if ((stored == nullptr || stored->text != parameter.text) && isShowing())
                {
                    askArrayVariableMeaning(*variable);
                    return;
                }
            }
            break;
        }

        const auto result = soundDefinitionContext_.has_value()
            ? bridge_.editor().setParameterInSoundDefinition(
                  *soundDefinitionContext_,
                  selectedNodeId_,
                  std::move(parameter))
            : bridge_.editor().setParameter(
                  selectedNodeId_,
                  std::move(parameter));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refreshParameterValue();
            signalIllegalValue();
            return;
        }

        onDocumentChange_();
        valueEdited_ = false;
        illegalValueHeld_ = false;
        onStatus_(
            result.message.empty()
                ? juce::String{"Parameter updated"}
                : juceString(result.message));
    }

    // The question, and the field set to {?name} or ?name.
    void askArrayVariableMeaning(const std::string& name)
    {
        const auto question = talas::app::arrayVariableQuestion(name);
        auto* window = new juce::AlertWindow("Array parameter", juceString(question.message),
            juce::MessageBoxIconType::QuestionIcon, this);
        window->addButton(juceString(question.arrayOfTheVariable), 1);
        window->addButton(juceString(question.variableIsTheArray), 2, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<InspectorPanelImpl> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, name](int result) {
                if (safeThis == nullptr)
                    return;
                if (result == 0)
                {
                    safeThis->refreshParameterValue();
                    return;
                }
                const auto meaning = result == 1 ? talas::app::ArrayVariableMeaning::ArrayOfTheVariable
                                                 : talas::app::ArrayVariableMeaning::VariableIsTheArray;
                safeThis->textValue_.setText(
                    juceString(talas::app::arrayParameterVariableText(name, meaning)), juce::dontSendNotification);
                safeThis->arrayVariableAnswered_ = true;
                safeThis->commitParameter();
                safeThis->arrayVariableAnswered_ = false;
            }),
            true);
    }

    // The file name field's FromMemoryWriter check box sets
    // its Boolean parameter.
    void commitFromMemoryWriter()
    {
        if (!fromMemoryWriterParameter_.has_value() || selectedNodeId_.empty())
            return;
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = *fromMemoryWriterParameter_;
        parameter.boolean = fromMemoryWriterBox_.getToggleState();
        const auto result = soundDefinitionContext_.has_value()
            ? bridge_.editor().setParameterInSoundDefinition(*soundDefinitionContext_, selectedNodeId_, std::move(parameter))
            : bridge_.editor().setParameter(selectedNodeId_, std::move(parameter));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refreshParameterValue();
            return;
        }
        onDocumentChange_();
        onStatus_(fromMemoryWriterBox_.getToggleState() ? "Reads the MemoryWriter's recording" : "Reads the file");
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    BrowseResourceCallback onBrowseResource_;
    PreservedNodeActionCallback onPreservedNodeAction_;
    ExpressionLanguageUpgradeCallback
        onExpressionLanguageUpgrade_;
    ExpressionLanguageUpgradeCallback
        onTimedExpressionLanguageUpgrade_;
    DiskCacheCaptureProjectionCallback diskCacheCaptureProjection_;
    DiskCacheCaptureActionCallback onDiskCacheCaptureAction_;
    std::string selectedNodeId_;
    std::vector<std::string> inputPorts_;
    std::optional<std::string> soundDefinitionContext_;
    std::optional<std::string> focusedParameterId_;
    // The value field's own colour, and how many illegal values it has
    // signalled.
    std::function<void()> backgroundDoubleClick_;
    juce::Colour fieldColour_;
    bool flashing_ = false;
    std::size_t illegalValueSignals_ = 0;
    std::optional<std::string> selectedDefinitionName_;
    std::optional<talas::core::NodeTypeDescriptor> selectedDescriptor_;
    std::optional<talas::app::MissingNodeWorkbenchProjection>
        preservedProjection_;
    juce::Label nodeLabel_;
    // The class's name at the lower right of the parameters, a click on it
    // its help.
    juce::Label className_;
    std::function<void(const talas::core::NodeTypeDescriptor&)> describeSound_;
    ResourceLocator locateResource_;
    DraggedSoundValue draggedSoundValue_;
    SoundDroppedIntoInput soundDroppedIntoInput_;
    FileNameDisplay fileNameDisplay_;
    // The file name a file field shows, and the resource it stands for.
    std::optional<std::pair<std::string, std::string>> shownResource_;
    juce::TextButton nodeModeButton_;
    juce::TextButton variablesModeButton_;
    juce::TextButton generatorsModeButton_;
    juce::Label expressionLanguageLabel_;
    juce::TextButton upgradeExpressionsButton_;
    juce::TextButton upgradeTimedExpressionsButton_;
    juce::Label definitionTitle_;
    juce::ComboBox definitionBox_;
    juce::TextButton newDefinitionButton_;
    juce::TextEditor definitionName_;
    juce::TextEditor definitionParameters_;
    juce::ComboBox definitionResultDomain_;
    juce::TextEditor definitionSource_;
    juce::TextButton applyDefinitionButton_;
    juce::TextButton deleteDefinitionButton_;
    juce::Label structureFocusLabel_;
    juce::ComboBox parameterBox_;
    juce::TextEditor parameterHint_;
    ParameterValueEditor textValue_;
    // An illegal value typed and refused, left in the field to fix.
    bool illegalValueHeld_ = false;
    // The array variable question answered, so the commit that follows takes the
    // field as the answer wrote it.
    bool arrayVariableAnswered_ = false;
    SoundFieldIcons soundIcons_;
    juce::ComboBox choiceValue_;
    static constexpr int radioChoiceGroup = 0x5241;
    std::vector<std::unique_ptr<juce::ToggleButton>> radioChoices_;
    std::size_t radioColumns_ = 0;
    juce::ToggleButton booleanValue_;
    juce::TextButton applyButton_;
    juce::TextButton browseButton_;
    juce::ToggleButton fromMemoryWriterBox_{"FromMemoryWriter"};
    std::optional<std::string> fromMemoryWriterParameter_;
    juce::Label preservedStatus_;
    juce::TextEditor preservedDetails_;
    juce::TextButton retryButton_;
    juce::TextButton replacePreservedButton_;
    juce::TextButton disconnectButton_;
    juce::TextButton deletePreservedButton_;
    std::unique_ptr<ComponentReplacementPanel>
        componentReplacementPanel_;
    std::unique_ptr<ProjectVariablesPanel> variablesPanel_;
    std::unique_ptr<GeneratorPanel> generatorPanel_;
    ComponentReplacementPresentation
        componentReplacementPresentation_;
    bool componentReplacementEnabled_ = false;
    juce::Label preparedTitle_;
    juce::TextButton diskCacheCaptureButton_;
    juce::TextButton presetCaptureButton_;
    std::unique_ptr<ParameterFieldEditor> fieldEditor_;
    std::unique_ptr<DiskSplicerField> splicerField_;
    juce::TextEditor preparedContract_;
    TypedKeys typedKeys_;
    TabKeys tabKeys_;
    bool valueEdited_ = false;
    // The value field's text as shown, its colours given.
    juce::String shownValueText_;
    std::function<void()> leaveFields_;
    bool characterTyped_ = false;
    bool completingHotValue_ = false;
    bool variablesMode_ = false;
    bool generatorsMode_ = false;
};

}

std::unique_ptr<InspectorPanel> makeInspectorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    InspectorPanel::DocumentChangeCallback onDocumentChange,
    ProjectVariablesPanel::LiveMutationCallback onVariableLiveMutation,
    InspectorPanel::StatusCallback onStatus,
    InspectorPanel::BrowseResourceCallback onBrowseResource,
    InspectorPanel::PreservedNodeActionCallback onPreservedNodeAction,
    InspectorPanel::ExpressionLanguageUpgradeCallback
        onExpressionLanguageUpgrade,
    InspectorPanel::ExpressionLanguageUpgradeCallback
        onTimedExpressionLanguageUpgrade,
    InspectorPanel::DiskCacheCaptureProjectionCallback
        diskCacheCaptureProjection,
    InspectorPanel::DiskCacheCaptureActionCallback
        onDiskCacheCaptureAction)
{
    return std::make_unique<InspectorPanelImpl>(
        bridge,
        registry,
        std::move(onDocumentChange),
        std::move(onVariableLiveMutation),
        std::move(onStatus),
        std::move(onBrowseResource),
        std::move(onPreservedNodeAction),
        std::move(onExpressionLanguageUpgrade),
        std::move(onTimedExpressionLanguageUpgrade),
        std::move(diskCacheCaptureProjection),
        std::move(onDiskCacheCaptureAction));
}
}
