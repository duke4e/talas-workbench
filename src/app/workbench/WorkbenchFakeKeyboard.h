#pragma once

#include "talas/app/OnScreenKeyboardModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace talas::app::workbench
{
// The Fake Keyboard: keys on the screen that
// send MIDI note events to the playing Sound, with the channel and velocity
// a MIDI keyboard would send.
class FakeKeyboardPanel : public juce::Component
{
public:
    using NoteCallback =
        std::function<void(const talas::app::OnScreenKeyboardNote&)>;

    ~FakeKeyboardPanel() override = default;

    // Releases every held key, as closing the keyboard does.
    virtual void releaseAllKeys() = 0;
    [[nodiscard]] virtual const talas::app::OnScreenKeyboardModel&
    model() const noexcept = 0;
    // Smoke access to what a click on a key does.
    virtual void pressKeyForSmoke(int note) = 0;
    virtual void releaseKeyForSmoke(int note) = 0;
    virtual void setChannelForSmoke(int channel) = 0;
    virtual void setVelocityForSmoke(int velocity) = 0;
};

[[nodiscard]] std::unique_ptr<FakeKeyboardPanel> makeFakeKeyboardPanel(
    FakeKeyboardPanel::NoteCallback onNote);

// A window for the panel; closing it hides it and releases held keys.
[[nodiscard]] std::unique_ptr<juce::DocumentWindow> makeFakeKeyboardWindow(
    FakeKeyboardPanel& panel);
}
