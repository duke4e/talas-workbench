#include "app/workbench/WorkbenchTapeRecorder.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/app/WorkbenchUiPolicy.h"

#include "talas/audio/RealtimeOutputTake.h"
#include "talas/audio/WavReader.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
using talas::audio::TapeRecorderState;
using talas::audio::TimelineRecordingFormat;

[[nodiscard]] juce::String pathText(const std::filesystem::path& path)
{
    return juce::String::fromUTF8(talas::project::pathToUtf8(path).c_str());
}

[[nodiscard]] juce::String timeText(double seconds)
{
    const auto tenths = static_cast<long long>(std::floor(std::max(0.0, seconds) * 10.0));
    const auto minutes = tenths / 600;
    const auto rest = tenths % 600;
    return juce::String(minutes) + ":" + juce::String(rest / 10).paddedLeft('0', 2) + "." + juce::String(rest % 10);
}

[[nodiscard]] bool aiffExtension(const std::filesystem::path& path)
{
    const auto extension = pathText(path.extension()).toLowerCase();
    return extension == ".aif" || extension == ".aiff";
}

// The level bars of the metering section: the input now and the most
// since Reset.
class LevelMeter final : public juce::Component
{
public:
    void setLevels(float current, float maximum)
    {
        if (current == current_ && maximum == maximum_)
            return;
        current_ = current;
        maximum_ = maximum;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(workbenchTheme.canvas);
        graphics.fillRect(bounds);
        const auto height = [&bounds](float level) {
            return bounds.getHeight() * std::clamp(level, 0.0F, 1.0F);
        };
        graphics.setColour(workbenchTheme.audio);
        graphics.fillRect(bounds.withTop(bounds.getBottom() - height(current_)));
        graphics.setColour(maximum_ >= 1.0F ? workbenchTheme.error : workbenchTheme.control);
        graphics.fillRect(bounds.withTop(bounds.getBottom() - height(maximum_)).withHeight(2.0F));
        graphics.setColour(workbenchTheme.outline);
        graphics.drawRect(bounds);
    }

private:
    float current_ = 0.0F;
    float maximum_ = 0.0F;
};

