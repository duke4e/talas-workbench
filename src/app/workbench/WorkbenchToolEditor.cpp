#include "app/workbench/WorkbenchToolEditor.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory>

// The Tool editor.
namespace talas::app::workbench
{
namespace
{
namespace tool = talas::project::tool;

constexpr std::array<std::pair<const char*, tool::Kind>, 7> kindChoices{{
    {"Object", tool::Kind::Object},
    {"Number", tool::Kind::Number},
    {"String", tool::Kind::String},
    {"Random numbers", tool::Kind::RandomNumbers},
    {"Text file", tool::Kind::TextFile},
    {"Compiled Sound Grid", tool::Kind::CompiledSoundGrid},
    {"View", tool::Kind::View},
}};

constexpr std::array<std::pair<const char*, tool::WidgetKind>, 12> widgetChoices{{
    {"Not visible", tool::WidgetKind::None},
    {"Read only text", tool::WidgetKind::ReadOnlyText},
    {"Text", tool::WidgetKind::Text},
    {"Meter", tool::WidgetKind::Meter},
    {"Fader", tool::WidgetKind::Fader},
    {"Fader (no number)", tool::WidgetKind::FaderNoNumber},
    {"Radio Buttons", tool::WidgetKind::RadioButtons},
    {"Button", tool::WidgetKind::Button},
    {"Button (trigger)", tool::WidgetKind::TriggerButton},
    {"Button (toggle)", tool::WidgetKind::ToggleButton},
    {"Button (stick)", tool::WidgetKind::StickButton},
    {"View", tool::WidgetKind::View},
}};

constexpr std::array<std::pair<const char*, tool::EventAccess>, 4> accessChoices{{
    {"No Event Value", tool::EventAccess::None},
    {"Read", tool::EventAccess::ReadOnly},
    {"Write", tool::EventAccess::WriteOnly},
    {"Read and write", tool::EventAccess::ReadWrite},
}};

constexpr std::array<std::pair<const char*, tool::FileAccess>, 5> fileChoices{{
    {"Read only", tool::FileAccess::ReadOnly},
    {"Write only", tool::FileAccess::WriteOnly},
    {"Append", tool::FileAccess::Append},
    {"Read and append", tool::FileAccess::ReadAppend},
    {"Read and write", tool::FileAccess::ReadWrite},
}};

template <typename Enum, std::size_t Count>
void fill(juce::ComboBox& box, const std::array<std::pair<const char*, Enum>, Count>& choices)
{
    for (std::size_t index = 0; index < choices.size(); ++index)
        box.addItem(choices[index].first, static_cast<int>(index) + 1);
}

template <typename Enum, std::size_t Count>
void show(juce::ComboBox& box, const std::array<std::pair<const char*, Enum>, Count>& choices, Enum value)
{
    const auto found = std::ranges::find(choices, value, &std::pair<const char*, Enum>::second);
    box.setSelectedId(found == choices.end() ? 1 : static_cast<int>(found - choices.begin()) + 1, juce::dontSendNotification);
}

template <typename Enum, std::size_t Count>
[[nodiscard]] Enum chosen(const juce::ComboBox& box, const std::array<std::pair<const char*, Enum>, Count>& choices)
{
    const auto id = box.getSelectedId();
    return id >= 1 && id <= static_cast<int>(choices.size()) ? choices[static_cast<std::size_t>(id - 1)].second : choices.front().second;
}

[[nodiscard]] std::optional<double> numberIn(const juce::String& text)
{
    const auto trimmed = text.trim().toStdString();
    if (trimmed.empty())
        return std::nullopt;
    double value = 0.0;
    const auto [end, failed] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (failed != std::errc{} || end != trimmed.data() + trimmed.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] juce::String numberText(std::optional<double> value)
{
    return value.has_value() ? juce::String{*value} : juce::String{};
}

// A list of names, its selection and double-click reported.
class NameList final : public juce::ListBoxModel
{
public:
    std::vector<std::string> names;
    std::function<void(int)> selected;
    std::function<void(int)> opened;

    int getNumRows() override
    {
        return static_cast<int>(names.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool isSelected) override
    {
        if (row < 0 || row >= getNumRows())
            return;
        if (isSelected)
            graphics.fillAll(workbenchTheme.selectedRow);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::body());
        graphics.drawText(juceString(names[static_cast<std::size_t>(row)]), 6, 0, width - 12, height,
            juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int row) override
    {
        if (selected)
            selected(row);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (opened)
            opened(row);
    }
};

// The Layout: the widgets of the global variables and of the
// selected state's, the selected one highlighted, a double-click on one
// editing its variable, over the grid LayoutGrid gives.
class LayoutView final : public juce::Component
{
public:
    struct Box
    {
        bool global = true;
        std::string name;
        juce::Rectangle<int> bounds;
    };

    std::vector<Box> boxes;
    std::optional<std::pair<bool, std::string>> highlighted;
    std::optional<std::pair<double, double>> grid;
    std::function<void(bool, const std::string&)> opened;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(workbenchTheme.controlTile);
        const auto scale = this->scale();
        if (grid.has_value())
        {
            graphics.setColour(secondaryTextColour.withAlpha(0.25F));
            const auto across = static_cast<float>(grid->first) * scale;
            const auto down = static_cast<float>(grid->second) * scale;
            // Too fine a grid to see is not drawn.
            if (across >= 3.0F)
            {
                for (auto x = across; x < static_cast<float>(getWidth()); x += across)
                    graphics.drawVerticalLine(juce::roundToInt(x), 0.0F, static_cast<float>(getHeight()));
            }
            if (down >= 3.0F)
            {
                for (auto y = down; y < static_cast<float>(getHeight()); y += down)
                    graphics.drawHorizontalLine(juce::roundToInt(y), 0.0F, static_cast<float>(getWidth()));
            }
        }
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds());
        for (const auto& box : boxes)
        {
            const auto area = scaled(box.bounds, scale);
            const auto lit = highlighted.has_value() && highlighted->first == box.global && highlighted->second == box.name;
            graphics.setColour(lit ? selectionColour.withAlpha(0.35F) : panelColour);
            graphics.fillRect(area);
            graphics.setColour(lit ? selectionColour : secondaryTextColour);
            graphics.drawRect(area, lit ? 2 : 1);
            graphics.setFont(WorkbenchType::micro());
            graphics.drawFittedText(juceString(box.name), area.reduced(2), juce::Justification::centred, 1);
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        const auto scale = this->scale();
        for (const auto& box : boxes)
        {
            if (scaled(box.bounds, scale).contains(event.getPosition()) && opened)
            {
                opened(box.global, box.name);
                return;
            }
        }
    }

    [[nodiscard]] std::optional<juce::Rectangle<int>> highlightedBounds() const
    {
        for (const auto& box : boxes)
        {
            if (highlighted.has_value() && highlighted->first == box.global && highlighted->second == box.name)
                return box.bounds;
        }
        return std::nullopt;
    }

private:
    // The Tool's window drawn to fit the field.
    [[nodiscard]] float scale() const
    {
        auto right = 400;
        auto bottom = 300;
        for (const auto& box : boxes)
        {
            right = std::max(right, box.bounds.getRight());
            bottom = std::max(bottom, box.bounds.getBottom());
        }
        return std::min(static_cast<float>(getWidth()) / static_cast<float>(right),
            static_cast<float>(getHeight()) / static_cast<float>(bottom));
    }

    [[nodiscard]] static juce::Rectangle<int> scaled(juce::Rectangle<int> bounds, float scale)
    {
        return bounds.toFloat().transformedBy(juce::AffineTransform::scale(scale)).toNearestInt();
    }
};

class ToolEditorPanelImpl final : public ToolEditorPanel
{
public:
    explicit ToolEditorPanelImpl(ToolEditorHost host)
        : host_(std::move(host))
    {
        for (auto* list : {&globals_, &states_, &locals_, &responses_})
        {
            list->setRowHeight(20);
            list->setColour(juce::ListBox::backgroundColourId, panelColour);
            addAndMakeVisible(*list);
        }
        globals_.setModel(&globalNames_);
        states_.setModel(&stateNames_);
        locals_.setModel(&localNames_);
        responses_.setModel(&responseNames_);
        globalNames_.selected = [this](int row) { selectVariable(true, row); };
        localNames_.selected = [this](int row) { selectVariable(false, row); };
        globalNames_.opened = [this](int row) { selectVariable(true, row); };
        localNames_.opened = [this](int row) { selectVariable(false, row); };
        stateNames_.selected = [this](int row) { selectState(row); };
        responseNames_.selected = [this](int row) { selectResponse(row); };
        layout_.opened = [this](bool global, const std::string& name) { (void)selectVariableForSmoke(global, name); };
        addAndMakeVisible(layout_);
        for (const auto* text : {"Global Variables", "States", "Local Variables", "Triggered Responses", "Trigger", "Response",
                 "Layout", "Variable Settings"})
        {
            auto label = std::make_unique<juce::Label>();
            label->setText(text, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::labelStrong());
            addAndMakeVisible(*label);
            headings_.push_back(std::move(label));
        }
        addGlobal_.onClick = [this] { askToAdd(true); };
        addLocal_.onClick = [this] { askToAdd(false); };
        editGlobal_.onClick = [this] { selectVariable(true, globals_.getSelectedRow()); };
        editLocal_.onClick = [this] { selectVariable(false, locals_.getSelectedRow()); };
        removeGlobal_.onClick = [this] { removeVariable(true); };
        removeLocal_.onClick = [this] { removeVariable(false); };
        addState_.onClick = [this] {
            host_.askName("The new state's name", [this](std::string name) { (void)addStateForSmoke(name); });
        };
        removeState_.onClick = [this] { removeState(); };
        addResponse_.onClick = [this] { (void)addResponseForSmoke(); };
        removeResponse_.onClick = [this] { removeResponse(); };
        trigger_.onTextChange = [this] { editResponse(); };
        response_.setMultiLine(true, false);
        response_.setReturnKeyStartsNewLine(true);
        response_.onTextChange = [this] { editResponse(); };
        addAndMakeVisible(trigger_);
        addAndMakeVisible(response_);
        fill(kind_, kindChoices);
        fill(widget_, widgetChoices);
        fill(access_, accessChoices);
        fill(fileAccess_, fileChoices);
        for (const auto& [editor, hint] : std::initializer_list<std::pair<juce::TextEditor*, const char*>>{{&name_, "Name"},
                 {&initially_, "Initially (Smalltalk)"}, {&left_, "Left"}, {&top_, "Top"}, {&width_, "Width"}, {&height_, "Height"},
                 {&digits_, "Digits"}, {&minimum_, "Minimum"}, {&maximum_, "Maximum"}, {&grid_, "Grid"}, {&columns_, "Columns"},
                 {&strings_, "Strings, one a line"}, {&down_, "downValue"}, {&up_, "upValue"}, {&eventValue_, "Event Value"}})
        {
            editor->setTextToShowWhenEmpty(hint, secondaryTextColour);
            editor->setTooltip(hint);
            addAndMakeVisible(*editor);
        }
        strings_.setMultiLine(true, false);
        strings_.setReturnKeyStartsNewLine(true);
        // Set Value Options... locates a Text file's or
        // Compiled Sound Grid's file with the file dialog, its name the
        // variable's Initially.
        addAndMakeVisible(valueOptions_);
        valueOptions_.setTooltip("Locate the file a Text file or Compiled Sound Grid variable names");
        valueOptions_.onClick = [this] {
            const auto files = tool::valueOptionsFiles(chosen(kind_, kindChoices));
            if (!files.has_value())
            {
                host_.status("Set Value Options... locates the file of a Text file or Compiled Sound Grid variable");
                return;
            }
            auto chooser = std::make_shared<juce::FileChooser>("Locate the file", juce::File{}, juceString(std::string{*files}));
            juce::Component::SafePointer<juce::Component> safeThis(this);
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this, safeThis, chooser](const juce::FileChooser& picked) {
                    if (safeThis == nullptr || picked.getResult() == juce::File{})
                        return;
                    initially_.setText(juceString(tool::initiallyNamingFile(picked.getResult().getFullPathName().toStdString())), false);
                });
        };
        // The colour menu sets a variable's Initially to a colour,
        // or Transparent, for BackgroundColor.
        for (const auto& name : tool::colourChoices())
            colour_.addItem(juceString(name), colour_.getNumItems() + 1);
        colour_.setTextWhenNothingSelected("Color");
        colour_.setTooltip("The color menu: a color, or Transparent, as the variable's Initially");
        colour_.onChange = [this] {
            if (const auto index = colour_.getSelectedItemIndex(); index >= 0)
                initially_.setText(juceString(tool::colourInitially(tool::colourChoices()[static_cast<std::size_t>(index)])), false);
        };
        addAndMakeVisible(colour_);
        for (auto* component : std::initializer_list<juce::Component*>{&kind_, &widget_, &access_, &fileAccess_, &labelled_,
                 &logTaper_, &step_, &applySettings_, &addGlobal_, &editGlobal_, &removeGlobal_, &addState_, &removeState_,
                 &addLocal_, &editLocal_, &removeLocal_, &addResponse_, &removeResponse_, &run_, &save_, &saveAs_,
                 &firstTrue_})
            addAndMakeVisible(*component);
        // A step runs every true condition's response
        // unless the Tool runs only the first.
        firstTrue_.setTooltip("Each step runs only the first response whose condition is true, "
                              "rather than every one in order");
        firstTrue_.onClick = [this] {
            tool_.stepRule = firstTrue_.getToggleState() ? tool::StepRule::FirstTrueResponse : tool::StepRule::EveryTrueResponse;
        };
        applySettings_.onClick = [this] { (void)applySettingsForSmoke(); };
        run_.onClick = [this] { runForSmoke(); };
        save_.onClick = [this] { host_.save(tool_, false); };
        saveAs_.onClick = [this] { host_.save(tool_, true); };
        setSize(1060, 660);
        setTool(tool::newTool());
    }

