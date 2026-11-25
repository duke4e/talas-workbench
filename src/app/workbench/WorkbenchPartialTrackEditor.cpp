#include "app/workbench/WorkbenchComponentInternal.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/PartialTrackFilters.h"
#include "talas/project/SosSpectrumFile.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
inline constexpr std::size_t maximumPartialTrackAccessibleItems = 4096;

[[nodiscard]] juce::String pointIdentityText(
    const talas::project::MaterializedPartialTrackPointIdentity& identity)
{
    if (identity.origin
        == talas::project::MaterializedPartialTrackPointOrigin::Inserted)
    {
        return "inserted " + juceString(identity.insertedEditId);
    }
    return "source segment "
        + juce::String(static_cast<juce::int64>(identity.source.segmentId))
        + ", frame "
        + juce::String(static_cast<juce::int64>(identity.source.sourceFrame));
}

[[nodiscard]] std::string pointFocusIdentity(
    const talas::project::MaterializedPartialTrackPointIdentity& identity)
{
    if (identity.origin
        == talas::project::MaterializedPartialTrackPointOrigin::Inserted)
    {
        return "partial-track.point.inserted:" + identity.insertedEditId;
    }
    return "partial-track.point.source:"
        + std::to_string(identity.source.segmentId) + ":"
        + std::to_string(identity.source.sourceFrame) + ":"
        + std::to_string(identity.source.trackId);
}

[[nodiscard]] std::string defaultEditSetId(std::string_view assetId)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : assetId)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "partial_";
    for (int shift = 60; shift >= 0; shift -= 4)
        result.push_back(digits[(hash >> shift) & 0x0fU]);
    return result;
}

class PartialTrackEditorPanelImpl final
    : public PartialTrackEditorPanel,
      private juce::ScrollBar::Listener
{
    struct PendingPointCandidate
    {
        talas::app::PartialTrackEditorPointGeometry original;
        std::uint64_t frame = 0;
        double frequencyHz = 0.0;
    };

    class AccessibilityContainer final : public juce::Component
    {
    public:
        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            return juce::Component::createIgnoredAccessibilityHandler(*this);
        }
    };

    class AccessibilityEntity final : public juce::Component
    {
    public:
        AccessibilityEntity(
            PartialTrackEditorPanelImpl& owner,
            std::string focusIdentity)
            : owner_(owner)
            , focusIdentity_(std::move(focusIdentity))
        {
            setComponentID(juceString(focusIdentity_));
            setAccessible(true);
            setWantsKeyboardFocus(true);
            setInterceptsMouseClicks(false, false);
        }

        void update(
            talas::app::PartialTrackEditorSelection selection,
            juce::String title,
            juce::Rectangle<int> bounds,
            bool selected)
        {
            selection_ = std::move(selection);
            selected_ = selected;
            describeControl(*this, title, title);
            setBounds(bounds);
        }

        [[nodiscard]] std::string_view focusIdentity() const noexcept
        {
            return focusIdentity_;
        }

        void focusGained(juce::Component::FocusChangeType) override
        {
            owner_.select(selection_);
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            const auto globalModifier =
                key.getModifiers().isCommandDown()
                || key.getModifiers().isCtrlDown();
            const auto direction = key == juce::KeyPress::rightKey
                    || key == juce::KeyPress::downKey
                ? 1
                : (key == juce::KeyPress::leftKey
                        || key == juce::KeyPress::upKey
                    ? -1
                    : 0);
            if (direction != 0 && !globalModifier)
                return owner_.moveAccessibilityFocus(
                    focusIdentity_, direction);
            if (owner_.handleEditKey(key))
                return true;
            if (key == juce::KeyPress::escapeKey)
            {
                owner_.grabKeyboardFocus();
                return true;
            }
            return false;
        }

        bool keyStateChanged(bool) override
        {
            owner_.endKeyboardGestureIfReleased();
            return false;
        }

        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            juce::Component::SafePointer<AccessibilityEntity> safeThis(this);
            auto actions = juce::AccessibilityActions{}
                .addAction(
                    juce::AccessibilityActionType::focus,
                    [safeThis] {
                        if (safeThis != nullptr)
                            safeThis->grabKeyboardFocus();
                    })
                .addAction(
                    juce::AccessibilityActionType::press,
                    [safeThis] {
                        if (safeThis != nullptr)
                            safeThis->owner_.select(safeThis->selection_);
                    });
            class Handler final : public juce::AccessibilityHandler
            {
            public:
                Handler(
                    AccessibilityEntity& entity,
                    juce::AccessibilityActions actions)
                    : juce::AccessibilityHandler(
                          entity,
                          juce::AccessibilityRole::treeItem,
                          std::move(actions))
                    , entity_(entity)
                {
                }

                juce::AccessibleState getCurrentState() const override
                {
                    auto state = juce::AccessibilityHandler::getCurrentState()
                        .withSelectable();
                    return entity_.selected_ ? state.withSelected() : state;
                }

            private:
                AccessibilityEntity& entity_;
            };
            return std::make_unique<Handler>(*this, std::move(actions));
        }

    private:
        PartialTrackEditorPanelImpl& owner_;
        std::string focusIdentity_;
        talas::app::PartialTrackEditorSelection selection_;
        bool selected_ = false;
    };

    class Viewport final : public juce::Component
    {
    public:
        explicit Viewport(PartialTrackEditorPanelImpl& owner)
            : owner_(owner)
        {
            setComponentID("partial-tracks.viewport");
            setWantsKeyboardFocus(true);
            setFocusContainerType(
                juce::Component::FocusContainerType::focusContainer);
            describeControl(
                *this,
                "Partial Tracks editor",
                "Virtualized time and frequency view of stable partial tracks. Tab reaches visible track and point identities; arrow keys move between them; Escape clears selection.");
            accessibilityContainer_.setComponentID(
                "partial-tracks.accessibility");
            accessibilityContainer_.setAccessible(true);
            accessibilityContainer_.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(accessibilityContainer_);
        }

        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            return std::make_unique<juce::AccessibilityHandler>(
                *this,
                juce::AccessibilityRole::tree);
        }

        void resized() override
        {
            accessibilityContainer_.setBounds(getLocalBounds());
        }

        void paint(juce::Graphics& graphics) override
        {
            WorkbenchStyle::paintWell(graphics, getLocalBounds().toFloat(), 6.0F);
            graphics.setColour(panelBorderColour);
            graphics.drawRoundedRectangle(
                getLocalBounds().toFloat().reduced(0.5F), 6.0F,
                std::ceil(WorkbenchType::focusOutlinePixels()) * 0.5F);
            const auto plot = owner_.plotBounds();
            if (owner_.projection_.state
                != talas::app::PartialTrackEditorState::Ready)
            {
                graphics.setColour(
                    owner_.projection_.state
                            == talas::app::PartialTrackEditorState::Error
                        ? errorColour
                        : secondaryTextColour);
                graphics.setFont(WorkbenchType::bodyStrong());
                graphics.drawFittedText(
                    juceString(owner_.projection_.message),
                    getLocalBounds().reduced(workbenchScaledUiPixels(18)),
                    juce::Justification::centred,
                    3);
                return;
            }
            // The spectrum on black, where a track
            // saved away leaves black.
            graphics.setColour(juce::Colour{talas::app::spectrumEditorBackgroundColour()});
            graphics.fillRect(plot);

            graphics.setFont(WorkbenchType::micro());
            for (const auto& tick : owner_.projection_.timeTicks)
            {
                const auto x = plot.getX()
                    + static_cast<int>(std::lround(tick.pixel));
                graphics.setColour(workbenchTheme.graphGrid);
                graphics.drawVerticalLine(
                    x,
                    static_cast<float>(plot.getY()),
                    static_cast<float>(plot.getBottom()));
                graphics.setColour(secondaryTextColour);
                graphics.drawText(
                    juce::String(tick.value / owner_.sampleRate_, 3) + " s",
                    x + 3,
                    2,
                    workbenchScaledUiPixels(72),
                    workbenchScaledUiPixels(18),
                    juce::Justification::centredLeft);
            }
            for (const auto& tick : owner_.projection_.frequencyTicks)
            {
                const auto y = plot.getY()
                    + static_cast<int>(std::lround(tick.pixel));
                graphics.setColour(workbenchTheme.graphGrid);
                graphics.drawHorizontalLine(
                    y,
                    static_cast<float>(plot.getX()),
                    static_cast<float>(plot.getRight()));
                graphics.setColour(secondaryTextColour);
                graphics.drawText(
                    juce::String(tick.value, tick.value < 100.0 ? 1 : 0)
                        + " Hz",
                    3,
                    y - workbenchScaledUiPixels(8),
                    plot.getX() - workbenchScaledUiPixels(8),
                    workbenchScaledUiPixels(16),
                    juce::Justification::centredRight);
            }

            graphics.saveState();
            graphics.reduceClipRegion(plot);
            // FREQ in green or AMP in red labels the vertical axis.
            graphics.setColour(juce::Colour{talas::app::partialTrackAxisColour(owner_.axis_)});
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText(juce::String{std::string{talas::app::partialTrackAxisLabel(owner_.axis_)}},
                plot.getX() + 2, plot.getY() + 2, 40, 14, juce::Justification::centredLeft);
            const auto amplitudeAxis = owner_.axis_ == talas::app::PartialTrackVerticalAxis::Amplitude;
            const auto amplitudeY = [&plot](double amplitude) {
                return static_cast<float>(plot.getBottom()) - static_cast<float>(std::clamp(amplitude, 0.0, 1.0)) * static_cast<float>(plot.getHeight());
            };
            const auto radius = static_cast<float>(
                workbenchScaledUiPixels(3));
            if (amplitudeAxis && owner_.joinDots_)
            {
                // Each track's amplitude envelope, point to point.
                std::map<std::uint64_t, std::vector<const talas::app::PartialTrackEditorPointGeometry*>> tracks;
                for (const auto& point : owner_.projection_.points)
                    tracks[point.trackId].push_back(&point);
                graphics.setColour(audioColour.withAlpha(0.68F));
                for (auto& [trackId, points] : tracks)
                {
                    std::ranges::sort(points, {}, &talas::app::PartialTrackEditorPointGeometry::sourceFrame);
                    for (std::size_t index = 1; index < points.size(); ++index)
                        if (talas::app::partialTrackDotsJoined(true, points[index]->x - points[index - 1]->x, 2.0 * radius))
                            graphics.drawLine(static_cast<float>(plot.getX() + points[index - 1]->x), amplitudeY(points[index - 1]->amplitude),
                            static_cast<float>(plot.getX() + points[index]->x), amplitudeY(points[index]->amplitude),
                            WorkbenchType::focusOutlinePixels());
                }
            }
            for (const auto& segment : owner_.projection_.segments)
            {
                // Each frame a dot alone, or the dots joined where there is a
                // gap between them.
                if (!owner_.joinDots_ || amplitudeAxis)
                    break;
                if (!talas::app::partialTrackDotsJoined(true, segment.endX - segment.startX, 2.0 * radius))
                    continue;
                const auto start = juce::Point<float>{
                    static_cast<float>(plot.getX() + segment.startX),
                    static_cast<float>(plot.getY() + segment.startY)};
                const auto end = juce::Point<float>{
                    static_cast<float>(plot.getX() + segment.endX),
                    static_cast<float>(plot.getY() + segment.endY)};
                graphics.setColour(audioColour.withAlpha(0.68F));
                if (segment.interpolation
                    == talas::project::PartialTrackPointInterpolation::Step)
                {
                    graphics.drawLine(
                        start.x, start.y, end.x, start.y,
                        WorkbenchType::focusOutlinePixels());
                    graphics.drawLine(
                        end.x, start.y, end.x, end.y,
                        WorkbenchType::focusOutlinePixels());
                }
                else
                {
                    graphics.drawLine(
                        {start, end},
                        WorkbenchType::focusOutlinePixels());
                }
            }
            std::set<std::uint64_t> clearedTracks;
            for (const auto& row : owner_.projection_.rows)
                if (row.muted)
                    clearedTracks.insert(row.trackId);
            for (const auto& point : owner_.projection_.points)
            {
                const auto x = static_cast<float>(plot.getX() + point.x);
                const auto y = amplitudeAxis ? amplitudeY(point.amplitude) : static_cast<float>(plot.getY() + point.y);
                // Amplitude as colour, a selected track brighter, a cleared
                // one dark gray.
                const auto selected = point.selected || owner_.regionSelection_.contains(point.trackId, point.sourceFrame);
                graphics.setColour(juce::Colour{!selected && clearedTracks.contains(point.trackId)
                        ? talas::app::partialTrackClearedColour()
                        : talas::app::partialTrackAmplitudeColour(point.amplitude, selected)});
                graphics.fillEllipse(
                    x - radius,
                    y - radius,
                    radius * 2.0F,
                    radius * 2.0F);
                if (point.selected)
                {
                    graphics.setColour(textColour);
                    graphics.drawEllipse(
                        x - radius - 2.0F,
                        y - radius - 2.0F,
                        radius * 2.0F + 4.0F,
                        radius * 2.0F + 4.0F,
                        WorkbenchType::focusOutlinePixels());
                }
            }
            if (!owner_.regionSelection_.empty())
            {
                graphics.setColour(selectionColour);
                for (const auto& point : owner_.projection_.points)
                {
                    if (!owner_.regionSelection_.contains(point.trackId, point.sourceFrame))
                        continue;
                    const auto x = static_cast<float>(plot.getX() + point.x);
                    const auto y = static_cast<float>(plot.getY() + point.y);
                    graphics.drawEllipse(x - radius - 1.5F, y - radius - 1.5F, radius * 2.0F + 3.0F,
                        radius * 2.0F + 3.0F, WorkbenchType::focusOutlinePixels());
                }
            }
            if (owner_.boxStart_.has_value() && owner_.boxEnd_.has_value())
            {
                graphics.setColour(selectionColour.withAlpha(0.18F));
                const auto box = juce::Rectangle<int>{*owner_.boxStart_, *owner_.boxEnd_};
                graphics.fillRect(box);
                graphics.setColour(selectionColour);
                graphics.drawRect(box);
            }
            const auto drawFrameLine = [&](std::uint64_t frame, juce::Colour colour, const juce::String& label) {
                const auto x = owner_.xAtFrame(frame);
                if (x < plot.getX() || x > plot.getRight())
                    return;
                graphics.setColour(colour);
                graphics.drawVerticalLine(x, static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
                if (label.isNotEmpty())
                    graphics.drawText(label, x + 2, plot.getY() + 2, workbenchScaledUiPixels(80),
                        workbenchScaledUiPixels(14), juce::Justification::centredLeft);
            };
            for (std::size_t index = 0; index < owner_.markers_.markers.size(); ++index)
            {
                const auto& marker = owner_.markers_.markers[index];
                drawFrameLine(marker.frame, owner_.selectedMarker_ == index ? selectionColour : controlColour,
                    juceString(marker.name));
            }
            const juce::Colour loopMarker{talas::app::spectrumEditorLoopMarkerColour()};
            if (owner_.markers_.loopStart.has_value())
                drawFrameLine(*owner_.markers_.loopStart, loopMarker, "start");
            if (owner_.markers_.loopEnd.has_value())
                drawFrameLine(*owner_.markers_.loopEnd, loopMarker, "end");
            // The yellow time cursor, its time in white at
            // the top.
            drawFrameLine(owner_.scrubFrame_, juce::Colour{talas::app::partialTrackScrubBarColour()}, {});
            if (owner_.sourceAsset_ != nullptr)
            {
                graphics.setColour(juce::Colour{talas::app::partialTrackScrubTimeColour()});
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(juceString(talas::app::partialTrackScrubTimeText(owner_.scrubFrame_, owner_.sourceAsset_->sampleRate)),
                    plot.getX(), std::max(0, plot.getY() - workbenchScaledUiPixels(14)), workbenchScaledUiPixels(90),
                    workbenchScaledUiPixels(14), juce::Justification::centredLeft);
            }
            if (owner_.pendingSnap_.has_value())
            {
                const auto& pending = *owner_.pendingSnap_;
                const auto range = owner_.horizontal_.getCurrentRange();
                const auto x = static_cast<float>(plot.getX())
                    + static_cast<float>(
                        (static_cast<double>(pending.frame)
                            - range.getStart())
                        * static_cast<double>(plot.getWidth())
                        / std::max(1.0, range.getLength()));
                const auto y = static_cast<float>(plot.getY())
                    + static_cast<float>(
                        (owner_.maximumFrequencyHz_
                            - pending.frequencyHz)
                        * static_cast<double>(plot.getHeight())
                        / (owner_.maximumFrequencyHz_
                            - owner_.minimumFrequencyHz_));
                const auto candidateRadius = radius + 3.0F;
                graphics.setColour(controlColour);
                graphics.drawEllipse(
                    x - candidateRadius,
                    y - candidateRadius,
                    candidateRadius * 2.0F,
                    candidateRadius * 2.0F,
                    WorkbenchType::focusOutlinePixels());
            }
            graphics.restoreState();
            if (owner_.readout_.isNotEmpty())
            {
                graphics.setColour(textColour);
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(owner_.readout_, plot.getX(), 2, plot.getWidth() - 4,
                    workbenchScaledUiPixels(16), juce::Justification::centredRight);
            }

            const auto rowHeight = std::max(
                1,
                plot.getHeight()
                    / std::max<int>(1, static_cast<int>(owner_.projection_.rows.size())));
            for (std::size_t index = 0;
                 index < owner_.projection_.rows.size(); ++index)
            {
                const auto& row = owner_.projection_.rows[index];
                const auto y = plot.getY()
                    + static_cast<int>(index) * rowHeight;
                if (row.selected)
                {
                    graphics.setColour(workbenchTheme.selectedRow);
                    graphics.fillRect(0, y, plot.getX(), rowHeight);
                }
                graphics.setColour(row.muted
                        ? secondaryTextColour
                        : (row.solo ? controlColour : textColour));
                graphics.drawText(
                    "T" + juce::String(static_cast<juce::int64>(row.trackId))
                        + juceString(" · ")
                        + juce::String(static_cast<int>(row.pointCount)),
                    workbenchScaledUiPixels(4),
                    y,
                    plot.getX() - workbenchScaledUiPixels(8),
                    rowHeight,
                    juce::Justification::centredLeft);
            }
        }

        void mouseDown(const juce::MouseEvent& event) override
        {
            const auto plot = owner_.plotBounds();
            // The ruler above the display holds the scrub bar and the
            // markers.
            if (event.y < plot.getY() && event.x >= plot.getX() && event.mods.isLeftButtonDown())
            {
                owner_.pressRuler(event.x);
                return;
            }
            if (event.x < plot.getX())
            {
                const auto rowHeight = std::max(
                    1,
                    plot.getHeight()
                        / std::max<int>(
                            1,
                            static_cast<int>(owner_.projection_.rows.size())));
                const auto row = event.y >= plot.getY()
                    ? static_cast<std::size_t>((event.y - plot.getY()) / rowHeight)
                    : owner_.projection_.rows.size();
                if (row < owner_.projection_.rows.size())
                {
                    owner_.select({
                        talas::app::PartialTrackEditorItemKind::Track,
                        owner_.projection_.rows[row].trackId,
                        {},
                    });
                    if (event.mods.isPopupMenu())
                        owner_.showContextMenu(event.getPosition());
                    return;
                }
            }
            // In drawing mode the pointer redraws the selected envelope
            //.
            if (owner_.drawMode_ && event.mods.isLeftButtonDown() && plot.contains(event.getPosition()))
            {
                owner_.beginDrawing(event.getPosition());
                return;
            }
            const auto radius = static_cast<double>(
                workbenchScaledUiPixels(10));
            const auto point = std::ranges::min_element(
                owner_.projection_.points,
                {},
                [&](const auto& candidate) {
                    const auto dx = static_cast<double>(event.x - plot.getX())
                        - candidate.x;
                    const auto dy = static_cast<double>(event.y - plot.getY())
                        - candidate.y;
                    return dx * dx + dy * dy;
                });
            if (point != owner_.projection_.points.end())
            {
                const auto dx = static_cast<double>(event.x - plot.getX())
                    - point->x;
                const auto dy = static_cast<double>(event.y - plot.getY())
                    - point->y;
                if (dx * dx + dy * dy <= radius * radius)
                {
                    const auto selectedPoint = *point;
                    owner_.select({
                        talas::app::PartialTrackEditorItemKind::Point,
                        point->trackId,
                        point->identity,
                    });
                    if (event.mods.isPopupMenu())
                    {
                        owner_.showContextMenu(event.getPosition());
                        return;
                    }
                    if (event.mods.isLeftButtonDown())
                    {
                        owner_.startPointGesture(
                            selectedPoint,
                            event.mods.isShiftDown());
                    }
                    return;
                }
            }
            owner_.clearSelection();
            if (event.mods.isPopupMenu())
            {
                owner_.showContextMenu(event.getPosition());
                return;
            }
            // A box drawn on the display selects the points in it.
            if (event.mods.isLeftButtonDown() && plot.contains(event.getPosition()))
                owner_.beginBox(event.getPosition(), event.mods.isShiftDown(),
                    event.mods.isCommandDown() || event.mods.isCtrlDown());
        }

        void mouseDrag(const juce::MouseEvent& event) override
        {
            if (owner_.drawing_)
                owner_.drawAt(event.getPosition());
            else if (owner_.scrubbing_)
                owner_.moveScrubBar(event.x);
            else if (owner_.boxStart_.has_value())
                owner_.dragBox(event.getPosition());
            else
                owner_.dragPointTo(event.getPosition());
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (owner_.drawing_)
                owner_.finishDrawing();
            else if (owner_.scrubbing_)
                owner_.scrubbing_ = false;
            else if (owner_.boxStart_.has_value())
                owner_.finishBox();
            else
                owner_.finishPointGesture();
        }

        void mouseMove(const juce::MouseEvent& event) override
        {
            owner_.updateReadout(event.getPosition());
        }

        void mouseDoubleClick(const juce::MouseEvent& event) override
        {
            if (event.mods.isLeftButtonDown())
                (void) owner_.insertPointAt(event.getPosition());
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            return owner_.handleEditKey(key);
        }

        bool keyStateChanged(bool) override
        {
            owner_.endKeyboardGestureIfReleased();
            return false;
        }

    private:
        PartialTrackEditorPanelImpl& owner_;
        AccessibilityContainer accessibilityContainer_;

        friend class PartialTrackEditorPanelImpl;
    };

