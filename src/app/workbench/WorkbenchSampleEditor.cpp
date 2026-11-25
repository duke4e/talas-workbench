#include "app/workbench/WorkbenchSampleEditor.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/TimelineRecording.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace talas::app::workbench
{
namespace
{
// The sample file format a type saves in.
talas::audio::SampleFileFormat audioFormatOf(talas::app::SampleFileType type)
{
    switch (type)
    {
    case talas::app::SampleFileType::Wav: return talas::audio::SampleFileFormat::Wave;
    case talas::app::SampleFileType::Aiff: return talas::audio::SampleFileFormat::Aiff;
    case talas::app::SampleFileType::Ircam: return talas::audio::SampleFileFormat::Ircam;
    case talas::app::SampleFileType::SoundDesignerI: return talas::audio::SampleFileFormat::SoundDesignerI;
    case talas::app::SampleFileType::SoundDesignerII: return talas::audio::SampleFileFormat::SoundDesignerII;
    }
    return talas::audio::SampleFileFormat::Wave;
}

// The type lists of Save and Edit Info: their names, and the type at an
// index.
juce::StringArray sampleFileTypeNames()
{
    juce::StringArray names;
    for (const auto type : talas::app::sampleFileTypes())
        names.add(juce::String{talas::app::sampleFileTypeName(type)});
    return names;
}

talas::app::SampleFileType sampleFileTypeAt(int index)
{
    const auto types = talas::app::sampleFileTypes();
    return types[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(types.size()) - 1))];
}

int sampleFileTypeIndex(talas::app::SampleFileType type)
{
    const auto types = talas::app::sampleFileTypes();
    const auto found = std::ranges::find(types, type);
    return found == types.end() ? 0 : static_cast<int>(found - types.begin());
}

class SampleEditorPanelImpl final : public SampleEditorPanel, private juce::Timer
{
public:
    SampleEditorPanelImpl(StatusCallback onStatus, UseInProjectCallback onUseInProject, PlayCallback onPlay,
        CopiedCallback onCopied)
        : onStatus_(std::move(onStatus))
        , onUseInProject_(std::move(onUseInProject))
        , onPlay_(std::move(onPlay))
        , onCopied_(std::move(onCopied))
    {
        setTitle("Sample Editor");
        setWantsKeyboardFocus(true);
        // The insertion point blinks.
        startTimer(250);
        for (const auto& name : {"New", "Open...", "Save...", "Use in Project...", "Play", "Paste File...",
                 "Edit Info...", "Cut", "Copy", "Paste", "Clear", "Trim", "Undo", "Redo", "Select All", "Fill",
                 "Zoom In", "Zoom Out", "Level In", "Level Out", "<", ">", "Insert"})
        {
            auto button = std::make_unique<juce::TextButton>(name);
            button->onClick = [this, action = juce::String{name}] { (void)perform(action); };
            addAndMakeVisible(*button);
            buttons_.push_back(std::move(button));
        }
        pencil_.setButtonText("Pencil");
        pencil_.setTooltip("Draw samples with the mouse instead of selecting");
        addAndMakeVisible(pencil_);
        lissajous_.setButtonText("Lissajous");
        lissajous_.setTooltip("Show the waveform across against itself some samples later up");
        lissajous_.onClick = [this] {
            if (lissajous_.getToggleState())
                askLissajousDelay();
            repaint();
        };
        addAndMakeVisible(lissajous_);
        int id = 1;
        for (const auto kind : talas::app::sampleTemplates())
            templates_.addItem(juce::String{std::string{talas::app::sampleTemplateName(kind)}}, id++);
        // The Programs: each template's Smalltalk code to read, change and
        // run.
        templates_.addSectionHeading("Programs");
        for (const auto program : talas::app::sampleTemplatePrograms())
            templates_.addItem(juce::String{std::string{talas::app::sampleTemplateProgramName(program)}}, id++);
        templates_.setTitle("Generator template");
        templates_.onChange = [this] { showTemplateFields(); };
        addAndMakeVisible(templates_);
        templates_.setSelectedItemIndex(0, juce::sendNotificationSync);
        newFile(48000.0, 1, 0.0);
        setSize(820, 520);
    }

    talas::app::SampleEditorModel& model() noexcept override
    {
        return model_;
    }

    void newFile(double sampleRate, std::size_t channels, double seconds) override
    {
        model_ = talas::app::SampleEditorModel(sampleRate, channels,
            static_cast<std::size_t>(std::max(0.0, seconds) * sampleRate));
        path_.clear();
        savedFormat_.reset();
        applyView(talas::app::sampleViewOfWholeFile(model_.frames()));
        repaint();
    }