    void setTool(tool::Definition tool) override
    {
        tool_ = std::move(tool);
        firstTrue_.setToggleState(tool_.stepRule == tool::StepRule::FirstTrueResponse, juce::dontSendNotification);
        stateIndex_ = tool_.states.empty() ? -1 : 0;
        variable_.reset();
        responseIndex_ = -1;
        refreshLists();
        showSettings();
    }

    [[nodiscard]] tool::Definition tool() const override
    {
        return tool_;
    }

    bool addVariableForSmoke(bool global, const std::string& name) override
    {
        if (name.empty() || (!global && state() == nullptr))
            return false;
        // A global's name begins with an uppercase letter, a
        // local's with a lowercase one.
        auto named = name;
        named.front() = static_cast<char>(global ? std::toupper(static_cast<unsigned char>(named.front()))
                                                 : std::tolower(static_cast<unsigned char>(named.front())));
        auto& variables = global ? tool_.variables : state()->variables;
        if (std::ranges::any_of(variables, [&](const tool::Variable& other) { return other.name == named; }))
        {
            host_.status("There is already a variable named " + juceString(named));
            return false;
        }
        tool::Variable variable;
        variable.name = named;
        variables.push_back(std::move(variable));
        refreshLists();
        // The Variable Settings open on the new variable.
        return selectVariableForSmoke(global, named);
    }