public:
    explicit PartialTrackEditorPanelImpl(
        PartialTrackEditorEditingHooks hooks)
        : hooks_(std::move(hooks))
        , horizontal_(false)
        , vertical_(true)
        , viewport_(*this)
    {
        setComponentID("partial-tracks.editor");
        setWantsKeyboardFocus(true);
        setFocusContainerType(
            juce::Component::FocusContainerType::focusContainer);
        describeControl(
            *this,
            "Partial Tracks",
            "Nondestructive PartialSpectrum source projection with exact asset generation, stable point identities, and workspace-local view filters.");
        for (auto* component : {
                 static_cast<juce::Component*>(&title_),
                 static_cast<juce::Component*>(&source_),
                 static_cast<juce::Component*>(&quality_),
                 static_cast<juce::Component*>(&amplitudeLabel_),
                 static_cast<juce::Component*>(&amplitude_),
                 static_cast<juce::Component*>(&includeMuted_),
                 static_cast<juce::Component*>(&soloOnly_),
                 static_cast<juce::Component*>(&spectrumMenu_),
                 static_cast<juce::Component*>(&preview_),
                 static_cast<juce::Component*>(&renderAudio_),
                 static_cast<juce::Component*>(&renderPartial_),
                 static_cast<juce::Component*>(&cancelOperation_),
                 static_cast<juce::Component*>(&importDerivative_),
                 static_cast<juce::Component*>(&operationStatus_),
                 static_cast<juce::Component*>(&viewport_),
                 static_cast<juce::Component*>(&horizontal_),
                 static_cast<juce::Component*>(&vertical_)})
        {
            addAndMakeVisible(*component);
        }
        title_.setText("Partial Tracks", juce::dontSendNotification);
        title_.setFont(WorkbenchType::panelTitle());
        title_.setColour(juce::Label::textColourId, textColour);
        source_.setFont(WorkbenchType::micro());
        source_.setColour(juce::Label::textColourId, dataColour);
        quality_.setFont(WorkbenchType::micro());
        quality_.setColour(juce::Label::textColourId, secondaryTextColour);
        amplitudeLabel_.setText("Min amplitude", juce::dontSendNotification);
        amplitudeLabel_.setFont(WorkbenchType::label());
        amplitude_.setComponentID("partial-tracks.filter.amplitude");
        amplitude_.setSliderStyle(juce::Slider::LinearHorizontal);
        amplitude_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false,
            workbenchScaledUiPixels(58), workbenchScaledUiPixels(20));
        amplitude_.setRange(0.0, 4.0, 0.01);
        amplitude_.setValue(0.0, juce::dontSendNotification);
        includeMuted_.setComponentID("partial-tracks.filter.muted");
        includeMuted_.setButtonText("Muted");
        includeMuted_.setToggleState(true, juce::dontSendNotification);
        soloOnly_.setComponentID("partial-tracks.filter.solo");
        soloOnly_.setButtonText("Solo only");
        spectrumMenu_.setComponentID("partial-tracks.spectrum");
        spectrumMenu_.setButtonText("Spectrum");
        spectrumMenu_.onClick = [this] { showSpectrumMenu(); };
        // The buttons across the bottom, in groups of four as the function
        // keys are.
        for (const auto& info : talas::app::spectrumEditorButtons())
        {
            auto* button = stripButtons_.add(new juce::TextButton(juce::String{info.label.data(), info.label.size()}));
            button->setComponentID("partial-tracks.button.f" + juce::String(info.functionKey));
            button->setTooltip("F" + juce::String(info.functionKey));
            button->onClick = [this, which = info.button] { pressSpectrumButton(which); };
            addAndMakeVisible(*button);
        }
        describeControl(
            spectrumMenu_,
            "Spectrum editing",
            "Play all, the selection or between or outside the loop markers; place markers and loop markers at the scrub bar; cut between them; select by criteria.");
        preview_.setComponentID("partial-tracks.preview");
        preview_.setButtonText("Preview");
        renderAudio_.setComponentID("partial-tracks.render-audio");
        renderAudio_.setButtonText("Render Audio");
        renderPartial_.setComponentID("partial-tracks.render-partial");
        renderPartial_.setButtonText("Render Partial");
        cancelOperation_.setComponentID("partial-tracks.cancel-render");
        cancelOperation_.setButtonText("Cancel Render");
        importDerivative_.setComponentID("partial-tracks.import-render");
        importDerivative_.setButtonText("Import Result");
        operationStatus_.setFont(WorkbenchType::micro());
        operationStatus_.setColour(
            juce::Label::textColourId, secondaryTextColour);
        describeControl(
            amplitude_,
            "Minimum partial amplitude filter",
            "Workspace-local view filter. It does not edit, decode, or prepare the source asset.");
        describeControl(
            includeMuted_,
            "Show muted partial tracks",
            "Workspace-local visibility only; source mute state is unchanged.");
        describeControl(
            soloOnly_,
            "Show solo partial tracks only",
            "Workspace-local visibility only; source solo state is unchanged.");
        horizontal_.setComponentID("partial-tracks.time-scroll");
        vertical_.setComponentID("partial-tracks.track-scroll");
        horizontal_.addListener(this);
        vertical_.addListener(this);
        amplitude_.onValueChange = [this] { applyFilter(); };
        includeMuted_.onClick = [this] { applyFilter(); };
        soloOnly_.onClick = [this] { applyFilter(); };
        preview_.onClick = [this] {
            playRequest_ = talas::app::PartialTrackPlay::All;
            if (operationState_.previewActive
                || operationState_.previewPreparing)
            {
                if (hooks_.stopPreview)
                    hooks_.stopPreview();
            }
            else if (hooks_.startPreview)
            {
                hooks_.startPreview();
            }
        };
        renderAudio_.onClick = [this] {
            if (hooks_.renderAudioDerivative)
                hooks_.renderAudioDerivative();
        };
        renderPartial_.onClick = [this] {
            if (hooks_.renderPartialDerivative)
                hooks_.renderPartialDerivative();
        };
        cancelOperation_.onClick = [this] {
            if (hooks_.cancelDerivative)
                hooks_.cancelDerivative();
        };
        importDerivative_.onClick = [this] {
            if (hooks_.importDerivative)
                hooks_.importDerivative();
        };
        describeControl(
            preview_,
            "Preview edited partial tracks",
            "Prepares one immutable generation off-thread and atomically adopts it without blocking the audio callback.");
        describeControl(
            renderAudio_,
            "Render edited partial tracks to Audio",
            "Writes a new PCM WAV through private staging. Existing files and the source asset are never overwritten.");
        describeControl(
            renderPartial_,
            "Render edited PartialSpectrum derivative",
            "Writes one materialized PartialSpectrum asset through private no-replace staging.");
        describeControl(
            importDerivative_,
            "Import verified derivative",
            "Explicitly adds the last complete inspected derivative as a new project asset.");
        updateOperationControls();
        refreshProjection();
    }

    ~PartialTrackEditorPanelImpl() override
    {
        horizontal_.removeListener(this);
        vertical_.removeListener(this);
    }

    void setLoading() override
    {
        cancelGesture();
        manifest_.reset();
        sourceAsset_.reset();
        currentEdits_.reset();
        externalMessage_.clear();
        model_.setLoading();
        sourceFrameCount_ = 1;
        sampleRate_ = 1.0;
        trackCount_ = 0;
        refreshProjection();
    }

    void setUnavailable(juce::String message) override
    {
        cancelGesture();
        manifest_.reset();
        sourceAsset_.reset();
        currentEdits_.reset();
        externalMessage_ = std::move(message);
        model_.setError(externalMessage_.toStdString());
        refreshProjection();
    }

    bool setSource(
        talas::project::ProjectAsset manifest,
        std::shared_ptr<const talas::project::PartialTrackAsset> source,
        std::optional<talas::project::ProjectPartialTrackEditSet> edits) override
    {
        if (source == nullptr)
        {
            setUnavailable("Partial-track source is unavailable");
            return false;
        }
        const auto prepared = model_.setSource({manifest, source, edits});
        if (!prepared)
        {
            manifest_.reset();
            sourceAsset_.reset();
            refreshProjection();
            return false;
        }
        // A spectrum file opened brings the markers kept in
        // its header.
        const auto anotherFile = !manifest_.has_value() || manifest_->id != manifest.id
            || manifest_->contentHash != manifest.contentHash;
        manifest_ = std::move(manifest);
        sourceAsset_ = std::move(source);
        currentEdits_ = std::move(edits);
        if (anotherFile)
        {
            markers_ = talas::app::partialTrackMarkersOf(*sourceAsset_);
            selectedMarker_.reset();
        }
        externalMessage_.clear();
        sourceFrameCount_ = sourceAsset_->sourceFrameCount;
        sampleRate_ = sourceAsset_->sampleRate;
        std::vector<std::uint64_t> tracks;
        double minimumFrequency = std::numeric_limits<double>::infinity();
        double maximumFrequency = 0.0;
        for (const auto& frame : sourceAsset_->frames)
        {
            for (const auto& point : frame.points)
            {
                tracks.push_back(point.trackId);
                minimumFrequency = std::min(
                    minimumFrequency, point.frequencyHz);
                maximumFrequency = std::max(
                    maximumFrequency, point.frequencyHz);
            }
        }
        std::ranges::sort(tracks);
        tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
        trackCount_ = tracks.size();
        if (std::isfinite(minimumFrequency))
        {
            minimumFrequencyHz_ = std::max(
                0.001, minimumFrequency * 0.9);
            maximumFrequencyHz_ = std::min(
                sampleRate_ * 0.5,
                maximumFrequency * 1.1);
            if (maximumFrequencyHz_ <= minimumFrequencyHz_)
            {
                minimumFrequencyHz_ = std::max(
                    0.001, minimumFrequency * 0.5);
                maximumFrequencyHz_ = std::min(
                    sampleRate_ * 0.5,
                    std::max(minimumFrequency + 1.0,
                             minimumFrequency * 1.5));
            }
        }
        resetScrollRanges();
        refreshProjection();
        return true;
    }

    void refreshEdits(
        const talas::project::ProjectDocument& document) override
    {
        if (!manifest_.has_value() || sourceAsset_ == nullptr)
            return;
        const auto currentAsset = std::ranges::find(
            document.assets,
            manifest_->id,
            &talas::project::ProjectAsset::id);
        if (currentAsset == document.assets.end()
            || currentAsset->kind != manifest_->kind
            || currentAsset->generation != manifest_->generation
            || currentAsset->contentHash != manifest_->contentHash)
        {
            setUnavailable(
                juceString("Partial-track source authority changed · stage the project to load its new immutable generation"));
            return;
        }
        try
        {
            const auto edits = std::ranges::find(
                document.partialTrackEdits,
                manifest_->id,
                &talas::project::ProjectPartialTrackEditSet::assetId);
            std::optional<talas::project::ProjectPartialTrackEditSet> candidate;
            if (edits != document.partialTrackEdits.end())
                candidate = *edits;
            if (model_.setSource(
                    {*manifest_, sourceAsset_, candidate}))
            {
                currentEdits_ = std::move(candidate);
            }
            else
            {
                currentEdits_.reset();
            }
        }
        catch (...)
        {
            setUnavailable(
                "Partial-track edit projection could not be prepared");
            return;
        }
        refreshProjection();
    }

    void setOperationState(
        PartialTrackEditorOperationState state) override
    {
        operationState_ = std::move(state);
        updateOperationControls();
    }

    std::optional<PartialTrackEditorSourceSnapshot>
    sourceSnapshot() const override
    {
        if (!manifest_.has_value() || sourceAsset_ == nullptr)
            return std::nullopt;
        return PartialTrackEditorSourceSnapshot{
            *manifest_, sourceAsset_, currentEdits_, playRequest_, regionSelection_, markers_, scrubRequest_};
    }

    std::optional<PartialTrackEditorOperationSourceInfo>
    operationSourceInfo() const noexcept override
    {
        if (!manifest_.has_value()
            || sourceAsset_ == nullptr
            || projection_.state
                == talas::app::PartialTrackEditorState::Error)
        {
            return std::nullopt;
        }
        return PartialTrackEditorOperationSourceInfo{
            sourceAsset_->sampleRate,
            model_.maximumRenderedSegmentTrackCount(),
        };
    }

    talas::app::PartialTrackEditorState stateForSmoke() const noexcept override
    {
        return projection_.state;
    }

    std::size_t visibleAccessibleItemCountForSmoke() const noexcept override
    {
        return accessibilityEntities_.size();
    }

    bool sourceTextContainsForSmoke(
        const juce::String& text) const override
    {
        return source_.getText().contains(text);
    }

    bool compactLayoutForSmoke() const noexcept override
    {
        return compactLayout_;
    }

    bool frequencyLabelsFitForSmoke() const noexcept override
    {
        const auto plot = plotBounds();
        const auto halfLabelHeight = workbenchScaledUiPixels(8);
        return plot.getY() >= halfLabelHeight
            && plot.getBottom() <= viewport_.getHeight() - halfLabelHeight;
    }

    bool nudgeFirstPointForSmoke() override
    {
        if (projection_.points.empty())
            return false;
        const auto point = projection_.points.front();
        select({
            talas::app::PartialTrackEditorItemKind::Point,
            point.trackId,
            point.identity,
        });
        const juce::KeyPress nudge{
            juce::KeyPress::rightKey,
            juce::ModifierKeys::commandModifier,
            0};
        if (!handleEditKey(nudge))
            return false;
        endGesture();
        const auto* moved = selectedPoint();
        return moved != nullptr
            && moved->identity == point.identity
            && moved->sourceFrame == point.sourceFrame + 1;
    }

    bool exerciseEditCommandsForSmoke() override
    {
        const auto* point = selectedPoint();
        if (point == nullptr)
            return false;
        if (!setInterpolation(
                talas::project::PartialTrackPointInterpolation::Step))
            return false;
        if (projection_.rows.empty())
            return false;
        select({
            talas::app::PartialTrackEditorItemKind::Track,
            projection_.rows.front().trackId,
            {},
        });
        if (!toggleTrackState(false))
            return false;
        const auto candidate = projection_.points.empty()
            ? std::optional<talas::app::PartialTrackEditorPointGeometry>{}
            : std::optional<talas::app::PartialTrackEditorPointGeometry>{
                projection_.points.front()};
        if (!candidate.has_value())
            return false;
        select({
            talas::app::PartialTrackEditorItemKind::Point,
            candidate->trackId,
            candidate->identity,
        });
        startPointGesture(*candidate, true);
        const auto plot = plotBounds();
        dragPointTo({
            plot.getX() + plot.getWidth() / 4,
            plot.getCentreY()});
        finishPointGesture();
        if (!pendingSnap_.has_value()
            || !handleEditKey(juce::KeyPress{
                juce::KeyPress::returnKey}))
        {
            return false;
        }
        if (!insertPointAt({
                plot.getX() + (plot.getWidth() * 3) / 4,
                plot.getCentreY()}))
        {
            return false;
        }
        const auto inserted = std::ranges::find_if(
            projection_.points,
            [](const auto& candidatePoint) {
                return candidatePoint.identity.origin
                    == talas::project::MaterializedPartialTrackPointOrigin::Inserted;
            });
        if (inserted == projection_.points.end())
            return false;
        select({
            talas::app::PartialTrackEditorItemKind::Point,
            inserted->trackId,
            inserted->identity,
        });
        if (!deleteSelection() || projection_.rows.empty())
            return false;
        const auto splitTrack = projection_.rows.front().trackId;
        select({
            talas::app::PartialTrackEditorItemKind::Track,
            splitTrack,
            {},
        });
        if (!splitSelectedVisibleRegion() || projection_.rows.empty())
            return false;
        const auto newTrack = std::ranges::max_element(
            projection_.rows,
            {},
            &talas::app::PartialTrackEditorTrackRow::trackId);
        if (newTrack == projection_.rows.end())
            return false;
        select({
            talas::app::PartialTrackEditorItemKind::Track,
            newTrack->trackId,
            {},
        });
        const juce::KeyPress nudgeRegion{
            juce::KeyPress::rightKey,
            juce::ModifierKeys::commandModifier,
            0};
        if (!handleEditKey(nudgeRegion))
            return false;
        endGesture();
        return exerciseClearTrimAndFiltersForSmoke();
    }

    // A box, a number key, a marker, the loop markers, a play request and
    // the scissors.
    bool exerciseSpectrumEditingForSmoke() override
    {
        clearSelection();
        if (projection_.points.size() < 2 || sourceAsset_ == nullptr)
            return false;
        const auto plot = plotBounds();
        playRequest_ = talas::app::PartialTrackPlay::All;
        if (!selectBoxPixels(plot, plot.getTopLeft(), false) || regionSelection_.empty()
            || playRequest_ != talas::app::PartialTrackPlay::Selection)
            return false;
        // F1 plays every track, F2 the selected ones.
        playRequest_ = talas::app::PartialTrackPlay::Selection;
        if (!handleEditKey(juce::KeyPress{' ', juce::ModifierKeys::shiftModifier, 0}) || playRequest_ != talas::app::PartialTrackPlay::All)
            return false;
        if (!handleEditKey(juce::KeyPress{juce::KeyPress::F1Key}) || playRequest_ != talas::app::PartialTrackPlay::All
            || !handleEditKey(juce::KeyPress{juce::KeyPress::F2Key}) || playRequest_ != talas::app::PartialTrackPlay::Selection)
            return false;
        // The buttons across the bottom, F3 and F4 the loop plays.
        if (stripButtons_.size() != 12 || stripButtons_[9]->getButtonText() != "Filters"
            || !handleEditKey(juce::KeyPress{juce::KeyPress::F3Key}) || playRequest_ != talas::app::PartialTrackPlay::BetweenLoopMarkers
            || !handleEditKey(juce::KeyPress{juce::KeyPress::F4Key}) || playRequest_ != talas::app::PartialTrackPlay::OutsideLoopMarkers)
            return false;
        playRequest_ = talas::app::PartialTrackPlay::Selection;
        const auto all = regionSelection_.points().size();
        const auto& first = projection_.points.front();
        const juce::Point<int> at{plot.getX() + static_cast<int>(std::lround(first.x)),
            plot.getY() + static_cast<int>(std::lround(first.y))};
        (void) selectBoxPixels(juce::Rectangle<int>{at.translated(-2, -2), at.translated(2, 2)}, at, true);
        if (regionSelection_.points().size() >= all || regionSelection_.contains(first.trackId, first.sourceFrame))
            return false;
        regionSelection_.clear();
        const auto track = projection_.rows.empty() ? std::uint64_t{0} : projection_.rows.front().trackId;
        if (track >= 1 && track <= 10)
        {
            const auto key = static_cast<int>(track == 10 ? '0' : '0' + track);
            if (!handleEditKey(juce::KeyPress{key}) || regionSelection_.trackIds() != std::vector<std::uint64_t>{track})
                return false;
        }
        regionSelection_.clear();
        const auto frames = sourceAsset_->frames;
        if (frames.size() < 2)
            return false;
        scrubFrame_ = frames[1].sourceFrame;
        performSpectrumCommand(10);
        if (markers_.markers.size() != 1 || markers_.markers.front().name != "m1")
            return false;
        scrubFrame_ = frames[0].sourceFrame;
        performSpectrumCommand(11);
        scrubFrame_ = frames[1].sourceFrame;
        performSpectrumCommand(12);
        playRequest_ = talas::app::PartialTrackPlay::BetweenLoopMarkers;
        const auto snapshot = sourceSnapshot();
        if (!snapshot.has_value() || snapshot->play != talas::app::PartialTrackPlay::BetweenLoopMarkers
            || snapshot->markers.loopStart != std::optional<std::uint64_t>{frames[0].sourceFrame})
            return false;
        playRequest_ = talas::app::PartialTrackPlay::All;
        const auto before = projection_.points.size();
        if (!cutBetweenLoopMarkers() || projection_.points.size() >= before)
            return false;
        markers_ = {};
        selectedMarker_.reset();
        // The display: Zoom in, a Control box, the full display, the
        // amplitude axis and joined dots.
        const auto wide = horizontal_.getCurrentRange().getLength();
        performSpectrumCommand(42);
        if (!(horizontal_.getCurrentRange().getLength() < wide))
            return false;
        performSpectrumCommand(44);
        if (horizontal_.getCurrentRange().getStart() != 0.0 || zoomHighHz_ != std::optional<double>{maximumFrequencyHz_})
            return false;
        zoomToBoxPixels(plot.withSizeKeepingCentre(plot.getWidth() / 2, plot.getHeight() / 2));
        if (!(horizontal_.getCurrentRange().getLength() < static_cast<double>(sourceFrameCount_)) || !(*zoomHighHz_ < maximumFrequencyHz_))
            return false;
        // Enter zooms all the way out too.
        if (!handleEditKey(juce::KeyPress{juce::KeyPress::returnKey}) || horizontal_.getCurrentRange().getStart() != 0.0
            || zoomHighHz_ != std::optional<double>{maximumFrequencyHz_})
            return false;
        performSpectrumCommand(40);
        performSpectrumCommand(41);
        if (axis_ != talas::app::PartialTrackVerticalAxis::Amplitude || joinDots_)
            return false;
        performSpectrumCommand(40);
        performSpectrumCommand(41);
        clearSelection();
        const auto scrub = scrubFrame_;
        if (!handleEditKey(juce::KeyPress{juce::KeyPress::rightKey}) || scrubFrame_ != std::min(scrub + 1, sourceFrameCount_ - 1))
            return false;
        return true;
    }

    // Clear, Trim and a track filter.
    [[nodiscard]] bool exerciseClearTrimAndFiltersForSmoke()
    {
        if (projection_.rows.size() < 2)
            return false;
        const auto track = projection_.rows.front().trackId;
        const auto muted = [&](std::uint64_t trackId) {
            const auto row = std::ranges::find(projection_.rows, trackId, &talas::app::PartialTrackEditorTrackRow::trackId);
            return row != projection_.rows.end() && row->muted;
        };
        const auto selectTrack = [&] { select({talas::app::PartialTrackEditorItemKind::Track, track, {}}); };
        selectTrack();
        if (!clearOrTrimSelectedTrack(false) || !muted(track) || !toggleTrackState(false) || muted(track))
            return false;
        selectTrack();
        if (!clearOrTrimSelectedTrack(true) || muted(track)
            || !std::ranges::all_of(projection_.rows, [&](const auto& row) { return row.trackId == track || row.muted; }))
            return false;
        auto restore = makeBatch();
        for (const auto& row : projection_.rows)
            restore.trackStateMutations.push_back({row.trackId, false, false});
        if (!applyBatch(std::move(restore), 0, "Restored the trimmed tracks"))
            return false;
        selectTrack();
        std::vector<std::pair<talas::project::MaterializedPartialTrackPointIdentity, double>> before;
        for (const auto& point : projection_.points)
        {
            if (point.trackId == track)
                before.emplace_back(point.identity, point.amplitude);
        }
        talas::app::PartialTrackFilterSettings settings;
        settings.scale = 0.5;
        if (before.empty() || !applyTrackFilter(talas::app::PartialTrackFilter::ScaleAmplitudes, settings))
            return false;
        return std::ranges::all_of(before, [&](const auto& entry) {
            const auto point = std::ranges::find(projection_.points, entry.first, &talas::app::PartialTrackEditorPointGeometry::identity);
            return point != projection_.points.end() && std::abs(point->amplitude - entry.second * 0.5) <= 1.0e-9;
        });
    }

    bool operationControlsMatchForSmoke(
        bool previewEnabled,
        bool audioDerivativeEnabled,
        bool partialDerivativeEnabled) const override
    {
        return preview_.isEnabled() == previewEnabled
            && renderAudio_.isEnabled() == audioDerivativeEnabled
            && renderPartial_.isEnabled() == partialDerivativeEnabled
            && !cancelOperation_.isEnabled();
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
    }

    void resized() override
    {
        const auto* topLevel = getTopLevelComponent();
        compactLayout_ = getHeight() < workbenchScaledUiPixels(330)
            || getWidth() < workbenchScaledUiPixels(620)
            || (topLevel != nullptr
                && topLevel != this
                && topLevel->getWidth()
                    <= workbenchScaledUiPixels(
                        talas::app::workbenchMinimumWidth));
        auto bounds = getLocalBounds().reduced(workbenchScaledUiPixels(8));
        title_.setBounds(bounds.removeFromTop(workbenchScaledUiPixels(24)));
        source_.setBounds(bounds.removeFromTop(workbenchScaledUiPixels(18)));
        quality_.setVisible(!compactLayout_);
        if (!compactLayout_)
            quality_.setBounds(bounds.removeFromTop(workbenchScaledUiPixels(18)));
        else
            quality_.setBounds({});
        auto controls = bounds.removeFromTop(workbenchScaledUiPixels(28));
        amplitudeLabel_.setBounds(
            controls.removeFromLeft(workbenchScaledUiPixels(92)));
        amplitude_.setBounds(
            controls.removeFromLeft(
                std::min(workbenchScaledUiPixels(220), controls.getWidth() / 2)));
        includeMuted_.setBounds(
            controls.removeFromLeft(workbenchScaledUiPixels(82)));
        soloOnly_.setBounds(
            controls.removeFromLeft(workbenchScaledUiPixels(96)));
        spectrumMenu_.setBounds(
            controls.removeFromLeft(std::min(workbenchScaledUiPixels(92), controls.getWidth())));
        auto operations = bounds.removeFromTop(workbenchScaledUiPixels(28));
        preview_.setBounds(
            operations.removeFromLeft(workbenchScaledUiPixels(78)));
        renderAudio_.setBounds(
            operations.removeFromLeft(workbenchScaledUiPixels(108)));
        renderPartial_.setBounds(
            operations.removeFromLeft(workbenchScaledUiPixels(112)));
        cancelOperation_.setBounds(
            operations.removeFromLeft(workbenchScaledUiPixels(108)));
        importDerivative_.setBounds(
            operations.removeFromLeft(workbenchScaledUiPixels(104)));
        operationStatus_.setBounds(operations);
        bounds.removeFromTop(workbenchScaledUiPixels(4));
        auto strip = bounds.removeFromBottom(workbenchScaledUiPixels(26));
        const auto groupGap = workbenchScaledUiPixels(10);
        const auto buttonWidth = std::max(0, (strip.getWidth() - 2 * groupGap) / std::max(1, stripButtons_.size()));
        for (int index = 0; index < stripButtons_.size(); ++index)
        {
            if (index > 0 && index % 4 == 0)
                strip.removeFromLeft(groupGap);
            stripButtons_[index]->setBounds(strip.removeFromLeft(buttonWidth).reduced(1));
        }
        horizontal_.setBounds(
            bounds.removeFromBottom(workbenchScaledUiPixels(14)));
        vertical_.setBounds(
            bounds.removeFromRight(workbenchScaledUiPixels(14)));
        viewport_.setBounds(bounds);
        updateVisibleRows();
        refreshProjection();
    }

