#include "app/workbench/WorkbenchClassEditor.h"

#include "app/workbench/WorkbenchSoundIconPainting.h"
#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/ClassEditorModel.h"
#include "talas/app/SoundClassIcon.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace talas::app::workbench
{
namespace
{
// The icon editor: the large
// or small icon a dot at a time, a click on a white dot drawing and on a
// black one erasing, continuously while the button is held; the icon at
// its actual size beside it; another Sound dragged in gives its icon.
class SoundIconEditor final : public juce::Component, public juce::DragAndDropTarget
{
public:
    std::function<void()> onChange;
    talas::project::ProjectSoundIcon icon;
    bool large = true;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colours::white);
        const auto grid = gridArea();
        graphics.setColour(juce::Colours::lightgrey);
        graphics.drawRect(grid);
        if (!icon.sameAs.empty() && icon.large.empty() && icon.small.empty())
        {
            graphics.setColour(juce::Colours::darkgrey);
            graphics.setFont(WorkbenchType::label());
            graphics.drawFittedText("The icon of " + juceString(icon.sameAs), grid, juce::Justification::centred, 2);
        }
        drawSoundIconPicture(graphics, grid.toFloat(), icon, large, juce::Colours::black);
        // The icon at its actual size.
        const auto actual = previewArea();
        graphics.setColour(juce::Colours::lightgrey);
        graphics.drawRect(actual.expanded(1));
        drawSoundIconPicture(graphics, actual.toFloat(), icon, large, juce::Colours::black);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto dot = dotAt(event.getPosition());
        if (!dot.has_value())
            return;
        drawing_ = !talas::app::soundIconDot(icon, large, *dot);
        last_ = *dot;
        stroke(*dot, *dot);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const auto dot = dotAt(event.getPosition());
        if (!dot.has_value() || !last_.has_value())
            return;
        stroke(*last_, *dot);
        last_ = *dot;
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        last_.reset();
    }

    void stroke(talas::app::SoundIconDot from, talas::app::SoundIconDot to)
    {
        talas::app::drawSoundIconStroke(icon, large, talas::app::soundIconStroke(from, to), drawing_);
        repaint();
        if (onChange)
            onChange();
    }

    // The Sound or prototype a drag description names.
    [[nodiscard]] static std::optional<std::string> draggedSound(const juce::var& description)
    {
        const auto text = description.toString().toStdString();
        for (const auto prefix : {prototypeDragPrefix, soundDragPrefix})
        {
            if (text.starts_with(prefix) && text.size() > prefix.size())
                return text.substr(prefix.size());
        }
        return std::nullopt;
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return draggedSound(details.description).has_value();
    }

    void itemDropped(const SourceDetails& details) override
    {
        (void)drop(details.description);
    }

    bool drop(const juce::var& description)
    {
        const auto sound = draggedSound(description);
        if (!sound.has_value())
            return false;
        // The large and small icons become that Sound's.
        const auto png = icon.png;
        icon = talas::app::soundIconSameAs(*sound);
        icon.png = png;
        repaint();
        if (onChange)
            onChange();
        return true;
    }

private:
    [[nodiscard]] juce::Rectangle<int> gridArea() const
    {
        const auto width = static_cast<int>(talas::app::soundIconWidth(large));
        const auto height = static_cast<int>(talas::app::soundIconHeight(large));
        const auto bounds = getLocalBounds().withTrimmedRight(80).reduced(2);
        const auto dot = std::max(1, std::min(bounds.getWidth() / width, bounds.getHeight() / height));
        return bounds.withSize(dot * width, dot * height);
    }

    [[nodiscard]] juce::Rectangle<int> previewArea() const
    {
        return getLocalBounds().removeFromRight(76).withSizeKeepingCentre(
            static_cast<int>(talas::app::soundIconWidth(large)), static_cast<int>(talas::app::soundIconHeight(large)));
    }

    [[nodiscard]] std::optional<talas::app::SoundIconDot> dotAt(juce::Point<int> point) const
    {
        const auto grid = gridArea();
        if (!grid.contains(point))
            return std::nullopt;
        const auto dot = grid.getWidth() / static_cast<int>(talas::app::soundIconWidth(large));
        return talas::app::SoundIconDot{static_cast<std::size_t>((point.x - grid.getX()) / dot),
            static_cast<std::size_t>((point.y - grid.getY()) / dot)};
    }

    bool drawing_ = true;
    std::optional<talas::app::SoundIconDot> last_;
};

