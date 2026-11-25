#include "app/workbench/WorkbenchSoundBrowser.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <array>
#include <atomic>
#include <memory>
#include <thread>

namespace talas::app::workbench
{
namespace
{
using Kind = talas::app::SoundBrowserKind;

constexpr std::array<Kind, 8> boxKinds{
    Kind::SoundFile, Kind::Sound, Kind::Sample, Kind::Spectrum, Kind::Ga, Kind::Re, Kind::Midi, Kind::Expression};

[[nodiscard]] juce::String text(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str());
}

// The coloured boxes at the top of the browser: the leftmost grey
// one shows every kind; a click on a colour shows that kind only, and
// Shift+Click adds or removes it. Each box's help names its kind.
class KindBoxes final : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void(std::optional<Kind>, bool)> onClick;
    std::function<bool(Kind)> shows;

    void paint(juce::Graphics& graphics) override
    {
        for (std::size_t index = 0; index <= boxKinds.size(); ++index)
        {
            const auto bounds = boxBounds(index).toFloat();
            const auto colour = index == 0 ? juce::Colour{0xffb8b8b8}
                                           : juce::Colour{talas::app::soundBrowserKindColour(boxKinds[index - 1])};
            const auto shown = index == 0 || (shows && shows(boxKinds[index - 1]));
            graphics.setColour(shown ? colour : colour.withAlpha(0.25F));
            graphics.fillRoundedRectangle(bounds, 3.0F);
            graphics.setColour(panelBorderColour);
            graphics.drawRoundedRectangle(bounds, 3.0F, 1.0F);
        }
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        const auto index = boxAt(event.getPosition());
        setTooltip(!index.has_value() ? juce::String{}
                : *index == 0         ? juce::String{"All kinds of file"}
                                      : text(std::string{talas::app::soundBrowserKindName(boxKinds[*index - 1])}));
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto index = boxAt(event.getPosition());
        if (!index.has_value() || !onClick)
            return;
        onClick(*index == 0 ? std::optional<Kind>{} : std::optional<Kind>{boxKinds[*index - 1]}, event.mods.isShiftDown());
        repaint();
    }

private:
    [[nodiscard]] juce::Rectangle<int> boxBounds(std::size_t index) const
    {
        return {static_cast<int>(index) * 24, 2, 20, std::max(4, getHeight() - 4)};
    }

    [[nodiscard]] std::optional<std::size_t> boxAt(juce::Point<int> point) const
    {
        for (std::size_t index = 0; index <= boxKinds.size(); ++index)
        {
            if (boxBounds(index).contains(point))
                return index;
        }
        return std::nullopt;
    }
};

class SoundBrowserPanelImpl final : public SoundBrowserPanel, public juce::DragAndDropContainer, private juce::ListBoxModel
{
public:
    explicit SoundBrowserPanelImpl(SoundBrowserHost host)
        : host_(std::move(host))
    {
        for (auto* button : {&play_, &stop_, &audioInput_, &substitute_, &connection_, &refresh_})
            addAndMakeVisible(*button);
        describeControl(play_, "Play", "Play the selected Sound, sample or file (Space pauses and resumes, Ctrl+Space plays it again).");
        describeControl(stop_, "Stop", "Stop playing (Ctrl+K).");
        describeControl(audioInput_, "Audio input", "Make the next audio input the replacement input; after the last it comes back to Input 1.");
        describeControl(substitute_, "Substitute input", "Make the selected Sound or sample the replacement input.");
        describeControl(connection_, "Connection", "Engage or disengage the replacement input; disengaged, Sounds hear their own inputs.");
        describeControl(refresh_, "Refresh", "Read the folders and Sound files again.");
        connection_.setClickingTogglesState(false);
        play_.onClick = [this] {
            const auto mods = juce::ModifierKeys::currentModifiers;
            playSelected(mods.isCommandDown() || mods.isCtrlDown());
        };
        stop_.onClick = [this] {
            if (host_.stop)
                host_.stop();
        };
        audioInput_.onClick = [this] { pressAudioInput(); };
        substitute_.onClick = [this] { pressSubstitute(); };
        connection_.onClick = [this] { pressConnection(); };
        refresh_.onClick = [this] {
            model_.refresh();
            refresh();
        };
        inputField_.setReadOnly(true);
        inputField_.setComponentID("sound-browser.input");
        describeControl(inputField_, "Replacement input", "The input substituted for the input of each Sound that processes one.");
        addAndMakeVisible(inputField_);
        kinds_.shows = [this](Kind kind) { return model_.shows(kind); };
        kinds_.onClick = [this](std::optional<Kind> kind, bool shift) { clickKind(kind, shift); };
        addAndMakeVisible(kinds_);
        list_.setModel(this);
        list_.setRowHeight(20);
        list_.setComponentID("sound-browser.list");
        list_.setTitle("Sound Browser");
        list_.setWantsKeyboardFocus(true);
        list_.addKeyListener(&keys_);
        keys_.owner = this;
        addAndMakeVisible(list_);
        info_.setMultiLine(true);
        info_.setReadOnly(true);
        info_.setComponentID("sound-browser.info");
        info_.setTitle("Information on the selected Sound or file");
        addAndMakeVisible(info_);
        setSize(560, 520);
        showInput();
    }