    bool openFile(const std::filesystem::path& path) override
    {
        auto read = talas::audio::readSampleFile(path);
        if (!read.ok())
        {
            onStatus_("The sample could not be read: " + juce::String{read.error});
            return false;
        }
        if (read.channels.size() > talas::app::maximumSampleEditorChannels)
            read.channels.resize(talas::app::maximumSampleEditorChannels);
        auto opened = talas::app::SampleEditorModel::fromChannels(read.sampleRate, std::move(read.channels));
        if (!opened)
        {
            onStatus_("The sample editor takes mono and stereo files");
            return false;
        }
        model_ = std::move(*opened);
        path_ = path;
        savedFormat_.reset();
        applyView(talas::app::sampleViewOfWholeFile(model_.frames()));
        repaint();
        onStatus_("Opened " + juce::String{talas::project::pathToUtf8(path.filename())});
        return true;
    }

    void showSamples(const juce::String& name, std::vector<std::vector<double>> channels, double sampleRate) override
    {
        if (channels.size() > talas::app::maximumSampleEditorChannels)
            channels.resize(talas::app::maximumSampleEditorChannels);
        auto shown = talas::app::SampleEditorModel::fromChannels(sampleRate, std::move(channels));
        if (!shown)
        {
            onStatus_("The sample editor could not show " + name);
            return;
        }
        model_ = std::move(*shown);
        path_.clear();
        savedFormat_.reset();
        applyView(talas::app::sampleViewOfWholeFile(model_.frames()));
        repaint();
        onStatus_("Showing " + name);
    }

    bool saveFile(const std::filesystem::path& path) override
    {
        return saveFileAs(path, {talas::app::SampleFileType::Wav, model_.channelCount(), 16});
    }

    bool saveFileAs(const std::filesystem::path& path, talas::app::SampleFileFormat format) override
    {
        if (model_.frames() == 0)
        {
            onStatus_("An empty sample file has nothing to save");
            return false;
        }
        const auto channels = talas::app::sampleChannelsToSave(model_.channels(), format.channels);
        std::string error;
        if (format.type != talas::app::SampleFileType::Wav)
            error = talas::audio::writeSampleFile(path, model_.sampleRate(), channels, format.bits, audioFormatOf(format.type));
        else if (format.bits == 16)
            (void)talas::audio::writePcm16Wav(path, model_.sampleRate(), channels, error);
        else if (auto* file = std::fopen(path.c_str(), "wb"); file == nullptr)
            error = "the file could not be opened";
        else
        {
            talas::audio::Pcm16WavStreamWriter writer{file, talas::project::pathToUtf8(path), model_.sampleRate(), channels.size(),
                talas::audio::WavWordSize{format.bits}};
            if (!writer.ready() || !writer.write(channels) || !writer.finish())
                error = std::string{writer.error()};
        }
        if (!error.empty())
        {
            onStatus_("The sample could not be saved: " + juce::String{error});
            return false;
        }
        path_ = path;
        savedFormat_ = format;
        model_.markSaved();
        onStatus_("Saved " + juce::String{talas::project::pathToUtf8(path.filename())});
        return true;
    }

    bool saveAndUseInProject(const std::filesystem::path& path) override
    {
        if (!saveFile(path))
            return false;
        if (onUseInProject_)
            onUseInProject_(path);
        return true;
    }

    juce::String readoutForSmoke() const override
    {
        return readoutText();
    }

    void switchReadoutUnitsForSmoke() override
    {
        inSamples_ = !inSamples_;
        inLinear_ = !inLinear_;
    }

    bool pasteFileForSmoke(const std::filesystem::path& path) override
    {
        return pasteFile(path);
    }

    std::size_t lissajousPointsForSmoke(std::size_t delay) override
    {
        lissajousDelay_ = delay;
        lissajous_.setToggleState(true, juce::dontSendNotification);
        return talas::app::sampleLissajous(model_, lissajousDelay_).size();
    }

    bool chooseTemplateForSmoke(talas::app::SampleTemplate kind) override
    {
        const auto& all = talas::app::sampleTemplates();
        const auto found = std::ranges::find(all, kind);
        if (found == all.end())
            return false;
        templates_.setSelectedItemIndex(static_cast<int>(found - all.begin()), juce::sendNotificationSync);
        return true;
    }

    bool chooseProgramForSmoke(talas::app::SampleTemplateProgram program) override
    {
        const auto& all = talas::app::sampleTemplatePrograms();
        const auto found = std::ranges::find(all, program);
        if (found == all.end())
            return false;
        templates_.setSelectedItemIndex(
            static_cast<int>(talas::app::sampleTemplates().size()) + static_cast<int>(found - all.begin()), juce::sendNotificationSync);
        return selectedProgram() == program;
    }