// The Field Layout: a miniature Sound
// editor with each field where its rectangle puts it, the selected one gray.
class FieldLayoutView final : public juce::Component
{
public:
    std::vector<talas::app::ClassEditorLayoutBox> boxes;

    void paint(juce::Graphics& graphics) override
    {
        const auto area = getLocalBounds().reduced(1).toFloat();
        graphics.setColour(backgroundColour);
        graphics.fillRect(area);
        graphics.setColour(secondaryTextColour);
        graphics.drawRect(area);
        graphics.setFont(WorkbenchType::micro());
        for (const auto& box : boxes)
        {
            const auto field = juce::Rectangle<float>::leftTopRightBottom(
                area.getX() + static_cast<float>(box.field.left) * area.getWidth(),
                area.getY() + static_cast<float>(box.field.top) * area.getHeight(),
                area.getX() + static_cast<float>(box.field.right) * area.getWidth(),
                area.getY() + static_cast<float>(box.field.bottom) * area.getHeight())
                                   .reduced(1.0F);
            if (box.highlighted)
            {
                graphics.setColour(juce::Colours::grey);
                graphics.fillRect(field);
            }
            // A field overlapping another is outlined in red.
            graphics.setColour(box.overlaps ? juce::Colours::red : textColour);
            graphics.drawRect(field);
            graphics.setColour(textColour);
            graphics.drawText(juce::String{box.name}, field.reduced(2.0F), juce::Justification::topLeft, true);
        }
    }
};

// The Class Editor's input and output types, in the order of its lists
//.
constexpr std::array<const char*, 8> iconTypeNames{
    "none", "audio", "subaudio", "spectrum", "transparent", "constructor", "variable", "environment"};

[[nodiscard]] int iconTypeItem(const std::string& type)
{
    const auto found = std::ranges::find(iconTypeNames, type);
    return found == iconTypeNames.end() ? 1 : static_cast<int>(found - iconTypeNames.begin()) + 2;
}

[[nodiscard]] std::string iconTypeOfItem(int item)
{
    return item < 2 || item - 2 >= static_cast<int>(iconTypeNames.size()) ? std::string{}
                                                                           : std::string{iconTypeNames[static_cast<std::size_t>(item - 2)]};
}

