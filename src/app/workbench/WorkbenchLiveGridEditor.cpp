#include "app/workbench/WorkbenchLiveGridEditor.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/CompiledSounds.h"
#include "talas/app/LiveGridAuthoringModel.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <utility>

namespace talas::app::workbench
{
namespace
{
constexpr auto rows = talas::project::projectLiveSetRows;
constexpr auto columns = talas::project::projectLiveSetColumns;

class LiveGridEditorPanelImpl final : public LiveGridEditorPanel, private juce::Timer
{
public:
    LiveGridEditorPanelImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus,
        MidiInputsCallback midiInputs)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , midiInputs_(std::move(midiInputs))
    {
        setOpaque(true);
        for (auto* button : {&newSet_, &renameSet_, &deleteSet_, &saveSet_, &newTool_, &deleteTool_})
            addAndMakeVisible(*button);
        // The grid saved as a file of its own, .prg.
        saveSet_.setButtonText("Save...");
        saveSet_.onClick = [this] { saveLiveSet(); };
        newSet_.setButtonText("New Grid");
        renameSet_.setButtonText("Rename...");
        deleteSet_.setButtonText("Delete");
        newTool_.setButtonText("New Tool");
        deleteTool_.setButtonText("Delete Tool");
        newSet_.onClick = [this] { (void)newLiveSet(); };
        renameSet_.onClick = [this] { promptRenameLiveSet(); };
        deleteSet_.onClick = [this] { (void)deleteLiveSet(); };
        newTool_.onClick = [this] { (void)newTool(); };
        deleteTool_.onClick = [this] { (void)deleteTool(); };
        describeControl(setSelector_, "Live Grid", "The grid being edited");
        describeControl(channel_, "Program channel",
            "The MIDI channel on which each box answers to its program number");
        describeControl(toolSelector_, "Live Tool",
            "The tool a box's Add Step adds to");
        setSelector_.onChange = [this] {
            const auto& sets = document().liveSetLibrary.liveSets;
            const auto index = setSelector_.getSelectedItemIndex();
            if (index >= 0 && static_cast<std::size_t>(index) < sets.size())
                selectedSetId_ = sets[static_cast<std::size_t>(index)].liveSetId;
            refresh();
        };
        toolSelector_.onChange = [this] {
            const auto& tools = document().liveSetLibrary.tools;
            const auto index = toolSelector_.getSelectedItemIndex();
            if (index >= 0 && static_cast<std::size_t>(index) < tools.size())
                selectedToolId_ = tools[static_cast<std::size_t>(index)].toolId;
        };
        channel_.addItem("Off", 1);
        for (int value = 1; value <= 16; ++value)
            channel_.addItem("Channel " + juce::String(value), value + 1);
        channel_.onChange = [this] {
            if (!refreshing_)
                (void)setProgramChannel(channel_.getSelectedId() - 1);
        };
        on_.setButtonText("On");
        describeControl(on_, "On", "Plays the grid's compiled Sounds; a click in a box loads and starts its Sound");
        on_.onClick = [this] {
            const auto wanted = on_.getToggleState();
            if (onHandler_ == nullptr || !onHandler_(selectedSetId_, wanted))
                on_.setToggleState(!wanted, juce::dontSendNotification);
            refresh();
        };
        addAndMakeVisible(on_);
        channelLabel_.setText("Programs on", juce::dontSendNotification);
        toolLabel_.setText("Tool", juce::dontSendNotification);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &setSelector_, &channelLabel_, &channel_, &toolLabel_, &toolSelector_})
        {
            addAndMakeVisible(*component);
        }
        for (std::uint32_t row = 0; row < rows; ++row)
        {
            for (std::uint32_t column = 0; column < columns; ++column)
            {
                auto* box = boxes_.add(new juce::TextButton);
                box->setComponentID(
                    "live-grid-box-" + juce::String(talas::app::liveGridProgramNumber(row, column)));
                // A click waits out the double-click time before the box's
                // menu, so a double-click edits the box's Sound instead.
                box->onClick = [this, row, column] {
                    if (on_.getToggleState())
                    {
                        (void)startBox(row, column);
                        return;
                    }
                    pendingMenu_ = {row, column};
                    startTimer(juce::MouseEvent::getDoubleClickTimeout());
                };
                box->addMouseListener(&doubleClicks_, false);
                gridContainer_.addAndMakeVisible(box);
            }
        }
        gridViewport_.setViewedComponent(&gridContainer_, false);
        gridViewport_.setScrollBarsShown(true, true);
        addAndMakeVisible(gridViewport_);
        setSize(980, 560);
        refresh();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto top = bounds.removeFromTop(28);
        setSelector_.setBounds(top.removeFromLeft(200));
        top.removeFromLeft(6);
        newSet_.setBounds(top.removeFromLeft(86));
        top.removeFromLeft(4);
        renameSet_.setBounds(top.removeFromLeft(86));
        top.removeFromLeft(4);
        deleteSet_.setBounds(top.removeFromLeft(64));
        top.removeFromLeft(4);
        saveSet_.setBounds(top.removeFromLeft(64));
        top.removeFromLeft(16);
        channelLabel_.setBounds(top.removeFromLeft(84));
        channel_.setBounds(top.removeFromLeft(120));
        top.removeFromLeft(12);
        on_.setBounds(top.removeFromLeft(56));
        bounds.removeFromTop(6);
        auto tools = bounds.removeFromTop(28);
        toolLabel_.setBounds(tools.removeFromLeft(40));
        toolSelector_.setBounds(tools.removeFromLeft(200));
        tools.removeFromLeft(6);
        newTool_.setBounds(tools.removeFromLeft(86));
        tools.removeFromLeft(4);
        deleteTool_.setBounds(tools.removeFromLeft(96));
        bounds.removeFromTop(8);
        gridViewport_.setBounds(bounds);
        constexpr int boxWidth = 92;
        constexpr int boxHeight = 52;
        constexpr int gap = 4;
        gridContainer_.setSize(
            static_cast<int>(columns) * (boxWidth + gap) + gap,
            static_cast<int>(rows) * (boxHeight + gap) + gap);
        for (std::uint32_t row = 0; row < rows; ++row)
        {
            for (std::uint32_t column = 0; column < columns; ++column)
            {
                box(row, column).setBounds(
                    gap + static_cast<int>(column) * (boxWidth + gap),
                    gap + static_cast<int>(row) * (boxHeight + gap),
                    boxWidth,
                    boxHeight);
            }
        }
    }

    void refresh() override
    {
        refreshing_ = true;
        const auto& library = document().liveSetLibrary;
        if (std::ranges::none_of(library.liveSets, [&](const auto& set) {
                return set.liveSetId == selectedSetId_;
            }))
        {
            selectedSetId_ = library.liveSets.empty()
                ? std::string{}
                : library.liveSets.front().liveSetId;
        }
        setSelector_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < library.liveSets.size(); ++index)
        {
            const auto& set = library.liveSets[index];
            setSelector_.addItem(juceString(set.displayName), static_cast<int>(index) + 1);
            if (set.liveSetId == selectedSetId_)
                setSelector_.setSelectedId(static_cast<int>(index) + 1, juce::dontSendNotification);
        }
        if (std::ranges::none_of(library.tools, [&](const auto& tool) {
                return tool.toolId == selectedToolId_;
            }))
        {
            selectedToolId_ = library.tools.empty() ? std::string{} : library.tools.front().toolId;
        }
        toolSelector_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < library.tools.size(); ++index)
        {
            const auto& tool = library.tools[index];
            toolSelector_.addItem(juceString(tool.displayName), static_cast<int>(index) + 1);
            if (tool.toolId == selectedToolId_)
                toolSelector_.setSelectedId(static_cast<int>(index) + 1, juce::dontSendNotification);
        }
        const auto* set = selectedSet();
        const auto channel = set == nullptr
            ? std::optional<int>{}
            : talas::app::liveGridProgramChannel(*set);
        channel_.setSelectedId(channel.value_or(0) + 1, juce::dontSendNotification);
        channel_.setEnabled(set != nullptr && !on_.getToggleState());
        on_.setEnabled(set != nullptr);
        renameSet_.setEnabled(set != nullptr);
        deleteSet_.setEnabled(set != nullptr);
        saveSet_.setEnabled(set != nullptr);
        deleteTool_.setEnabled(!selectedToolId_.empty());
        for (std::uint32_t row = 0; row < rows; ++row)
        {
            for (std::uint32_t column = 0; column < columns; ++column)
            {
                auto& button = box(row, column);
                const auto program = talas::app::liveGridProgramNumber(row, column);
                const auto* cell = set == nullptr
                    ? nullptr
                    : talas::app::liveGridCellAt(*set, row, column);
                const auto text = boxText(row, column);
                button.setButtonText(text);
                button.setEnabled(set != nullptr);
                button.setToggleState(cell != nullptr, juce::dontSendNotification);
                button.setTitle("Box " + juce::String(program));
                button.setDescription(cell == nullptr
                        ? juce::String{"Empty box; choose what it plays from its menu"}
                        : juceString(talas::app::liveGridTargetLabel(document(), cell->target)));
            }
        }
        refreshing_ = false;
    }

    [[nodiscard]] std::string selectedLiveSetId() const override
    {
        return selectedSetId_;
    }

    [[nodiscard]] std::string selectedToolId() const override
    {
        return selectedToolId_;
    }

    void setGridHandlers(OnCallback on, CellCallback start, SoundCallback edit) override
    {
        onHandler_ = std::move(on);
        startHandler_ = std::move(start);
        editHandler_ = std::move(edit);
    }

    [[nodiscard]] bool gridOn() const override
    {
        return on_.getToggleState();
    }

    bool addGrid() override
    {
        return newLiveSet();
    }

    bool setOnForSmoke(bool on) override
    {
        on_.setToggleState(on, juce::dontSendNotification);
        on_.onClick();
        return on_.getToggleState() == on;
    }

    bool doubleClickBoxForSmoke(std::uint32_t row, std::uint32_t column) override
    {
        return editBox(row, column);
    }

    bool newLiveSetForSmoke() override
    {
        return newLiveSet();
    }

    bool renameLiveSetForSmoke(const juce::String& name) override
    {
        return renameLiveSet(name);
    }

    bool deleteLiveSetForSmoke() override
    {
        return deleteLiveSet();
    }

    [[nodiscard]] juce::PopupMenu boxMenuForSmoke(
        std::uint32_t row,
        std::uint32_t column) override
    {
        return boxMenu(row, column);
    }

    bool chooseBoxMenuItemForSmoke(
        std::uint32_t row,
        std::uint32_t column,
        int itemId) override
    {
        return chooseBoxMenuItem(row, column, itemId);
    }

    bool renameBoxForSmoke(
        std::uint32_t row,
        std::uint32_t column,
        const juce::String& name) override
    {
        return renameBox(row, column, name);
    }

    bool setProgramChannelForSmoke(int channel) override
    {
        return setProgramChannel(channel);
    }

    bool newToolForSmoke() override
    {
        return newTool();
    }

    bool deleteToolForSmoke() override
    {
        return deleteTool();
    }

    [[nodiscard]] juce::String boxTextForSmoke(
        std::uint32_t row,
        std::uint32_t column) const override
    {
        return boxText(row, column);
    }

