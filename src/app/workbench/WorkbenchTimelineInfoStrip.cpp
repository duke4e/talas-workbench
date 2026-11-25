#include "app/workbench/WorkbenchTimelineInfoStrip.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/TimelineInfo.h"

#include <algorithm>

namespace talas::app::workbench
{
namespace
{
constexpr std::array<const char*, 9> fieldTitles{
    "Cursor time",
    "Sound start",
    "Sound anchor",
    "Sound duration",
    "Sound end",
    "Marker name",
    "Marker time",
    "Point time",
    "Point value",
};

// The Sound's four time fields take their help from the model.
const std::array<std::string_view, 9> fieldHelp{
    "The time cursor's position; type a time and press Enter to move the cursor there.",
    talas::app::timelineTimeFieldHelp(talas::app::TimelineTimeField::Start),
    talas::app::timelineTimeFieldHelp(talas::app::TimelineTimeField::Anchor),
    talas::app::timelineTimeFieldHelp(talas::app::TimelineTimeField::Duration),
    talas::app::timelineTimeFieldHelp(talas::app::TimelineTimeField::End),
    "The selected marker's name; type a name and press Enter to rename it. Space plays and pauses here: Ctrl+L opens a larger window for a name with spaces.",
    "The selected marker's time; type a time and press Enter to move it.",
    "The earliest selected point's time; type a time and press Enter to move the selected points there together.",
    "The earliest selected point's value; type a value and press Enter to give it to the selected points.",
};

[[nodiscard]] std::size_t indexOf(TimelineInfoField field) noexcept
{
    return static_cast<std::size_t>(field);
}

// The marker name field takes no spaces.
class MarkerNameFilter final : public juce::TextEditor::InputFilter
{
public:
    juce::String filterNewText(juce::TextEditor&, const juce::String& newInput) override
    {
        return juceString(talas::app::timelineMarkerNameTyped(newInput.toStdString()));
    }
};

// Space there plays and pauses the Timeline instead.
class SpaceKey final : public juce::KeyListener
{
public:
    explicit SpaceKey(std::function<void()>& onSpace) : onSpace_(onSpace) {}

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key != juce::KeyPress{' '})
            return false;
        if (onSpace_)
            onSpace_();
        return true;
    }

private:
    std::function<void()>& onSpace_;
};
}

TimelineInfoStrip::TimelineInfoStrip()
{
    for (std::size_t index = 0; index < fields_.size(); ++index)
    {
        auto& field = fields_[index];
        const auto which = static_cast<TimelineInfoField>(index);
        field.setFont(WorkbenchType::label());
        field.setJustification(juce::Justification::centredLeft);
        field.setSelectAllWhenFocused(true);
        field.setComponentID(juce::String("timeline.info.") + juce::String(index));
        describeControl(field, fieldTitles[index], juceString(fieldHelp[index]));
        // The pointer in a field shows what it holds.
        field.setTooltip(juceString(fieldHelp[index]));
        field.onReturnKey = [this, which] {
            const auto text = editor(which).getText().toStdString();
            if (!enterField(which, text))
                show(state_);
            typing_.reset();
            unfocusAllComponents();
        };
        field.onEscapeKey = [this] {
            typing_.reset();
            unfocusAllComponents();
            show(state_);
        };
        addChildComponent(field);
    }
    addAndMakeVisible(editor(TimelineInfoField::Cursor));
    auto& markerName = editor(TimelineInfoField::MarkerName);
    markerName.setInputFilter(new MarkerNameFilter, true);
    markerNameKeys_ = std::make_unique<SpaceKey>(onSpace);
    markerName.addKeyListener(markerNameKeys_.get());
    for (int program = 1; program <= 128; ++program)
        markerProgram_.addItem(juce::String(program), program);
    describeControl(markerProgram_, "Marker program change",
        "The MIDI program change that jumps the cursor to the selected marker.");
    markerProgram_.onChange = [this] {
        const auto program = markerProgram_.getSelectedId();
        if (program >= 1 && program <= 128 && program != state_.markerProgram && onMarkerProgram)
            (void) onMarkerProgram(static_cast<std::uint8_t>(program));
    };
    addChildComponent(markerProgram_);
    describeControl(markerButton_, "Marker",
        "Insert a marker: type its time and click OK, or press Enter or Capture to place it at the cursor.");
    markerButton_.onClick = [this] {
        if (onMarkerButton)
            onMarkerButton();
    };
    addAndMakeVisible(markerButton_);
    describeControl(anchorButton_, "Anchor",
        "Mark the selected Sound's anchor at the cursor, the moment you hear its event.");
    anchorButton_.onClick = [this] {
        if (onAnchorButton)
            (void) onAnchorButton();
    };
    addChildComponent(anchorButton_);
    // The rightmost menu colours the selected Sounds.
    const auto colours = talas::app::timelineClipColours();
    for (std::size_t index = 0; index < colours.size(); ++index)
        colour_.addItem(juce::String(std::string{colours[index].name}), static_cast<int>(index) + 1);
    describeControl(colour_, "Colour", "The colour of the selected Sounds; Track for their track's.");
    colour_.onChange = [this] {
        const auto index = colour_.getSelectedItemIndex();
        const auto chosen = talas::app::timelineClipColours();
        if (index >= 0 && static_cast<std::size_t>(index) < chosen.size()
            && chosen[static_cast<std::size_t>(index)].rgb != state_.soundColour && onColour)
            (void) onColour(chosen[static_cast<std::size_t>(index)].rgb);
    };
    addChildComponent(colour_);
}