class ClassEditorPanelImpl final : public ClassEditorPanel, public juce::DragAndDropTarget, private juce::ListBoxModel
{
public:
    ClassEditorPanelImpl(talas::runtime::ProjectRuntimeBridge& bridge, const talas::core::NodeRegistry& registry,
        DocumentChangeCallback onDocumentChange, StatusCallback onStatus)
        : bridge_(bridge)
        , registry_(registry)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
    {
        setTitle("Class Editor");
        definitions_.setTitle("Sound class");
        definitions_.onChange = [this] { showDefinition(); };
        parameters_.setModel(this);
        parameters_.setTitle("Parameter");
        for (const auto* name :
            {"Name", "Parameter Type", "Type", "Field", "Minimum", "Maximum", "Field Rectangle", "Comment", "Class Description",
                "Field Layout"})
        {
            auto label = std::make_unique<juce::Label>();
            label->setText(name, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            addAndMakeVisible(*label);
            labels_.push_back(std::move(label));
        }
        int id = 1;
        for (const auto* name : {"scalar", "hertz", "seconds", "note number", "samples"})
            type_.addItem(name, id++);
        // The parameter types fill an expression's type
        // and range; Apply keeps them.
        talasType_.setTitle("Parameter type");
        talasType_.onChange = [this] { showTalasType(); };
        // The Field list: the kinds of
        // field the selected parameter's kind can have.
        fieldKind_.setTitle("Field");
        comment_.setMultiLine(true);
        description_.setMultiLine(true);
        // The Expand box: checked, a ParameterTransformer
        // expands the Sound before transforming it.
        expand_.setTooltip("Expand before parameter transformations");
        apply_.onClick = [this] { (void)apply(); };
        iconLabel_.setText("Icon", juce::dontSendNotification);
        iconLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
        smallIcon_.onClick = [this] {
            iconEditor_.large = !smallIcon_.getToggleState();
            iconEditor_.repaint();
        };
        // The PNG icon box, a picture named for the class.
        pngIcon_.setTooltip("Use the class's PNG file, its name in lowercase and .png in a frequently used folder");
        pngIcon_.onClick = [this] { iconEditor_.icon.png = pngIcon_.getToggleState(); };
        clearIcon_.onClick = [this] {
            iconEditor_.icon = {};
            pngIcon_.setToggleState(false, juce::dontSendNotification);
            inputType_.setSelectedId(1, juce::dontSendNotification);
            outputType_.setSelectedId(1, juce::dontSendNotification);
            sideEffects_.setToggleState(false, juce::dontSendNotification);
            inputChannels_.setSelectedId(1, juce::dontSendNotification);
            outputChannels_.setSelectedId(1, juce::dontSendNotification);
            iconEditor_.repaint();
        };
        // The numbers of independent input and output
        // channels, drawn as the lines in and out of the class's Sounds.
        for (auto* box : {&inputChannels_, &outputChannels_})
        {
            const juce::String side = box == &inputChannels_ ? "In: " : "Out: ";
            box->addItem(side + "as drawn", 1);
            box->addItem(side + "1 (mono)", 2);
            box->addItem(side + "2", 3);
            box->onChange = [this] { iconTypesChanged(); };
        }
        inputChannels_.setTitle("Input channels");
        outputChannels_.setTitle("Output channels");
        // The input and output types and the
        // Nonaudio side effects box, shaping the icon's outline only.
        for (auto* box : {&inputType_, &outputType_})
        {
            box->addItem(box == &inputType_ ? "Input: from its inputs" : "Output: from its outputs", 1);
            for (const auto* type : iconTypeNames)
                box->addItem((box == &inputType_ ? "Input: " : "Output: ") + juce::String{type}, iconTypeItem(type));
            box->onChange = [this] { iconTypesChanged(); };
        }
        inputType_.setTitle("Input type");
        outputType_.setTitle("Output type");
        sideEffects_.onClick = [this] { iconTypesChanged(); };
        for (auto* component : std::initializer_list<juce::Component*>{&definitions_, &parameters_, &name_, &talasType_, &type_, &fieldKind_,
                 &minimum_, &maximum_, &field_, &comment_, &description_, &expand_, &apply_, &iconLabel_, &iconEditor_,
                 &smallIcon_, &pngIcon_, &clearIcon_, &inputType_, &outputType_, &sideEffects_, &inputChannels_,
                 &outputChannels_, &fieldLayout_})
            addAndMakeVisible(*component);
        setSize(760, 640);
        refresh();
    }

    void refresh() override
    {
        const auto previous = definitionId_;
        definitions_.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& definition : bridge_.editor().document().soundDefinitions)
            definitions_.addItem(juce::String{definition.displayName + " · " + definition.definitionId}, id++);
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto found = std::ranges::find(definitions, previous, &talas::project::ProjectSoundDefinition::definitionId);
        if (found != definitions.end())
            definitions_.setSelectedItemIndex(static_cast<int>(found - definitions.begin()), juce::dontSendNotification);
        else if (!definitions.empty())
            definitions_.setSelectedItemIndex(0, juce::dontSendNotification);
        showDefinition();
    }

    [[nodiscard]] std::string selectedDefinition() const override { return definitionId_; }