    bool selectVariableForSmoke(bool global, const std::string& name) override
    {
        const auto* variables = global ? &tool_.variables : (state() == nullptr ? nullptr : &state()->variables);
        if (variables == nullptr)
            return false;
        const auto found = std::ranges::find(*variables, name, &tool::Variable::name);
        if (found == variables->end())
            return false;
        auto& list = global ? globals_ : locals_;
        list.selectRow(static_cast<int>(found - variables->begin()), true, true);
        (global ? locals_ : globals_).deselectAllRows();
        variable_ = {global, name};
        showSettings();
        return true;
    }

    bool setSettingForSmoke(const std::string& field, const juce::String& value) override
    {
        const auto choose = [&](juce::ComboBox& box) {
            for (int index = 0; index < box.getNumItems(); ++index)
            {
                if (box.getItemText(index) == value)
                {
                    box.setSelectedItemIndex(index, juce::dontSendNotification);
                    return true;
                }
            }
            return false;
        };
        if (field == "kind")
            return choose(kind_);
        if (field == "widget")
            return choose(widget_);
        if (field == "access")
            return choose(access_);
        if (field == "fileAccess")
            return choose(fileAccess_);
        if (field == "labelled" || field == "logTaper" || field == "step")
        {
            (field == "labelled" ? labelled_ : field == "logTaper" ? logTaper_ : step_).setToggleState(value == "true", juce::dontSendNotification);
            return true;
        }
        const std::array<std::pair<const char*, juce::TextEditor*>, 15> editors{{{"name", &name_}, {"initially", &initially_},
            {"left", &left_}, {"top", &top_}, {"width", &width_}, {"height", &height_}, {"digits", &digits_},
            {"minimum", &minimum_}, {"maximum", &maximum_}, {"grid", &grid_}, {"columns", &columns_}, {"strings", &strings_},
            {"down", &down_}, {"up", &up_}, {"eventValue", &eventValue_}}};
        for (const auto& [key, editor] : editors)
        {
            if (field == key)
            {
                editor->setText(value, false);
                return true;
            }
        }
        return false;
    }

