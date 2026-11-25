#include "talas/app/OnScreenKeyboardModel.h"

#include <algorithm>

namespace talas::app
{
int OnScreenKeyboardModel::channel() const noexcept
{
    return channel_;
}

int OnScreenKeyboardModel::velocity() const noexcept
{
    return velocity_;
}

int OnScreenKeyboardModel::lowestVisibleNote() const noexcept
{
    return lowestVisibleNote_;
}

const std::vector<int>& OnScreenKeyboardModel::heldNotes() const noexcept
{
    return held_;
}

std::vector<OnScreenKeyboardNote> OnScreenKeyboardModel::setChannel(int channel)
{
    channel = std::clamp(channel, 1, 16);
    if (channel == channel_)
        return {};
    auto released = releaseAll();
    channel_ = channel;
    return released;
}

void OnScreenKeyboardModel::setVelocity(int velocity) noexcept
{
    velocity_ = std::clamp(velocity, 1, 127);
}

void OnScreenKeyboardModel::shiftOctave(int octaves) noexcept
{
    constexpr int highestLowest = 127 - (visibleKeyCount - 1);
    auto lowest = lowestVisibleNote_ + 12 * octaves;
    while (lowest > highestLowest)
        lowest -= 12;
    while (lowest < 0)
        lowest += 12;
    lowestVisibleNote_ = lowest;
}

std::optional<OnScreenKeyboardNote> OnScreenKeyboardModel::press(int note)
{
    if (note < 0 || note > 127 || std::ranges::find(held_, note) != held_.end())
        return std::nullopt;
    held_.push_back(note);
    return OnScreenKeyboardNote{channel_, note, velocity_};
}

std::optional<OnScreenKeyboardNote> OnScreenKeyboardModel::release(int note)
{
    const auto found = std::ranges::find(held_, note);
    if (found == held_.end())
        return std::nullopt;
    held_.erase(found);
    return OnScreenKeyboardNote{channel_, note, 0};
}

std::vector<OnScreenKeyboardNote> OnScreenKeyboardModel::releaseAll()
{
    std::vector<OnScreenKeyboardNote> released;
    released.reserve(held_.size());
    for (const auto note : held_)
        released.push_back({channel_, note, 0});
    held_.clear();
    return released;
}

std::vector<OnScreenKeyboardNote> midiNotesOff()
{
    std::vector<OnScreenKeyboardNote> notes;
    notes.reserve(16 * 128);
    for (int channel = 1; channel <= 16; ++channel)
    {
        for (int note = 0; note <= 127; ++note)
            notes.push_back({channel, note, 0});
    }
    return notes;
}
}