private:
    void updateOperationControls()
    {
        const auto hasSource = manifest_.has_value()
            && sourceAsset_ != nullptr
            && projection_.state
                != talas::app::PartialTrackEditorState::Error;
        preview_.setButtonText(
            operationState_.previewPreparing
                ? "Cancel Preview"
                : operationState_.previewActive
                    ? "Stop Preview"
                    : "Preview");
        preview_.setEnabled(
            hasSource
            && !operationState_.derivativeRunning
            && (operationState_.previewPreparing
                || operationState_.previewActive
                || operationState_.canStartPreview));
        renderAudio_.setEnabled(
            hasSource
            && operationState_.canStartAudioDerivative
            && !operationState_.derivativeRunning);
        renderPartial_.setEnabled(
            hasSource
            && operationState_.canStartPartialDerivative
            && !operationState_.derivativeRunning);
        cancelOperation_.setEnabled(operationState_.derivativeRunning);
        importDerivative_.setEnabled(
            operationState_.canImportDerivative
            && !operationState_.derivativeRunning);
        auto status = juceString(operationState_.status);
        if (operationState_.derivativeRunning
            || operationState_.previewPreparing)
        {
            status += juceString(" · ")
                + juce::String(operationState_.progressPermille / 10)
                + "%";
        }
        operationStatus_.setText(status, juce::dontSendNotification);
        operationStatus_.setTitle(
            status.isNotEmpty()
                ? "Partial-track operation: " + status
                : "Partial-track operation idle");
    }

    [[nodiscard]] juce::Rectangle<int> plotBounds() const
    {
        return viewport_.getLocalBounds()
            .withTrimmedTop(workbenchScaledUiPixels(22))
            .withTrimmedLeft(workbenchScaledUiPixels(82))
            .reduced(0, workbenchScaledUiPixels(8));
    }

    [[nodiscard]] const talas::app::PartialTrackEditorPointGeometry*
    selectedPoint() const noexcept
    {
        const auto found = std::ranges::find_if(
            projection_.points,
            [](const auto& point) { return point.selected; });
        return found == projection_.points.end() ? nullptr : &*found;
    }

    [[nodiscard]] const talas::app::PartialTrackEditorTrackRow*
    selectedTrack() const noexcept
    {
        const auto found = std::ranges::find_if(
            projection_.rows,
            [](const auto& row) { return row.selected; });
        return found == projection_.rows.end() ? nullptr : &*found;
    }

    [[nodiscard]] talas::project::ProjectPartialTrackEditBatch
    makeBatch() const
    {
        talas::project::ProjectPartialTrackEditBatch batch;
        if (!manifest_.has_value())
            return batch;
        batch.editSetId = currentEdits_.has_value()
            ? currentEdits_->editSetId
            : defaultEditSetId(manifest_->id);
        batch.assetId = manifest_->id;
        batch.assetGeneration = manifest_->generation;
        batch.sourceContentHash = manifest_->contentHash;
        return batch;
    }

    [[nodiscard]] bool applyBatch(
        talas::project::ProjectPartialTrackEditBatch batch,
        talas::project::ProjectEditor::HistoryTransactionToken token,
        juce::String successMessage)
    {
        if (sourceAsset_ == nullptr || !hooks_.apply)
            return false;
        const auto result = hooks_.apply(
            *sourceAsset_, std::move(batch), token);
        if (!result.ok())
        {
            if (hooks_.setStatus)
                hooks_.setStatus(juceString(result.message));
            if (token != 0)
                cancelGesture();
            return false;
        }
        if (hooks_.documentChanged)
            hooks_.documentChanged();
        if (hooks_.setStatus)
            hooks_.setStatus(successMessage);
        return true;
    }

    [[nodiscard]] bool updatePoint(
        const talas::app::PartialTrackEditorPointGeometry& point,
        std::uint64_t frame,
        double frequencyHz,
        double amplitude,
        double bandwidthHz,
        std::uint64_t trackId,
        talas::project::PartialTrackPointInterpolation interpolation,
        talas::project::ProjectEditor::HistoryTransactionToken token,
        juce::String status)
    {
        auto batch = makeBatch();
        batch.pointMutations.push_back({
            talas::project::ProjectPartialTrackPointMutationKind::Update,
            point.identity,
            point.segmentId,
            frame,
            {trackId, amplitude, frequencyHz, bandwidthHz},
            interpolation,
        });
        return applyBatch(std::move(batch), token, std::move(status));
    }

    void beginGesture()
    {
        if (gestureToken_ == 0 && hooks_.beginGesture)
            gestureToken_ = hooks_.beginGesture();
    }

    void endGesture()
    {
        if (gestureToken_ != 0 && hooks_.endGesture)
            hooks_.endGesture(gestureToken_);
        gestureToken_ = 0;
        dragPoint_.reset();
    }

    void cancelGesture()
    {
        if (gestureToken_ != 0 && hooks_.cancelGesture)
            hooks_.cancelGesture(gestureToken_);
        gestureToken_ = 0;
        dragPoint_.reset();
        pendingSnap_.reset();
    }

    [[nodiscard]] bool deleteSelection()
    {
        const auto* point = selectedPoint();
        if (point == nullptr)
            return false;
        auto batch = makeBatch();
        batch.pointMutations.push_back({
            talas::project::ProjectPartialTrackPointMutationKind::Delete,
            point->identity,
        });
        return applyBatch(
            std::move(batch), 0,
            juceString("Deleted exact partial-track point · source bytes unchanged"));
    }

    [[nodiscard]] bool setInterpolation(
        talas::project::PartialTrackPointInterpolation interpolation)
    {
        const auto* point = selectedPoint();
        if (point == nullptr || point->interpolation == interpolation)
            return false;
        return updatePoint(
            *point,
            point->sourceFrame,
            point->frequencyHz,
            point->amplitude,
            point->bandwidthHz,
            point->trackId,
            interpolation,
            0,
            interpolation
                    == talas::project::PartialTrackPointInterpolation::Step
                ? "Set exact point interpolation to Step"
                : "Set exact point interpolation to Linear");
    }

    [[nodiscard]] bool toggleTrackState(bool solo)
    {
        const auto* row = selectedTrack();
        if (row == nullptr)
            return false;
        auto batch = makeBatch();
        batch.trackStateMutations.push_back({
            row->trackId,
            solo ? false : !row->muted,
            solo ? !row->solo : false,
        });
        return applyBatch(
            std::move(batch), 0,
            solo ? "Updated exact partial-track solo state"
                 : "Updated exact partial-track mute state");
    }

    [[nodiscard]] std::optional<std::uint64_t> nextTrackId() const
    {
        std::uint64_t maximum = 0;
        if (sourceAsset_ != nullptr)
        {
            for (const auto& frame : sourceAsset_->frames)
            {
                for (const auto& point : frame.points)
                    maximum = std::max(maximum, point.trackId);
            }
        }
        if (currentEdits_.has_value())
        {
            for (const auto& point : currentEdits_->pointOverrides)
            {
                if (!point.excluded)
                    maximum = std::max(maximum, point.output.trackId);
            }
            for (const auto& point : currentEdits_->insertedPoints)
                maximum = std::max(maximum, point.point.trackId);
        }
        if (maximum == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        return maximum + 1;
    }

    [[nodiscard]] bool retrackSelectedPoint()
    {
        const auto* point = selectedPoint();
        const auto target = nextTrackId();
        if (point == nullptr || !target.has_value())
            return false;
        return updatePoint(
            *point,
            point->sourceFrame,
            point->frequencyHz,
            point->amplitude,
            point->bandwidthHz,
            *target,
            point->interpolation,
            0,
            "Retracked exact point to a new stable partial track");
    }

    [[nodiscard]] bool transformSelectedVisibleRegion(
        std::int64_t frameDelta,
        double frequencyScale,
        std::optional<std::uint64_t> targetTrack,
        juce::String status,
        talas::project::ProjectEditor::HistoryTransactionToken token = 0)
    {
        const auto reject = [&](const juce::String& message) {
            if (hooks_.setStatus)
                hooks_.setStatus(message);
            if (token != 0)
                cancelGesture();
            return false;
        };
        const auto* row = selectedTrack();
        if (row == nullptr)
            return reject(
                "Visible partial-track region target is stale or offscreen");
        std::vector<talas::app::PartialTrackEditorPointGeometry> region;
        region.reserve(std::min<std::size_t>(
            projection_.points.size(),
            talas::project::maximumProjectPartialTrackBatchPointMutations));
        for (const auto& point : projection_.points)
        {
            if (point.trackId == row->trackId)
                region.push_back(point);
        }
        if (region.empty()
            || region.size()
                > talas::project::maximumProjectPartialTrackBatchPointMutations)
        {
            return reject(
                "Visible partial-track region is empty or exceeds the fixed edit limit");
        }
        auto batch = makeBatch();
        batch.pointMutations.reserve(region.size());
        for (const auto& point : region)
        {
            if ((frameDelta < 0
                    && point.sourceFrame
                        < static_cast<std::uint64_t>(-frameDelta))
                || (frameDelta > 0
                    && static_cast<std::uint64_t>(frameDelta)
                        > sourceFrameCount_ - 1 - point.sourceFrame))
            {
                return reject(
                    "Visible region transform would leave the exact source extent");
            }
            const auto frame = frameDelta < 0
                ? point.sourceFrame
                    - static_cast<std::uint64_t>(-frameDelta)
                : point.sourceFrame
                    + static_cast<std::uint64_t>(frameDelta);
            const auto frequency = point.frequencyHz * frequencyScale;
            if (!std::isfinite(frequency)
                || frequency <= 0.0
                || frequency > sampleRate_ * 0.5)
            {
                return reject(
                    "Visible region transform would leave the frequency domain");
            }
            batch.pointMutations.push_back({
                talas::project::ProjectPartialTrackPointMutationKind::Update,
                point.identity,
                point.segmentId,
                frame,
                {targetTrack.value_or(point.trackId),
                 point.amplitude,
                 frequency,
                 point.bandwidthHz},
                point.interpolation,
            });
        }
        const auto movedWholeTrack = region.size() == row->pointCount;
        if (targetTrack.has_value()
            && movedWholeTrack
            && currentEdits_.has_value())
        {
            const auto existingState = std::ranges::find(
                currentEdits_->trackStates,
                row->trackId,
                &talas::project::ProjectPartialTrackState::trackId);
            if (existingState != currentEdits_->trackStates.end())
            {
                if (std::ranges::none_of(
                        currentEdits_->trackStates,
                        [&](const auto& state) {
                            return state.trackId == *targetTrack;
                        }))
                {
                    batch.trackStateMutations.push_back({
                        *targetTrack,
                        existingState->muted,
                        existingState->solo,
                    });
                }
                batch.trackStateMutations.push_back({row->trackId, false, false});
            }
        }
        return applyBatch(
            std::move(batch), token, std::move(status));
    }

    [[nodiscard]] bool splitSelectedVisibleRegion()
    {
        const auto target = nextTrackId();
        return target.has_value()
            && transformSelectedVisibleRegion(
                0,
                1.0,
                *target,
                "Split visible region to a new stable partial track");
    }

    [[nodiscard]] bool mergeSelectedVisibleRegion()
    {
        const auto* row = selectedTrack();
        if (row == nullptr)
            return false;
        const auto found = std::ranges::find(
            projection_.rows,
            row->trackId,
            &talas::app::PartialTrackEditorTrackRow::trackId);
        if (found == projection_.rows.begin()
            || found == projection_.rows.end())
        {
            return false;
        }
        return transformSelectedVisibleRegion(
            0,
            1.0,
            std::prev(found)->trackId,
            "Merged visible region into the preceding stable partial track");
    }

    [[nodiscard]] std::uint64_t frameAtX(int x) const
    {
        const auto plot = plotBounds();
        const auto range = horizontal_.getCurrentRange();
        const auto fraction = static_cast<double>(x - plot.getX()) / static_cast<double>(std::max(1, plot.getWidth()));
        const auto frame = range.getStart() + std::clamp(fraction, 0.0, 1.0) * range.getLength();
        return std::min(static_cast<std::uint64_t>(std::max(0.0, std::round(frame))),
            sourceFrameCount_ > 0 ? sourceFrameCount_ - 1 : 0);
    }

    [[nodiscard]] int xAtFrame(std::uint64_t frame) const
    {
        const auto plot = plotBounds();
        const auto range = horizontal_.getCurrentRange();
        return plot.getX()
            + static_cast<int>(std::lround((static_cast<double>(frame) - range.getStart())
                * static_cast<double>(plot.getWidth()) / std::max(1.0, range.getLength())));
    }

    // The ruler: a marker near the click is selected and the scrub bar moves
    // to it; elsewhere the scrub bar moves there.
    void pressRuler(int x)
    {
        selectedMarker_.reset();
        for (std::size_t index = 0; index < markers_.markers.size(); ++index)
        {
            if (std::abs(xAtFrame(markers_.markers[index].frame) - x) <= workbenchScaledUiPixels(4))
                selectedMarker_ = index;
        }
        if (selectedMarker_.has_value())
        {
            const auto& marker = markers_.markers[*selectedMarker_];
            scrubFrame_ = marker.frame;
            readout_ = juceString(marker.name) + " · " + juce::String(static_cast<double>(marker.frame) / sampleRate_, 3) + " s";
        }
        else
        {
            scrubbing_ = true;
            moveScrubBar(x);
        }
        viewport_.grabKeyboardFocus();
        viewport_.repaint();
    }

    std::optional<std::uint64_t> scrubForPitchBend(int bend) override
    {
        if (sourceFrameCount_ == 0)
            return std::nullopt;
        scrubFrame_ = talas::app::partialTrackScrubForPitchBend(bend, sourceFrameCount_);
        viewport_.repaint();
        auditionAt(scrubFrame_, false);
        return scrubFrame_;
    }

    void moveScrubBar(int x)
    {
        scrubFrame_ = frameAtX(x);
        viewport_.repaint();
        auditionAt(scrubFrame_, false);
    }

    void beginBox(juce::Point<int> position, bool shift, bool zoom = false)
    {
        boxStart_ = position;
        boxEnd_ = position;
        boxShift_ = shift;
        boxZooms_ = zoom;
    }

    void dragBox(juce::Point<int> position)
    {
        boxEnd_ = position;
        viewport_.repaint();
    }

    // The points within the box drawn, in display coordinates: begun inside
    // the selection's extent, Shift cuts a hole.
    bool selectBoxPixels(juce::Rectangle<int> box, juce::Point<int> start, bool shift)
    {
        const auto plot = plotBounds();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> inBox;
        juce::Rectangle<int> selectedExtent;
        bool anySelected = false;
        for (const auto& point : projection_.points)
        {
            const juce::Point<int> at{plot.getX() + static_cast<int>(std::lround(point.x)),
                plot.getY() + static_cast<int>(std::lround(point.y))};
            if (box.contains(at))
                inBox.emplace_back(point.trackId, point.sourceFrame);
            if (regionSelection_.contains(point.trackId, point.sourceFrame))
            {
                selectedExtent = anySelected ? selectedExtent.getUnion({at, at.translated(1, 1)})
                                             : juce::Rectangle<int>{at, at.translated(1, 1)};
                anySelected = true;
            }
        }
        regionSelection_.selectPoints(inBox, shift, shift && anySelected && selectedExtent.contains(start));
        playNewSelection();
        if (hooks_.setStatus)
        {
            hooks_.setStatus("Selected " + juce::String(static_cast<int>(regionSelection_.points().size())) + " points of "
                + juce::String(static_cast<int>(regionSelection_.trackIds().size())) + " tracks");
        }
        viewport_.repaint();
        return !inBox.empty();
    }

    void finishBox()
    {
        const auto start = *boxStart_;
        const auto box = juce::Rectangle<int>{start, boxEnd_.value_or(start)};
        boxStart_.reset();
        boxEnd_.reset();
        if (box.getWidth() < 3 && box.getHeight() < 3)
        {
            if (!boxShift_)
                regionSelection_.clear();
            viewport_.repaint();
            return;
        }
        // Drawn with Control or Command, the box is magnified to fill the
        // display.
        if (boxZooms_)
        {
            zoomToBoxPixels(box);
            return;
        }
        (void) selectBoxPixels(box, start, boxShift_);
    }

    [[nodiscard]] talas::app::PartialTrackEditorViewport displayViewport() const
    {
        const auto range = horizontal_.getCurrentRange();
        talas::app::PartialTrackEditorViewport viewport;
        viewport.startFrame = static_cast<std::uint64_t>(std::max(0.0, std::floor(range.getStart())));
        viewport.endFrame = std::max(viewport.startFrame + 1, static_cast<std::uint64_t>(std::ceil(range.getEnd())));
        viewport.minimumFrequencyHz = zoomLowHz_.value_or(minimumFrequencyHz_);
        viewport.maximumFrequencyHz = zoomHighHz_.value_or(maximumFrequencyHz_);
        return viewport;
    }

    void applyDisplayViewport(const talas::app::PartialTrackEditorViewport& viewport)
    {
        horizontal_.setCurrentRange(static_cast<double>(viewport.startFrame),
            static_cast<double>(viewport.endFrame - viewport.startFrame), juce::dontSendNotification);
        zoomLowHz_ = viewport.minimumFrequencyHz;
        zoomHighHz_ = viewport.maximumFrequencyHz;
        refreshProjection();
        viewport_.repaint();
    }

    [[nodiscard]] double frequencyAtY(int y) const
    {
        const auto plot = plotBounds();
        const auto view = displayViewport();
        const auto fraction = std::clamp(static_cast<double>(y - plot.getY()) / std::max(1, plot.getHeight()), 0.0, 1.0);
        return view.maximumFrequencyHz - fraction * (view.maximumFrequencyHz - view.minimumFrequencyHz);
    }

    void zoomToBoxPixels(juce::Rectangle<int> box)
    {
        applyDisplayViewport(talas::app::partialTrackZoomedToBox(displayViewport(), frameAtX(box.getX()), frameAtX(box.getRight()),
            frequencyAtY(box.getBottom()), frequencyAtY(box.getY())));
    }

    // The point under the mouse: track, time, dB with the raw value, and
    // frequency.
    void updateReadout(juce::Point<int> position)
    {
        const auto plot = plotBounds();
        const auto radius = static_cast<double>(workbenchScaledUiPixels(8));
        juce::String next;
        double nearest = radius * radius;
        for (const auto& point : projection_.points)
        {
            const auto dx = static_cast<double>(position.x - plot.getX()) - point.x;
            const auto dy = static_cast<double>(position.y - plot.getY()) - point.y;
            if (dx * dx + dy * dy <= nearest)
            {
                nearest = dx * dx + dy * dy;
                next = juceString(talas::app::partialTrackReadout(
                    {point.trackId, point.amplitude, point.frequencyHz, point.bandwidthHz}, point.sourceFrame, sampleRate_));
            }
        }
        if (next != readout_)
        {
            readout_ = next;
            viewport_.repaint();
        }
    }

    // A number key selects tracks 1 to 10, with Shift adding or removing one
    //.
    bool selectTrackByNumber(char key, bool shift)
    {
        const auto track = talas::app::partialTrackForNumberKey(key);
        const auto materialized = materializedEdits();
        if (!track.has_value() || !materialized.has_value())
            return false;
        regionSelection_.selectTrack(*materialized, *track, shift);
        if (std::ranges::any_of(projection_.rows, [&](const auto& row) { return row.trackId == *track; }))
            select({talas::app::PartialTrackEditorItemKind::Track, *track, {}});
        playNewSelection();
        viewport_.repaint();
        return true;
    }

    bool copyRegionTo(const std::filesystem::path& destination)
    {
        const auto materialized = materializedEdits();
        if (!materialized.has_value() || !hooks_.copyRegion)
            return false;
        const auto region = talas::app::partialTrackRegionAsset(materialized->asset, regionSelection_);
        return region.has_value() && hooks_.copyRegion(*region, destination);
    }

    // Asks for the new spectrum file's name.
    void promptCopyRegion()
    {
        copyChooser_ = std::make_unique<juce::FileChooser>("Copy the selection to a new spectrum file",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("region.talaspartial"),
            juce::String{std::string{talas::project::spectrumFilePatterns}});
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        copyChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void) safeThis->copyRegionTo(std::filesystem::path{chooser.getResult().getFullPathName().toStdString()});
            });
    }

    // Save as...: the
    // spectrum as edited, cleared tracks and cut frames gone for good.
    bool saveAsTo(const std::filesystem::path& destination)
    {
        const auto materialized = materializedEdits();
        // The markers go into the saved file's header.
        return materialized.has_value() && hooks_.saveAs
            && hooks_.saveAs(talas::app::partialTrackAssetWithMarkers(talas::app::partialTrackAssetSaved(*materialized), markers_), destination);
    }

    bool promptSaveAs() override
    {
        if (!materializedEdits().has_value())
            return false;
        // Saved as a .spc spectrum file or a .talaspartial one, as the
        // name ends.
        copyChooser_ = std::make_unique<juce::FileChooser>("Save the spectrum as a new file",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("spectrum.talaspartial"),
            juce::String{std::string{talas::project::spectrumFilePatterns}});
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        copyChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void) safeThis->saveAsTo(std::filesystem::path{chooser.getResult().getFullPathName().toStdString()});
            });
        return true;
    }

    [[nodiscard]] bool saveAsForSmoke(const std::filesystem::path& destination) override { return saveAsTo(destination); }

    [[nodiscard]] bool copyRegionForSmoke(const std::filesystem::path& destination) override
    {
        const auto plot = plotBounds();
        if (!selectBoxPixels(plot, plot.getTopLeft(), false))
            return false;
        const auto copied = copyRegionTo(destination);
        regionSelection_.clear();
        return copied;
    }

    // Drawing mode: the pen
    // redraws the selected track's amplitude or frequency envelope, the
    // axis shown, at each frame it passes; one undo takes the stroke back,
    // and the track plays once it is drawn.
    [[nodiscard]] std::optional<std::uint64_t> drawnTrack() const
    {
        const auto tracks = regionSelection_.trackIds();
        if (tracks.size() == 1)
            return tracks.front();
        if (const auto* row = selectedTrack())
            return row->trackId;
        return std::nullopt;
    }

    void beginDrawing(juce::Point<int> position)
    {
        if (!drawnTrack().has_value())
        {
            if (hooks_.setStatus)
                hooks_.setStatus("Select the one track whose envelope to draw");
            return;
        }
        cancelGesture();
        beginGesture();
        drawing_ = true;
        drawAt(position);
    }

    bool drawAt(juce::Point<int> position)
    {
        const auto track = drawnTrack();
        const auto plot = plotBounds();
        if (!drawing_ || !track.has_value() || plot.isEmpty())
            return false;
        const auto x = static_cast<double>(position.x - plot.getX());
        const auto point = std::ranges::min_element(projection_.points, {}, [&](const auto& candidate) {
            return candidate.trackId == *track ? std::abs(candidate.x - x) : std::numeric_limits<double>::infinity();
        });
        if (point == projection_.points.end() || point->trackId != *track)
            return false;
        const auto heightRatio = static_cast<double>(position.y - plot.getY()) / static_cast<double>(plot.getHeight());
        const auto value = talas::app::partialTrackDrawnValue(axis_, heightRatio,
            zoomLowHz_.value_or(minimumFrequencyHz_), zoomHighHz_.value_or(maximumFrequencyHz_));
        const auto amplitudeAxis = axis_ == talas::app::PartialTrackVerticalAxis::Amplitude;
        const auto drawnPoint = *point;
        const auto drawn = updatePoint(drawnPoint, drawnPoint.sourceFrame, amplitudeAxis ? drawnPoint.frequencyHz : value,
            amplitudeAxis ? value : drawnPoint.amplitude, drawnPoint.bandwidthHz, drawnPoint.trackId,
            drawnPoint.interpolation, gestureToken_, "Drew the envelope");
        // Drawing, you hear what you draw.
        if (drawn)
            auditionAt(drawnPoint.sourceFrame, true);
        return drawn;
    }

    void finishDrawing()
    {
        drawing_ = false;
        endGesture();
        playNewSelection();
    }

    [[nodiscard]] std::optional<double> drawForSmoke(double heightRatio) override
    {
        const auto plot = plotBounds();
        if (projection_.points.empty() || plot.isEmpty())
            return std::nullopt;
        const auto first = projection_.points.front();
        const auto materialized = materializedEdits();
        if (!materialized.has_value())
            return std::nullopt;
        regionSelection_.selectTrack(*materialized, first.trackId, false);
        const auto wasDrawing = drawMode_;
        performSpectrumCommand(45);
        const auto position = juce::Point<int>{plot.getX() + static_cast<int>(std::lround(first.x)),
            plot.getY() + static_cast<int>(std::lround(heightRatio * plot.getHeight()))};
        beginDrawing(position);
        finishDrawing();
        performSpectrumCommand(45);
        if (drawMode_ != wasDrawing)
            return std::nullopt;
        const auto drawn = std::ranges::find_if(projection_.points, [&](const auto& point) {
            return point.trackId == first.trackId && point.sourceFrame == first.sourceFrame;
        });
        if (drawn == projection_.points.end())
            return std::nullopt;
        return axis_ == talas::app::PartialTrackVerticalAxis::Amplitude ? drawn->amplitude : drawn->frequencyHz;
    }

    // A selection made is played at once, so you hear what you selected
    //.
    void playNewSelection()
    {
        if (!regionSelection_.empty())
            playRegion(talas::app::PartialTrackPlay::Selection);
    }

    bool selectAdjacentTrack(bool up)
    {
        const auto materialized = materializedEdits();
        if (!materialized.has_value())
            return false;
        std::vector<std::uint64_t> trackIds;
        for (const auto& row : projection_.rows)
            trackIds.push_back(row.trackId);
        const auto selected = regionSelection_.trackIds();
        const auto next = talas::app::partialTrackAdjacent(trackIds,
            selected.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{selected.front()}, up);
        if (!next.has_value())
            return false;
        regionSelection_.selectTrack(*materialized, *next, false);
        select({talas::app::PartialTrackEditorItemKind::Track, *next, {}});
        playNewSelection();
        viewport_.repaint();
        return true;
    }

    void addMarkerAtScrubBar()
    {
        if (sourceAsset_ == nullptr)
            return;
        markers_.markers.push_back({talas::app::defaultPartialTrackMarkerName(*sourceAsset_, scrubFrame_), scrubFrame_});
        selectedMarker_ = markers_.markers.size() - 1;
        viewport_.repaint();
    }

    void renameSelectedMarker()
    {
        if (!selectedMarker_.has_value() || *selectedMarker_ >= markers_.markers.size())
            return;
        auto* window = new juce::AlertWindow("Marker", {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", juceString(markers_.markers[*selectedMarker_].name), "Name");
        window->addButton("Rename", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        const auto index = *selectedMarker_;
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window, index](int result) {
                const auto name = window->getTextEditorContents("name").trim();
                if (safeThis == nullptr || result != 1 || name.isEmpty() || index >= safeThis->markers_.markers.size())
                    return;
                safeThis->markers_.markers[index].name = name.toStdString();
                safeThis->viewport_.repaint();
            }),
            true);
    }

    void promptPrePostRoll()
    {
        auto* window = new juce::AlertWindow("Pre- and post-roll", {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("pre", juce::String(markers_.preRollSeconds), "Pre-roll (s)");
        window->addTextEditor("post", juce::String(markers_.postRollSeconds), "Post-roll (s)");
        window->addButton("Set", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                const auto pre = window->getTextEditorContents("pre").getDoubleValue();
                const auto post = window->getTextEditorContents("post").getDoubleValue();
                if (safeThis == nullptr || result != 1)
                    return;
                safeThis->markers_.preRollSeconds = std::max(0.0, pre);
                safeThis->markers_.postRollSeconds = std::max(0.0, post);
            }),
            true);
    }

    // The scissors ask to verify the cut first.
    void confirmCutBetweenLoopMarkers()
    {
        if (!markers_.loopStart.has_value() || !markers_.loopEnd.has_value() || *markers_.loopStart >= *markers_.loopEnd)
        {
            (void) cutBetweenLoopMarkers();
            return;
        }
        juce::Component::SafePointer<juce::Component> safeThis(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Cut",
            juce::String{talas::app::cutPartialTrackFramesQuestion(*markers_.loopStart, *markers_.loopEnd)}, "Delete", "Cancel",
            this, juce::ModalCallbackFunction::create([this, safeThis](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) cutBetweenLoopMarkers();
            }));
    }

    // The scissors: the frames from the loop start up to the loop end go
    //.
    [[nodiscard]] bool cutBetweenLoopMarkers()
    {
        const auto materialized = materializedEdits();
        if (!materialized.has_value() || !markers_.loopStart.has_value() || !markers_.loopEnd.has_value()
            || *markers_.loopStart >= *markers_.loopEnd)
        {
            if (hooks_.setStatus)
                hooks_.setStatus("Place a loop start before a loop end to cut between them");
            return false;
        }
        auto batch = makeBatch();
        batch.pointMutations = talas::app::cutPartialTrackFrames(*materialized, *markers_.loopStart, *markers_.loopEnd);
        if (batch.pointMutations.empty()
            || batch.pointMutations.size() > talas::project::maximumProjectPartialTrackBatchPointMutations)
        {
            if (hooks_.setStatus)
                hooks_.setStatus("Nothing to cut between the loop markers, or more points than one edit may change");
            return false;
        }
        const auto length = *markers_.loopEnd - *markers_.loopStart;
        if (!applyBatch(std::move(batch), 0, "Cut the frames between the loop markers"))
            return false;
        markers_.loopEnd = markers_.loopStart;
        for (auto& marker : markers_.markers)
        {
            if (marker.frame >= *markers_.loopStart + length)
                marker.frame -= length;
        }
        regionSelection_.clear();
        return true;
    }

    void playRegion(talas::app::PartialTrackPlay play)
    {
        playRequest_ = play;
        scrubRequest_.reset();
        if (hooks_.startPreview)
            hooks_.startPreview();
    }

    // The scrub bar moved, or a stroke drawn:
    // a fifth of a second from there, at the natural rate, at most five
    // times a second; of the tracks the play button used last chose, or,
    // drawing, of the track drawn.
    void auditionAt(std::uint64_t frame, bool drawn)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (!hooks_.startPreview || sourceAsset_ == nullptr || now - lastAudition_ < 200)
            return;
        lastAudition_ = now;
        scrubRequest_ = PartialTrackEditorSourceSnapshot::Scrub{frame,
            static_cast<std::uint64_t>(std::max(1.0, sourceAsset_->sampleRate * 0.2)),
            drawn || playRequest_ == talas::app::PartialTrackPlay::Selection ? talas::app::PartialTrackPlay::Selection
                                                                          : talas::app::PartialTrackPlay::All};
        hooks_.startPreview();
        scrubRequest_.reset();
    }

    [[nodiscard]] static juce::PopupMenu criteriaMenu()
    {
        juce::PopupMenu criteria;
        criteria.addItem(20, "Amplitudes within a range...");
        criteria.addItem(21, "Above amplitude threshold...");
        criteria.addItem(22, "Complement selection");
        criteria.addItem(23, "Even tracks");
        criteria.addItem(24, "Odd tracks");
        criteria.addItem(25, "Octave harmonics");
        criteria.addItem(26, "Fibonacci tracks");
        criteria.addItem(27, "Prime numbered tracks");
        return criteria;
    }

    void pressSpectrumButton(talas::app::SpectrumEditorButton button)
    {
        using Button = talas::app::SpectrumEditorButton;
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        const auto at = [this](Button which) {
            const auto buttons = talas::app::spectrumEditorButtons();
            const auto found = std::ranges::find(buttons, which, &talas::app::SpectrumEditorButtonInfo::button);
            return found == buttons.end() ? nullptr : stripButtons_[static_cast<int>(found - buttons.begin())];
        };
        switch (button)
        {
        case Button::PlayAll: performSpectrumCommand(1); break;
        case Button::PlaySelection: performSpectrumCommand(2); break;
        case Button::PlayBetweenLoopMarkers: performSpectrumCommand(3); break;
        case Button::PlayOutsideLoopMarkers: performSpectrumCommand(4); break;
        case Button::Marker: performSpectrumCommand(10); break;
        case Button::LoopStart: performSpectrumCommand(11); break;
        case Button::LoopEnd: performSpectrumCommand(12); break;
        case Button::PrePostRoll: performSpectrumCommand(13); break;
        case Button::Cut: performSpectrumCommand(14); break;
        case Button::Draw: performSpectrumCommand(45); break;
        case Button::SelectionCriteria:
            criteriaMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(at(button)), [safeThis](int choice) {
                if (safeThis != nullptr && choice != 0)
                    safeThis->performSpectrumCommand(choice);
            });
            break;
        case Button::TrackFilters:
        {
            juce::PopupMenu filters;
            addTrackFilterItems(filters);
            filters.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(at(button)), [safeThis](int choice) {
                if (safeThis != nullptr && choice >= 30 && choice <= 39)
                    safeThis->promptTrackFilter(static_cast<talas::app::PartialTrackFilter>(choice - 30));
            });
            break;
        }
        }
    }

    void showSpectrumMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Play all");
        menu.addItem(2, "Play selection", !regionSelection_.empty());
        const auto looped = markers_.loopStart.has_value() && markers_.loopEnd.has_value()
            && *markers_.loopStart < *markers_.loopEnd;
        menu.addItem(3, "Play between loop markers", looped);
        if (fourthPlaysToStart_)
            menu.addItem(4, "Play up to the start marker", markers_.loopStart.has_value());
        else
            menu.addItem(4, "Play outside loop markers", looped);
        // The fourth button plays up to the start marker, or outside the loop
        // markers.
        menu.addItem(46, "Fourth button plays up to the start marker", true, fourthPlaysToStart_);
        menu.addSeparator();
        menu.addItem(10, "Marker at scrub bar");
        menu.addItem(11, "Loop start at scrub bar");
        menu.addItem(12, "Loop end at scrub bar");
        menu.addItem(13, "Pre- and post-roll...");
        menu.addItem(14, "Cut between loop markers", looped);
        menu.addItem(15, "Rename marker...", selectedMarker_.has_value());
        menu.addItem(16, "Delete marker", selectedMarker_.has_value());
        menu.addItem(17, "Save as...");
        menu.addSeparator();
        menu.addSubMenu("Select by", criteriaMenu());
        menu.addItem(28, "Clear selection", !regionSelection_.empty());
        menu.addSeparator();
        // The display.
        menu.addItem(40, "Amplitude on the vertical axis", true, axis_ == talas::app::PartialTrackVerticalAxis::Amplitude);
        menu.addItem(45, "Draw (pencil)", true, drawMode_);
        menu.addItem(41, "Join the dots", true, joinDots_);
        menu.addItem(42, "Zoom in (Ctrl+])");
        menu.addItem(43, "Zoom out (Ctrl+[)");
        menu.addItem(44, "Full display");
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&spectrumMenu_), [safeThis](int choice) {
            if (safeThis != nullptr)
                safeThis->performSpectrumCommand(choice);
        });
    }

    void performSpectrumCommand(int choice)
    {
        using Criterion = talas::app::PartialTrackSelectionCriterion;
        const auto select = [this](Criterion criterion, int low = 0, int high = 127) {
            if (const auto materialized = materializedEdits())
                regionSelection_ = talas::app::selectPartialTracksBy(*materialized, criterion, low, high);
            viewport_.repaint();
        };
        const auto askRaw = [this](juce::String title, bool range, std::function<void(int, int)> chosen) {
            auto* window = new juce::AlertWindow(title, "Raw amplitudes run from 0 to 127", juce::MessageBoxIconType::NoIcon, this);
            window->addTextEditor("low", range ? "0" : "64", range ? "From" : "Above");
            if (range)
                window->addTextEditor("high", "127", "To");
            window->addButton("Select", 1, juce::KeyPress{juce::KeyPress::returnKey});
            window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
            juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
            window->enterModalState(
                true,
                juce::ModalCallbackFunction::create([safeThis, window, range, chosen = std::move(chosen)](int result) {
                    const auto low = window->getTextEditorContents("low").getIntValue();
                    const auto high = range ? window->getTextEditorContents("high").getIntValue() : 127;
                    if (safeThis != nullptr && result == 1)
                        chosen(low, high);
                }),
                true);
        };
        switch (choice)
        {
        case 1: playRegion(talas::app::PartialTrackPlay::All); break;
        case 40:
            axis_ = axis_ == talas::app::PartialTrackVerticalAxis::Amplitude ? talas::app::PartialTrackVerticalAxis::Frequency
                                                                          : talas::app::PartialTrackVerticalAxis::Amplitude;
            viewport_.repaint();
            break;
        case 45:
            drawMode_ = !drawMode_;
            if (hooks_.setStatus)
                hooks_.setStatus(drawMode_ ? "Drawing: drag over the selected track to redraw its envelope; Draw again to stop"
                                           : "Drawing off");
            break;
        case 41:
            joinDots_ = !joinDots_;
            viewport_.repaint();
            break;
        case 42: applyDisplayViewport(talas::app::partialTrackZoomedIn(displayViewport())); break;
        case 43:
            applyDisplayViewport(
                talas::app::partialTrackZoomedOut(displayViewport(), sourceFrameCount_, minimumFrequencyHz_, maximumFrequencyHz_));
            break;
        case 44:
            applyDisplayViewport(
                talas::app::partialTrackFullDisplay(displayViewport(), sourceFrameCount_, minimumFrequencyHz_, maximumFrequencyHz_));
            break;
        case 2: playRegion(talas::app::PartialTrackPlay::Selection); break;
        case 3: playRegion(talas::app::PartialTrackPlay::BetweenLoopMarkers); break;
        case 4: playRegion(talas::app::spectrumEditorFourthButtonPlay(fourthPlaysToStart_)); break;
        case 46:
        {
            fourthPlaysToStart_ = !fourthPlaysToStart_;
            const auto buttons = talas::app::spectrumEditorButtons();
            const auto fourth = std::ranges::find(
                buttons, talas::app::SpectrumEditorButton::PlayOutsideLoopMarkers, &talas::app::SpectrumEditorButtonInfo::button);
            const auto index = static_cast<int>(fourth - buttons.begin());
            if (fourth != buttons.end() && index < stripButtons_.size())
            {
                const auto label = talas::app::spectrumEditorFourthButtonLabel(fourthPlaysToStart_);
                stripButtons_[index]->setButtonText(juce::String{label.data(), label.size()});
            }
            break;
        }
        case 10: addMarkerAtScrubBar(); break;
        case 11: markers_.loopStart = scrubFrame_; viewport_.repaint(); break;
        case 12: markers_.loopEnd = scrubFrame_; viewport_.repaint(); break;
        case 13: promptPrePostRoll(); break;
        case 14: confirmCutBetweenLoopMarkers(); break;
        case 15: renameSelectedMarker(); break;
        case 17: (void) promptSaveAs(); break;
        case 16:
            if (selectedMarker_.has_value() && *selectedMarker_ < markers_.markers.size())
                markers_.markers.erase(markers_.markers.begin() + static_cast<std::ptrdiff_t>(*selectedMarker_));
            selectedMarker_.reset();
            viewport_.repaint();
            break;
        case 20:
            askRaw("Amplitudes within a range", true, [select](int low, int high) { select(Criterion::AmplitudesWithinRange, low, high); });
            break;
        case 21:
            askRaw("Above amplitude threshold", false, [select](int low, int) { select(Criterion::AboveAmplitudeThreshold, low); });
            break;
        case 22:
            if (const auto materialized = materializedEdits())
                regionSelection_.selectComplement(*materialized);
            viewport_.repaint();
            break;
        case 23: select(Criterion::EvenTracks); break;
        case 24: select(Criterion::OddTracks); break;
        case 25: select(Criterion::OctaveHarmonics); break;
        case 26: select(Criterion::FibonacciTracks); break;
        case 27: select(Criterion::PrimeTracks); break;
        case 28: regionSelection_.clear(); viewport_.repaint(); break;
        default: break;
        }
    }

    [[nodiscard]] std::optional<talas::project::MaterializedPartialTrackEdits>
    materializedEdits() const
    {
        if (sourceAsset_ == nullptr || !manifest_.has_value())
            return std::nullopt;
        const auto batch = makeBatch();
        auto edits = currentEdits_.value_or(talas::project::ProjectPartialTrackEditSet{
            batch.editSetId, batch.assetId, batch.assetGeneration, batch.sourceContentHash, {}, {}, {}});
        auto result = talas::project::materializeProjectPartialTrackEdits(*sourceAsset_, edits);
        if (!result.ok())
            return std::nullopt;
        return std::move(result.materialized);
    }

    // Clear zeroes the selected track and Trim every other one; they stay,
    // muted, until restored or rendered.
    [[nodiscard]] bool clearOrTrimSelectedTrack(bool trim)
    {
        const auto* row = selectedTrack();
        const auto materialized = materializedEdits();
        // A region selected, Clear zeroes part of the tracks'
        // duration, the selected points alone.
        if (!trim && materialized.has_value() && !regionSelection_.empty())
        {
            auto batch = makeBatch();
            batch.pointMutations = talas::app::clearPartialTrackPoints(*materialized, regionSelection_.points());
            if (batch.pointMutations.empty())
                return false;
            return applyBatch(std::move(batch), 0, "Cleared the selected points: their amplitude zeroed");
        }
        if (row == nullptr || !materialized.has_value())
            return false;
        const std::array tracks{row->trackId};
        auto batch = makeBatch();
        batch.trackStateMutations = trim
            ? talas::app::trimPartialTracks(*materialized, tracks)
            : talas::app::clearPartialTracks(*materialized, tracks);
        if (batch.trackStateMutations.empty())
            return false;
        return applyBatch(
            std::move(batch), 0,
            trim ? "Trimmed: every other track zeroed until restored or rendered"
                 : "Cleared the selected track until restored or rendered");
    }

    // A track filter reshapes the selected track, or with none selected
    // every track on screen, within the visible time.
    [[nodiscard]] bool applyTrackFilter(
        talas::app::PartialTrackFilter filter,
        const talas::app::PartialTrackFilterSettings& settings)
    {
        const auto materialized = materializedEdits();
        if (!materialized.has_value() || projection_.rows.empty())
            return false;
        talas::app::PartialTrackFilterRegion region;
        const auto span = regionSelection_.frameSpan();
        if (span.has_value())
        {
            region.trackIds = regionSelection_.trackIds();
            region.startFrame = span->first;
            region.endFrame = span->second;
            auto filtered = talas::app::filterPartialTracks(*materialized, region, filter, settings);
            if (!filtered.ok())
            {
                if (hooks_.setStatus)
                    hooks_.setStatus(juceString(filtered.error));
                return false;
            }
            auto batch = makeBatch();
            batch.pointMutations = std::move(filtered.mutations);
            return applyBatch(std::move(batch), 0, "Applied the track filter to the selection");
        }
        if (const auto* row = selectedTrack())
            region.trackIds = {row->trackId};
        else
        {
            for (const auto& row : projection_.rows)
                region.trackIds.push_back(row.trackId);
        }
        const auto horizontalRange = horizontal_.getCurrentRange();
        region.startFrame = static_cast<std::uint64_t>(std::max(0.0, std::floor(horizontalRange.getStart())));
        region.endFrame = static_cast<std::uint64_t>(std::max(
            static_cast<double>(region.startFrame), std::ceil(horizontalRange.getEnd()) - 1.0));
        auto filtered = talas::app::filterPartialTracks(*materialized, region, filter, settings);
        if (!filtered.ok())
        {
            if (hooks_.setStatus)
                hooks_.setStatus(juceString(filtered.error));
            return false;
        }
        auto batch = makeBatch();
        batch.pointMutations = std::move(filtered.mutations);
        return applyBatch(std::move(batch), 0, "Applied the track filter to the visible region");
    }

    // The track filters ask for what they need: a scale, a neighborhood, a
    // guide track, a deviation and its rate, or a delay.
    void promptTrackFilter(talas::app::PartialTrackFilter filter)
    {
        using Filter = talas::app::PartialTrackFilter;
        std::vector<std::pair<juce::String, double>> fields;
        switch (filter)
        {
        case Filter::ScaleAmplitudes:
            fields = {{"Scale amplitudes by", 0.5}};
            break;
        case Filter::SmoothAmplitude:
        case Filter::RemoveAmplitudeOutliers:
        case Filter::SmoothFrequency:
        case Filter::RemoveFrequencyOutliers:
            fields = {{"Neighborhood size", 3.0}};
            break;
        case Filter::ForceHarmonicFrequency:
            fields = {{"Guide track number", 1.0}};
            break;
        case Filter::AddFrequencyNoise:
            fields = {{"Deviation (Hz)", 5.0}, {"Changes per second", 10.0}};
            break;
        case Filter::Arpeggiate:
            fields = {{"Delay each track by (s)", 0.1}};
            break;
        case Filter::AverageAmplitude:
        case Filter::AverageFrequency:
            break;
        }
        if (fields.empty())
        {
            (void) applyTrackFilter(filter, {});
            return;
        }
        auto* window = new juce::AlertWindow("Track filter", {}, juce::MessageBoxIconType::NoIcon, this);
        for (std::size_t index = 0; index < fields.size(); ++index)
            window->addTextEditor("field" + juce::String(static_cast<int>(index)), juce::String(fields[index].second), fields[index].first);
        window->addButton("Apply", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window, filter](int result) {
                const auto first = window->getTextEditorContents("field0").getDoubleValue();
                const auto second = window->getTextEditorContents("field1").getDoubleValue();
                if (safeThis == nullptr || result != 1)
                    return;
                talas::app::PartialTrackFilterSettings settings;
                settings.scale = first;
                settings.neighborhood = static_cast<std::size_t>(std::max(0.0, std::round(first)));
                settings.guideTrack = static_cast<std::uint64_t>(std::max(0.0, std::round(first)));
                settings.deviationHz = first;
                settings.changesPerSecond = second;
                settings.delaySeconds = first;
                (void) safeThis->applyTrackFilter(filter, settings);
            }),
            true);
    }

    void addTrackFilterMenu(juce::PopupMenu& menu)
    {
        juce::PopupMenu filters;
        addTrackFilterItems(filters);
        menu.addSubMenu("Track filters", filters);
    }

    static void addTrackFilterItems(juce::PopupMenu& filters)
    {
        filters.addItem(30, "Scale amplitudes...");
        filters.addItem(31, "Replace amplitude with average");
        filters.addItem(32, "Smooth amplitude...");
        filters.addItem(33, "Remove outliers in amplitude...");
        filters.addItem(34, "Force to harmonic frequency...");
        filters.addItem(35, "Replace frequency with average");
        filters.addItem(36, "Smooth frequency...");
        filters.addItem(37, "Remove outliers in frequency...");
        filters.addItem(38, "Add noise to frequency...");
        filters.addItem(39, "Arpeggiate...");
    }

    [[nodiscard]] bool nudgeSelectedVisibleRegion(
        const juce::KeyPress& key)
    {
        std::int64_t frameDelta = 0;
        double frequencyScale = 1.0;
        const auto step = key.getModifiers().isShiftDown() ? 16 : 1;
        if (key.getKeyCode() == juce::KeyPress::leftKey)
            frameDelta = -step;
        else if (key.getKeyCode() == juce::KeyPress::rightKey)
            frameDelta = step;
        else if (key.getKeyCode() == juce::KeyPress::upKey)
            frequencyScale = 1.01;
        else if (key.getKeyCode() == juce::KeyPress::downKey)
            frequencyScale = 1.0 / 1.01;
        else
            return false;
        beginGesture();
        return transformSelectedVisibleRegion(
            frameDelta,
            frequencyScale,
            std::nullopt,
            "Transformed exact visible partial-track region",
            gestureToken_);
    }

    [[nodiscard]] bool nudgeSelectedPoint(
        const juce::KeyPress& key)
    {
        const auto* point = selectedPoint();
        if (point == nullptr)
            return false;
        auto frame = point->sourceFrame;
        auto frequency = point->frequencyHz;
        auto amplitude = point->amplitude;
        const auto step = key.getModifiers().isShiftDown() ? 16U : 1U;
        if (key.getKeyCode() == juce::KeyPress::leftKey)
        {
            frame = frame >= step ? frame - step : 0;
        }
        else if (key.getKeyCode() == juce::KeyPress::rightKey)
        {
            if (sourceFrameCount_ <= 1)
                return false;
            frame = std::min<std::uint64_t>(
                sourceFrameCount_ - 1,
                frame + std::min<std::uint64_t>(
                    step, sourceFrameCount_ - 1 - frame));
        }
        else if (key.getKeyCode() == juce::KeyPress::upKey)
        {
            if (key.getModifiers().isAltDown())
                amplitude = std::min(4.0, amplitude + 0.01);
            else
                frequency = std::min(sampleRate_ * 0.5, frequency * 1.01);
        }
        else if (key.getKeyCode() == juce::KeyPress::downKey)
        {
            if (key.getModifiers().isAltDown())
                amplitude = std::max(0.0, amplitude - 0.01);
            else
                frequency = std::max(0.001, frequency / 1.01);
        }
        else
        {
            return false;
        }
        beginGesture();
        const auto stablePoint = *point;
        return updatePoint(
            stablePoint,
            frame,
            frequency,
            amplitude,
            stablePoint.bandwidthHz,
            stablePoint.trackId,
            stablePoint.interpolation,
            gestureToken_,
            "Nudged exact partial-track point");
    }

    [[nodiscard]] bool handleEditKey(const juce::KeyPress& key)
    {
        const auto globalModifier =
            key.getModifiers().isCommandDown()
            || key.getModifiers().isCtrlDown();
        if (key == juce::KeyPress::escapeKey)
        {
            if (gestureToken_ != 0 || pendingSnap_.has_value())
                cancelGesture();
            else
            {
                clearSelection();
                regionSelection_.clear();
                selectedMarker_.reset();
                viewport_.repaint();
            }
            return true;
        }
        // Select All selects every track.
        if (globalModifier && (key.getKeyCode() == 'a' || key.getKeyCode() == 'A') && !key.getModifiers().isShiftDown())
        {
            if (const auto materialized = materializedEdits())
                regionSelection_.selectAll(*materialized);
            viewport_.repaint();
            return true;
        }
        // A selected marker is renamed with Enter and removed with Delete;
        // a number key selects a track.
        if (!globalModifier && selectedMarker_.has_value() && !pendingSnap_.has_value())
        {
            if (key == juce::KeyPress::returnKey)
            {
                renameSelectedMarker();
                return true;
            }
            if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            {
                performSpectrumCommand(16);
                return true;
            }
        }
        if (const auto code = key.getKeyCode();
            !globalModifier && code >= '0' && code <= '9' && selectedPoint() == nullptr)
        {
            return selectTrackByNumber(static_cast<char>(code), key.getModifiers().isShiftDown());
        }
        // Ctrl+] and Ctrl+[ zoom in and out; F1 plays all the tracks and F2
        // the selected ones;
        // with no point selected, the up and down arrows select the track
        // above or below and the left and right arrows step the scrub bar
        //.
        if (globalModifier && (key.getTextCharacter() == ']' || key.getTextCharacter() == '['))
        {
            performSpectrumCommand(key.getTextCharacter() == ']' ? 42 : 43);
            return true;
        }
        // Copy: the selected region to a new spectrum file.
        if (globalModifier && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C') && !regionSelection_.empty())
        {
            promptCopyRegion();
            return true;
        }
        // Shift+Space plays all the tracks too.
        if (key == juce::KeyPress{' ', juce::ModifierKeys::shiftModifier, 0})
        {
            playRegion(talas::app::PartialTrackPlay::All);
            return true;
        }
        // F1 to F12 press the buttons across the bottom.
        for (const auto& info : talas::app::spectrumEditorButtons())
        {
            if (key.getKeyCode() == juce::KeyPress::F1Key + info.functionKey - 1 && !globalModifier)
            {
                pressSpectrumButton(info.button);
                return true;
            }
        }
        if (!globalModifier && !key.getModifiers().isShiftDown() && selectedPoint() == nullptr && !pendingSnap_.has_value())
        {
            if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
                return selectAdjacentTrack(key.getKeyCode() == juce::KeyPress::upKey);
            if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
            {
                scrubFrame_ = talas::app::partialTrackScrubStepped(scrubFrame_, sourceFrameCount_, key.getKeyCode() == juce::KeyPress::rightKey);
                viewport_.repaint();
                auditionAt(scrubFrame_, false);
                return true;
            }
        }
        if (pendingSnap_.has_value()
            && (key == juce::KeyPress::returnKey
                || key == juce::KeyPress::spaceKey))
        {
            const auto candidate = *pendingSnap_;
            pendingSnap_.reset();
            return updatePoint(
                candidate.original,
                candidate.frame,
                candidate.frequencyHz,
                candidate.original.amplitude,
                candidate.original.bandwidthHz,
                candidate.original.trackId,
                candidate.original.interpolation,
                0,
                "Approved explicit snapped partial-track candidate");
        }
        // Enter zooms all the way out, as the full display button does
        //.
        if (!globalModifier && key == juce::KeyPress::returnKey)
        {
            performSpectrumCommand(44);
            return true;
        }
        const auto character = key.getTextCharacter();
        const auto arrow = key.getKeyCode() == juce::KeyPress::leftKey
            || key.getKeyCode() == juce::KeyPress::rightKey
            || key.getKeyCode() == juce::KeyPress::upKey
            || key.getKeyCode() == juce::KeyPress::downKey;
        const auto localEditCharacter = !globalModifier
            && (character == '1' || character == '2'
                || character == 'm' || character == 'M'
                || character == 's' || character == 'S'
                || character == 'r' || character == 'R'
                || character == 'g' || character == 'G');
        if (projection_.selectionRetained
            && selectedPoint() == nullptr
            && selectedTrack() == nullptr
            && (localEditCharacter || (globalModifier && arrow)
                || (!globalModifier
                    && (key == juce::KeyPress::deleteKey
                        || key == juce::KeyPress::backspaceKey))))
        {
            if (hooks_.setStatus)
                hooks_.setStatus(
                    juceString("Partial-track edit target is offscreen or filtered · reveal the exact identity before editing"));
            return true;
        }
        if (!globalModifier
            && (key == juce::KeyPress::deleteKey
            || key == juce::KeyPress::backspaceKey)
           )
            return deleteSelection();
        if (!globalModifier && key.getTextCharacter() == '1')
            return setInterpolation(
                talas::project::PartialTrackPointInterpolation::Step);
        if (!globalModifier && key.getTextCharacter() == '2')
            return setInterpolation(
                talas::project::PartialTrackPointInterpolation::Linear);
        if (!globalModifier && (key.getTextCharacter() == 'm'
            || key.getTextCharacter() == 'M')
           )
            return toggleTrackState(false);
        if (!globalModifier && (key.getTextCharacter() == 's'
            || key.getTextCharacter() == 'S')
           )
            return toggleTrackState(true);
        if (!globalModifier && (key.getTextCharacter() == 'r'
            || key.getTextCharacter() == 'R'))
        {
            return selectedPoint() != nullptr
                ? retrackSelectedPoint()
                : splitSelectedVisibleRegion();
        }
        if (!globalModifier && (key.getTextCharacter() == 'g'
            || key.getTextCharacter() == 'G'))
            return mergeSelectedVisibleRegion();
        if (!globalModifier)
            return false;
        return selectedTrack() != nullptr
            ? nudgeSelectedVisibleRegion(key)
            : nudgeSelectedPoint(key);
    }

    void endKeyboardGestureIfReleased()
    {
        if (gestureToken_ == 0)
            return;
        const auto anyArrow =
            juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::leftKey)
            || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::rightKey)
            || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::upKey)
            || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::downKey);
        if (!anyArrow)
            endGesture();
    }

    void startPointGesture(
        talas::app::PartialTrackEditorPointGeometry point,
        bool snappedCandidate)
    {
        cancelGesture();
        dragPoint_ = std::move(point);
        dragUsesSnap_ = snappedCandidate;
        if (!dragUsesSnap_)
            beginGesture();
    }

    void dragPointTo(juce::Point<int> position)
    {
        if (!dragPoint_.has_value())
            return;
        const auto plot = plotBounds();
        if (plot.isEmpty())
            return;
        const auto xRatio = std::clamp(
            static_cast<double>(position.x - plot.getX())
                / static_cast<double>(plot.getWidth()),
            0.0,
            1.0);
        const auto yRatio = std::clamp(
            static_cast<double>(position.y - plot.getY())
                / static_cast<double>(plot.getHeight()),
            0.0,
            1.0);
        const auto range = horizontal_.getCurrentRange();
        auto frame = static_cast<std::uint64_t>(std::clamp(
            std::llround(std::lerp(range.getStart(), range.getEnd(), xRatio)),
            0LL,
            static_cast<long long>(sourceFrameCount_ - 1)));
        auto frequency = std::lerp(
            maximumFrequencyHz_, minimumFrequencyHz_, yRatio);
        if (dragUsesSnap_)
        {
            constexpr std::uint64_t frameGrid = 128;
            frame = std::min<std::uint64_t>(
                sourceFrameCount_ - 1,
                ((frame + frameGrid / 2) / frameGrid) * frameGrid);
            const auto midi = 69.0
                + 12.0 * std::log2(frequency / 440.0);
            frequency = 440.0
                * std::exp2((std::round(midi) - 69.0) / 12.0);
            frequency = std::clamp(
                frequency, 0.001, sampleRate_ * 0.5);
            pendingSnap_ = PendingPointCandidate{
                *dragPoint_, frame, frequency};
            if (hooks_.setStatus)
            {
                hooks_.setStatus(
                    juceString("Snapped candidate ready · Return approves · Escape cancels"));
            }
            viewport_.repaint();
            return;
        }
        const auto stablePoint = *dragPoint_;
        (void) updatePoint(
            stablePoint,
            frame,
            frequency,
            stablePoint.amplitude,
            stablePoint.bandwidthHz,
            stablePoint.trackId,
            stablePoint.interpolation,
            gestureToken_,
            "Moved exact partial-track point");
    }

    void finishPointGesture()
    {
        if (!dragPoint_.has_value())
            return;
        if (!dragUsesSnap_)
            endGesture();
        dragPoint_.reset();
        dragUsesSnap_ = false;
    }

    [[nodiscard]] bool insertPointAt(juce::Point<int> position)
    {
        if (sourceAsset_ == nullptr || sourceFrameCount_ <= 1)
            return false;
        const auto plot = plotBounds();
        if (!plot.contains(position))
            return false;
        const auto xRatio = std::clamp(
            static_cast<double>(position.x - plot.getX())
                / static_cast<double>(plot.getWidth()),
            0.0,
            1.0);
        const auto yRatio = std::clamp(
            static_cast<double>(position.y - plot.getY())
                / static_cast<double>(plot.getHeight()),
            0.0,
            1.0);
        const auto range = horizontal_.getCurrentRange();
        const auto frame = static_cast<std::uint64_t>(std::clamp(
            std::llround(std::lerp(range.getStart(), range.getEnd(), xRatio)),
            0LL,
            static_cast<long long>(sourceFrameCount_ - 1)));
        const auto frequency = std::lerp(
            maximumFrequencyHz_, minimumFrequencyHz_, yRatio);
        std::uint64_t trackId = 0;
        std::uint64_t segmentId = 0;
        if (const auto* point = selectedPoint())
        {
            trackId = point->trackId;
            segmentId = point->segmentId;
        }
        else if (const auto* row = selectedTrack())
        {
            trackId = row->trackId;
        }
        if (trackId == 0 && !projection_.rows.empty())
            trackId = projection_.rows.front().trackId;
        if (segmentId == 0 && !sourceAsset_->frames.empty())
            segmentId = sourceAsset_->frames.front().segmentId;
        if (trackId == 0 || segmentId == 0)
            return false;
        auto editId = std::string{};
        for (std::size_t attempt = 0; attempt < 1024; ++attempt)
        {
            editId = "insert_"
                + std::to_string(manifest_->generation) + "_"
                + std::to_string(++insertSerial_);
            const auto exists = currentEdits_.has_value()
                && std::ranges::any_of(
                    currentEdits_->insertedPoints,
                    [&](const auto& point) {
                        return point.editId == editId;
                    });
            if (!exists)
                break;
            editId.clear();
        }
        if (editId.empty())
            return false;
        auto batch = makeBatch();
        batch.pointMutations.push_back({
            talas::project::ProjectPartialTrackPointMutationKind::Insert,
            {talas::project::MaterializedPartialTrackPointOrigin::Inserted,
             {},
             editId},
            segmentId,
            frame,
            {trackId, 0.1, frequency, 0.0},
            talas::project::PartialTrackPointInterpolation::Linear,
        });
        return applyBatch(
            std::move(batch), 0,
            "Inserted nondestructive partial-track point");
    }

    void showContextMenu(juce::Point<int> position)
    {
        const auto available = [&](talas::app::PartialTrackEditorCommand command) {
            return (projection_.commandMask
                    & talas::app::partialTrackEditorCommandBit(command)) != 0;
        };
        juce::PopupMenu menu;
        if (selectedPoint() != nullptr)
        {
            menu.addItem(1, "Delete point", available(
                talas::app::PartialTrackEditorCommand::DeletePoint));
            menu.addItem(2, "Step interpolation", available(
                talas::app::PartialTrackEditorCommand::SetStepInterpolation));
            menu.addItem(3, "Linear interpolation", available(
                talas::app::PartialTrackEditorCommand::SetLinearInterpolation));
            menu.addItem(4, "Retrack point to new track", available(
                talas::app::PartialTrackEditorCommand::RetrackPoint));
        }
        else if (selectedTrack() != nullptr)
        {
            menu.addItem(10, "Toggle mute", available(
                talas::app::PartialTrackEditorCommand::MuteTrack));
            menu.addItem(11, "Toggle solo", available(
                talas::app::PartialTrackEditorCommand::SoloTrack));
            menu.addItem(12, "Split visible region to new track", available(
                talas::app::PartialTrackEditorCommand::SplitVisibleRegion));
            menu.addItem(
                13,
                "Merge visible region into preceding track",
                available(
                    talas::app::PartialTrackEditorCommand::MergeVisibleRegion));
            menu.addItem(14, "Clear", available(talas::app::PartialTrackEditorCommand::MuteTrack));
            menu.addItem(15, "Trim", available(talas::app::PartialTrackEditorCommand::MuteTrack));
            addTrackFilterMenu(menu);
        }
        else
        {
            menu.addItem(20, "Insert point here");
            addTrackFilterMenu(menu);
        }
        juce::Component::SafePointer<PartialTrackEditorPanelImpl> safeThis(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(&viewport_),
            [safeThis, position](int choice) {
                if (safeThis == nullptr)
                    return;
                switch (choice)
                {
                case 1:
                    (void) safeThis->deleteSelection();
                    break;
                case 2:
                    (void) safeThis->setInterpolation(
                        talas::project::PartialTrackPointInterpolation::Step);
                    break;
                case 3:
                    (void) safeThis->setInterpolation(
                        talas::project::PartialTrackPointInterpolation::Linear);
                    break;
                case 4:
                    (void) safeThis->retrackSelectedPoint();
                    break;
                case 10:
                    (void) safeThis->toggleTrackState(false);
                    break;
                case 11:
                    (void) safeThis->toggleTrackState(true);
                    break;
                case 12:
                    (void) safeThis->splitSelectedVisibleRegion();
                    break;
                case 13:
                    (void) safeThis->mergeSelectedVisibleRegion();
                    break;
                case 14:
                case 15:
                    (void) safeThis->clearOrTrimSelectedTrack(choice == 15);
                    break;
                case 20:
                    (void) safeThis->insertPointAt(position);
                    break;
                default:
                    if (choice >= 30 && choice <= 39)
                        safeThis->promptTrackFilter(static_cast<talas::app::PartialTrackFilter>(choice - 30));
                    break;
                }
            });
    }

    void applyFilter()
    {
        const talas::app::PartialTrackEditorFilter filter{
            {},
            amplitude_.getValue(),
            includeMuted_.getToggleState(),
            soloOnly_.getToggleState(),
        };
        if (model_.setFilter(filter))
            refreshProjection();
    }

    void resetScrollRanges()
    {
        const auto visibleFrames = std::max<double>(
            1.0,
            std::min<double>(
                static_cast<double>(sourceFrameCount_),
                sampleRate_ * 8.0));
        horizontal_.setRangeLimits(
            0.0,
            static_cast<double>(sourceFrameCount_),
            juce::dontSendNotification);
        horizontal_.setCurrentRange(
            0.0,
            visibleFrames,
            juce::dontSendNotification);
        updateVisibleRows();
    }

    void updateVisibleRows()
    {
        const auto rows = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::max(
                1,
                plotBounds().getHeight()
                    / std::max(1, workbenchScaledUiPixels(22)))));
        visibleRows_ = std::min(
            rows,
            talas::app::maximumPartialTrackEditorVisibleRows);
        vertical_.setRangeLimits(
            0.0,
            static_cast<double>(std::max(trackCount_, visibleRows_)),
            juce::dontSendNotification);
        vertical_.setCurrentRange(
            vertical_.getCurrentRangeStart(),
            static_cast<double>(visibleRows_),
            juce::dontSendNotification);
    }

    void scrollBarMoved(juce::ScrollBar*, double) override
    {
        refreshProjection();
    }

    void refreshProjection()
    {
        const auto horizontalRange = horizontal_.getCurrentRange();
        const auto start = static_cast<std::uint64_t>(std::max(
            0.0,
            std::floor(horizontalRange.getStart())));
        auto end = static_cast<std::uint64_t>(std::max(
            static_cast<double>(start + 1U),
            std::ceil(horizontalRange.getEnd())));
        end = std::min(end, sourceFrameCount_);
        if (end <= start)
            end = std::min(sourceFrameCount_, start + 1U);
        const auto plot = plotBounds();
        projection_ = model_.project({
            start,
            end,
            zoomLowHz_.value_or(minimumFrequencyHz_),
            zoomHighHz_.value_or(maximumFrequencyHz_),
            static_cast<std::size_t>(std::max(1, plot.getWidth())),
            static_cast<std::size_t>(std::max(1, plot.getHeight())),
            static_cast<std::size_t>(std::max(
                0.0,
                std::floor(vertical_.getCurrentRangeStart()))),
            visibleRows_,
        });
        if (manifest_.has_value())
        {
            source_.setText(
                juceString(projection_.sourceAuthorityText),
                juce::dontSendNotification);
            quality_.setText(
                juceString(projection_.qualityText + " · "
                    + projection_.latencyText),
                juce::dontSendNotification);
        }
        else
        {
            source_.setText(
                externalMessage_.isNotEmpty()
                    ? externalMessage_
                    : juceString(projection_.message),
                juce::dontSendNotification);
            quality_.setText({}, juce::dontSendNotification);
        }
        rebuildAccessibility();
        updateOperationControls();
        viewport_.repaint();
    }

    void select(talas::app::PartialTrackEditorSelection selection)
    {
        const auto* before = selectedTrack();
        const auto trackBefore = before == nullptr ? std::nullopt : std::optional<std::uint64_t>{before->trackId};
        if (model_.setSelection(std::move(selection)))
            refreshProjection();
        // A cleared track,
        // greyed, comes back once selected.
        if (const auto* row = selectedTrack())
        {
            if (const auto restored = talas::app::partialTrackRestoredBySelection(row->trackId, row->muted, row->solo, trackBefore))
            {
                auto batch = makeBatch();
                batch.trackStateMutations.push_back(*restored);
                (void)applyBatch(std::move(batch), 0, "Restored the cleared partial track as it was selected");
            }
        }
    }

    void clearSelection()
    {
        model_.clearSelection();
        refreshProjection();
    }

    bool moveAccessibilityFocus(
        std::string_view current,
        int direction)
    {
        const auto found = std::ranges::find_if(
            accessibilityEntities_,
            [&](const auto& entity) {
                return entity->focusIdentity() == current;
            });
        if (found == accessibilityEntities_.end())
            return false;
        const auto index = static_cast<std::ptrdiff_t>(
            found - accessibilityEntities_.begin());
        const auto next = index + direction;
        if (next < 0
            || next >= static_cast<std::ptrdiff_t>(
                accessibilityEntities_.size()))
        {
            return false;
        }
        accessibilityEntities_[static_cast<std::size_t>(next)]
            ->grabKeyboardFocus();
        return true;
    }

    void rebuildAccessibility()
    {
        std::optional<std::string> focusedIdentity;
        for (const auto& entity : accessibilityEntities_)
        {
            if (entity != nullptr && entity->hasKeyboardFocus(false))
            {
                focusedIdentity = std::string{entity->focusIdentity()};
                break;
            }
        }
        std::vector<std::unique_ptr<AccessibilityEntity>> next;
        next.reserve(std::min(
            maximumPartialTrackAccessibleItems,
            projection_.rows.size() + projection_.points.size()));
        std::unordered_map<std::string_view, std::size_t> previousByIdentity;
        previousByIdentity.reserve(accessibilityEntities_.size());
        for (std::size_t index = 0;
             index < accessibilityEntities_.size(); ++index)
        {
            if (accessibilityEntities_[index] != nullptr)
            {
                previousByIdentity.emplace(
                    accessibilityEntities_[index]->focusIdentity(),
                    index);
            }
        }
        const auto takeEntity = [&](std::string_view identity) {
            const auto found = previousByIdentity.find(identity);
            if (found == previousByIdentity.end())
                return std::unique_ptr<AccessibilityEntity>{};
            auto entity = std::move(accessibilityEntities_[found->second]);
            previousByIdentity.erase(found);
            return entity;
        };
        const auto plot = plotBounds();
        const auto rowHeight = std::max(
            1,
            plot.getHeight()
                / std::max<int>(1, static_cast<int>(projection_.rows.size())));
        for (std::size_t index = 0;
             index < projection_.rows.size()
                && next.size() < maximumPartialTrackAccessibleItems;
             ++index)
        {
            const auto& row = projection_.rows[index];
            auto identity = "partial-track.track:"
                + std::to_string(row.trackId);
            auto entity = takeEntity(identity);
            if (entity == nullptr)
            {
                entity = std::make_unique<AccessibilityEntity>(
                    *this, identity);
                viewport_.accessibilityContainer_.addAndMakeVisible(*entity);
            }
            auto title = "Track "
                + juce::String(static_cast<juce::int64>(row.trackId))
                + ", " + juce::String(static_cast<int>(row.pointCount))
                + " points";
            if (row.muted)
                title += ", muted";
            if (row.solo)
                title += ", solo";
            title += ". M toggles mute, S toggles solo, R splits the visible region, G merges it, Command plus arrows transforms it.";
            entity->update(
                {talas::app::PartialTrackEditorItemKind::Track, row.trackId, {}},
                title,
                {0,
                 plot.getY() + static_cast<int>(index) * rowHeight,
                 plot.getX(),
                 rowHeight},
                row.selected);
            next.push_back(std::move(entity));
        }
        for (const auto& point : projection_.points)
        {
            if (next.size() >= maximumPartialTrackAccessibleItems)
                break;
            auto identity = pointFocusIdentity(point.identity);
            auto entity = takeEntity(identity);
            if (entity == nullptr)
            {
                entity = std::make_unique<AccessibilityEntity>(
                    *this, identity);
                viewport_.accessibilityContainer_.addAndMakeVisible(*entity);
            }
            auto title = "Track "
                + juce::String(static_cast<juce::int64>(point.trackId))
                + " point, " + pointIdentityText(point.identity)
                + ", " + juce::String(point.frequencyHz, 3)
                + " hertz, amplitude " + juce::String(point.amplitude, 4)
                + (point.interpolation
                        == talas::project::PartialTrackPointInterpolation::Step
                    ? ", Step interpolation"
                    : ", Linear interpolation");
            title += ". Command plus arrows edits the point; 1 or 2 selects interpolation; R retracks; Delete removes.";
            const auto x = plot.getX()
                + static_cast<int>(std::lround(point.x));
            const auto y = plot.getY()
                + static_cast<int>(std::lround(point.y));
            const auto size = workbenchScaledUiPixels(12);
            entity->update(
                {talas::app::PartialTrackEditorItemKind::Point,
                 point.trackId,
                 point.identity},
                title,
                {x - size / 2, y - size / 2, size, size},
                point.selected);
            next.push_back(std::move(entity));
        }
        accessibilityEntities_ = std::move(next);
        if (focusedIdentity.has_value())
        {
            const auto retained = std::ranges::find_if(
                accessibilityEntities_,
                [&](const auto& entity) {
                    return entity->focusIdentity() == *focusedIdentity;
                });
            if (retained != accessibilityEntities_.end())
                (*retained)->grabKeyboardFocus();
            else
                viewport_.grabKeyboardFocus();
        }
        if (auto* handler = viewport_.getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::structureChanged);
        }
    }

    PartialTrackEditorEditingHooks hooks_;
    talas::app::PartialTrackEditorModel model_;
    std::optional<talas::project::ProjectAsset> manifest_;
    std::shared_ptr<const talas::project::PartialTrackAsset> sourceAsset_;
    std::optional<talas::project::ProjectPartialTrackEditSet> currentEdits_;
    // The spectrum editor's region selection, markers, scrub bar and readout
    //; workspace-local, not saved.
    talas::app::PartialTrackSelection regionSelection_;
    talas::app::PartialTrackMarkers markers_;
    std::optional<std::size_t> selectedMarker_;
    std::uint64_t scrubFrame_ = 0;
    bool scrubbing_ = false;
    bool boxZooms_ = false;
    talas::app::PartialTrackVerticalAxis axis_ = talas::app::PartialTrackVerticalAxis::Frequency;
    bool joinDots_ = true;
    bool drawMode_ = false;
    bool drawing_ = false;
    std::unique_ptr<juce::FileChooser> copyChooser_;
    std::optional<double> zoomLowHz_;
    std::optional<double> zoomHighHz_;
    std::optional<juce::Point<int>> boxStart_;
    std::optional<juce::Point<int>> boxEnd_;
    bool boxShift_ = false;
    juce::String readout_;
    talas::app::PartialTrackPlay playRequest_ = talas::app::PartialTrackPlay::All;
    // The fourth button plays up to the start marker
    // when chosen in the Spectrum menu, else outside the loop markers.
    bool fourthPlaysToStart_ = false;
    std::optional<PartialTrackEditorSourceSnapshot::Scrub> scrubRequest_;
    juce::uint32 lastAudition_ = 0;
    talas::project::ProjectEditor::HistoryTransactionToken gestureToken_ = 0;
    std::optional<talas::app::PartialTrackEditorPointGeometry> dragPoint_;
    std::optional<PendingPointCandidate> pendingSnap_;
    std::uint64_t insertSerial_ = 0;
    bool dragUsesSnap_ = false;
    talas::app::PartialTrackEditorProjection projection_;
    juce::Label title_;
    juce::Label source_;
    juce::Label quality_;
    juce::Label amplitudeLabel_;
    juce::Slider amplitude_;
    juce::ToggleButton includeMuted_;
    juce::ToggleButton soloOnly_;
    juce::TextButton preview_;
    juce::TextButton renderAudio_;
    juce::TextButton renderPartial_;
    juce::TextButton cancelOperation_;
    juce::TextButton importDerivative_;
    juce::TextButton spectrumMenu_;
    juce::OwnedArray<juce::TextButton> stripButtons_;
    juce::Label operationStatus_;
    juce::ScrollBar horizontal_;
    juce::ScrollBar vertical_;
    Viewport viewport_;
    std::vector<std::unique_ptr<AccessibilityEntity>>
        accessibilityEntities_;
    juce::String externalMessage_;
    std::uint64_t sourceFrameCount_ = 1;
    double sampleRate_ = 1.0;
    double minimumFrequencyHz_ = 0.001;
    double maximumFrequencyHz_ = 0.5;
    std::size_t trackCount_ = 0;
    std::size_t visibleRows_ = 1;
    bool compactLayout_ = false;
    PartialTrackEditorOperationState operationState_;
};
}