    ~SoundBrowserPanelImpl() override
    {
        stopFind();
        if (finder_.joinable())
            finder_.join();
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
        list_.removeKeyListener(&keys_);
    }

    // Any click stops a search.
    void mouseDown(const juce::MouseEvent&) override
    {
        stopFind();
    }

    void setRoots(std::vector<std::filesystem::path> roots) override
    {
        model_.setRoots(std::move(roots));
        refresh();
    }

    void refresh() override
    {
        rows_ = model_.rows();
        list_.updateContent();
        list_.repaint();
        kinds_.repaint();
    }

    void showInput() override
    {
        const auto input = host_.input ? host_.input() : talas::app::SoundBrowserReplacementInput{};
        inputField_.setText(text(talas::app::soundBrowserInputText(input)), false);
        connection_.setToggleState(input.connected, juce::dontSendNotification);
        connection_.setButtonText(input.connected ? "Connected" : "Connect");
    }

    [[nodiscard]] std::vector<talas::app::SoundBrowserRow> rowsForSmoke() const override
    {
        return rows_;
    }

    bool selectRowForSmoke(std::size_t row) override
    {
        if (row >= rows_.size())
            return false;
        list_.selectRow(static_cast<int>(row));
        selectedRowsChanged(static_cast<int>(row));
        return true;
    }

    [[nodiscard]] juce::String informationForSmoke() const override
    {
        return info_.getText();
    }

    void toggleRowForSmoke(std::size_t row) override
    {
        if (row < rows_.size())
            toggle(rows_[row]);
    }

    void clickKindForSmoke(std::optional<Kind> kind, bool shift) override
    {
        clickKind(kind, shift);
    }

    bool findForSmoke(const std::string& wanted) override
    {
        lastFind_ = wanted;
        return findNext(std::nullopt);
    }

    bool findAgainForSmoke() override
    {
        return findNext(selected());
    }

    [[nodiscard]] std::optional<talas::app::SoundBrowserRow> selectedRowForSmoke() const override
    {
        return selected();
    }

    [[nodiscard]] std::optional<talas::app::SoundBrowserRow> selectedRow() const override { return selected(); }

    void pressAudioInputForSmoke() override
    {
        pressAudioInput();
    }

    void pressSubstituteForSmoke() override
    {
        pressSubstitute();
    }

    void pressConnectionForSmoke() override
    {
        pressConnection();
    }

    void pressPlayForSmoke(bool control) override
    {
        playSelected(control);
    }

    [[nodiscard]] juce::String inputTextForSmoke() const override
    {
        return inputField_.getText();
    }

    bool keyForSmoke(const juce::KeyPress& key) override
    {
        return handleKey(key);
    }

