#pragma once

#include "app/workbench/WorkbenchTimelinePanel.h"
#include "talas/app/TimelineInfo.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::app::workbench
{
// The strip at the top of the Timeline: the
// cursor's time, the Marker button, the selected marker's name, program
// change and time, the selected Sound's name, cache state, start, anchor,
// duration and end with the Anchor button, and the time and value of the
// earliest selected point of a control. Each is a display and a
// field: typing into it and pressing Enter changes what it shows.
class TimelineInfoStrip final : public juce::Component
{
public:
    struct State
    {
        std::string cursor;
        std::optional<talas::app::TimelineSoundInfo> sound;
        // The selected Sound's colour, none for its track's (format 146).
        std::optional<std::uint32_t> soundColour;
        std::optional<talas::project::ScoreMarker> marker;
        std::uint8_t markerProgram = 1;
        std::optional<talas::project::ScoreTime> pointTime;
        double pointValue = 0.0;
        talas::project::ProjectMusicalTime musicalTime;
    };

    TimelineInfoStrip();
    ~TimelineInfoStrip() override;

    // A field's Enter: the field and its text; false leaves the field as
    // it was, with what it showed before.
    std::function<bool(TimelineInfoField, const std::string&)> onField;
    std::function<bool(std::uint8_t)> onMarkerProgram;
    std::function<void()> onMarkerButton;
    std::function<bool()> onAnchorButton;
    // The rightmost menu, the selected Sounds'
    // colour, none for their track's.
    std::function<bool(std::optional<std::uint32_t>)> onColour;
    // Space in the marker name field plays and pauses the Timeline.
    std::function<void()> onSpace;

    // Shows the state; a field being typed into keeps its text.
    void show(const State& state);
    [[nodiscard]] std::string fieldText(TimelineInfoField field) const;
    // A field typed into and Enter pressed, as its editor does.
    bool enterField(TimelineInfoField field, const std::string& text);
    // Opens a field for its text to be typed over, as Enter beat or time
    // does for a Sound put into a track; the field
    // opened so, until Enter or Escape closes it.
    void beginTyping(TimelineInfoField field);
    [[nodiscard]] std::optional<TimelineInfoField> typing() const noexcept { return typing_; }
    // A key pressed in a field, its listeners first, as the peer sends it.
    bool pressKey(TimelineInfoField field, const juce::KeyPress& key);

    void resized() override;
    void paint(juce::Graphics& graphics) override;

private:
    [[nodiscard]] juce::TextEditor& editor(TimelineInfoField field);
    [[nodiscard]] const juce::TextEditor& editor(TimelineInfoField field) const;

    std::array<juce::TextEditor, 9> fields_;
    juce::ComboBox markerProgram_;
    juce::TextButton markerButton_{"Marker..."};
    juce::TextButton anchorButton_{"Anchor"};
    juce::ComboBox colour_;
    State state_;
    std::optional<TimelineInfoField> typing_;
    std::unique_ptr<juce::KeyListener> markerNameKeys_;
};
}