    bool setTemplateFieldForSmoke(const std::string& name, const std::string& text) override
    {
        const auto found = std::ranges::find_if(fields_, [&](const auto& field) { return field.name == name; });
        if (found == fields_.end())
            return false;
        found->editor->setText(juce::String{text}, juce::dontSendNotification);
        return true;
    }

    bool insertTemplateForSmoke() override
    {
        return perform("Insert");
    }

    bool performForSmoke(const juce::String& action) override
    {
        return perform(action);
    }

    void startSave() override { (void)perform("Save..."); }

    void dragForSmoke(std::size_t fromFrame, double fromValue, std::size_t toFrame, double toValue, bool pencil) override
    {
        pencil_.setToggleState(pencil, juce::dontSendNotification);
        beginDrag(fromFrame, fromValue);
        continueDrag(toFrame, toValue);
    }

    bool templatesBelowGraphicEditorForSmoke() const override
    {
        return !detail_.isEmpty() && overview_.getBottom() <= detail_.getY() && templates_.getY() >= detail_.getBottom();
    }

    void resized() override
    {
        const auto layout = talas::app::sampleEditorLayout(getWidth(), getHeight());
        const auto area = [](const talas::app::SampleEditorArea& part) {
            return juce::Rectangle<int>{part.x, part.y, part.width, part.height};
        };
        auto bottom = area(layout.templates);
        auto toolbar = area(layout.toolbar);
        overview_ = area(layout.overview);
        detail_ = area(layout.detail);
        for (std::size_t index = 0; index + 1 < buttons_.size(); ++index)
            buttons_[index]->setBounds(toolbar.removeFromLeft(std::max(30, toolbar.getWidth() / 25)).reduced(1));
        pencil_.setBounds(toolbar.removeFromLeft(toolbar.getWidth() / 2).reduced(1));
        lissajous_.setBounds(toolbar.reduced(1));
        auto templateRow = bottom.removeFromTop(28);
        templates_.setBounds(templateRow.removeFromLeft(220).reduced(1));
        buttons_.back()->setBounds(templateRow.removeFromLeft(80).reduced(1));
        for (auto& field : fields_)
        {
            // A Program's code takes the rest of the space.
            auto row = field.editor->isMultiLine() ? bottom : bottom.removeFromTop(24);
            field.label->setBounds(row.removeFromLeft(160).withHeight(24));
            field.editor->setBounds(row.removeFromLeft(std::max(320, row.getWidth())).reduced(1));
        }
    }