    bool selectDefinitionForSmoke(std::string_view definitionId) override
    {
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto found = std::ranges::find(definitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (found == definitions.end())
            return false;
        definitions_.setSelectedItemIndex(static_cast<int>(found - definitions.begin()), juce::sendNotificationSync);
        return definitionId_ == definitionId;
    }

    bool selectParameterForSmoke(std::string_view parameterId) override
    {
        const auto* definition = current();
        if (definition == nullptr)
            return false;
        const auto found = std::ranges::find(definition->exposedParameters, parameterId,
            &talas::project::ProjectSoundExposedParameter::parameterId);
        if (found == definition->exposedParameters.end())
            return false;
        parameters_.selectRow(static_cast<int>(found - definition->exposedParameters.begin()));
        return parameterId_ == parameterId;
    }

    bool setFieldForSmoke(std::string_view field, const juce::String& text) override
    {
        if (field == "talasType")
        {
            for (int index = 0; index < talasType_.getNumItems(); ++index)
            {
                if (talasType_.getItemText(index) == text)
                {
                    talasType_.setSelectedItemIndex(index, juce::sendNotificationSync);
                    return true;
                }
            }
            return false;
        }
        if (field == "type")
        {
            for (int index = 0; index < type_.getNumItems(); ++index)
            {
                if (type_.getItemText(index) == text)
                {
                    type_.setSelectedItemIndex(index, juce::dontSendNotification);
                    return true;
                }
            }
            return false;
        }
        if (field == "expand")
        {
            expand_.setToggleState(text == "true", juce::dontSendNotification);
            return text == "true" || text == "false";
        }
        if (field == "fieldKind")
        {
            for (int index = 0; index < fieldKind_.getNumItems(); ++index)
            {
                if (fieldKind_.getItemText(index) == text)
                {
                    fieldKind_.setSelectedItemIndex(index, juce::dontSendNotification);
                    return true;
                }
            }
            return false;
        }
        auto* editor = editorFor(field);
        if (editor == nullptr)
            return false;
        editor->setText(text, false);
        return true;
    }

    juce::String fieldForSmoke(std::string_view field) const override
    {
        if (field == "type")
            return type_.getText();
        if (field == "talasType")
            return talasType_.getText();
        if (field == "fieldKind")
            return fieldKind_.getText();
        if (field == "expand")
            return expand_.getToggleState() ? "true" : "false";
        if (field == "fieldLayout")
        {
            // Each field drawn, the highlighted one marked with a *.
            juce::StringArray names;
            for (const auto& box : fieldLayout_.boxes)
                names.add(juce::String{box.name} + (box.highlighted ? "*" : "") + (box.overlaps ? "!" : ""));
            return names.joinIntoString(", ");
        }
        auto* editor = const_cast<ClassEditorPanelImpl*>(this)->editorFor(field);
        return editor == nullptr ? juce::String{} : editor->getText();
    }

    bool applyForSmoke() override
    {
        return apply();
    }

    bool applyChanges() override
    {
        return apply();
    }

    [[nodiscard]] bool hasChangesNotKept() const override
    {
        const auto* definition = current();
        if (definition == nullptr)
            return false;
        const auto icon = iconEditor_.icon == talas::project::ProjectSoundIcon{}
            ? std::nullopt
            : std::optional<talas::project::ProjectSoundIcon>{iconEditor_.icon};
        if (description_.getText().toStdString() != definition->description
            || expand_.getToggleState() != definition->expandsBeforeTransformation || icon != definition->icon)
            return true;
        const auto parameter = std::ranges::find(definition->exposedParameters, parameterId_,
            &talas::project::ProjectSoundExposedParameter::parameterId);
        return parameter != definition->exposedParameters.end()
            && talas::app::classEditorFieldsChanged(*parameter,
                {name_.getText().toStdString(), type_.getText().toStdString(), minimum_.getText().toStdString(),
                    maximum_.getText().toStdString(), field_.getText().toStdString(), comment_.getText().toStdString(),
                    fieldKind_.getText().toStdString()});
    }

    talas::project::ProjectSoundIcon iconForSmoke() const override
    {
        return iconEditor_.icon;
    }

    void strokeIconForSmoke(bool large, std::size_t fromX, std::size_t fromY, std::size_t toX, std::size_t toY) override
    {
        smallIcon_.setToggleState(!large, juce::sendNotificationSync);
        const auto on = !talas::app::soundIconDot(iconEditor_.icon, large, {fromX, fromY});
        talas::app::drawSoundIconStroke(iconEditor_.icon, large, talas::app::soundIconStroke({fromX, fromY}, {toX, toY}), on);
        iconEditor_.repaint();
    }

    bool dropOnIconForSmoke(const juce::String& description) override
    {
        return iconEditor_.drop(description);
    }

    // Another Sound dragged onto the
    // Parameter list gives the parameters its namesakes' types.
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return SoundIconEditor::draggedSound(details.description).has_value();
    }