    bool applySettingsForSmoke() override
    {
        auto* variable = selectedVariable();
        if (variable == nullptr)
        {
            host_.status("Select a variable to change its settings");
            return false;
        }
        auto edited = *variable;
        edited.name = name_.getText().trim().toStdString();
        if (edited.name.empty())
        {
            host_.status("A variable needs a name");
            return false;
        }
        edited.kind = chosen(kind_, kindChoices);
        edited.initially = initially_.getText().toStdString();
        edited.widget.kind = chosen(widget_, widgetChoices);
        edited.widget.labelled = labelled_.getToggleState();
        for (const auto& [editor, destination] : std::initializer_list<std::pair<juce::TextEditor*, double*>>{
                 {&left_, &edited.widget.left}, {&top_, &edited.widget.top}, {&width_, &edited.widget.width},
                 {&height_, &edited.widget.height}})
        {
            if (const auto number = numberIn(editor->getText()))
                *destination = *number;
        }
        const auto digits = numberIn(digits_.getText());
        edited.widget.digits = digits.has_value() ? std::optional<int>{static_cast<int>(std::clamp(*digits, 0.0, 12.0))} : std::nullopt;
        edited.minimum = numberIn(minimum_.getText());
        edited.maximum = numberIn(maximum_.getText());
        edited.grid = numberIn(grid_.getText());
        edited.widget.logTaper = logTaper_.getToggleState();
        edited.widget.columns = static_cast<int>(std::max(1.0, numberIn(columns_.getText()).value_or(1.0)));
        edited.widget.strings.clear();
        for (const auto& line : juce::StringArray::fromLines(strings_.getText()))
        {
            if (line.trim().isNotEmpty())
                edited.widget.strings.push_back(line.trim().toStdString());
        }
        edited.widget.downValue = down_.getText().toStdString();
        edited.widget.upValue = up_.getText().toStdString();
        edited.eventValue = eventValue_.getText().trim().toStdString();
        edited.access = chosen(access_, accessChoices);
        edited.fileAccess = chosen(fileAccess_, fileChoices);
        edited.stepWhenChanged = step_.getToggleState();
        const auto global = variable_->first;
        auto& variables = global ? tool_.variables : state()->variables;
        if (edited.name != variable->name
            && std::ranges::any_of(variables, [&](const tool::Variable& other) { return other.name == edited.name; }))
        {
            host_.status("There is already a variable named " + juceString(edited.name));
            return false;
        }
        *variable = std::move(edited);
        variable_ = {global, variable->name};
        refreshLists();
        (void)selectVariableForSmoke(global, variable_->second);
        host_.status("Variable settings applied");
        return true;
    }