std::unique_ptr<PartialTrackEditorPanel> makePartialTrackEditorPanel(
    PartialTrackEditorEditingHooks hooks)
{
    return std::make_unique<PartialTrackEditorPanelImpl>(std::move(hooks));
}

void WorkbenchComponent::refreshPartialTrackEditor(
    const talas::core::ResourceSnapshotPtr& snapshot)
{
    const auto& document = projectBridge_.editor().document();
    // The spectrum opened, else the project's first.
    const auto* manifest = talas::app::editedSpectrumAsset(document, spectrumEditorAssetId_);
    if (manifest == nullptr)
    {
        partialTrackEditorPanel_->setUnavailable(
            "No PartialSpectrum asset is available in this project");
        return;
    }
    if (snapshot == nullptr
        || snapshot->kind
            != talas::core::ResourceSnapshotKind::PartialSpectrum
        || snapshot->id != manifest->id
        || snapshot->generation != manifest->generation
        || snapshot->contentHash != manifest->contentHash
        || snapshot->sampleRate != manifest->sampleRate
        || snapshot->frameCount != manifest->frameCount
        || snapshot->encoding != manifest->encoding
        || snapshot->partialTrackBytes.empty())
    {
        partialTrackEditorPanel_->setUnavailable(
            "Exact PartialSpectrum source snapshot is unavailable");
        return;
    }
    auto decoded = talas::project::decodePartialTrackAsset(
        snapshot->partialTrackBytes);
    if (!decoded.ok())
    {
        partialTrackEditorPanel_->setUnavailable(
            "PartialSpectrum source decode failed: "
            + juceString(decoded.error.has_value()
                    ? decoded.error->message
                    : std::string_view{"unknown bounded decode error"}));
        return;
    }
    const auto edits = std::ranges::find(
        document.partialTrackEdits,
        manifest->id,
        &talas::project::ProjectPartialTrackEditSet::assetId);
    std::optional<talas::project::ProjectPartialTrackEditSet> candidate;
    if (edits != document.partialTrackEdits.end())
        candidate = *edits;
    try
    {
        auto source = std::make_shared<const talas::project::PartialTrackAsset>(
            std::move(decoded.asset));
        if (!partialTrackEditorPanel_->setSource(
                *manifest,
                std::move(source),
                std::move(candidate)))
        {
            setStatus("Partial Tracks editor rejected stale or invalid source authority");
        }
    }
    catch (const std::bad_alloc&)
    {
        partialTrackEditorPanel_->setUnavailable(
            "Partial Tracks editor source allocation failed");
    }
}