    void activateRowForSmoke(std::size_t row) override
    {
        activate(static_cast<int>(row));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto top = bounds.removeFromTop(26);
        play_.setBounds(top.removeFromLeft(54));
        top.removeFromLeft(4);
        stop_.setBounds(top.removeFromLeft(54));
        top.removeFromLeft(10);
        refresh_.setBounds(top.removeFromRight(70));
        top.removeFromRight(6);
        connection_.setBounds(top.removeFromRight(84));
        top.removeFromRight(4);
        audioInput_.setBounds(top.removeFromLeft(50));
        top.removeFromLeft(4);
        substitute_.setBounds(top.removeFromLeft(36));
        top.removeFromLeft(4);
        inputField_.setBounds(top);
        bounds.removeFromTop(6);
        kinds_.setBounds(bounds.removeFromTop(20));
        bounds.removeFromTop(4);
        info_.setBounds(bounds.removeFromBottom(92));
        bounds.removeFromBottom(6);
        list_.setBounds(bounds);
    }

private:
    struct Keys final : juce::KeyListener
    {
        SoundBrowserPanelImpl* owner = nullptr;
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override
        {
            return owner != nullptr && owner->handleKey(key);
        }
    };

    // A search under way, which a click or a key stops.
    struct FindJob
    {
        talas::app::SoundBrowserModel model;
        std::string text;
        std::optional<talas::app::SoundBrowserRow> after;
        std::optional<talas::app::SoundBrowserRow> found;
        std::atomic<bool> stop{false};
    };

    [[nodiscard]] std::optional<talas::app::SoundBrowserRow> selected() const
    {
        const auto row = list_.getSelectedRow();
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return std::nullopt;
        return rows_[static_cast<std::size_t>(row)];
    }