    void timerCallback() override
    {
        if (model_.selection().empty() && isShowing())
            repaint(overview_.getUnion(detail_));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        drawWave(graphics, overview_, 0, std::max<std::size_t>(model_.frames(), 1), 1.0, true);
        if (lissajous_.getToggleState())
            drawLissajous(graphics, detail_);
        else
            drawWave(graphics, detail_, viewStart_, viewFrames_, level_, false);
        // The numbers beside the overview: the magnification above, the
        // selection below; a click switches seconds and samples, or dB and
        // linear.
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        const auto readout = overview_.withTrimmedLeft(overview_.getWidth() - 360);
        peakArea_ = readout.withHeight(readout.getHeight() / 2);
        selectionArea_ = readout.withTrimmedTop(readout.getHeight() / 2);
        graphics.drawText("magnification " + juce::String{talas::app::sampleMagnificationReadout(viewFrames_, detail_.getWidth())}
                + " · peak " + juce::String{talas::app::samplePeakReadout(model_.peak(), !inLinear_)},
            peakArea_, juce::Justification::topRight);
        graphics.drawText(juce::String{talas::app::sampleSelectionReadout(model_, inSamples_)} + (model_.modified() ? " *" : ""),
            selectionArea_, juce::Justification::bottomRight);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        if (selectionArea_.contains(event.getPosition()))
        {
            inSamples_ = !inSamples_;
            repaint();
            return;
        }
        if (peakArea_.contains(event.getPosition()))
        {
            inLinear_ = !inLinear_;
            repaint();
            return;
        }
        if (overview_.contains(event.getPosition()))
        {
            // The overview places the detail view, a drag moving it along
            //.
            overviewDragging_ = true;
            const auto frame = frameAt(overview_, event.position.x, 0, std::max<std::size_t>(model_.frames(), 1));
            applyView(talas::app::sampleViewCentredAt(view(), frame));
            repaint();
            return;
        }
        if (!detail_.contains(event.getPosition()))
            return;
        beginDrag(frameAt(detail_, event.position.x, viewStart_, viewFrames_), valueAt(event.position.y));
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (overviewDragging_)
        {
            const auto frame = frameAt(overview_, event.position.x, 0, std::max<std::size_t>(model_.frames(), 1));
            applyView(talas::app::sampleViewCentredAt(view(), frame));
            repaint();
            return;
        }
        if (!dragging_)
            return;
        continueDrag(frameAt(detail_, event.position.x, viewStart_, viewFrames_), valueAt(event.position.y));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragging_ = false;
        overviewDragging_ = false;
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto command = key.getModifiers().isCommandDown();
        const auto code = key.getKeyCode();
        if (command && (code == 'x' || code == 'X'))
            return perform("Cut");
        if (command && (code == 'c' || code == 'C'))
            return perform("Copy");
        if (command && (code == 'v' || code == 'V'))
            return perform("Paste");
        if (command && (code == 'z' || code == 'Z'))
            return perform(key.getModifiers().isShiftDown() ? "Redo" : "Undo");
        if (command && (code == 'a' || code == 'A'))
            return perform("Select All");
        // Ctrl+S saves the file in its own format, or asks
        // for one and a name when it has none yet.
        if (command && (code == 's' || code == 'S'))
        {
            if (path_.empty())
                return perform("Save...");
            // A format changed in Edit Info takes its type's extension.
            const auto format = currentFormat();
            auto target = path_;
            if (talas::app::sampleFileTypeOfExtension(target) != format.type)
                target.replace_extension(talas::app::sampleFileExtension(format.type));
            (void)saveFileAs(target, format);
            return true;
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
            return perform("Clear");
        return false;
    }

private:
    struct Field
    {
        std::string name;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::TextEditor> editor;
    };

    [[nodiscard]] std::size_t frameAt(juce::Rectangle<int> area, float x, std::size_t start, std::size_t frames) const
    {
        const auto proportion = std::clamp((x - static_cast<float>(area.getX())) / std::max(1.0F, static_cast<float>(area.getWidth())), 0.0F, 1.0F);
        return std::min(model_.frames(), start + static_cast<std::size_t>(std::llround(proportion * static_cast<float>(frames))));
    }

    [[nodiscard]] double valueAt(float y) const
    {
        const auto centre = static_cast<float>(detail_.getCentreY());
        return static_cast<double>((centre - y) / std::max(1.0F, static_cast<float>(detail_.getHeight()) * 0.5F)) / level_;
    }

    void beginDrag(std::size_t frame, double value)
    {
        dragging_ = true;
        anchorFrame_ = frame;
        lastFrame_ = frame;
        lastValue_ = value;
        if (pencil_.getToggleState())
        {
            model_.beginStroke();
            (void)model_.drawLine(std::nullopt, frame, value, frame, value);
        }
        else
        {
            model_.select(frame, frame);
        }
        repaint();
    }

    void continueDrag(std::size_t frame, double value)
    {
        if (pencil_.getToggleState())
            (void)model_.drawLine(std::nullopt, lastFrame_, lastValue_, frame, value);
        else
            model_.select(anchorFrame_, frame);
        lastFrame_ = frame;
        lastValue_ = value;
        repaint();
    }

    void showTemplateFields()
    {
        for (auto& field : fields_)
        {
            removeChildComponent(field.label.get());
            removeChildComponent(field.editor.get());
        }
        fields_.clear();
        const auto index = templates_.getSelectedItemIndex();
        if (const auto program = selectedProgram())
        {
            for (const auto& [name, text] : {std::pair{std::string{"Duration"}, std::string{"4096 samp"}},
                     std::pair{std::string{"Program"}, std::string{talas::app::sampleTemplateProgramSource(*program)}}})
            {
                Field field{name, std::make_unique<juce::Label>(), std::make_unique<juce::TextEditor>()};
                field.label->setText(juce::String{name}, juce::dontSendNotification);
                if (name == "Program")
                {
                    field.editor->setMultiLine(true, false);
                    field.editor->setReturnKeyStartsNewLine(true);
                    field.editor->setTabKeyUsedAsCharacter(true);
                    field.editor->setFont(juce::Font{juce::FontOptions{juce::Font::getDefaultMonospacedFontName(), 12.0F, juce::Font::plain}});
                }
                field.editor->setText(juce::String{text}, juce::dontSendNotification);
                field.editor->setTitle(juce::String{name});
                addAndMakeVisible(*field.label);
                addAndMakeVisible(*field.editor);
                fields_.push_back(std::move(field));
            }
            resized();
            return;
        }
        if (index < 0 || index >= static_cast<int>(talas::app::sampleTemplates().size()))
            return;
        for (const auto& given : talas::app::sampleTemplateFields(talas::app::sampleTemplates()[static_cast<std::size_t>(index)]))
        {
            Field field{given.name, std::make_unique<juce::Label>(), std::make_unique<juce::TextEditor>()};
            field.label->setText(juce::String{given.name}, juce::dontSendNotification);
            field.editor->setText(juce::String{given.defaultText}, juce::dontSendNotification);
            field.editor->setTitle(juce::String{given.name});
            addAndMakeVisible(*field.label);
            addAndMakeVisible(*field.editor);
            fields_.push_back(std::move(field));
        }
        resized();
    }

    // The Program chosen in the templates' list, if one is.
    [[nodiscard]] std::optional<talas::app::SampleTemplateProgram> selectedProgram() const
    {
        const auto index = templates_.getSelectedItemIndex() - static_cast<int>(talas::app::sampleTemplates().size());
        const auto& programs = talas::app::sampleTemplatePrograms();
        if (index < 0 || index >= static_cast<int>(programs.size()))
            return std::nullopt;
        return programs[static_cast<std::size_t>(index)];
    }

    // A Program run for the Duration's samples, inserted as a template's
    // samples are.
    bool insertProgram()
    {
        std::string duration;
        std::string source;
        for (const auto& field : fields_)
            (field.name == "Program" ? source : duration) = field.editor->getText().toStdString();
        const auto frames = talas::app::sampleTemplateFrames(duration, model_.sampleRate());
        if (!frames)
        {
            onStatus_("The Duration should be a number of samples or a time");
            return false;
        }
        const auto result = talas::app::runSampleTemplateProgram(source, *frames);
        if (!result.ok())
        {
            onStatus_("The program did not run: " + juce::String{result.error});
            return false;
        }
        if (!model_.insertSegment(result.samples))
        {
            onStatus_("The segment does not fit in the sample file");
            return false;
        }
        // The overview shows the new segment; the
        // detail keeps its start.
        applyView(talas::app::sampleViewAfterInsert(view(), model_.frames()));
        onStatus_(juce::String{std::string{talas::app::sampleTemplateProgramName(*selectedProgram())}} + " applied");
        return true;
    }

    bool insertTemplate()
    {
        if (selectedProgram())
            return insertProgram();
        const auto index = templates_.getSelectedItemIndex();
        if (index < 0)
            return false;
        std::map<std::string, std::string> values;
        for (const auto& field : fields_)
            values[field.name] = field.editor->getText().toStdString();
        const auto kind = talas::app::sampleTemplates()[static_cast<std::size_t>(index)];
        const auto result = talas::app::applySampleTemplate(model_, kind, values);
        if (!result.ok())
        {
            onStatus_(juce::String{result.error});
            return false;
        }
        applyView(talas::app::sampleViewAfterInsert(view(), model_.frames()));
        onStatus_(juce::String{std::string{talas::app::sampleTemplateName(kind)}} + " applied");
        return true;
    }

    bool perform(const juce::String& action)
    {
        auto changed = false;
        if (action == "New")
        {
            newFile(model_.sampleRate(), model_.channelCount(), 0.0);
            changed = true;
        }
        else if (action == "Open...")
        {
            chooser_ = std::make_unique<juce::FileChooser>("Open a Sample File", juce::File{}, "*.wav;*.aif;*.aiff;*.sf;*.sd1;*.sd2");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser) {
                    if (chooser.getResult() != juce::File{})
                        (void)openFile(talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
                });
            return true;
        }
        else if (action == "Save..." || action == "Use in Project...")
        {
            // The file type, channels and bits, Enter, then
            // the file's name.
            const auto use = action == "Use in Project...";
            auto* window = new juce::AlertWindow("Save the Sample File", "Choose the file type, the number of channels "
                "and the bits per sample, and press Enter.", juce::MessageBoxIconType::NoIcon, this);
            window->addComboBox("type", sampleFileTypeNames(), "File type");
            window->addComboBox("channels", {"1 (mono)", "2 (stereo)"}, "Channels");
            window->addComboBox("bits", {"8", "16", "24"}, "Bits per sample");
            window->getComboBoxComponent("type")->setSelectedItemIndex(0, juce::dontSendNotification);
            window->getComboBoxComponent("channels")->setSelectedItemIndex(model_.channelCount() > 1 ? 1 : 0, juce::dontSendNotification);
            window->getComboBoxComponent("bits")->setSelectedItemIndex(1, juce::dontSendNotification);
            window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
            window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
            juce::Component::SafePointer<juce::Component> safeThis(this);
            window->enterModalState(true, juce::ModalCallbackFunction::create([this, safeThis, window, use](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                const talas::app::SampleFileFormat format{
                    sampleFileTypeAt(window->getComboBoxComponent("type")->getSelectedItemIndex()),
                    static_cast<std::size_t>(window->getComboBoxComponent("channels")->getSelectedItemIndex() + 1),
                    static_cast<std::uint16_t>(8 * (window->getComboBoxComponent("bits")->getSelectedItemIndex() + 1))};
                const auto extension = juce::String{talas::app::sampleFileExtension(format.type)};
                chooser_ = std::make_unique<juce::FileChooser>("Save the Sample File",
                    path_.empty() ? juce::File{} : juce::File{juce::String{talas::project::pathToUtf8(path_)}}, "*" + extension);
                chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
                    [this, use, format, extension](const juce::FileChooser& chooser) {
                        if (chooser.getResult() == juce::File{})
                            return;
                        const auto path = talas::project::pathFromUtf8(
                            chooser.getResult().withFileExtension(extension).getFullPathName().toStdString());
                        if (saveFileAs(path, format) && use && onUseInProject_)
                            onUseInProject_(path);
                    });
            }),
                true);
            return true;
        }
        else if (action == "Cut" || action == "Copy")
        {
            changed = action == "Cut" ? model_.cut() : model_.copy();
            if (changed && onCopied_)
                onCopied_(path_);
        }
        else if (action == "Paste")
            changed = model_.paste();
        else if (action == "Clear")
            changed = model_.clear();
        else if (action == "Trim")
            changed = model_.trim();
        else if (action == "Undo")
            changed = model_.undo();
        else if (action == "Redo")
            changed = model_.redo();
        else if (action == "Select All")
        {
            model_.selectAll();
            changed = true;
        }
        else if (action == "Fill")
        {
            // The selection exactly fills the display, across and up.
            if (!model_.selection().empty())
            {
                applyView(talas::app::sampleViewFilled(view(), model_.selection(), model_.peak()));
                changed = true;
            }
        }
        else if (action == "Zoom In" || action == "Zoom Out")
        {
            applyView(talas::app::sampleViewZoomed(view(), model_.frames(), action == "Zoom In"));
            changed = true;
        }
        else if (action == "Level In" || action == "Level Out")
        {
            applyView(talas::app::sampleViewLevelled(view(), action == "Level In"));
            changed = true;
        }
        else if (action == "<" || action == ">")
        {
            // A page is the selection's size when there is one.
            applyView(talas::app::sampleViewPaged(view(), model_.frames(), action == ">", model_.selection().length()));
            changed = true;
        }
        else if (action == "Insert")
            changed = insertTemplate();
        else if (action == "Play")
        {
            // The play button plays the selection, or the whole file.
            if (!onPlay_ || model_.frames() == 0)
                return false;
            onPlay_(talas::app::sampleEditorPlayed(model_), model_.sampleRate());
            return true;
        }
        else if (action == "Paste File...")
        {
            chooser_ = std::make_unique<juce::FileChooser>("Paste a Sample File", juce::File{}, "*.wav;*.aif;*.aiff;*.sf;*.sd1;*.sd2");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser) {
                    if (chooser.getResult() != juce::File{})
                        (void)pasteFile(talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
                });
            return true;
        }
        else if (action == "Edit Info...")
        {
            editInfo();
            return true;
        }
        repaint();
        return changed;
    }

    // The disk button: another file's samples over the selection, or all of
    // it at the insertion point.
    bool pasteFile(const std::filesystem::path& path)
    {
        auto read = talas::audio::readSampleFile(path);
        if (!read.ok() || read.channels.empty())
        {
            onStatus_("The sample could not be read: " + juce::String{read.error});
            return false;
        }
        if (!model_.replaceSelection(read.channels))
        {
            onStatus_("The file is too long to paste here");
            return false;
        }
        repaint();
        onStatus_("Pasted " + juce::String{talas::project::pathToUtf8(path.filename())}
            + (read.sampleRate != model_.sampleRate() ? " · its sample rate differs" : ""));
        return true;
    }

    // Edit info: the file's format and the information in its
    // header.
    // The format the file is saved in: as last saved, else by its name's
    // extension, 16 bits.
    [[nodiscard]] talas::app::SampleFileFormat currentFormat() const
    {
        return savedFormat_.value_or(
            talas::app::SampleFileFormat{talas::app::sampleFileTypeOfExtension(path_), model_.channelCount(), 16});
    }

    // Edit Info: the file's format and its
    // header's rate and channels.
    void editInfo()
    {
        const auto format = currentFormat();
        auto* window = new juce::AlertWindow("Edit Info",
            juce::String{talas::app::sampleFileFormatText(format)} + " · " + juce::String(static_cast<juce::int64>(model_.frames()))
                + " frames",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("rate", juce::String(model_.sampleRate()), "Sample rate");
        window->addComboBox("channels", {"Mono", "Stereo"}, "Channels");
        window->getComboBoxComponent("channels")->setSelectedItemIndex(model_.channelCount() == 2 ? 1 : 0, juce::dontSendNotification);
        window->addComboBox("type", sampleFileTypeNames(), "Format");
        window->getComboBoxComponent("type")->setSelectedItemIndex(sampleFileTypeIndex(format.type),
            juce::dontSendNotification);
        window->addComboBox("bits", {"8", "16", "24"}, "Bits per sample");
        window->getComboBoxComponent("bits")->setSelectedItemIndex(format.bits == 8 ? 0 : format.bits == 24 ? 2 : 1,
            juce::dontSendNotification);
        window->addButton("Set", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SampleEditorPanelImpl> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                const auto rate = window->getTextEditorContents("rate").getDoubleValue();
                const auto channels = window->getComboBoxComponent("channels")->getSelectedItemIndex() == 1 ? 2U : 1U;
                const auto type = sampleFileTypeAt(window->getComboBoxComponent("type")->getSelectedItemIndex());
                const std::array<std::uint16_t, 3> bitChoices{8, 16, 24};
                const auto bits = bitChoices[static_cast<std::size_t>(
                    std::clamp(window->getComboBoxComponent("bits")->getSelectedItemIndex(), 0, 2))];
                if (safeThis == nullptr || result != 1)
                    return;
                if (!safeThis->model_.setHeader(rate, channels))
                    safeThis->onStatus_("The header was left as it was");
                else if (const auto chosen = talas::app::sampleFileFormatFromInfo(type, bits, channels))
                    safeThis->savedFormat_ = *chosen;
                safeThis->repaint();
            }),
            true);
    }

    void askLissajousDelay()
    {
        auto* window = new juce::AlertWindow("Display Lissajous", "Samples of delay", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("delay", juce::String(static_cast<juce::int64>(lissajousDelay_)), "Delay");
        window->addButton("Show", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SampleEditorPanelImpl> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                const auto delay = window->getTextEditorContents("delay").getIntValue();
                if (safeThis == nullptr)
                    return;
                if (result == 1 && delay >= 0)
                    safeThis->lissajousDelay_ = static_cast<std::size_t>(delay);
                else
                    safeThis->lissajous_.setToggleState(false, juce::dontSendNotification);
                safeThis->repaint();
            }),
            true);
    }

    [[nodiscard]] juce::String readoutText() const
    {
        return "magnification " + juce::String{talas::app::sampleMagnificationReadout(viewFrames_, std::max(1, detail_.getWidth()))}
            + " · peak " + juce::String{talas::app::samplePeakReadout(model_.peak(), !inLinear_)} + " · "
            + juce::String{talas::app::sampleSelectionReadout(model_, inSamples_)};
    }

    void drawLissajous(juce::Graphics& graphics, juce::Rectangle<int> area) const
    {
        graphics.setColour(workbenchTheme.controlTile);
        graphics.fillRect(area);
        const auto square = area.withSizeKeepingCentre(std::min(area.getWidth(), area.getHeight()), std::min(area.getWidth(), area.getHeight()));
        graphics.setColour(workbenchTheme.graphGrid);
        graphics.drawRect(square);
        graphics.setColour(audioColour);
        const auto half = static_cast<float>(square.getWidth()) * 0.5F;
        for (const auto& [across, up] : talas::app::sampleLissajous(model_, lissajousDelay_))
        {
            const auto x = static_cast<float>(square.getCentreX()) + static_cast<float>(across * level_) * half;
            const auto y = static_cast<float>(square.getCentreY()) - static_cast<float>(up * level_) * half;
            graphics.fillRect(x, y, 1.5F, 1.5F);
        }
    }

    void drawWave(juce::Graphics& graphics, juce::Rectangle<int> area, std::size_t start, std::size_t frames,
        double level, bool overview) const
    {
        graphics.setColour(workbenchTheme.controlTile);
        graphics.fillRect(area);
        const auto& selection = model_.selection();
        if (!selection.empty() && frames > 0)
        {
            const auto x0 = area.getX() + static_cast<float>(static_cast<double>(selection.start) - static_cast<double>(start)) / static_cast<float>(frames) * static_cast<float>(area.getWidth());
            const auto x1 = area.getX() + static_cast<float>(static_cast<double>(selection.end) - static_cast<double>(start)) / static_cast<float>(frames) * static_cast<float>(area.getWidth());
            graphics.setColour(selectionColour.withAlpha(0.25F));
            graphics.fillRect(juce::Rectangle<float>::leftTopRightBottom(std::max(x0, static_cast<float>(area.getX())), static_cast<float>(area.getY()),
                std::min(x1, static_cast<float>(area.getRight())), static_cast<float>(area.getBottom())));
        }
        else if (frames > 0 && selection.start >= start && selection.start <= start + frames
            && talas::app::sampleInsertionPointLit(juce::Time::getMillisecondCounter()))
        {
            const auto x = area.getX() + static_cast<float>(static_cast<double>(selection.start - start) / static_cast<double>(frames)) * static_cast<float>(area.getWidth());
            graphics.setColour(juce::Colour{talas::app::sampleInsertionPointColour()});
            graphics.drawVerticalLine(juce::roundToInt(x), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
        }
        if (overview && model_.frames() > 0)
        {
            graphics.setColour(textColour.withAlpha(0.5F));
            const auto x0 = area.getX() + static_cast<float>(viewStart_) / static_cast<float>(model_.frames()) * static_cast<float>(area.getWidth());
            const auto width = static_cast<float>(viewFrames_) / static_cast<float>(model_.frames()) * static_cast<float>(area.getWidth());
            graphics.drawRect(juce::Rectangle<float>{x0, static_cast<float>(area.getY()), std::max(2.0F, width), static_cast<float>(area.getHeight())});
        }
        const auto channels = talas::app::sampleEditorShownChannels(model_.channelCount());
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const auto lane = area.withHeight(area.getHeight() / static_cast<int>(channels))
                                  .withY(area.getY() + area.getHeight() / static_cast<int>(channels) * static_cast<int>(channel));
            graphics.setColour(workbenchTheme.graphGrid);
            graphics.drawHorizontalLine(lane.getCentreY(), static_cast<float>(lane.getX()), static_cast<float>(lane.getRight()));
            if (channel >= model_.channelCount() || frames == 0)
                continue;
            const auto& data = model_.channels()[channel];
            graphics.setColour(audioColour);
            for (int x = 0; x < lane.getWidth(); ++x)
            {
                const auto first = start + static_cast<std::size_t>(static_cast<double>(x) / lane.getWidth() * static_cast<double>(frames));
                const auto last = std::max(first + 1, start + static_cast<std::size_t>(static_cast<double>(x + 1) / lane.getWidth() * static_cast<double>(frames)));
                auto low = 0.0;
                auto high = 0.0;
                for (auto frame = first; frame < std::min(last, data.size()); ++frame)
                {
                    low = std::min(low, data[frame]);
                    high = std::max(high, data[frame]);
                }
                const auto half = static_cast<double>(lane.getHeight()) * 0.5 * level;
                graphics.drawVerticalLine(lane.getX() + x,
                    static_cast<float>(std::max<double>(lane.getY(), lane.getCentreY() - high * half)),
                    static_cast<float>(std::min<double>(lane.getBottom(), lane.getCentreY() - low * half + 1.0)));
            }
        }
    }

    StatusCallback onStatus_;
    UseInProjectCallback onUseInProject_;
    PlayCallback onPlay_;
    CopiedCallback onCopied_;
    juce::ToggleButton lissajous_;
    std::size_t lissajousDelay_ = 16;
    bool inSamples_ = false;
    bool inLinear_ = false;
    juce::Rectangle<int> peakArea_;
    juce::Rectangle<int> selectionArea_;
    talas::app::SampleEditorModel model_;
    std::filesystem::path path_;
    // The format the file was last saved in, for Ctrl+S to keep.
    std::optional<talas::app::SampleFileFormat> savedFormat_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    juce::ToggleButton pencil_;
    juce::ComboBox templates_;
    std::vector<Field> fields_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Rectangle<int> overview_;
    juce::Rectangle<int> detail_;
    [[nodiscard]] talas::app::SampleEditorView view() const noexcept
    {
        return {viewStart_, viewFrames_, level_};
    }

    void applyView(const talas::app::SampleEditorView& next) noexcept
    {
        viewStart_ = next.start;
        viewFrames_ = next.frames;
        level_ = next.level;
    }

    std::size_t viewStart_ = 0;
    std::size_t viewFrames_ = 64;
    double level_ = 1.0;
    bool dragging_ = false;
    bool overviewDragging_ = false;
    std::size_t anchorFrame_ = 0;
    std::size_t lastFrame_ = 0;
    double lastValue_ = 0.0;
};
}

std::unique_ptr<SampleEditorPanel> makeSampleEditorPanel(
    SampleEditorPanel::StatusCallback onStatus,
    SampleEditorPanel::UseInProjectCallback onUseInProject,
    SampleEditorPanel::PlayCallback onPlay,
    SampleEditorPanel::CopiedCallback onCopied)
{
    return std::make_unique<SampleEditorPanelImpl>(
        std::move(onStatus), std::move(onUseInProject), std::move(onPlay), std::move(onCopied));
}
}