    bool addStateForSmoke(const std::string& name) override
    {
        if (name.empty() || std::ranges::any_of(tool_.states, [&](const tool::State& state) { return state.name == name; }))
            return false;
        tool_.states.push_back({name, {}, {}});
        refreshLists();
        return selectStateForSmoke(name);
    }

    bool selectStateForSmoke(const std::string& name) override
    {
        const auto found = std::ranges::find(tool_.states, name, &tool::State::name);
        if (found == tool_.states.end())
            return false;
        states_.selectRow(static_cast<int>(found - tool_.states.begin()));
        return stateIndex_ == static_cast<int>(found - tool_.states.begin());
    }

    bool addResponseForSmoke() override
    {
        auto* current = state();
        if (current == nullptr)
            return false;
        // Add puts the new response before the selected one.
        const auto at = responseIndex_ >= 0 && responseIndex_ < static_cast<int>(current->responses.size())
            ? static_cast<std::size_t>(responseIndex_)
            : current->responses.size();
        current->responses.insert(current->responses.begin() + static_cast<std::ptrdiff_t>(at), {"true", "nil"});
        refreshLists();
        return selectResponseForSmoke(at);
    }

    bool selectResponseForSmoke(std::size_t index) override
    {
        if (state() == nullptr || index >= state()->responses.size())
            return false;
        responses_.selectRow(static_cast<int>(index));
        return responseIndex_ == static_cast<int>(index);
    }

    void setTriggerForSmoke(const juce::String& trigger) override
    {
        // The text editor tells of a change later; the smoke edits at once.
        trigger_.setText(trigger, false);
        editResponse();
    }

    void setResponseForSmoke(const juce::String& response) override
    {
        response_.setText(response, false);
        editResponse();
    }

    [[nodiscard]] std::optional<juce::Rectangle<int>> highlightedForSmoke() const override
    {
        return layout_.highlightedBounds();
    }