class TapeRecorderPanelImpl final
    : public TapeRecorderPanel
    , private juce::Timer
{
public:
    TapeRecorderPanelImpl(talas::audio::TapeRecorderEngine& engine, SampleRateQuery sampleRate, DestinationCheck allowed)
        : engine_(engine), sampleRate_(std::move(sampleRate)), destinationAllowed_(std::move(allowed))
    {
        setOpaque(true);
        filename_.setButtonText("Filename");
        filename_.setTooltip("Choose the file, its format and bits");
        filename_.onClick = [this] { chooseFile(false); };
        addAndMakeVisible(filename_);
        file_.setText("No file chosen", juce::dontSendNotification);
        file_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(file_);

        formatMenu_.addItem("WAV", 1);
        formatMenu_.addItem("AIFF", 2);
        formatMenu_.setSelectedId(1, juce::dontSendNotification);
        formatMenu_.setTitle("File format");
        formatMenu_.onChange = [this] {
            setFormat(formatMenu_.getSelectedId() == 2 ? TimelineRecordingFormat::Aiff : TimelineRecordingFormat::Wav);
        };
        addAndMakeVisible(formatMenu_);
        for (const auto bits : {8, 16, 24})
            bitsMenu_.addItem(juce::String(bits) + "-bit", bits);
        bitsMenu_.setSelectedId(16, juce::dontSendNotification);
        bitsMenu_.setTitle("Bits per sample");
        bitsMenu_.onChange = [this] { wordBits_ = static_cast<std::uint16_t>(bitsMenu_.getSelectedId()); };
        addAndMakeVisible(bitsMenu_);

        const std::array<std::pair<juce::TextButton*, juce::String>, 4> transport{{
            {&stop_, juce::String::fromUTF8("\xe2\x96\xa0")},
            {&play_, juce::String::fromUTF8("\xe2\x96\xb6")},
            {&pause_, juce::String::fromUTF8("\xe2\x9d\x9a\xe2\x9d\x9a")},
            {&record_, juce::String::fromUTF8("\xe2\x97\x8f")},
        }};
        for (const auto& [button, glyph] : transport)
        {
            button->setButtonText(glyph);
            addAndMakeVisible(*button);
        }
        stop_.setTitle("Stop");
        play_.setTitle("Play");
        pause_.setTitle("Pause");
        record_.setTitle("Record");
        record_.setColour(juce::TextButton::textColourOffId, workbenchTheme.error);
        record_.setColour(juce::TextButton::textColourOnId, workbenchTheme.error);
        stop_.onClick = [this] { stop(); };
        play_.onClick = [this] { play(); };
        pause_.onClick = [this] { pause(); };
        record_.onClick = [this] { record(); };

        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            auto& select = select_[channel];
            select.setButtonText(channel == 0 ? "Left" : "Right");
            select.setClickingTogglesState(true);
            select.setToggleState(engine_.channelEnabled(channel), juce::dontSendNotification);
            select.onClick = [this, channel] { engine_.setChannelEnabled(channel, select_[channel].getToggleState()); };
            addAndMakeVisible(select);

            auto& fader = faders_[channel];
            fader.setSliderStyle(juce::Slider::LinearVertical);
            // The fader boosts a level too low, from unity.
            fader.setRange(1.0, talas::audio::tapeRecorderMaximumInputGain, 0.001);
            fader.setSkewFactorFromMidPoint(4.0);
            fader.setValue(engine_.inputLevel(channel), juce::dontSendNotification);
            fader.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 20);
            fader.setTitle(channel == 0 ? "Input(L)" : "Input(R)");
            fader.onValueChange = [this, channel] {
                engine_.setInputLevel(channel, static_cast<float>(faders_[channel].getValue()));
            };
            addAndMakeVisible(fader);
            faderLabels_[channel].setText(channel == 0 ? "Input(L)" : "Input(R)", juce::dontSendNotification);
            faderLabels_[channel].setJustificationType(juce::Justification::centred);
            addAndMakeVisible(faderLabels_[channel]);

            addAndMakeVisible(meters_[channel]);
            maxima_[channel].setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(maxima_[channel]);
        }
        reset_.setButtonText("Reset");
        reset_.onClick = [this] { resetMaxima(); };
        addAndMakeVisible(reset_);

        counter_.setFont(WorkbenchType::data().withHeight(22.0F));
        counter_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(counter_);
        status_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(status_);
        setStatus("Choose Filename, then press record to monitor the input");
        refresh();
        startTimerHz(15);
        setSize(560, 300);
    }

    ~TapeRecorderPanelImpl() override
    {
        stopTimer();
        stop();
        engine_.setPlaybackClip(nullptr);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10);
        // The Filename button, the file's name just below it.
        const auto area = bounds.removeFromTop(2 * 28 + 4);
        const auto parts = talas::app::tapeRecorderTop(area.getWidth(), 28, 4);
        const auto place = [&area](juce::Component& component, const talas::app::WorkbenchEditorArea& part) {
            component.setBounds(area.getX() + part.x, area.getY() + part.y, part.width, part.height);
        };
        place(filename_, parts.filename);
        place(formatMenu_, parts.format);
        place(bitsMenu_, parts.bits);
        place(file_, parts.file);
        status_.setBounds(bounds.removeFromBottom(22));
        bounds.removeFromTop(10);

        auto faders = bounds.removeFromRight(150);
        auto selects = faders.removeFromTop(26);
        select_[0].setBounds(selects.removeFromLeft(selects.getWidth() / 2).reduced(2, 0));
        select_[1].setBounds(selects.reduced(2, 0));
        auto labels = faders.removeFromBottom(20);
        faderLabels_[0].setBounds(labels.removeFromLeft(labels.getWidth() / 2));
        faderLabels_[1].setBounds(labels);
        faders_[0].setBounds(faders.removeFromLeft(faders.getWidth() / 2));
        faders_[1].setBounds(faders);
        bounds.removeFromRight(10);

        auto metering = bounds.removeFromRight(130);
        auto maxima = metering.removeFromBottom(54);
        maxima_[0].setBounds(maxima.removeFromTop(18));
        maxima_[1].setBounds(maxima.removeFromTop(18));
        reset_.setBounds(maxima.withWidth(64));
        metering.removeFromBottom(4);
        meters_[0].setBounds(metering.removeFromLeft(24));
        metering.removeFromLeft(6);
        meters_[1].setBounds(metering.removeFromLeft(24));
        bounds.removeFromRight(10);

        counter_.setBounds(bounds.removeFromTop(bounds.getHeight() / 2));
        auto buttons = bounds.removeFromTop(44);
        const auto width = buttons.getWidth() / 4;
        for (auto* button : {&stop_, &play_, &pause_, &record_})
            button->setBounds(buttons.removeFromLeft(width).reduced(3, 0));
    }

    void stop() override
    {
        transport_.stop();
        engine_.setMonitoring(false);
        engine_.setPlaying(false);
        engine_.rewind();
        if (take_ != nullptr)
            finishRecording();
        refresh();
    }

    [[nodiscard]] TapeRecorderState state() const noexcept override { return transport_.state(); }
    [[nodiscard]] bool hasRecording() const noexcept override { return clipFrames_ > 0; }
    [[nodiscard]] std::filesystem::path destination() const override { return destination_; }
    [[nodiscard]] juce::String statusText() const override { return status_.getText(); }
    [[nodiscard]] juce::String counterText() const override { return counter_.getText(); }
    [[nodiscard]] juce::String maxLevelText(std::size_t channel) const override
    {
        return channel < maxima_.size() ? maxima_[channel].getText() : juce::String{};
    }

    void pressForSmoke(TapeRecorderButton button) override
    {
        switch (button)
        {
        case TapeRecorderButton::Stop:
            stop();
            break;
        case TapeRecorderButton::Play:
            play();
            break;
        case TapeRecorderButton::Pause:
            pause();
            break;
        case TapeRecorderButton::Record:
            record();
            break;
        case TapeRecorderButton::Reset:
            resetMaxima();
            break;
        }
    }

    void setFileForSmoke(
        const std::filesystem::path& destination, TimelineRecordingFormat format, std::uint16_t bits) override
    {
        setDestination(destination);
        formatMenu_.setSelectedId(format == TimelineRecordingFormat::Aiff ? 2 : 1, juce::sendNotificationSync);
        bitsMenu_.setSelectedId(bits, juce::sendNotificationSync);
    }

    void setChannelForSmoke(std::size_t channel, bool enabled) override
    {
        if (channel < select_.size() && select_[channel].isEnabled())
        {
            select_[channel].setToggleState(enabled, juce::dontSendNotification);
            engine_.setChannelEnabled(channel, enabled);
        }
    }

    void setInputLevelForSmoke(std::size_t channel, double level) override
    {
        if (channel < faders_.size())
            faders_[channel].setValue(level, juce::sendNotificationSync);
    }

    void refreshForSmoke() override { timerCallback(); }
    void setSampleRateForSmoke(double sampleRate) override
    {
        sampleRate_ = [sampleRate] { return sampleRate; };
    }