void WorkbenchComponent::refreshPartialTrackEditorEdits()
{
    partialTrackEditorPanel_->refreshEdits(
        projectBridge_.editor().document());
}

talas::core::ResourceSnapshotPtr
WorkbenchComponent::partialTrackEditorSnapshotFor(
    const talas::runtime::RenderSettings& settings) const
{
    const auto* manifest = talas::app::editedSpectrumAsset(projectBridge_.editor().document(), spectrumEditorAssetId_);
    if (manifest == nullptr)
        return {};
    const auto snapshot = std::ranges::find(
        settings.resourceSnapshots,
        manifest->id,
        [](const auto& value) -> const std::string& {
            return value->id;
        });
    return snapshot == settings.resourceSnapshots.end()
        ? talas::core::ResourceSnapshotPtr{}
        : *snapshot;
}

std::optional<std::string> WorkbenchComponent::runE10StartupSmoke(
    const std::filesystem::path& smokeDirectory)
{
    if (auto error = runPreparedStreamDetailSmoke())
        return error;
    if (auto error = runProcessorPlacementSmoke())
        return error;
    if (auto error = runDeviceStopOffMessageThreadSmoke())
        return error;
    if (auto error = runValueGestureHistorySmoke())
        return error;
    return runPartialTrackEditorShellSmoke(smokeDirectory);
}

