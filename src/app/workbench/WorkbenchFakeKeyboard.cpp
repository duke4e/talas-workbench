#include "app/workbench/WorkbenchFakeKeyboard.h"

#include "app/workbench/WorkbenchTheme.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <utility>

namespace talas::app::workbench
{
namespace
{
class FakeKeyboardPanelImpl final
    : public FakeKeyboardPanel
    , private juce::MidiKeyboardState::Listener
{
public:
    explicit FakeKeyboardPanelImpl(NoteCallback onNote)
        : onNote_(std::move(onNote))
    {
        setOpaque(true);
        keyboard_.setAvailableRange(0, 127);
        keyboard_.setLowestVisibleKey(model_.lowestVisibleNote());
        keyboard_.setMidiChannel(model_.channel());
        keyboard_.setVelocity(
            static_cast<float>(model_.velocity()) / 127.0F, false);
        keyboard_.setScrollButtonsVisible(false);
        keyboard_.setKeyPressBaseOctave(model_.lowestVisibleNote() / 12);
        keyboard_.setTitle("Fake Keyboard keys");
        keyboard_.setDescription(
            "Click a key, or type the computer keys under it, to send its "
            "MIDI note to the playing Sound");
        state_.addListener(this);
        addAndMakeVisible(keyboard_);

        octaveDown_.setButtonText("Octave -");
        octaveUp_.setButtonText("Octave +");
        octaveDown_.onClick = [this] { shiftOctave(-1); };
        octaveUp_.onClick = [this] { shiftOctave(1); };
        addAndMakeVisible(octaveDown_);
        addAndMakeVisible(octaveUp_);

        channelLabel_.setText("Channel", juce::dontSendNotification);
        addAndMakeVisible(channelLabel_);
        for (int channel = 1; channel <= 16; ++channel)
            channel_.addItem(juce::String(channel), channel);
        channel_.setSelectedId(model_.channel(), juce::dontSendNotification);
        channel_.setTitle("MIDI channel");
        channel_.onChange = [this] { changeChannel(channel_.getSelectedId()); };
        addAndMakeVisible(channel_);

        velocityLabel_.setText("Velocity", juce::dontSendNotification);
        addAndMakeVisible(velocityLabel_);
        velocity_.setRange(1.0, 127.0, 1.0);
        velocity_.setValue(model_.velocity(), juce::dontSendNotification);
        velocity_.setSliderStyle(juce::Slider::LinearHorizontal);
        velocity_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 22);
        velocity_.setTitle("Velocity");
        velocity_.onValueChange = [this] {
            changeVelocity(static_cast<int>(velocity_.getValue()));
        };
        addAndMakeVisible(velocity_);
        setSize(720, 190);
    }

    ~FakeKeyboardPanelImpl() override
    {
        state_.removeListener(this);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto controls = bounds.removeFromTop(28);
        octaveDown_.setBounds(controls.removeFromLeft(80));
        controls.removeFromLeft(6);
        octaveUp_.setBounds(controls.removeFromLeft(80));
        controls.removeFromLeft(16);
        channelLabel_.setBounds(controls.removeFromLeft(60));
        channel_.setBounds(controls.removeFromLeft(64));
        controls.removeFromLeft(16);
        velocityLabel_.setBounds(controls.removeFromLeft(64));
        velocity_.setBounds(controls);
        bounds.removeFromTop(8);
        keyboard_.setBounds(bounds);
        // The model's visible octaves fill the width in white keys.
        constexpr auto whiteKeys =
            (talas::app::OnScreenKeyboardModel::visibleKeyCount / 12) * 7 + 1;
        keyboard_.setKeyWidth(
            static_cast<float>(bounds.getWidth()) / static_cast<float>(whiteKeys));
        keyboard_.setLowestVisibleKey(model_.lowestVisibleNote());
    }

    void releaseAllKeys() override
    {
        state_.allNotesOff(0);
        for (const auto& note : model_.releaseAll())
            send(note);
    }

    [[nodiscard]] const talas::app::OnScreenKeyboardModel&
    model() const noexcept override
    {
        return model_;
    }

    void pressKeyForSmoke(int note) override
    {
        state_.noteOn(
            model_.channel(),
            note,
            static_cast<float>(model_.velocity()) / 127.0F);
    }

    void releaseKeyForSmoke(int note) override
    {
        state_.noteOff(model_.channel(), note, 0.0F);
    }

    void setChannelForSmoke(int channel) override
    {
        channel_.setSelectedId(channel, juce::sendNotificationSync);
    }

    void setVelocityForSmoke(int velocity) override
    {
        velocity_.setValue(velocity, juce::sendNotificationSync);
    }

private:
    void handleNoteOn(juce::MidiKeyboardState*, int, int note, float) override
    {
        if (const auto event = model_.press(note))
            send(*event);
    }

    void handleNoteOff(juce::MidiKeyboardState*, int, int note, float) override
    {
        if (const auto event = model_.release(note))
            send(*event);
    }

    void send(const talas::app::OnScreenKeyboardNote& note)
    {
        if (onNote_)
            onNote_(note);
    }

    void shiftOctave(int octaves)
    {
        releaseAllKeys();
        model_.shiftOctave(octaves);
        keyboard_.setLowestVisibleKey(model_.lowestVisibleNote());
        keyboard_.setKeyPressBaseOctave(model_.lowestVisibleNote() / 12);
    }

    void changeChannel(int channel)
    {
        if (channel <= 0)
            return;
        state_.allNotesOff(0);
        for (const auto& note : model_.setChannel(channel))
            send(note);
        keyboard_.setMidiChannel(model_.channel());
    }

    void changeVelocity(int velocity)
    {
        model_.setVelocity(velocity);
        keyboard_.setVelocity(
            static_cast<float>(model_.velocity()) / 127.0F, false);
    }

    NoteCallback onNote_;
    talas::app::OnScreenKeyboardModel model_;
    juce::MidiKeyboardState state_;
    juce::MidiKeyboardComponent keyboard_{
        state_, juce::KeyboardComponentBase::horizontalKeyboard};
    juce::TextButton octaveDown_;
    juce::TextButton octaveUp_;
    juce::Label channelLabel_;
    juce::ComboBox channel_;
    juce::Label velocityLabel_;
    juce::Slider velocity_;
};

class FakeKeyboardWindow final : public juce::DocumentWindow
{
public:
    explicit FakeKeyboardWindow(FakeKeyboardPanel& panel)
        : juce::DocumentWindow(
              "Fake Keyboard",
              backgroundColour,
              juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
        , panel_(panel)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&panel_, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        panel_.releaseAllKeys();
        setVisible(false);
    }

private:
    FakeKeyboardPanel& panel_;
};
}

std::unique_ptr<FakeKeyboardPanel> makeFakeKeyboardPanel(
    FakeKeyboardPanel::NoteCallback onNote)
{
    return std::make_unique<FakeKeyboardPanelImpl>(std::move(onNote));
}

std::unique_ptr<juce::DocumentWindow> makeFakeKeyboardWindow(
    FakeKeyboardPanel& panel)
{
    return std::make_unique<FakeKeyboardWindow>(panel);
}
}