    [[nodiscard]] std::optional<std::pair<double, double>> gridForSmoke() const override
    {
        return layout_.grid;
    }

    void runForSmoke() override
    {
        host_.run(tool_);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto bottom = bounds.removeFromBottom(28);
        run_.setBounds(bottom.removeFromLeft(80));
        bottom.removeFromLeft(8);
        save_.setBounds(bottom.removeFromLeft(80));
        bottom.removeFromLeft(8);
        saveAs_.setBounds(bottom.removeFromLeft(90));
        bottom.removeFromLeft(12);
        firstTrue_.setBounds(bottom.removeFromLeft(240));
        bounds.removeFromBottom(8);
        const auto column = [&](int width) {
            auto area = bounds.removeFromLeft(width);
            bounds.removeFromLeft(10);
            return area;
        };
        const auto listWithButtons = [](juce::Rectangle<int> area, juce::Label& heading, juce::ListBox& list,
                                         std::initializer_list<juce::Button*> buttons) {
            heading.setBounds(area.removeFromTop(20));
            auto row = area.removeFromBottom(26);
            const auto width = row.getWidth() / static_cast<int>(buttons.size());
            for (auto* button : buttons)
                button->setBounds(row.removeFromLeft(width).reduced(2, 1));
            list.setBounds(area);
        };
        auto first = column(220);
        listWithButtons(first.removeFromTop(first.getHeight() / 2), *headings_[0], globals_, {&addGlobal_, &editGlobal_, &removeGlobal_});
        first.removeFromTop(6);
        listWithButtons(first, *headings_[1], states_, {&addState_, &removeState_});
        auto second = column(240);
        listWithButtons(second.removeFromTop(second.getHeight() / 3), *headings_[2], locals_, {&addLocal_, &editLocal_, &removeLocal_});
        second.removeFromTop(6);
        listWithButtons(second.removeFromTop(second.getHeight() / 3), *headings_[3], responses_, {&addResponse_, &removeResponse_});
        second.removeFromTop(6);
        headings_[4]->setBounds(second.removeFromTop(20));
        trigger_.setBounds(second.removeFromTop(26));
        second.removeFromTop(6);
        headings_[5]->setBounds(second.removeFromTop(20));
        response_.setBounds(second);
        auto third = bounds;
        headings_[6]->setBounds(third.removeFromTop(20));
        layout_.setBounds(third.removeFromTop(200));
        third.removeFromTop(6);
        headings_[7]->setBounds(third.removeFromTop(20));
        const auto row = [&third](std::initializer_list<juce::Component*> components) {
            auto line = third.removeFromTop(26);
            third.removeFromTop(3);
            const auto width = line.getWidth() / static_cast<int>(components.size());
            for (auto* component : components)
                component->setBounds(line.removeFromLeft(width).reduced(2, 0));
        };
        row({&name_, &kind_});
        row({&initially_, &colour_, &valueOptions_});
        row({&widget_, &labelled_});
        row({&left_, &top_, &width_, &height_});
        row({&digits_, &minimum_, &maximum_, &grid_, &logTaper_});
        row({&columns_, &down_, &up_});
        row({&eventValue_, &access_});
        row({&fileAccess_, &step_, &applySettings_});
        strings_.setBounds(third.removeFromTop(48).reduced(2, 0));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

private:
    [[nodiscard]] tool::State* state()
    {
        return stateIndex_ >= 0 && stateIndex_ < static_cast<int>(tool_.states.size())
            ? &tool_.states[static_cast<std::size_t>(stateIndex_)]
            : nullptr;
    }

    [[nodiscard]] tool::Variable* selectedVariable()
    {
        if (!variable_.has_value())
            return nullptr;
        auto* variables = variable_->first ? &tool_.variables : (state() == nullptr ? nullptr : &state()->variables);
        if (variables == nullptr)
            return nullptr;
        const auto found = std::ranges::find(*variables, variable_->second, &tool::Variable::name);
        return found == variables->end() ? nullptr : &*found;
    }

    void askToAdd(bool global)
    {
        host_.askName(global ? "The new global variable's name" : "The new local variable's name",
            [this, global](std::string name) { (void)addVariableForSmoke(global, name); });
    }

    // A double-click on a name, or Edit with it selected.
    void selectVariable(bool global, int row)
    {
        if (!global && state() == nullptr)
            return;
        const auto name = tool::variableToEdit(
            tool_, global ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(stateIndex_)}, row);
        if (!name.has_value())
            return;
        variable_ = {global, *name};
        (global ? locals_ : globals_).deselectAllRows();
        showSettings();
    }

