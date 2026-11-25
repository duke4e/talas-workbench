#include "app/workbench/WorkbenchDiskSplicerField.h"

#include "app/workbench/WorkbenchTheme.h"

#include <algorithm>
#include <cmath>

namespace talas::app::workbench
{
namespace
{
// The icon buttons from left to right, and what each does.
struct IconButton
{
    const char* action;
    const char* label;
    const char* tooltip;
};
constexpr IconButton iconButtons[] = {
    {"<", "<", "Scroll one page of samples to the left"},
    {"Play", "Play", "Play the selected samples"},
    {"Disk", "Disk", "Paste a disk file over the selection"},
    {"Fill", "Fill", "Make the selection exactly fill the display"},
    {"Time Out", "T-", "Contract the display in time"},
    {"Time In", "T+", "Expand the display in time"},
    {"Level Out", "A-", "Contract the display in amplitude"},
    {"Level In", "A+", "Expand the display in amplitude"},
    {">", ">", "Scroll one page of samples to the right"},
};
}

DiskSplicerField::DiskSplicerField(CommitCallback onCommit)
    : onCommit_(std::move(onCommit))
{
    setTitle("Segments");
    setWantsKeyboardFocus(true);
    for (const auto& icon : iconButtons)
    {
        auto button = std::make_unique<juce::TextButton>(icon.label);
        button->setTooltip(icon.tooltip);
        button->onClick = [this, action = juce::String{icon.action}] { (void)perform(action); };
        addAndMakeVisible(*button);
        buttons_.push_back(std::move(button));
    }
    editButton_.setTooltip("Cut, Copy, Paste, Clear or Trim the selection");
    editButton_.onClick = [this] { showEditMenu(); };
    addAndMakeVisible(editButton_);
}

void DiskSplicerField::setHost(Host host)
{
    host_ = std::move(host);
    files_.clear();
}

void DiskSplicerField::showSegments(const std::string& text)
{
    setVisible(true);
    if (text == shownText_ && problem_.isEmpty())
        return;
    shownText_ = text;
    files_.clear();
    std::string error;
    const auto read = talas::app::readSplicedSegments(text, [this](std::string_view file) { return framesOf(file); }, error);
    problem_ = read ? juce::String{} : juce::String{error};
    splicer_ = talas::app::DiskSplicerSegments{read.value_or(std::vector<talas::app::SplicedSegment>{})};
    view_ = {0, std::max<std::size_t>(splicer_.frames(), 64), 1.0};
    repaint();
}

std::optional<std::size_t> DiskSplicerField::framesOf(std::string_view file)
{
    const auto* channels = channelsOf(std::string{file});
    if (channels == nullptr || channels->empty())
        return std::nullopt;
    return channels->front().size();
}

const DiskSplicerField::Channels* DiskSplicerField::channelsOf(const std::string& file)
{
    auto found = files_.find(file);
    if (found == files_.end())
        found = files_.emplace(file, host_.samples ? host_.samples(file) : std::nullopt).first;
    return found->second ? &*found->second : nullptr;
}

double DiskSplicerField::sampleRate() const
{
    const auto rate = host_.sampleRate ? host_.sampleRate() : 48000.0;
    return rate > 0.0 ? rate : 48000.0;
}

void DiskSplicerField::commit()
{
    shownText_ = talas::app::splicedSegmentsText(
        splicer_.segments(), [this](std::string_view file) { return framesOf(file); });
    if (onCommit_)
        onCommit_(shownText_);
}

bool DiskSplicerField::perform(const juce::String& action)
{
    const auto total = splicer_.frames();
    auto edited = false;
    if (action == "Cut")
        edited = splicer_.cut();
    else if (action == "Copy")
        return splicer_.copy();
    else if (action == "Paste")
        edited = splicer_.paste();
    else if (action == "Clear")
        edited = splicer_.clear();
    else if (action == "Trim")
        edited = splicer_.trim();
    else if (action == "Select All")
        splicer_.selectAll();
    else if (action == "<" || action == ">")
        view_ = talas::app::sampleViewPaged(view_, total, action == ">");
    else if (action == "Fill")
    {
        if (splicer_.selection().empty())
            return false;
        view_ = talas::app::sampleViewFilled(view_, splicer_.selection());
    }
    else if (action == "Time Out" || action == "Time In")
        view_ = talas::app::sampleViewZoomed(view_, total, action == "Time In");
    else if (action == "Level Out" || action == "Level In")
        view_ = talas::app::sampleViewLevelled(view_, action == "Level In");
    else if (action == "Play")
    {
        auto samples = played();
        if (!host_.play || samples.empty() || samples.front().empty())
            return false;
        host_.play(std::move(samples), sampleRate());
        return true;
    }
    else if (action == "Disk")
    {
        if (!host_.chooseFile)
            return false;
        juce::Component::SafePointer<DiskSplicerField> safeThis(this);
        host_.chooseFile([safeThis](std::string file) {
            if (safeThis != nullptr)
                (void)safeThis->pasteFile(file);
        });
        return true;
    }
    else
        return false;
    if (edited)
        commit();
    repaint();
    return action == "Cut" || action == "Paste" || action == "Clear" || action == "Trim" ? edited : true;
}

bool DiskSplicerField::pasteFile(const std::string& file)
{
    files_.erase(file);
    const auto frames = framesOf(file);
    if (!frames || !splicer_.pasteFile(file, *frames))
        return false;
    problem_.clear();
    commit();
    repaint();
    return true;
}

void DiskSplicerField::selectSamples(std::size_t from, std::size_t to)
{
    splicer_.select(from, to);
    repaint();
}

void DiskSplicerField::showSamples(std::size_t from, std::size_t to)
{
    view_ = talas::app::splicedViewBetween(view_, from, to, splicer_.frames());
    repaint();
}

void DiskSplicerField::switchUnits()
{
    inSamples_ = !inSamples_;
    repaint();
}

juce::String DiskSplicerField::readout() const
{
    const auto& selection = splicer_.selection();
    return juce::String{talas::app::sampleMagnificationReadout(view_.frames, std::max(1, detail_.getWidth()))} + " · "
        + juce::String{talas::app::splicedSelectionSize(selection, sampleRate(), inSamples_)} + " · "
        + juce::String{talas::app::splicedSelectionRange(selection, sampleRate(), inSamples_)};
}

// The selected samples, or all of them when nothing is selected.
DiskSplicerField::Channels DiskSplicerField::played()
{
    return talas::app::splicedSamples(splicer_.selection().empty() ? splicer_.segments() : splicer_.selected(),
        [this](std::string_view file) { return channelsOf(std::string{file}); });
}

void DiskSplicerField::showEditMenu()
{
    juce::PopupMenu menu;
    const auto selected = !splicer_.selection().empty();
    menu.addItem(1, "Cut", selected);
    menu.addItem(2, "Copy", selected);
    menu.addItem(3, "Paste", splicer_.hasClipboard());
    menu.addItem(4, "Clear", selected);
    menu.addItem(5, "Trim", selected);
    menu.addSeparator();
    menu.addItem(6, "Select All");
    juce::Component::SafePointer<DiskSplicerField> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&editButton_), [safeThis](int chosen) {
        static const char* const actions[] = {"Cut", "Copy", "Paste", "Clear", "Trim", "Select All"};
        if (safeThis != nullptr && chosen >= 1 && chosen <= 6)
            (void)safeThis->perform(actions[chosen - 1]);
    });
}