    void itemDropped(const SourceDetails& details) override
    {
        if (parameters_.getBounds().contains(details.localPosition))
            (void)typeParametersAs(details.description);
    }

    bool dropOnParametersForSmoke(const juce::String& description) override
    {
        return typeParametersAs(description);
    }

    bool typeParametersAs(const juce::var& description)
    {
        const auto sound = SoundIconEditor::draggedSound(description);
        const auto* definition = current();
        if (!sound.has_value() || definition == nullptr)
            return false;
        auto edited = *definition;
        const auto changed = talas::app::classParametersTypedAs(edited, bridge_.editor().document(), registry_, *sound);
        if (!changed.has_value())
        {
            onStatus_("The Sound dropped has no parameters to give their types");
            return false;
        }
        if (changed->empty())
        {
            onStatus_("No parameter has a namesake of another type in " + juceString(*sound));
            return true;
        }
        const auto result = bridge_.editor().replaceSoundDefinition(definitionId_, std::move(edited));
        if (!result.ok())
        {
            onStatus_(juce::String{result.message});
            return false;
        }
        onDocumentChange_();
        const auto keep = parameterId_;
        refresh();
        (void)selectParameterForSmoke(keep);
        onStatus_("Typed " + juce::String{static_cast<int>(changed->size())} + " parameters as in " + juceString(*sound));
        return true;
    }

    void setPngIconForSmoke(bool png) override
    {
        pngIcon_.setToggleState(png, juce::sendNotificationSync);
    }

    void setIconTypesForSmoke(const std::string& input, const std::string& output, bool sideEffects) override
    {
        inputType_.setSelectedId(iconTypeItem(input), juce::sendNotificationSync);
        outputType_.setSelectedId(iconTypeItem(output), juce::sendNotificationSync);
        sideEffects_.setToggleState(sideEffects, juce::sendNotificationSync);
    }

    juce::TextEditor& descriptionFieldForSmoke() override
    {
        return description_;
    }

    void setIconChannelsForSmoke(unsigned input, unsigned output) override
    {
        inputChannels_.setSelectedId(static_cast<int>(input) + 1, juce::sendNotificationSync);
        outputChannels_.setSelectedId(static_cast<int>(output) + 1, juce::sendNotificationSync);
    }