    void removeVariable(bool global)
    {
        auto* variables = global ? &tool_.variables : (state() == nullptr ? nullptr : &state()->variables);
        const auto row = (global ? globals_ : locals_).getSelectedRow();
        if (variables == nullptr || row < 0 || row >= static_cast<int>(variables->size()))
            return;
        variables->erase(variables->begin() + row);
        variable_.reset();
        refreshLists();
        showSettings();
    }

    void selectState(int row)
    {
        stateIndex_ = row;
        responseIndex_ = -1;
        if (variable_.has_value() && !variable_->first)
            variable_.reset();
        refreshLists();
        showSettings();
    }

    void removeState()
    {
        if (state() == nullptr || state()->name == "InitialState")
        {
            host_.status("A Tool keeps its InitialState");
            return;
        }
        tool_.states.erase(tool_.states.begin() + stateIndex_);
        stateIndex_ = 0;
        refreshLists();
    }

    void selectResponse(int row)
    {
        responseIndex_ = row;
        showResponse();
    }

    void removeResponse()
    {
        auto* current = state();
        if (current == nullptr || responseIndex_ < 0 || responseIndex_ >= static_cast<int>(current->responses.size()))
            return;
        current->responses.erase(current->responses.begin() + responseIndex_);
        responseIndex_ = -1;
        refreshLists();
    }

    void editResponse()
    {
        if (showingResponse_)
            return;
        auto* current = state();
        if (current == nullptr || responseIndex_ < 0 || responseIndex_ >= static_cast<int>(current->responses.size()))
            return;
        auto& response = current->responses[static_cast<std::size_t>(responseIndex_)];
        response.condition = trigger_.getText().toStdString();
        response.response = response_.getText().toStdString();
        responseNames_.names[static_cast<std::size_t>(responseIndex_)] = responseTitle(response);
        responses_.repaintRow(responseIndex_);
    }

    [[nodiscard]] static std::string responseTitle(const tool::TriggeredResponse& response)
    {
        // TriggerButton turnedOn ==> <theResponse>
        return response.condition + " ==> <theResponse>";
    }

    void refreshLists()
    {
        globalNames_.names = tool::editorVariableNames(tool_, std::nullopt);
        stateNames_.names.clear();
        for (const auto& each : tool_.states)
            stateNames_.names.push_back(each.name);
        localNames_.names.clear();
        responseNames_.names.clear();
        if (const auto* current = state())
        {
            localNames_.names = tool::editorVariableNames(tool_, static_cast<std::size_t>(stateIndex_));
            for (const auto& response : current->responses)
                responseNames_.names.push_back(responseTitle(response));
        }
        for (auto* list : {&globals_, &states_, &locals_, &responses_})
            list->updateContent();
        if (stateIndex_ >= 0)
            states_.selectRow(stateIndex_, true, false);
        // The Layout of the global state and the selected one.
        layout_.boxes.clear();
        const auto boxesOf = [&](const std::vector<tool::Variable>& variables, bool global) {
            for (const auto& variable : variables)
            {
                if (variable.widget.kind == tool::WidgetKind::None)
                    continue;
                layout_.boxes.push_back({global, variable.name,
                    juce::Rectangle<double>{variable.widget.left, variable.widget.top, variable.widget.width, variable.widget.height}
                        .toNearestInt()});
            }
        };
        boxesOf(tool_.variables, true);
        if (const auto* current = state())
            boxesOf(current->variables, false);
        layout_.highlighted = variable_;
        layout_.grid = tool::layoutGridOf(tool_);
        layout_.repaint();
        showResponse();
    }

    void showResponse()
    {
        showingResponse_ = true;
        const auto* current = state();
        const auto valid = current != nullptr && responseIndex_ >= 0 && responseIndex_ < static_cast<int>(current->responses.size());
        trigger_.setText(valid ? juceString(current->responses[static_cast<std::size_t>(responseIndex_)].condition) : juce::String{}, false);
        response_.setText(valid ? juceString(current->responses[static_cast<std::size_t>(responseIndex_)].response) : juce::String{}, false);
        trigger_.setEnabled(valid);
        response_.setEnabled(valid);
        showingResponse_ = false;
    }