void DiskSplicerField::resized()
{
    auto bounds = getLocalBounds().reduced(4);
    auto left = bounds.removeFromLeft(bounds.getWidth() / 3);
    bounds.removeFromLeft(6);
    info_ = left.removeFromBottom(36);
    overview_ = left;
    units_ = info_.withTrimmedLeft(info_.getWidth() / 3);
    auto toolbar = bounds.removeFromBottom(26);
    detail_ = bounds;
    const auto width = toolbar.getWidth() / (static_cast<int>(buttons_.size()) + 1);
    for (auto& button : buttons_)
        button->setBounds(toolbar.removeFromLeft(width).reduced(1));
    editButton_.setBounds(toolbar.reduced(1));
}

std::size_t DiskSplicerField::frameAt(juce::Rectangle<int> area, float x, std::size_t start, std::size_t frames) const
{
    const auto proportion = std::clamp(
        (x - static_cast<float>(area.getX())) / std::max(1.0F, static_cast<float>(area.getWidth())), 0.0F, 1.0F);
    return std::min(splicer_.frames(),
        start + static_cast<std::size_t>(std::llround(static_cast<double>(proportion) * static_cast<double>(frames))));
}

void DiskSplicerField::drawSamples(juce::Graphics& graphics, juce::Rectangle<int> area, std::size_t start,
    std::size_t frames, double level, bool overview)
{
    graphics.setColour(workbenchTheme.controlTile);
    graphics.fillRect(area);
    if (frames == 0 || area.getWidth() <= 0)
        return;
    const auto xOf = [&](std::size_t frame) {
        return static_cast<float>(area.getX())
            + static_cast<float>((static_cast<double>(frame) - static_cast<double>(start)) / static_cast<double>(frames)
                * area.getWidth());
    };
    const auto clampX = [&](float x) {
        return std::clamp(x, static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
    };
    const auto& selection = splicer_.selection();
    if (!selection.empty())
    {
        graphics.setColour(selectionColour.withAlpha(0.25F));
        graphics.fillRect(juce::Rectangle<float>::leftTopRightBottom(clampX(xOf(selection.start)),
            static_cast<float>(area.getY()), clampX(xOf(selection.end)), static_cast<float>(area.getBottom())));
    }
    if (overview)
    {
        // The part the right half shows.
        graphics.setColour(textColour.withAlpha(0.5F));
        const auto x0 = clampX(xOf(view_.start));
        graphics.drawRect(juce::Rectangle<float>::leftTopRightBottom(x0, static_cast<float>(area.getY()),
            std::max(x0 + 2.0F, clampX(xOf(view_.start + view_.frames))), static_cast<float>(area.getBottom())));
    }
    // Where one segment is spliced to the next.
    graphics.setColour(workbenchTheme.interactiveOutline);
    for (const auto splice : splicer_.segmentStarts())
    {
        if (splice > start && splice < start + frames)
            graphics.drawVerticalLine(static_cast<int>(xOf(splice)), static_cast<float>(area.getY()),
                static_cast<float>(area.getBottom()));
    }
    graphics.setColour(workbenchTheme.outline);
    graphics.drawHorizontalLine(area.getCentreY(), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
    graphics.setColour(audioColour);
    const auto& segments = splicer_.segments();
    const auto starts = splicer_.segmentStarts();
    const auto half = static_cast<double>(area.getHeight()) * 0.5 * level;
    const auto end = std::min(start + frames, splicer_.frames());
    for (int x = 0; x < area.getWidth(); ++x)
    {
        const auto first = start + static_cast<std::size_t>(static_cast<double>(x) / area.getWidth() * static_cast<double>(frames));
        const auto last = std::min(end, std::max(first + 1,
            start + static_cast<std::size_t>(static_cast<double>(x + 1) / area.getWidth() * static_cast<double>(frames))));
        if (first >= last)
            break;
        // At most 64 samples a pixel are looked at.
        const auto stride = std::max<std::size_t>(1, (last - first) / 64);
        auto low = 0.0;
        auto high = 0.0;
        auto segment = static_cast<std::size_t>(
            std::upper_bound(starts.begin(), starts.end(), first) - starts.begin()) - 1;
        for (auto frame = first; frame < last && segment < segments.size(); frame += stride)
        {
            while (segment + 1 < segments.size() && frame >= starts[segment + 1])
                ++segment;
            const auto* file = channelsOf(segments[segment].file);
            if (file == nullptr || file->empty())
                continue;
            const auto& data = file->front();
            const auto sample = segments[segment].first + (frame - starts[segment]);
            if (sample < data.size())
            {
                low = std::min(low, data[sample]);
                high = std::max(high, data[sample]);
            }
        }
        graphics.drawVerticalLine(area.getX() + x,
            static_cast<float>(std::max<double>(area.getY(), area.getCentreY() - high * half)),
            static_cast<float>(std::min<double>(area.getBottom(), area.getCentreY() - low * half + 1.0)));
    }
}

void DiskSplicerField::paint(juce::Graphics& graphics)
{
    graphics.fillAll(panelColour);
    drawSamples(graphics, overview_, 0, std::max<std::size_t>(splicer_.frames(), 1), 1.0, true);
    drawSamples(graphics, detail_, view_.start, view_.frames, view_.level, false);
    graphics.setFont(WorkbenchType::micro());
    if (problem_.isNotEmpty())
    {
        graphics.setColour(errorColour);
        graphics.drawFittedText(problem_, detail_.reduced(6), juce::Justification::centred, 3);
    }
    // The magnification on the left, the size and range on the right.
    graphics.setColour(secondaryTextColour);
    const auto& selection = splicer_.selection();
    graphics.drawText(juce::String{talas::app::sampleMagnificationReadout(view_.frames, std::max(1, detail_.getWidth()))},
        info_.withWidth(info_.getWidth() / 3), juce::Justification::centredLeft);
    graphics.drawText(juce::String{talas::app::splicedSelectionSize(selection, sampleRate(), inSamples_)},
        units_.withHeight(units_.getHeight() / 2), juce::Justification::centredRight);
    graphics.drawText(juce::String{talas::app::splicedSelectionRange(selection, sampleRate(), inSamples_)},
        units_.withTrimmedTop(units_.getHeight() / 2), juce::Justification::centredRight);
}

void DiskSplicerField::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    const auto position = event.getPosition();
    if (units_.contains(position))
    {
        switchUnits();
        return;
    }
    if (overview_.contains(position))
    {
        drag_ = Drag::Overview;
        dragFrom_ = frameAt(overview_, event.position.x, 0, splicer_.frames());
    }
    else if (detail_.contains(position))
    {
        drag_ = Drag::Detail;
        dragFrom_ = frameAt(detail_, event.position.x, view_.start, view_.frames);
        selectSamples(dragFrom_, dragFrom_);
    }
}

void DiskSplicerField::mouseDrag(const juce::MouseEvent& event)
{
    if (drag_ == Drag::Overview)
        showSamples(dragFrom_, frameAt(overview_, event.position.x, 0, splicer_.frames()));
    else if (drag_ == Drag::Detail)
        selectSamples(dragFrom_, frameAt(detail_, event.position.x, view_.start, view_.frames));
}

void DiskSplicerField::mouseUp(const juce::MouseEvent& event)
{
    // A click in the overview without a drag centres the right half there.
    if (drag_ == Drag::Overview && event.getDistanceFromDragStart() < 3)
    {
        view_ = talas::app::sampleViewCentredAt(view_, dragFrom_);
        repaint();
    }
    drag_ = Drag::None;
}

bool DiskSplicerField::keyPressed(const juce::KeyPress& key)
{
    const auto command = key.getModifiers().isCommandDown();
    const auto code = key.getKeyCode();
    if (command && (code == 'x' || code == 'X'))
        return perform("Cut");
    if (command && (code == 'c' || code == 'C'))
        return perform("Copy");
    if (command && (code == 'v' || code == 'V'))
        return perform("Paste");
    if (command && (code == 'a' || code == 'A'))
        return perform("Select All");
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        return perform("Clear");
    return false;
}
}