TimelineInfoStrip::~TimelineInfoStrip()
{
    editor(TimelineInfoField::MarkerName).removeKeyListener(markerNameKeys_.get());
}

juce::TextEditor& TimelineInfoStrip::editor(TimelineInfoField field)
{
    return fields_[indexOf(field)];
}

const juce::TextEditor& TimelineInfoStrip::editor(TimelineInfoField field) const
{
    return fields_[indexOf(field)];
}

void TimelineInfoStrip::show(const State& state)
{
    state_ = state;
    const auto set = [this](TimelineInfoField field, const std::string& text) {
        auto& target = editor(field);
        if (!target.hasKeyboardFocus(true))
            target.setText(juce::String(text), false);
    };
    set(TimelineInfoField::Cursor, state.cursor);
    const auto hasSound = state.sound.has_value();
    for (const auto field : {TimelineInfoField::SoundStart, TimelineInfoField::SoundAnchor,
             TimelineInfoField::SoundDuration, TimelineInfoField::SoundEnd})
        editor(field).setVisible(hasSound);
    anchorButton_.setVisible(hasSound);
    colour_.setVisible(hasSound);
    {
        const auto colours = talas::app::timelineClipColours();
        const auto shown = std::ranges::find(colours, state.soundColour, &talas::app::TimelineClipColour::rgb);
        colour_.setSelectedItemIndex(shown == colours.end() ? 0 : static_cast<int>(shown - colours.begin()), juce::dontSendNotification);
    }
    if (hasSound)
    {
        set(TimelineInfoField::SoundStart, talas::app::timelineTimeText(state.sound->start, state.musicalTime));
        set(TimelineInfoField::SoundAnchor, talas::app::timelineTimeText(state.sound->anchor, state.musicalTime));
        set(TimelineInfoField::SoundDuration, talas::app::timelineTimeText(state.sound->duration, state.musicalTime, true));
        set(TimelineInfoField::SoundEnd, talas::app::timelineTimeText(state.sound->end, state.musicalTime));
    }
    const auto hasMarker = state.marker.has_value();
    editor(TimelineInfoField::MarkerName).setVisible(hasMarker);
    editor(TimelineInfoField::MarkerTime).setVisible(hasMarker);
    markerProgram_.setVisible(hasMarker);
    if (hasMarker)
    {
        set(TimelineInfoField::MarkerName, state.marker->title);
        set(TimelineInfoField::MarkerTime, talas::app::timelineTimeText(state.marker->start, state.musicalTime));
        markerProgram_.setSelectedId(state.markerProgram, juce::dontSendNotification);
    }
    const auto hasPoint = state.pointTime.has_value();
    editor(TimelineInfoField::PointTime).setVisible(hasPoint);
    editor(TimelineInfoField::PointValue).setVisible(hasPoint);
    if (hasPoint)
    {
        set(TimelineInfoField::PointTime, talas::app::timelineTimeText(*state.pointTime, state.musicalTime));
        set(TimelineInfoField::PointValue, juce::String(state.pointValue).toStdString());
    }
    resized();
    repaint();
}

std::string TimelineInfoStrip::fieldText(TimelineInfoField field) const
{
    const auto& target = editor(field);
    return target.isVisible() ? target.getText().toStdString() : std::string{};
}