    // Space plays or pauses, Ctrl+Space plays from the beginning, Ctrl+K
    // stops, Ctrl+R plays the last Sound again, Ctrl+F finds and Ctrl+G
    // finds again, Ctrl+C copies the Sound's widgets.
    bool handleKey(const juce::KeyPress& key)
    {
        // Any key stops a search.
        if (finding_ != nullptr)
        {
            stopFind();
            return true;
        }
        const auto command = key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown();
        if (key.getKeyCode() == juce::KeyPress::spaceKey)
        {
            if (command)
                playSelected();
            else if (host_.pauseOrResume)
                host_.pauseOrResume();
            return true;
        }
        if (!command)
        {
            if (key.getKeyCode() == juce::KeyPress::returnKey)
            {
                activate(list_.getSelectedRow());
                return true;
            }
            return false;
        }
        const auto character = juce::CharacterFunctions::toLowerCase(key.getTextCharacter() != 0
                ? key.getTextCharacter()
                : static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (character == 'k' && host_.stop)
            host_.stop();
        else if (character == 'r' && host_.replay)
            host_.replay();
        else if (character == 'f')
            askFind();
        else if (character == 'g')
            startFind(selected());
        else if (const auto row = selected(); character == 'c' && row.has_value() && host_.copy)
            host_.copy(*row);
        else
            return false;
        return true;
    }

    void playSelected(bool once = false)
    {
        const auto row = selected();
        if (!row.has_value())
            return;
        if (once && host_.playOnce)
            host_.playOnce(*row);
        else if (host_.play)
            host_.play(*row);
    }

    void pressAudioInput()
    {
        if (!host_.setInput)
            return;
        host_.setInput(talas::app::nextAudioInput(host_.input ? host_.input() : talas::app::SoundBrowserReplacementInput{},
            host_.audioInputCount ? host_.audioInputCount() : 2));
        showInput();
    }

    void pressSubstitute()
    {
        const auto row = selected();
        const auto input = row.has_value() ? talas::app::substituteInput(*row) : std::nullopt;
        if (!input.has_value() || !host_.setInput)
            return;
        host_.setInput(*input);
        showInput();
    }

    void pressConnection()
    {
        if (!host_.setInput || !host_.input)
            return;
        auto input = host_.input();
        input.connected = !input.connected;
        host_.setInput(input);
        showInput();
    }

    void clickKind(std::optional<Kind> kind, bool shift)
    {
        if (!kind.has_value())
            model_.showAll();
        else if (shift)
            model_.toggleKind(*kind);
        else
            model_.showOnly(*kind);
        refresh();
    }

    void toggle(const talas::app::SoundBrowserRow& row)
    {
        if (row.kind != Kind::Folder && row.kind != Kind::SoundFile)
            return;
        model_.toggle(row.path);
        refresh();
    }

    void askFind()
    {
        auto* window = new juce::AlertWindow("Find", "Type a name, or part of one.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", text(lastFind_));
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SoundBrowserPanelImpl> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                safeThis->lastFind_ = window->getTextEditorContents("name").toStdString();
                safeThis->startFind(safeThis->selected());
            }),
            true);
    }

    bool findNext(const std::optional<talas::app::SoundBrowserRow>& after)
    {
        const auto found = model_.find(lastFind_, after);
        return found.has_value() && showFound(*found);
    }

    // Find away from the message thread, on a copy of the browser, so that
    // a click or a key can stop it; the copy, with
    // the folders holding the match open, takes the browser's place.
    void startFind(const std::optional<talas::app::SoundBrowserRow>& after)
    {
        if (finding_ != nullptr || lastFind_.empty())
            return;
        if (finder_.joinable())
            finder_.join();
        auto job = std::make_shared<FindJob>();
        job->model = model_;
        job->text = lastFind_;
        job->after = after;
        finding_ = job;
        info_.setText("Finding \"" + text(lastFind_) + "\"... click or press any key to stop.", false);
        juce::Desktop::getInstance().addGlobalMouseListener(this);
        if (list_.isShowing())
            list_.grabKeyboardFocus();
        juce::Component::SafePointer<SoundBrowserPanelImpl> safeThis(this);
        finder_ = std::thread([job, safeThis] {
            job->found = job->model.find(job->text, job->after, [&job] { return job->stop.load(); });
            juce::MessageManager::callAsync([job, safeThis] {
                if (safeThis != nullptr)
                    safeThis->finishFind(job);
            });
        });
    }

    void stopFind()
    {
        if (finding_ != nullptr)
            finding_->stop = true;
    }

    void finishFind(const std::shared_ptr<FindJob>& job)
    {
        if (job != finding_)
            return;
        finding_.reset();
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
        if (job->stop)
        {
            info_.setText("Find stopped.", false);
            return;
        }
        if (!job->found.has_value())
        {
            info_.setText("Nothing more named \"" + text(job->text) + "\".", false);
            return;
        }
        model_ = std::move(job->model);
        (void) showFound(*job->found);
    }

    bool showFound(const talas::app::SoundBrowserRow& found)
    {
        refresh();
        const auto index = std::ranges::find_if(rows_, [&found](const talas::app::SoundBrowserRow& row) {
            return row.path == found.path && row.definitionId == found.definitionId;
        });
        if (index == rows_.end())
            return false;
        const auto row = static_cast<int>(index - rows_.begin());
        list_.selectRow(row);
        list_.scrollToEnsureRowIsOnscreen(row);
        selectedRowsChanged(row);
        return true;
    }

    int getNumRows() override
    {
        return static_cast<int>(rows_.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool isSelected) override
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        const auto& entry = rows_[static_cast<std::size_t>(row)];
        if (isSelected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        auto indent = 6 + static_cast<int>(entry.depth) * 16;
        // A sideways triangle and faded colour for a closed folder or
        // Sound file, a downward one and saturated colour for an open one.
        const auto look = talas::app::soundBrowserRowLook(entry);
        auto colour = look.colour.has_value() ? juce::Colour{*look.colour} : workbenchTheme.primaryText;
        if (look.faded)
            colour = colour.withAlpha(0.65F);
        if (look.triangle != talas::app::SoundBrowserTriangle::None)
        {
            juce::Path triangle;
            const auto x = static_cast<float>(indent);
            const auto y = static_cast<float>(height) * 0.5F;
            if (look.triangle == talas::app::SoundBrowserTriangle::Down)
                triangle.addTriangle(x, y - 3.0F, x + 8.0F, y - 3.0F, x + 4.0F, y + 3.0F);
            else
                triangle.addTriangle(x + 1.0F, y - 4.0F, x + 1.0F, y + 4.0F, x + 7.0F, y);
            graphics.setColour(colour);
            graphics.fillPath(triangle);
        }
        indent += 12;
        if (look.arrow)
        {
            // A cyan arrow beside a Sound that processes its input.
            graphics.setColour(juce::Colours::cyan);
            graphics.drawArrow({static_cast<float>(indent), height * 0.5F, static_cast<float>(indent + 10), height * 0.5F},
                1.5F, 5.0F, 5.0F);
            indent += 14;
        }
        graphics.setColour(colour);
        graphics.setFont(WorkbenchType::label());
        const auto name = text(entry.name.empty() ? entry.path.string() : entry.name);
        graphics.drawText(name, indent, 0, width - indent, height, juce::Justification::centredLeft);
        if (look.underlined)
            graphics.drawHorizontalLine(height - 3, static_cast<float>(indent),
                static_cast<float>(indent + std::min(width - indent, static_cast<int>(name.length()) * 7)));
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& event) override
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        const auto& entry = rows_[static_cast<std::size_t>(row)];
        // The triangle beside a folder or Sound file opens or closes it.
        const auto triangle = 6 + static_cast<int>(entry.depth) * 16;
        if (event.x >= triangle - 2 && event.x <= triangle + 12)
            toggle(entry);
    }

    void selectedRowsChanged(int row) override
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        // MIDI shows in green for a Sound that expects the keyboard.
        info_.setReadOnly(false);
        info_.clear();
        const auto information = talas::app::soundBrowserInformation(rows_[static_cast<std::size_t>(row)]);
        for (std::size_t index = 0; index < information.size(); ++index)
        {
            info_.setColour(juce::TextEditor::textColourId, information[index] == "MIDI" ? workbenchTheme.success : textColour);
            info_.insertTextAtCaret(text(information[index]) + (index + 1 < information.size() ? "\n" : ""));
        }
        info_.setColour(juce::TextEditor::textColourId, textColour);
        info_.setReadOnly(true);
    }

    // A Sound or a file dragged into a
    // Sound file window, a signal flow or the Timeline.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
    {
        if (rows.size() != 1)
            return {};
        return dragDescriptionForSmoke(static_cast<std::size_t>(std::max(0, rows[0])));
    }

    [[nodiscard]] juce::String dragDescriptionForSmoke(std::size_t row) const override
    {
        if (row >= rows_.size())
            return {};
        const auto description = talas::app::soundBrowserDragDescription(rows_[row]);
        return description.empty() ? juce::String{} : juce::String::fromUTF8(description.c_str());
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        activate(row);
    }

    // A double-click on a folder spawns a browser of it alone; on a
    // Sound it brings the Sound into the project, with the replacement
    // input while it is connected; on a sample or a spectrum it
    // opens its editor; on another file it plays it.
    void activate(int row)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        const auto entry = rows_[static_cast<std::size_t>(row)];
        switch (talas::app::soundBrowserActivation(entry.kind))
        {
        case talas::app::SoundBrowserActivation::Spawn:
            if (host_.spawn)
                host_.spawn(entry.path);
            return;
        case talas::app::SoundBrowserActivation::Toggle:
            toggle(entry);
            return;
        case talas::app::SoundBrowserActivation::OpenSoundFileWindow:
            if (host_.openSoundFileWindow)
                host_.openSoundFileWindow(entry.path);
            else
                toggle(entry);
            return;
        case talas::app::SoundBrowserActivation::Edit:
            if (host_.addToProject)
                host_.addToProject(entry);
            return;
        case talas::app::SoundBrowserActivation::OpenInEditor:
            if (host_.open)
                host_.open(entry);
            return;
        case talas::app::SoundBrowserActivation::Play:
            break;
        case talas::app::SoundBrowserActivation::Nothing:
            return;
        }
        if (host_.play)
            host_.play(entry);
    }

    SoundBrowserHost host_;
    talas::app::SoundBrowserModel model_;
    std::vector<talas::app::SoundBrowserRow> rows_;
    std::string lastFind_;
    std::shared_ptr<FindJob> finding_;
    std::thread finder_;
    juce::TextButton play_{"Play"};
    juce::TextButton stop_{"Stop"};
    juce::TextButton audioInput_{"Input"};
    juce::TextButton substitute_{"<"};
    juce::TextButton connection_{"Connect"};
    juce::TextButton refresh_{"Refresh"};
    juce::TextEditor inputField_;
    KindBoxes kinds_;
    juce::ListBox list_;
    juce::TextEditor info_;
    Keys keys_;
};
}

std::unique_ptr<SoundBrowserPanel> makeSoundBrowserPanel(SoundBrowserHost host)
{
    return std::make_unique<SoundBrowserPanelImpl>(std::move(host));
}
}