private:
    [[nodiscard]] const talas::project::ProjectDocument& document() const
    {
        return bridge_.editor().document();
    }

    [[nodiscard]] const talas::project::ProjectLiveSet* selectedSet() const
    {
        const auto& sets = document().liveSetLibrary.liveSets;
        const auto found = std::ranges::find(
            sets, selectedSetId_, &talas::project::ProjectLiveSet::liveSetId);
        return found == sets.end() ? nullptr : &*found;
    }

    [[nodiscard]] juce::TextButton& box(std::uint32_t row, std::uint32_t column) const
    {
        return *boxes_[static_cast<int>(row * columns + column)];
    }

    [[nodiscard]] juce::String boxText(std::uint32_t row, std::uint32_t column) const
    {
        const auto program = talas::app::liveGridProgramNumber(row, column);
        const auto* set = selectedSet();
        const auto* cell = set == nullptr ? nullptr : talas::app::liveGridCellAt(*set, row, column);
        return juce::String(program)
            + (cell == nullptr ? juce::String{} : "\n" + juceString(cell->displayName));
    }

    bool apply(const talas::project::EditResult& result, const juce::String& success)
    {
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refresh();
            return false;
        }
        if (onDocumentChange_)
            onDocumentChange_();
        refresh();
        onStatus_(result.message.empty() ? success : success + " · " + juceString(result.message));
        return true;
    }

    // The selected grid as a Compiled Sound Grid file, its name ending in
    // .prg.
    void saveLiveSet()
    {
        const auto file = talas::app::compiledSoundGridFile(bridge_.editor().document(), selectedSetId_);
        if (!file.has_value())
            return;
        const auto& sets = bridge_.editor().document().liveSetLibrary.liveSets;
        const auto set = std::ranges::find(sets, selectedSetId_, &talas::project::ProjectLiveSet::liveSetId);
        const auto name = set == sets.end() || set->displayName.empty() ? selectedSetId_ : set->displayName;
        auto chooser = std::make_shared<juce::FileChooser>("Save the grid", juce::File{}.getChildFile(
            juceString(talas::project::pathToUtf8(talas::app::compiledSoundGridFileName(name)))), "*.prg");
        juce::Component::SafePointer<LiveGridEditorPanelImpl> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, chooser, text = talas::project::serializeProjectDocument(*file)](const juce::FileChooser& chosen) {
                if (safeThis == nullptr || chosen.getResult() == juce::File{})
                    return;
                auto path = talas::project::pathFromUtf8(chosen.getResult().getFullPathName().toStdString());
                if (path.extension() != talas::app::compiledSoundGridExtension)
                    path += talas::app::compiledSoundGridExtension;
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                output << text;
                safeThis->onStatus_(output ? "Saved the grid as " + juceString(talas::project::pathToUtf8(path.filename()))
                                           : juce::String{"The grid could not be saved"});
            });
    }

    bool newLiveSet()
    {
        const auto& library = document().liveSetLibrary;
        auto set = talas::app::makeLiveGridSet(
            library, "Grid " + std::to_string(library.liveSets.size() + 1));
        const auto id = set.liveSetId;
        const auto name = set.displayName;
        if (!apply(bridge_.editor().upsertProjectLiveSet(std::move(set)), "Added Live Grid " + juceString(name)))
            return false;
        selectedSetId_ = id;
        refresh();
        return true;
    }

    void promptRenameLiveSet()
    {
        const auto* set = selectedSet();
        if (set == nullptr)
            return;
        promptName("Rename Live Grid", juceString(set->displayName),
            [this](juce::String name) { (void)renameLiveSet(name); });
    }

    bool renameLiveSet(const juce::String& name)
    {
        const auto* set = selectedSet();
        if (set == nullptr)
            return false;
        auto renamed = *set;
        renamed.displayName = name.trim().toStdString();
        return apply(bridge_.editor().upsertProjectLiveSet(std::move(renamed)), "Renamed the Live Grid");
    }

    bool deleteLiveSet()
    {
        if (selectedSet() == nullptr)
            return false;
        return apply(bridge_.editor().removeProjectLiveSet(selectedSetId_), "Deleted the Live Grid");
    }

    bool setProgramChannel(int channel)
    {
        const auto* set = selectedSet();
        if (set == nullptr)
            return false;
        std::string source;
        if (channel > 0)
        {
            const auto inputs = midiInputs_ ? midiInputs_() : std::vector<std::string>{};
            if (inputs.empty())
            {
                onStatus_("Choose a MIDI input in Audio Device Settings to map program changes");
                refresh();
                return false;
            }
            source = inputs.front();
        }
        return apply(
            bridge_.editor().upsertProjectLiveSet(talas::app::withLiveGridProgramChannel(
                *set,
                channel > 0 ? std::optional<int>{channel} : std::nullopt,
                source)),
            channel > 0
                ? "Boxes answer to program changes on channel " + juce::String(channel)
                : juce::String{"Boxes no longer answer to program changes"});
    }

    bool newTool()
    {
        const auto& library = document().liveSetLibrary;
        auto tool = talas::app::makeLiveGridTool(
            library, "Tool " + std::to_string(library.tools.size() + 1));
        const auto id = tool.toolId;
        if (!apply(bridge_.editor().upsertProjectLiveTool(std::move(tool)), "Added a Live Tool"))
            return false;
        selectedToolId_ = id;
        refresh();
        return true;
    }

    bool deleteTool()
    {
        if (selectedToolId_.empty())
            return false;
        return apply(bridge_.editor().removeProjectLiveTool(selectedToolId_), "Deleted the Live Tool");
    }

    [[nodiscard]] juce::PopupMenu boxMenu(std::uint32_t row, std::uint32_t column) const
    {
        juce::PopupMenu menu;
        const auto* set = selectedSet();
        if (set == nullptr)
            return menu;
        const auto* cell = talas::app::liveGridCellAt(*set, row, column);
        const auto choices = talas::app::liveGridTargetChoices(document());
        for (std::size_t index = 0; index < choices.size(); ++index)
        {
            menu.addItem(
                boxTargetBase + static_cast<int>(index),
                juceString(choices[index].label),
                true,
                cell != nullptr && cell->target == choices[index].target);
        }
        menu.addSeparator();
        juce::PopupMenu quantization;
        const auto kind = cell == nullptr
            ? talas::project::ProjectLiveSetQuantizationKind::Immediate
            : cell->quantization.kind;
        quantization.addItem(boxQuantizeImmediate, "Immediately", cell != nullptr,
            kind == talas::project::ProjectLiveSetQuantizationKind::Immediate);
        quantization.addItem(boxQuantizeNextBeat, "On the next beat", cell != nullptr,
            kind == talas::project::ProjectLiveSetQuantizationKind::NextBeat);
        quantization.addItem(boxQuantizeNextBar, "On the next bar", cell != nullptr,
            kind == talas::project::ProjectLiveSetQuantizationKind::NextBar);
        menu.addSubMenu("Start", quantization, cell != nullptr);
        menu.addItem(boxRename, "Rename...", cell != nullptr);
        menu.addItem(boxAddToolStep, "Add as Step of Tool", cell != nullptr && !selectedToolId_.empty());
        menu.addItem(boxClear, "Clear Box", cell != nullptr);
        return menu;
    }

    void timerCallback() override
    {
        stopTimer();
        if (pendingMenu_.has_value())
            showBoxMenu(pendingMenu_->first, pendingMenu_->second);
        pendingMenu_.reset();
    }

    [[nodiscard]] const talas::project::ProjectLiveSetSoundTarget* soundIn(std::uint32_t row, std::uint32_t column) const
    {
        const auto* set = selectedSet();
        const auto* cell = set == nullptr ? nullptr : talas::app::liveGridCellAt(*set, row, column);
        return cell == nullptr ? nullptr : std::get_if<talas::project::ProjectLiveSetSoundTarget>(&cell->target);
    }

    bool startBox(std::uint32_t row, std::uint32_t column)
    {
        const auto* set = selectedSet();
        const auto* cell = set == nullptr ? nullptr : talas::app::liveGridCellAt(*set, row, column);
        return cell != nullptr && startHandler_ != nullptr && startHandler_(set->liveSetId, cell->cellId);
    }

    // A double-click edits the box's Sound while the grid is off.
    bool editBox(std::uint32_t row, std::uint32_t column)
    {
        stopTimer();
        pendingMenu_.reset();
        const auto* sound = soundIn(row, column);
        if (sound == nullptr || editHandler_ == nullptr)
            return false;
        if (on_.getToggleState())
        {
            onStatus_("Uncheck On to edit the box's Sound");
            return false;
        }
        editHandler_(sound->soundDefinitionId);
        return true;
    }

    void showBoxMenu(std::uint32_t row, std::uint32_t column)
    {
        juce::Component::SafePointer<LiveGridEditorPanelImpl> safeThis(this);
        boxMenu(row, column).showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(&box(row, column)),
            [safeThis, row, column](int result) {
                if (safeThis != nullptr && result != 0)
                    (void)safeThis->chooseBoxMenuItem(row, column, result);
            });
    }

    bool chooseBoxMenuItem(std::uint32_t row, std::uint32_t column, int itemId)
    {
        const auto* set = selectedSet();
        if (set == nullptr)
            return false;
        const auto* cell = talas::app::liveGridCellAt(*set, row, column);
        const auto program = juce::String(talas::app::liveGridProgramNumber(row, column));
        if (itemId >= boxTargetBase)
        {
            const auto choices = talas::app::liveGridTargetChoices(document());
            const auto index = static_cast<std::size_t>(itemId - boxTargetBase);
            if (index >= choices.size())
                return false;
            return apply(
                bridge_.editor().upsertProjectLiveSetCell(
                    selectedSetId_,
                    talas::app::assignLiveGridCell(document(), *set, row, column, choices[index])),
                "Box " + program + " plays " + juceString(choices[index].label));
        }
        if (cell == nullptr)
            return false;
        switch (itemId)
        {
        case boxClear:
            return apply(
                bridge_.editor().removeProjectLiveSetCell(selectedSetId_, cell->cellId),
                "Cleared box " + program);
        case boxRename:
        {
            promptName("Rename Box " + program, juceString(cell->displayName),
                [this, row, column](juce::String name) { (void)renameBox(row, column, name); });
            return true;
        }
        case boxAddToolStep:
        {
            const auto& tools = document().liveSetLibrary.tools;
            const auto tool = std::ranges::find(
                tools, selectedToolId_, &talas::project::ProjectLiveTool::toolId);
            if (tool == tools.end())
                return false;
            return apply(
                bridge_.editor().upsertProjectLiveTool(
                    talas::app::withLiveGridToolStep(*tool, selectedSetId_, *cell)),
                "Added box " + program + " as the next step of " + juceString(tool->displayName));
        }
        case boxQuantizeImmediate:
        case boxQuantizeNextBeat:
        case boxQuantizeNextBar:
        {
            auto edited = *cell;
            edited.quantization.kind = itemId == boxQuantizeImmediate
                ? talas::project::ProjectLiveSetQuantizationKind::Immediate
                : itemId == boxQuantizeNextBeat
                ? talas::project::ProjectLiveSetQuantizationKind::NextBeat
                : talas::project::ProjectLiveSetQuantizationKind::NextBar;
            edited.quantization.tickInterval = 0;
            return apply(
                bridge_.editor().upsertProjectLiveSetCell(selectedSetId_, std::move(edited)),
                "Box " + program + " start changed");
        }
        default:
            return false;
        }
    }

    bool renameBox(std::uint32_t row, std::uint32_t column, const juce::String& name)
    {
        const auto* set = selectedSet();
        const auto* cell = set == nullptr ? nullptr : talas::app::liveGridCellAt(*set, row, column);
        if (cell == nullptr)
            return false;
        auto renamed = *cell;
        renamed.displayName = name.trim().toStdString();
        return apply(
            bridge_.editor().upsertProjectLiveSetCell(selectedSetId_, std::move(renamed)),
            "Renamed box " + juce::String(talas::app::liveGridProgramNumber(row, column)));
    }

    void promptName(
        const juce::String& title,
        const juce::String& current,
        std::function<void(juce::String)> onName)
    {
        auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", current);
        window->addButton("Rename", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<LiveGridEditorPanelImpl> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [safeThis, window, onName = std::move(onName)](int result) {
                    const auto name = window->getTextEditorContents("name");
                    if (safeThis != nullptr && result == 1)
                        onName(name);
                }),
            true);
    }

    struct DoubleClicks final : juce::MouseListener
    {
        explicit DoubleClicks(LiveGridEditorPanelImpl& panel)
            : panel_(panel)
        {
        }

        void mouseDoubleClick(const juce::MouseEvent& event) override
        {
            const auto index = panel_.boxes_.indexOf(dynamic_cast<juce::TextButton*>(event.eventComponent));
            if (index >= 0)
                (void)panel_.editBox(static_cast<std::uint32_t>(index) / columns, static_cast<std::uint32_t>(index) % columns);
        }

        LiveGridEditorPanelImpl& panel_;
    };

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    MidiInputsCallback midiInputs_;
    std::string selectedSetId_;
    std::string selectedToolId_;
    bool refreshing_ = false;
    juce::ComboBox setSelector_;
    juce::TextButton newSet_;
    juce::TextButton renameSet_;
    juce::TextButton deleteSet_;
    juce::TextButton saveSet_;
    juce::Label channelLabel_;
    juce::ComboBox channel_;
    juce::Label toolLabel_;
    juce::ComboBox toolSelector_;
    juce::TextButton newTool_;
    juce::TextButton deleteTool_;
    // Before the boxes it listens to, so it outlives them.
    DoubleClicks doubleClicks_{*this};
    juce::Viewport gridViewport_;
    juce::Component gridContainer_;
    juce::OwnedArray<juce::TextButton> boxes_;
    juce::ToggleButton on_;
    OnCallback onHandler_;
    CellCallback startHandler_;
    SoundCallback editHandler_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> pendingMenu_;
};