std::optional<std::string>
WorkbenchComponent::runPartialTrackEditorShellSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto snapshot = [&](std::string_view fileName)
        -> std::optional<std::string> {
        const auto image = createComponentSnapshot(
            getLocalBounds(), true, 1.0F);
        if (!image.isValid())
            return "Partial Tracks smoke snapshot was invalid";
        auto stream = juce::File{juceString(
            (smokeDirectory / fileName).lexically_normal().string())}
                          .createOutputStream();
        if (stream == nullptr || !stream->openedOk()
            || !stream->setPosition(0)
            || stream->truncate().failed()
            || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        {
            return "Partial Tracks smoke snapshot could not be written";
        }
        stream->flush();
        return {};
    };
    const auto previousWidth = getWidth();
    const auto previousHeight = getHeight();
    const auto previousAccessibility = workspaceSession_.accessibility;
    partialTrackEditorPanel_->setLoading();
    if (partialTrackEditorPanel_->stateForSmoke()
        != talas::app::PartialTrackEditorState::Loading)
    {
        return "Partial Tracks panel did not expose Loading state";
    }
    partialTrackEditorPanel_->setUnavailable(
        "Exact PartialSpectrum smoke source unavailable");
    if (partialTrackEditorPanel_->stateForSmoke()
        != talas::app::PartialTrackEditorState::Error)
    {
        return "Partial Tracks panel did not expose Error state";
    }
    talas::project::PartialTrackAsset source{
        48000.0,
        4096,
        {{1, 0, {{1, 0.5, 440.0, 8.0}, {2, 0.25, 880.0, 12.0}}},
         {1, 2048, {{1, 0.4, 445.0, 9.0}, {2, 0.2, 875.0, 11.0}}}},
    };
    talas::project::ProjectAsset manifest;
    manifest.id = "smoke_partial_tracks";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "smoke-partial-tracks.talaspartials";
    const auto encodedSmokeSource =
        talas::project::encodePartialTrackAsset(source);
    if (!encodedSmokeSource.ok())
        return "Partial Tracks smoke source could not be encoded";
    manifest.contentHash = talas::project::calculateProjectAssetSha256(
        encodedSmokeSource.bytes);
    manifest.sampleRate = source.sampleRate;
    manifest.frameCount = source.sourceFrameCount;
    manifest.encoding = std::string{talas::project::partialTrackAssetEncoding};
    manifest.generation = 17;
    auto emptyManifest = manifest;
    emptyManifest.generation = 16;
    emptyManifest.frameCount = 1;
    const auto emptySource = std::make_shared<
        const talas::project::PartialTrackAsset>(
            talas::project::PartialTrackAsset{
                48000.0, 1, {{1, 0, {}}}});
    if (!partialTrackEditorPanel_->setSource(
            emptyManifest, emptySource, std::nullopt)
        || partialTrackEditorPanel_->stateForSmoke()
            != talas::app::PartialTrackEditorState::Empty)
    {
        return "Partial Tracks panel did not expose Empty state";
    }
    if (!partialTrackEditorPanel_->setSource(
            manifest,
            std::make_shared<const talas::project::PartialTrackAsset>(source),
            std::nullopt))
    {
        return "Partial Tracks smoke source was rejected";
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
    setSize(1500, 900);
    if (!partialTrackEditorPanel_->isVisible()
        || partialTrackEditorPanel_->stateForSmoke()
            != talas::app::PartialTrackEditorState::Ready
        || partialTrackEditorPanel_->visibleAccessibleItemCountForSmoke() < 4
        || !partialTrackEditorPanel_->sourceTextContainsForSmoke(
            "generation 17")
        || partialTrackEditorPanel_->compactLayoutForSmoke())
    {
        return "Partial Tracks full editor did not expose exact source, virtual geometry, or accessible identities";
    }
    if (auto error = snapshot("e10-partial-tracks-full.png"))
        return error;
    workspaceSession_.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent150;
    workspaceSession_.accessibility.enhancedContrast = true;
    applyAccessibilityPreferences();
    setSize(960, 640);
    resized();
    if (!partialTrackEditorPanel_->compactLayoutForSmoke())
        return "Partial Tracks compact layout did not engage";
    if (partialTrackEditorPanel_->getBounds().isEmpty())
        return "Partial Tracks compact editor was clipped out of layout";
    if (partialTrackEditorPanel_->visibleAccessibleItemCountForSmoke() < 4)
        return "Partial Tracks compact editor discarded visible accessible identities";
    if (!partialTrackEditorPanel_->frequencyLabelsFitForSmoke())
        return "Partial Tracks compact frequency labels escaped the visible viewport";
    if (WorkbenchType::interfaceScale() != 1.5F)
        return "Partial Tracks compact editor did not retain the 150 percent interface projection";
    if (auto error = snapshot("e10-partial-tracks-compact.png"))
        return error;

    const auto immutableSourceBeforeEdits = source;
    auto& editor = projectBridge_.editor();
    const auto previousDocument = editor.document();
    talas::project::ProjectDocument editDocument;
    editDocument.assets.push_back(manifest);
    if (!editor.replaceDocument(editDocument).ok())
        return "Partial Tracks edit smoke document was rejected";
    documentChanged();
    if (!partialTrackEditorPanel_->setSource(
            manifest,
            std::make_shared<const talas::project::PartialTrackAsset>(source),
            std::nullopt))
    {
        return "Partial Tracks edit smoke could not restore exact source";
    }
    if (!partialTrackEditorPanel_->nudgeFirstPointForSmoke()
        || editor.document().partialTrackEdits.size() != 1)
    {
        return "Partial Tracks keyboard nudge did not commit one exact edit recipe";
    }
    if (!editor.undo().ok()
        || !editor.document().partialTrackEdits.empty())
    {
        return "Partial Tracks owned keyboard gesture was not one Undo step";
    }
    documentChanged();
    if (!editor.redo().ok())
        return "Partial Tracks owned keyboard gesture could not be redone";
    documentChanged();
    if (!partialTrackEditorPanel_->exerciseEditCommandsForSmoke()
        || !partialTrackEditorPanel_->exerciseSpectrumEditingForSmoke()
        || editor.document().partialTrackEdits.empty()
        || source != immutableSourceBeforeEdits)
    {
        return "Partial Tracks shared interpolation/mute/snap/insert/delete commands lost authority or changed source bytes";
    }
    if (auto error = runPartialTrackOperationSmoke(
            smokeDirectory, source))
        return error;
    // MIDI pitch bend places the scrub bar across the file.
    const auto bentToEnd = partialTrackEditorPanel_->scrubForPitchBend(16383);
    if (!bentToEnd || *bentToEnd == 0 || partialTrackEditorPanel_->scrubForPitchBend(0) != std::optional<std::uint64_t>{0})
        return "MIDI pitch bend did not move the spectrum editor's scrub bar across the file";
    // Copy writes the selected region as a new spectrum
    // file, and a Sound to play it joins the Sound list.
    const auto soundsBefore = editor.document().soundDefinitions.size();
    const auto copied = smokeDirectory / "smoke region.talaspartial";
    if (!partialTrackEditorPanel_->copyRegionForSmoke(copied) || !std::filesystem::is_regular_file(copied)
        || editor.document().soundDefinitions.size() != soundsBefore + 1)
        return "Copy in the spectrum editor did not make a spectrum file of the region and a Sound playing it";
    // In drawing mode the pen redraws the selected track's
    // envelope, here its frequency at the display's middle, as one edit.
    const auto beforeDrawing = talas::project::serializeProjectDocument(editor.document());
    const auto drawn = partialTrackEditorPanel_->drawForSmoke(0.5);
    if (!drawn || !(*drawn > 0.0) || talas::project::serializeProjectDocument(editor.document()) == beforeDrawing)
        return "Drawing in the spectrum editor did not redraw the selected track's envelope: "
            + statusLabel_.getText().toStdString();
    if (!editor.replaceDocument(previousDocument).ok())
        return "Partial Tracks edit smoke could not restore the prior document";
    documentChanged();

    workspaceSession_.accessibility = previousAccessibility;
    applyAccessibilityPreferences();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    setSize(previousWidth, previousHeight);
    return {};
}
}