    // The Variable Settings of the selected variable.
    void showSettings()
    {
        const auto* variable = selectedVariable();
        layout_.highlighted = variable == nullptr ? std::nullopt : variable_;
        layout_.repaint();
        const tool::Variable shown = variable == nullptr ? tool::Variable{} : *variable;
        name_.setText(juceString(shown.name), false);
        show(kind_, kindChoices, shown.kind);
        initially_.setText(juceString(shown.initially), false);
        show(widget_, widgetChoices, shown.widget.kind);
        labelled_.setToggleState(shown.widget.labelled, juce::dontSendNotification);
        left_.setText(juce::String{shown.widget.left}, false);
        top_.setText(juce::String{shown.widget.top}, false);
        width_.setText(juce::String{shown.widget.width}, false);
        height_.setText(juce::String{shown.widget.height}, false);
        digits_.setText(shown.widget.digits.has_value() ? juce::String{*shown.widget.digits} : juce::String{}, false);
        minimum_.setText(numberText(shown.minimum), false);
        maximum_.setText(numberText(shown.maximum), false);
        grid_.setText(numberText(shown.grid), false);
        logTaper_.setToggleState(shown.widget.logTaper, juce::dontSendNotification);
        columns_.setText(juce::String{shown.widget.columns}, false);
        juce::StringArray strings;
        for (const auto& string : shown.widget.strings)
            strings.add(juceString(string));
        strings_.setText(strings.joinIntoString("\n"), false);
        down_.setText(juceString(shown.widget.downValue), false);
        up_.setText(juceString(shown.widget.upValue), false);
        eventValue_.setText(juceString(shown.eventValue), false);
        show(access_, accessChoices, shown.access);
        show(fileAccess_, fileChoices, shown.fileAccess);
        step_.setToggleState(shown.stepWhenChanged, juce::dontSendNotification);
        applySettings_.setEnabled(variable != nullptr);
    }

    ToolEditorHost host_;
    tool::Definition tool_;
    int stateIndex_ = -1;
    int responseIndex_ = -1;
    bool showingResponse_ = false;
    std::optional<std::pair<bool, std::string>> variable_;
    NameList globalNames_;
    NameList stateNames_;
    NameList localNames_;
    NameList responseNames_;
    juce::ListBox globals_;
    juce::ListBox states_;
    juce::ListBox locals_;
    juce::ListBox responses_;
    LayoutView layout_;
    std::vector<std::unique_ptr<juce::Label>> headings_;
    juce::TextEditor trigger_;
    juce::TextEditor response_;
    juce::TextEditor name_;
    juce::ComboBox kind_;
    juce::TextEditor initially_;
    juce::TextButton valueOptions_{"Set Value Options..."};
    juce::ComboBox colour_;
    juce::ComboBox widget_;
    juce::ToggleButton labelled_{"Label"};
    juce::TextEditor left_;
    juce::TextEditor top_;
    juce::TextEditor width_;
    juce::TextEditor height_;
    juce::TextEditor digits_;
    juce::TextEditor minimum_;
    juce::TextEditor maximum_;
    juce::TextEditor grid_;
    juce::ToggleButton logTaper_{"Log taper"};
    juce::TextEditor columns_;
    juce::TextEditor strings_;
    juce::TextEditor down_;
    juce::TextEditor up_;
    juce::TextEditor eventValue_;
    juce::ComboBox access_;
    juce::ComboBox fileAccess_;
    juce::ToggleButton step_{"Step when changed"};
    juce::TextButton applySettings_{"Apply Settings"};
    juce::TextButton addGlobal_{"Add"};
    juce::TextButton editGlobal_{"Edit"};
    juce::TextButton removeGlobal_{"Remove"};
    juce::TextButton addState_{"Add"};
    juce::TextButton removeState_{"Remove"};
    juce::TextButton addLocal_{"Add"};
    juce::TextButton editLocal_{"Edit"};
    juce::TextButton removeLocal_{"Remove"};
    juce::TextButton addResponse_{"Add"};
    juce::TextButton removeResponse_{"Remove"};
    juce::TextButton run_{"Run"};
    juce::TextButton save_{"Save"};
    juce::TextButton saveAs_{"Save As..."};
    juce::ToggleButton firstTrue_{"First true response only"};
};
}

std::unique_ptr<ToolEditorPanel> makeToolEditorPanel(ToolEditorHost host)
{
    return std::make_unique<ToolEditorPanelImpl>(std::move(host));
}
}