class ToolWindow final : public juce::DocumentWindow
{
public:
    ToolWindow(const juce::String& title, juce::Component& content, std::function<void()> onClose,
        std::function<bool()> mayClose)
        : juce::DocumentWindow(
              title,
              backgroundColour,
              juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
        , onClose_(std::move(onClose))
        , mayClose_(std::move(mayClose))
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        if (mayClose_ && !mayClose_())
            return;
        if (onClose_)
            onClose_();
        setVisible(false);
    }

private:
    std::function<void()> onClose_;
    std::function<bool()> mayClose_;
};
}

std::unique_ptr<LiveGridEditorPanel> makeLiveGridEditorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    LiveGridEditorPanel::DocumentChangeCallback onDocumentChange,
    LiveGridEditorPanel::StatusCallback onStatus,
    LiveGridEditorPanel::MidiInputsCallback midiInputs)
{
    return std::make_unique<LiveGridEditorPanelImpl>(
        bridge, std::move(onDocumentChange), std::move(onStatus), std::move(midiInputs));
}

std::unique_ptr<juce::DocumentWindow> makeToolWindow(
    const juce::String& title,
    juce::Component& content,
    std::function<void()> onClose,
    std::function<bool()> mayClose)
{
    return std::make_unique<ToolWindow>(title, content, std::move(onClose), std::move(mayClose));
}

bool isInToolWindow(const juce::Component& component)
{
    return component.findParentComponentOfClass<ToolWindow>() != nullptr;
}
}