private:
    void timerCallback() override
    {
        // A playback stops at the recording's end; a take that ends by
        // itself (the file's size limit) or fails ends the recording.
        if (transport_.plays() && !engine_.playing())
        {
            transport_.stop();
            engine_.rewind();
        }
        if (take_ != nullptr && take_->snapshot().terminal())
            stop();
        refresh();
    }

    void record()
    {
        if (transport_.recordDown())
            return;
        if (destination_.empty())
        {
            chooseFile(true);
            return;
        }
        engine_.setPlaying(false);
        transport_.record();
        engine_.setMonitoring(true);
        setStatus("Monitoring the input: press play or the pause button to record");
        refresh();
    }

    void play()
    {
        switch (transport_.state())
        {
        case TapeRecorderState::Monitoring:
            beginRecording();
            return;
        case TapeRecorderState::Stopped:
            if (!hasRecording())
            {
                setStatus("Nothing recorded yet: press record to monitor the input");
                return;
            }
            engine_.rewind();
            break;
        default:
            break;
        }
        transport_.play(hasRecording());
        apply();
    }

    void pause()
    {
        if (transport_.state() == TapeRecorderState::Monitoring)
        {
            beginRecording();
            return;
        }
        transport_.pause();
        apply();
    }

    void apply()
    {
        switch (transport_.state())
        {
        case TapeRecorderState::Recording:
            engine_.setRecordingPaused(false);
            setStatus("Recording to " + pathText(destination_.filename()));
            break;
        case TapeRecorderState::RecordingPaused:
            engine_.setRecordingPaused(true);
            setStatus("Recording paused: press pause again to resume");
            break;
        case TapeRecorderState::Playing:
            engine_.setPlaying(true);
            setStatus("Playing the recording");
            break;
        case TapeRecorderState::PlayingPaused:
            engine_.setPlaying(false);
            break;
        case TapeRecorderState::Monitoring:
        case TapeRecorderState::Stopped:
            break;
        }
        refresh();
    }

    void resetMaxima()
    {
        engine_.resetMaxLevels();
        refresh();
    }

    [[nodiscard]] std::filesystem::path temporaryTake() const
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("talas-tape-recorder-" + std::to_string(ticks) + ".wav");
    }

    // Recording starts from monitor mode: the take for the inputs chosen,
    // into the file itself for a WAV, or a WAV to save as AIFF at the end.
    void beginRecording()
    {
        const auto sampleRate = sampleRate_ ? sampleRate_() : 0.0;
        if (!(sampleRate > 0.0))
        {
            setStatus("Recording needs a running audio device");
            return;
        }
        const auto channels = engine_.enabledChannelCount();
        if (channels == 0)
        {
            setStatus("Choose Left, Right or both to record");
            return;
        }
        if (destinationAllowed_ && !destinationAllowed_(destination_))
        {
            setStatus("A recording cannot replace the open project, its package or a source asset");
            return;
        }
        talas::audio::RealtimeOutputTakeOptions options;
        options.destination = format_ == TimelineRecordingFormat::Wav ? destination_ : temporaryTake();
        options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
        options.sampleRate = sampleRate;
        options.channelCount = channels;
        options.maxBlockFrames = engine_.maximumBlockFrames();
        options.sourceStageId = talas::audio::TapeRecorderEngine::takeStageId;
        options.wordSize = talas::audio::WavWordSize{format_ == TimelineRecordingFormat::Wav ? wordBits_ : std::uint16_t{24}};
        std::string error;
        take_ = talas::audio::RealtimeOutputTake::start(options, error);
        if (take_ == nullptr)
        {
            setStatus("Could not record: " + juce::String(error));
            return;
        }
        takePath_ = options.destination;
        takeSampleRate_ = sampleRate;
        engine_.setRecordingPaused(false);
        engine_.setRecordingTake(take_.get());
        transport_.play(false);
        apply();
    }

    void finishRecording()
    {
        engine_.setRecordingTake(nullptr);
        engine_.setRecordingPaused(false);
        take_->requestStop();
        take_->join();
        const auto snapshot = take_->snapshot();
        take_.reset();
        const auto temporary = takePath_ != destination_;
        const auto discardTemporary = [&] {
            std::error_code ignored;
            if (temporary)
                std::filesystem::remove(takePath_, ignored);
        };
        if (snapshot.state != talas::audio::RealtimeOutputTakeState::Completed)
        {
            discardTemporary();
            setStatus("The recording was not saved: " + juce::String(snapshot.diagnostic));
            return;
        }
        auto recorded = talas::audio::readSampleFile(takePath_);
        if (!recorded.ok())
        {
            discardTemporary();
            setStatus("The recording could not be read back: " + juce::String(recorded.error));
            return;
        }
        if (temporary)
        {
            const auto error = talas::audio::writeAiffFile(destination_, takeSampleRate_, recorded.channels, wordBits_);
            discardTemporary();
            if (!error.empty())
            {
                setStatus("The AIFF file was not saved: " + juce::String(error));
                return;
            }
        }
        auto clip = std::make_shared<std::vector<std::vector<float>>>();
        for (const auto& channel : recorded.channels)
            clip->emplace_back(channel.begin(), channel.end());
        clipFrames_ = recorded.channels.empty() ? 0 : recorded.channels.front().size();
        clipSampleRate_ = takeSampleRate_;
        engine_.setPlaybackClip(std::move(clip));
        setStatus("Recorded " + timeText(static_cast<double>(clipFrames_) / clipSampleRate_) + " to "
            + pathText(destination_));
    }

    void setFormat(TimelineRecordingFormat format)
    {
        format_ = format;
        if (!destination_.empty())
        {
            auto path = destination_;
            if (format == TimelineRecordingFormat::Aiff && !aiffExtension(path))
                path.replace_extension(".aif");
            else if (format == TimelineRecordingFormat::Wav && aiffExtension(path))
                path.replace_extension(".wav");
            setDestination(path);
        }
    }

    void setDestination(std::filesystem::path path)
    {
        destination_ = std::move(path);
        file_.setText(destination_.empty() ? juce::String{"No file chosen"} : pathText(destination_.filename()),
            juce::dontSendNotification);
        file_.setTooltip(pathText(destination_));
    }

    void chooseFile(bool thenRecord)
    {
        if (take_ != nullptr)
            return;
        auto initial = destination_.empty()
            ? juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                  .getChildFile(format_ == TimelineRecordingFormat::Aiff ? "Recording.aif" : "Recording.wav")
            : juce::File{pathText(destination_)};
        chooser_ = std::make_unique<juce::FileChooser>("Tape Recorder file", initial, "*.wav;*.aif;*.aiff");
        juce::Component::SafePointer<TapeRecorderPanelImpl> safeThis{this};
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, thenRecord](const juce::FileChooser& chooser) {
                if (safeThis == nullptr)
                    return;
                auto selected = chooser.getResult();
                if (selected == juce::File{})
                    return;
                const auto extension = selected.getFileExtension().toLowerCase();
                if (extension == ".aif" || extension == ".aiff")
                    safeThis->formatMenu_.setSelectedId(2, juce::dontSendNotification);
                else if (extension == ".wav")
                    safeThis->formatMenu_.setSelectedId(1, juce::dontSendNotification);
                else
                    selected = selected.withFileExtension(
                        safeThis->formatMenu_.getSelectedId() == 2 ? ".aif" : ".wav");
                safeThis->format_ = safeThis->formatMenu_.getSelectedId() == 2 ? TimelineRecordingFormat::Aiff
                                                                            : TimelineRecordingFormat::Wav;
                safeThis->setDestination(talas::project::pathFromUtf8(selected.getFullPathName().toStdString()));
                if (thenRecord)
                    safeThis->record();
            });
    }

    void setStatus(const juce::String& text)
    {
        status_.setText(text, juce::dontSendNotification);
    }

    void refresh()
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            meters_[channel].setLevels(engine_.currentLevel(channel), engine_.maxLevel(channel));
            maxima_[channel].setText((channel == 0 ? "Max(L) " : "Max(R) ")
                    + juce::String(engine_.maxLevel(channel), 3),
                juce::dontSendNotification);
            // The file's channels are fixed once its recording starts.
            select_[channel].setEnabled(take_ == nullptr);
        }
        const auto recording = take_ != nullptr;
        filename_.setEnabled(!recording);
        formatMenu_.setEnabled(!recording);
        bitsMenu_.setEnabled(!recording);
        record_.setToggleState(transport_.recordDown(), juce::dontSendNotification);
        pause_.setToggleState(transport_.pauseDown(), juce::dontSendNotification);
        play_.setToggleState(transport_.records() || transport_.plays(), juce::dontSendNotification);
        const auto seconds = recording
            ? static_cast<double>(engine_.recordedFrames()) / takeSampleRate_
            : clipFrames_ > 0 && (transport_.plays() || transport_.state() == TapeRecorderState::PlayingPaused)
            ? static_cast<double>(engine_.playedFrames()) / clipSampleRate_
            : 0.0;
        counter_.setText(timeText(seconds), juce::dontSendNotification);
    }

    talas::audio::TapeRecorderEngine& engine_;
    SampleRateQuery sampleRate_;
    DestinationCheck destinationAllowed_;
    talas::audio::TapeRecorderTransport transport_;
    std::filesystem::path destination_;
    TimelineRecordingFormat format_ = TimelineRecordingFormat::Wav;
    std::uint16_t wordBits_ = 16;
    std::unique_ptr<talas::audio::RealtimeOutputTake> take_;
    std::filesystem::path takePath_;
    double takeSampleRate_ = 0.0;
    std::size_t clipFrames_ = 0;
    double clipSampleRate_ = 0.0;
    std::unique_ptr<juce::FileChooser> chooser_;

    juce::TextButton filename_;
    juce::Label file_;
    juce::ComboBox formatMenu_;
    juce::ComboBox bitsMenu_;
    juce::TextButton stop_;
    juce::TextButton play_;
    juce::TextButton pause_;
    juce::TextButton record_;
    std::array<juce::TextButton, 2> select_;
    std::array<juce::Slider, 2> faders_;
    std::array<juce::Label, 2> faderLabels_;
    std::array<LevelMeter, 2> meters_;
    std::array<juce::Label, 2> maxima_;
    juce::TextButton reset_;
    juce::Label counter_;
    juce::Label status_;
};

class TapeRecorderWindow final : public juce::DocumentWindow
{
public:
    explicit TapeRecorderWindow(TapeRecorderPanel& panel)
        : juce::DocumentWindow(
              "Tape Recorder", backgroundColour, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
        , panel_(panel)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&panel_, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        panel_.stop();
        setVisible(false);
    }

private:
    TapeRecorderPanel& panel_;
};
}

std::unique_ptr<TapeRecorderPanel> makeTapeRecorderPanel(
    talas::audio::TapeRecorderEngine& engine,
    TapeRecorderPanel::SampleRateQuery sampleRate,
    TapeRecorderPanel::DestinationCheck destinationAllowed)
{
    return std::make_unique<TapeRecorderPanelImpl>(engine, std::move(sampleRate), std::move(destinationAllowed));
}

std::unique_ptr<juce::DocumentWindow> makeTapeRecorderWindow(TapeRecorderPanel& panel)
{
    return std::make_unique<TapeRecorderWindow>(panel);
}
}
