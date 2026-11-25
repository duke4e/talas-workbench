#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace talas::app
{
// The Fake Keyboard (Tools menu): keys on the screen that send MIDI note
// events to the playing Sound, as a MIDI keyboard would. Velocity 0 is a
// note-off.
struct OnScreenKeyboardNote
{
    int channel = 1;
    int note = 60;
    int velocity = 0;

    bool operator==(const OnScreenKeyboardNote&) const = default;
};

class OnScreenKeyboardModel
{
public:
    static constexpr int visibleKeyCount = 37;

    [[nodiscard]] int channel() const noexcept;
    [[nodiscard]] int velocity() const noexcept;
    [[nodiscard]] int lowestVisibleNote() const noexcept;
    [[nodiscard]] const std::vector<int>& heldNotes() const noexcept;

    // A held note is released on its old channel before the channel
    // changes, so no note hangs.
    [[nodiscard]] std::vector<OnScreenKeyboardNote> setChannel(int channel);
    void setVelocity(int velocity) noexcept;
    // By octaves, keeping every visible key within MIDI's 0..127.
    void shiftOctave(int octaves) noexcept;

    [[nodiscard]] std::optional<OnScreenKeyboardNote> press(int note);
    [[nodiscard]] std::optional<OnScreenKeyboardNote> release(int note);
    [[nodiscard]] std::vector<OnScreenKeyboardNote> releaseAll();

private:
    int channel_ = 1;
    int velocity_ = 100;
    int lowestVisibleNote_ = 48;
    std::vector<int> held_;
};

// MIDI notes off (Ctrl+M): a note-off (key up) for every note on every MIDI
// channel, channel 1's notes 0 to 127 first.
[[nodiscard]] std::vector<OnScreenKeyboardNote> midiNotesOff();
}