    void iconTypesChanged()
    {
        iconEditor_.icon.inputType = iconTypeOfItem(inputType_.getSelectedId());
        iconEditor_.icon.outputType = iconTypeOfItem(outputType_.getSelectedId());
        iconEditor_.icon.sideEffects = sideEffects_.getToggleState();
        iconEditor_.icon.inputChannels = static_cast<std::uint8_t>(std::max(0, inputChannels_.getSelectedId() - 1));
        iconEditor_.icon.outputChannels = static_cast<std::uint8_t>(std::max(0, outputChannels_.getSelectedId() - 1));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        definitions_.setBounds(bounds.removeFromTop(26));
        bounds.removeFromTop(6);
        auto buttons = bounds.removeFromBottom(28);
        apply_.setBounds(buttons.removeFromRight(90));
        expand_.setBounds(buttons.removeFromRight(100));
        bounds.removeFromBottom(6);
        auto icon = bounds.removeFromBottom(108);
        bounds.removeFromBottom(6);
        iconLabel_.setBounds(icon.removeFromLeft(60).removeFromTop(24));
        auto types = icon.removeFromRight(170);
        inputType_.setBounds(types.removeFromTop(26).reduced(0, 1));
        outputType_.setBounds(types.removeFromTop(26).reduced(0, 1));
        sideEffects_.setBounds(types.removeFromTop(26));
        auto channels = types.removeFromTop(26).reduced(0, 1);
        inputChannels_.setBounds(channels.removeFromLeft(channels.getWidth() / 2).withTrimmedRight(2));
        outputChannels_.setBounds(channels);
        auto iconButtons = icon.removeFromRight(110);
        smallIcon_.setBounds(iconButtons.removeFromTop(26));
        pngIcon_.setBounds(iconButtons.removeFromTop(26));
        clearIcon_.setBounds(iconButtons.removeFromTop(26).reduced(0, 2));
        iconEditor_.setBounds(icon);
        auto list = bounds.removeFromLeft(180);
        auto layout = list.removeFromBottom(110);
        labels_[9]->setBounds(layout.removeFromTop(20));
        fieldLayout_.setBounds(layout);
        parameters_.setBounds(list.withTrimmedBottom(4));
        bounds.removeFromLeft(8);
        const std::array<juce::Component*, 9> editors{
            &name_, &talasType_, &type_, &fieldKind_, &minimum_, &maximum_, &field_, &comment_, &description_};
        for (std::size_t index = 0; index < editors.size(); ++index)
        {
            const auto height = index >= 7 ? 70 : 26;
            auto row = bounds.removeFromTop(height);
            labels_[index]->setBounds(row.removeFromLeft(120));
            editors[index]->setBounds(row);
            bounds.removeFromTop(4);
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

private:
    [[nodiscard]] const talas::project::ProjectSoundDefinition* current() const
    {
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto found = std::ranges::find(definitions, definitionId_, &talas::project::ProjectSoundDefinition::definitionId);
        return found == definitions.end() ? nullptr : &*found;
    }

    juce::TextEditor* editorFor(std::string_view field)
    {
        if (field == "name") return &name_;
        if (field == "minimum") return &minimum_;
        if (field == "maximum") return &maximum_;
        if (field == "field") return &field_;
        if (field == "comment") return &comment_;
        if (field == "description") return &description_;
        return nullptr;
    }

    void showDefinition()
    {
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto index = definitions_.getSelectedItemIndex();
        definitionId_ = index >= 0 && index < static_cast<int>(definitions.size())
            ? definitions[static_cast<std::size_t>(index)].definitionId
            : std::string{};
        const auto* definition = current();
        description_.setText(definition == nullptr ? juce::String{} : juce::String{definition->description}, false);
        iconEditor_.icon = definition == nullptr ? talas::project::ProjectSoundIcon{}
                                                  : definition->icon.value_or(talas::project::ProjectSoundIcon{});
        pngIcon_.setToggleState(iconEditor_.icon.png, juce::dontSendNotification);
        inputType_.setSelectedId(iconTypeItem(iconEditor_.icon.inputType), juce::dontSendNotification);
        outputType_.setSelectedId(iconTypeItem(iconEditor_.icon.outputType), juce::dontSendNotification);
        sideEffects_.setToggleState(iconEditor_.icon.sideEffects, juce::dontSendNotification);
        inputChannels_.setSelectedId(iconEditor_.icon.inputChannels + 1, juce::dontSendNotification);
        outputChannels_.setSelectedId(iconEditor_.icon.outputChannels + 1, juce::dontSendNotification);
        iconEditor_.repaint();
        expand_.setToggleState(definition == nullptr || definition->expandsBeforeTransformation, juce::dontSendNotification);
        parameters_.updateContent();
        parameters_.selectRow(definition != nullptr && !definition->exposedParameters.empty() ? 0 : -1);
        showParameter();
    }

    void showParameter()
    {
        chosenType_ = nullptr;
        const auto* definition = current();
        const auto row = parameters_.getSelectedRow();
        if (definition == nullptr || row < 0 || row >= static_cast<int>(definition->exposedParameters.size()))
        {
            parameterId_.clear();
            for (auto* editor : {&name_, &minimum_, &maximum_, &field_, &comment_})
                editor->setText({}, false);
            fieldKind_.clear(juce::dontSendNotification);
            fieldLayout_.boxes = definition == nullptr ? std::vector<talas::app::ClassEditorLayoutBox>{}
                                                       : talas::app::classEditorFieldLayout(*definition, {});
            fieldLayout_.repaint();
            return;
        }
        const auto& parameter = definition->exposedParameters[static_cast<std::size_t>(row)];
        parameterId_ = parameter.parameterId;
        fieldLayout_.boxes = talas::app::classEditorFieldLayout(*definition, parameterId_);
        fieldLayout_.repaint();
        const auto fields = talas::app::classEditorFields(parameter);
        name_.setText(juce::String{fields.displayName}, false);
        type_.setText(juce::String{fields.domain}, juce::dontSendNotification);
        minimum_.setText(juce::String{fields.minimum}, false);
        maximum_.setText(juce::String{fields.maximum}, false);
        field_.setText(juce::String{fields.field}, false);
        comment_.setText(juce::String{fields.comment}, false);
        fieldKind_.clear(juce::dontSendNotification);
        auto fieldItem = 1;
        for (const auto kind : talas::app::classEditorFieldKinds(parameter.kind))
            fieldKind_.addItem(juce::String{std::string{kind}}, fieldItem++);
        fieldKind_.setText(juce::String{fields.fieldKind}, juce::dontSendNotification);
        // The types this parameter's field takes, its own named one shown.
        talasType_.clear(juce::dontSendNotification);
        talasType_.addItem("(none)", 1);
        const auto types = talas::app::talasParameterTypes();
        for (std::size_t index = 0; index < types.size(); ++index)
        {
            if (talas::app::talasParameterTypeFits(parameter, types[index]))
                talasType_.addItem(juce::String{std::string{types[index].name}}, static_cast<int>(index) + 2);
        }
        const auto* named = talas::app::talasParameterTypeOf(parameter);
        talasType_.setSelectedId(named == nullptr ? 1 : static_cast<int>(named - types.data()) + 2, juce::dontSendNotification);
    }

    void showTalasType()
    {
        const auto id = talasType_.getSelectedId();
        const auto types = talas::app::talasParameterTypes();
        if (id < 2 || id - 2 >= static_cast<int>(types.size()))
            return;
        const auto& type = types[static_cast<std::size_t>(id - 2)];
        chosenType_ = &type;
        // A default field is selected for the type.
        fieldKind_.setText(juce::String{std::string{talas::app::talasParameterTypeField(type)}}, juce::dontSendNotification);
        if (type.kind == talas::core::ParameterKind::Expression)
        {
            type_.setText(juce::String{std::string{talas::app::classEditorDomainName(type.domain)}}, juce::dontSendNotification);
            const auto bound = [](const std::optional<double>& value) {
                if (!value)
                    return juce::String{};
                std::array<char, 64> buffer{};
                const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value);
                return failed == std::errc{} ? juce::String{std::string(buffer.data(), end)} : juce::String{};
            };
            minimum_.setText(bound(type.minimum), false);
            maximum_.setText(bound(type.maximum), false);
        }
        onStatus_("The " + juce::String{std::string{type.name}} + " type takes " + juce::String{std::string{type.takes}}
            + "; Apply to keep it");
    }

    bool apply()
    {
        const auto* definition = current();
        if (definition == nullptr)
        {
            onStatus_("Choose a Sound to edit its class");
            return false;
        }
        auto edited = *definition;
        edited.description = description_.getText().toStdString();
        edited.expandsBeforeTransformation = expand_.getToggleState();
        edited.icon = iconEditor_.icon == talas::project::ProjectSoundIcon{}
            ? std::nullopt
            : std::optional<talas::project::ProjectSoundIcon>{iconEditor_.icon};
        const auto parameter = std::ranges::find(edited.exposedParameters, parameterId_,
            &talas::project::ProjectSoundExposedParameter::parameterId);
        if (parameter != edited.exposedParameters.end())
        {
            std::string error;
            const auto changed = talas::app::applyClassEditorFields(*parameter,
                {name_.getText().toStdString(), type_.getText().toStdString(), minimum_.getText().toStdString(),
                    maximum_.getText().toStdString(), field_.getText().toStdString(), comment_.getText().toStdString(),
                    fieldKind_.getText().toStdString()},
                error);
            if (!changed)
            {
                onStatus_(juce::String{error});
                return false;
            }
            // A type chosen gives the parameter its
            // default value where the type does not take the one it had.
            *parameter = chosenType_ == nullptr ? *changed
                                                : talas::app::withTalasParameterTypeDefault(*changed, *chosenType_,
                                                      bridge_.editor().document().expressionLanguage);
        }
        const auto result = bridge_.editor().replaceSoundDefinition(definitionId_, std::move(edited));
        if (!result.ok())
        {
            onStatus_(juce::String{result.message});
            return false;
        }
        onDocumentChange_();
        const auto keep = parameterId_;
        refresh();
        (void)selectParameterForSmoke(keep);
        onStatus_("Class updated");
        return true;
    }

    int getNumRows() override
    {
        const auto* definition = current();
        return definition == nullptr ? 0 : static_cast<int>(definition->exposedParameters.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        const auto* definition = current();
        if (definition == nullptr || row < 0 || row >= static_cast<int>(definition->exposedParameters.size()))
            return;
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::body());
        graphics.drawText(juce::String{definition->exposedParameters[static_cast<std::size_t>(row)].displayName}, 6, 0,
            width - 12, height, juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int) override
    {
        showParameter();
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    std::string definitionId_;
    std::string parameterId_;
    juce::ComboBox definitions_;
    juce::ListBox parameters_;
    std::vector<std::unique_ptr<juce::Label>> labels_;
    juce::TextEditor name_;
    juce::ComboBox talasType_;
    // The parameter type chosen for the parameter shown since it was shown.
    const talas::app::TalasParameterType* chosenType_ = nullptr;
    juce::ComboBox type_;
    juce::ComboBox fieldKind_;
    juce::TextEditor minimum_;
    juce::TextEditor maximum_;
    juce::TextEditor field_;
    juce::TextEditor comment_;
    juce::TextEditor description_;
    juce::ToggleButton expand_{"Expand"};
    juce::TextButton apply_{"Apply"};
    juce::Label iconLabel_;
    SoundIconEditor iconEditor_;
    juce::ToggleButton smallIcon_{"Small icon"};
    juce::ToggleButton pngIcon_{"PNG icon"};
    juce::TextButton clearIcon_{"Clear icon"};
    juce::ComboBox inputType_;
    juce::ComboBox outputType_;
    juce::ToggleButton sideEffects_{"Nonaudio side effects"};
    juce::ComboBox inputChannels_;
    juce::ComboBox outputChannels_;
    FieldLayoutView fieldLayout_;
};
}

std::unique_ptr<ClassEditorPanel> makeClassEditorPanel(talas::runtime::ProjectRuntimeBridge& bridge, const talas::core::NodeRegistry& registry,
    ClassEditorPanel::DocumentChangeCallback onDocumentChange, ClassEditorPanel::StatusCallback onStatus)
{
    return std::make_unique<ClassEditorPanelImpl>(bridge, registry, std::move(onDocumentChange), std::move(onStatus));
}
}