bool TimelineInfoStrip::enterField(TimelineInfoField field, const std::string& text)
{
    if (!editor(field).isVisible() || !onField)
        return false;
    return onField(field, text);
}

bool TimelineInfoStrip::pressKey(TimelineInfoField field, const juce::KeyPress& key)
{
    auto& target = editor(field);
    if (field == TimelineInfoField::MarkerName && markerNameKeys_->keyPressed(key, &target))
        return true;
    return target.keyPressed(key);
}

void TimelineInfoStrip::beginTyping(TimelineInfoField field)
{
    auto& target = editor(field);
    if (!target.isVisible())
        return;
    typing_ = field;
    if (target.isShowing())
        target.grabKeyboardFocus();
    target.selectAll();
}

void TimelineInfoStrip::resized()
{
    auto bounds = getLocalBounds().reduced(0, 2);
    const auto place = [&bounds](juce::Component& component, int width) {
        if (!component.isVisible())
            return;
        component.setBounds(bounds.removeFromLeft(std::min(width, std::max(0, bounds.getWidth()))));
        bounds.removeFromLeft(4);
    };
    using enum talas::app::TimelineInfoStripPart;
    for (const auto part : talas::app::timelineInfoStripParts())
    {
        switch (part)
        {
        case MarkerName: place(editor(TimelineInfoField::MarkerName), 96); break;
        case MarkerProgram: place(markerProgram_, 54); break;
        case MarkerTime: place(editor(TimelineInfoField::MarkerTime), 74); break;
        case Cursor: place(editor(TimelineInfoField::Cursor), 84); break;
        case MarkerButton: place(markerButton_, 76); break;
        case PointTime: place(editor(TimelineInfoField::PointTime), 74); break;
        case PointValue: place(editor(TimelineInfoField::PointValue), 64); break;
        case SoundName:
            // Painted beside its times.
            if (state_.sound.has_value())
                bounds.removeFromLeft(std::min(110, std::max(0, bounds.getWidth())));
            break;
        case SoundStart: place(editor(TimelineInfoField::SoundStart), 74); break;
        case SoundAnchor: place(editor(TimelineInfoField::SoundAnchor), 74); break;
        case SoundDuration: place(editor(TimelineInfoField::SoundDuration), 74); break;
        case SoundEnd: place(editor(TimelineInfoField::SoundEnd), 74); break;
        case AnchorButton: place(anchorButton_, 60); break;
        }
    }
    if (colour_.isVisible())
        colour_.setBounds(getLocalBounds().reduced(0, 2).removeFromRight(std::min(84, std::max(0, bounds.getWidth()))));
}

void TimelineInfoStrip::paint(juce::Graphics& graphics)
{
    if (!state_.sound.has_value())
        return;
    // The selected Sound's name and whether it is cached, before its times;
    // the anchor's field glows blue.
    const auto& start = editor(TimelineInfoField::SoundStart);
    auto label = juce::Rectangle<int>{start.getX() - 114, start.getY(), 110, start.getHeight()};
    graphics.setColour(textColour);
    graphics.setFont(WorkbenchType::labelStrong());
    graphics.drawFittedText(juce::String(state_.sound->name) + " · " + juce::String(state_.sound->cacheState),
        label, juce::Justification::centredRight, 1);
    // The time fields' coloured edges: the start's left, a glow
    // round the anchor, the duration's top and sides, the end's right.
    graphics.setColour(dataColour.withAlpha(0.6F));
    using talas::app::TimelineTimeField;
    for (const auto [field, time] : {std::pair{TimelineInfoField::SoundStart, TimelineTimeField::Start},
             std::pair{TimelineInfoField::SoundAnchor, TimelineTimeField::Anchor},
             std::pair{TimelineInfoField::SoundDuration, TimelineTimeField::Duration},
             std::pair{TimelineInfoField::SoundEnd, TimelineTimeField::End}})
    {
        const auto box = editor(field).getBounds().expanded(1).toFloat();
        const auto border = talas::app::timelineTimeFieldBorder(time);
        if (border.left)
            graphics.fillRect(box.withWidth(2.0F));
        if (border.right)
            graphics.fillRect(box.withLeft(box.getRight() - 2.0F));
        if (border.top)
            graphics.fillRect(box.withHeight(2.0F));
        if (border.bottom)
            graphics.fillRect(box.withTop(box.getBottom() - 2.0F));
    }
}
}
