#include "app/workbench/WorkbenchPerformanceCanvas.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/WorkbenchSurfaceState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace talas::app::workbench
{
namespace
{
constexpr int toolbarHeight = 40;
constexpr int itemWidth = 172;
constexpr int itemHeight = 128;
constexpr int compactItemHeight = 84;
// Controls without a position of their own flow in rows as wide as the
// canvas shows, this far apart.
constexpr int autoFlowGap = 16;
constexpr int canvasPadding = 18;
constexpr int canvasGrid = 32;
constexpr double minimumCanvasZoom = 0.6;
constexpr double maximumCanvasZoom = 1.6;
constexpr double canvasZoomStep = 0.1;
constexpr std::uint32_t activityLifetimeMilliseconds = 720;

juce::String kindName(talas::project::ProjectPerformanceControlKind kind)
{
    switch (kind)
    {
    case talas::project::ProjectPerformanceControlKind::Knob:
        return "Knob";
    case talas::project::ProjectPerformanceControlKind::Fader:
        return "Fader";
    case talas::project::ProjectPerformanceControlKind::Gate:
        return "Gate";
    case talas::project::ProjectPerformanceControlKind::Toggle:
        return "Toggle";
    case talas::project::ProjectPerformanceControlKind::Macro:
        return "Macro";
    }
    return "Control";
}

juce::String activityName(talas::app::PerformanceActivitySource source)
{
    switch (source)
    {
    case talas::app::PerformanceActivitySource::UserInterface:
        return "UI";
    case talas::app::PerformanceActivitySource::Midi:
        return "MIDI";
    case talas::app::PerformanceActivitySource::Osc:
        return "OSC";
    case talas::app::PerformanceActivitySource::Snapshot:
        return "SNAP";
    case talas::app::PerformanceActivitySource::Macro:
        return "MACRO";
    case talas::app::PerformanceActivitySource::None:
    default:
        return {};
    }
}

std::optional<double> currentValue(
    const talas::runtime::SnapshotPerformanceState& state,
    const talas::project::SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        state.currentValues,
        target,
        &talas::project::SnapshotValueEntry::target);
    if (found == state.currentValues.end()
        || found->value.kind != talas::project::SnapshotValueKind::Number)
    {
        return std::nullopt;
    }
    return found->value.number;
}

std::optional<talas::project::SnapshotParameterRange> targetRange(
    const talas::project::ProjectDocument& document,
    const talas::runtime::SnapshotPerformanceState& state,
    const talas::project::SnapshotTargetIdentity& target)
{
    if (target.kind == talas::project::SnapshotTargetKind::EventValue)
    {
        const auto control = std::ranges::find_if(
            document.virtualControls,
            [&](const auto& candidate) {
                return candidate.eventName == target.eventName
                    && candidate.domain == target.domain;
            });
        if (control != document.virtualControls.end())
            return talas::project::SnapshotParameterRange{
                control->minimum, control->maximum};
    }
    const auto bank = std::ranges::find(
        document.snapshotLibrary.banks,
        state.activeBankId,
        &talas::project::ProjectSnapshotBank::bankId);
    if (bank == document.snapshotLibrary.banks.end())
        return std::nullopt;
    const auto policy = std::ranges::find(
        bank->parameterPolicies,
        target,
        &talas::project::SnapshotParameterPolicy::target);
    return policy == bank->parameterPolicies.end()
        ? std::optional<talas::project::SnapshotParameterRange>{}
        : std::optional<talas::project::SnapshotParameterRange>{policy->range};
}

juce::Colour controlAccent(
    talas::project::ProjectPerformanceControlKind kind)
{
    return kind == talas::project::ProjectPerformanceControlKind::Macro
        ? workbenchTheme.event
        : workbenchTheme.control;
}

// A control or meter tile: raised glass, a gradient cap in its kind's
// colour, and the focus outline with a soft glow when selected.
void paintPerformanceTile(juce::Graphics& graphics, juce::Rectangle<float> bounds, juce::Colour accent, bool selected)
{
    const auto radius = 7.0F;
    WorkbenchStyle::paintTile(graphics, bounds, radius, workbenchTheme.controlTile.interpolatedWith(accent, 0.05F));
    {
        juce::Graphics::ScopedSaveState clipped(graphics);
        juce::Path shape;
        shape.addRoundedRectangle(bounds, radius);
        graphics.reduceClipRegion(shape);
        if (!WorkbenchStyle::enhancedContrast())
        {
            graphics.setGradientFill(juce::ColourGradient{accent.withAlpha(0.16F), bounds.getX(), bounds.getY(),
                accent.withAlpha(0.0F), bounds.getX(), bounds.getY() + 36.0F, false});
            graphics.fillRect(bounds.withHeight(36.0F));
        }
        graphics.setGradientFill(juce::ColourGradient{accent.brighter(0.2F), bounds.getX(), bounds.getY(),
            accent.withRotatedHue(0.06F), bounds.getRight(), bounds.getY(), false});
        graphics.fillRect(bounds.withHeight(3.0F));
    }
    if (selected)
    {
        if (!WorkbenchStyle::enhancedContrast())
        {
            graphics.setColour(workbenchTheme.focus.withAlpha(0.25F));
            graphics.drawRoundedRectangle(bounds.reduced(1.0F), radius, 5.0F);
        }
        graphics.setColour(workbenchTheme.focus);
        graphics.drawRoundedRectangle(bounds.reduced(1.0F), radius, 2.0F);
    }
}

// A recessed meter lit from mint through amber to red, with a soft glow.
void paintLevelMeter(juce::Graphics& graphics, juce::Rectangle<float> meter, float normalized)
{
    WorkbenchStyle::paintWell(graphics, meter, 3.0F);
    const auto lit = meter.reduced(1.5F).withWidth(std::max(0.0F, (meter.getWidth() - 3.0F) * normalized));
    if (lit.isEmpty())
        return;
    juce::ColourGradient gradient{workbenchTheme.audio, meter.getX(), 0.0F, workbenchTheme.error, meter.getRight(),
        0.0F, false};
    gradient.addColour(0.72, workbenchTheme.control);
    if (!WorkbenchStyle::enhancedContrast())
    {
        graphics.setGradientFill(gradient);
        graphics.setOpacity(0.25F);
        graphics.fillRoundedRectangle(lit.expanded(2.0F), 4.0F);
        graphics.setOpacity(1.0F);
    }
    graphics.setGradientFill(gradient);
    graphics.fillRoundedRectangle(lit, 2.0F);
}

class PerformanceCanvasImpl;

class PerformanceViewport final : public juce::Viewport
{
public:
    std::function<void(const juce::Rectangle<int>&)> onVisibleAreaChanged;

    void visibleAreaChanged(const juce::Rectangle<int>& area) override
    {
        if (onVisibleAreaChanged)
            onVisibleAreaChanged(area);
    }
};

// A Performance control's slider: a knob for a continuous control, or a
// pad for a gate (on while held) or a toggle (each press flips it). A pad
// keeps the slider's value, keys and accessibility; only its pointer and
// its face differ.
class PerformanceControlSlider final : public juce::Slider
{
public:
    enum class Mode
    {
        Knob,
        Toggle,
        Gate,
    };

    std::function<void()> onPadPress;
    std::function<void()> onPadRelease;

    void setMode(Mode mode)
    {
        mode_ = mode;
        repaint();
    }

    [[nodiscard]] Mode mode() const noexcept
    {
        return mode_;
    }

    [[nodiscard]] bool on() const
    {
        return getValue() >= (getMinimum() + getMaximum()) * 0.5;
    }

    void paint(juce::Graphics& graphics) override
    {
        if (mode_ == Mode::Knob)
        {
            juce::Slider::paint(graphics);
            return;
        }
        const auto lit = on();
        WorkbenchStyle::paintPad(graphics, getLocalBounds().toFloat().reduced(1.0F), lit, isEnabled(),
            mode_ == Mode::Gate ? (lit ? "Open" : "Closed") : (lit ? "On" : "Off"));
        if (hasKeyboardFocus(false))
        {
            graphics.setColour(workbenchTheme.focus);
            graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0F),
                std::min(8.0F, static_cast<float>(getHeight()) * 0.22F), WorkbenchType::focusOutlinePixels());
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (mode_ == Mode::Knob)
        {
            juce::Slider::mouseDown(event);
            return;
        }
        if (isEnabled() && event.mods.isLeftButtonDown())
            press();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (mode_ == Mode::Knob)
            juce::Slider::mouseDrag(event);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (mode_ == Mode::Knob)
        {
            juce::Slider::mouseUp(event);
            return;
        }
        release();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (mode_ == Mode::Knob)
            juce::Slider::mouseDoubleClick(event);
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (mode_ == Mode::Knob)
            juce::Slider::mouseWheelMove(event, wheel);
    }

    // Space or Return presses a pad; a gate opens until the key is let go.
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (mode_ != Mode::Knob && isEnabled()
            && (key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey))
        {
            if (!pressed_)
                press();
            if (mode_ == Mode::Toggle)
                release();
            return true;
        }
        return juce::Slider::keyPressed(key);
    }

    bool keyStateChanged(bool isKeyDown) override
    {
        if (mode_ == Mode::Gate && pressed_ && !isKeyDown
            && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)
            && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey)
            && !juce::ModifierKeys::currentModifiers.isLeftButtonDown())
        {
            release();
            return true;
        }
        return juce::Slider::keyStateChanged(isKeyDown);
    }

private:
    void press()
    {
        pressed_ = true;
        if (onPadPress)
            onPadPress();
    }

    void release()
    {
        if (!pressed_)
            return;
        pressed_ = false;
        if (onPadRelease)
            onPadRelease();
    }

    Mode mode_ = Mode::Knob;
    bool pressed_ = false;
};

class PerformanceControlItem final : public juce::Component,
                                     private juce::Slider::Listener
{
public:
    PerformanceControlItem(
        PerformanceCanvasImpl& owner,
        talas::app::PerformanceControlViewModel model,
        double value,
        double minimum,
        double maximum,
        bool compact);

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override;

    [[nodiscard]] std::string_view controlId() const noexcept
    {
        return model_.controlId;
    }

    [[nodiscard]] talas::project::CanvasPosition canvasPosition() const noexcept
    {
        return model_.position;
    }

    [[nodiscard]] const talas::project::SnapshotTargetIdentity& target()
        const noexcept
    {
        return model_.target;
    }

    void setSelected(bool selected);
    void setExternalValue(double value);
    void setActivity(talas::app::PerformanceActivitySource source);
    [[nodiscard]] talas::app::PerformanceActivitySource activity() const noexcept
    {
        return activity_;
    }
    [[nodiscard]] bool setValueForSmoke(double value);

    [[nodiscard]] juce::String focusedComponentIdentity() const
    {
        const auto* focused = juce::Component::getCurrentlyFocusedComponent();
        return focused != nullptr
                && (focused == this || isParentOf(focused))
                && focused->getComponentID().isNotEmpty()
            ? focused->getComponentID()
            : getComponentID();
    }

    bool restoreFocusedComponent(const juce::String& identity)
    {
        auto* component = slider_.getComponentID() == identity
            ? static_cast<juce::Component*>(&slider_)
            : getComponentID() == identity
                ? static_cast<juce::Component*>(this)
                : nullptr;
        if (component == nullptr || !component->isVisible()
            || !component->isEnabled())
        {
            return false;
        }
        component->grabKeyboardFocus();
        return component->hasKeyboardFocus(true);
    }

private:
    void sliderValueChanged(juce::Slider*) override;
    void sliderDragStarted(juce::Slider*) override;
    void sliderDragEnded(juce::Slider*) override;
    void configureSlider();
    juce::String mappingSummary() const;

    [[nodiscard]] juce::String valueText(double value) const;
    [[nodiscard]] juce::String unitText() const;

    PerformanceCanvasImpl& owner_;
    talas::app::PerformanceControlViewModel model_;
    PerformanceControlSlider slider_;
    juce::Label valueLabel_;
    juce::Rectangle<int> unitArea_;
    juce::Point<int> dragOrigin_;
    talas::project::CanvasPosition originalPosition_;
    talas::app::PerformanceActivitySource activity_ =
        talas::app::PerformanceActivitySource::None;
    std::uint32_t activityAtMilliseconds_ = 0;
    bool compact_ = false;
    bool selected_ = false;
    bool moving_ = false;
    bool externalUpdate_ = false;
};

class PerformanceMeterItem final : public juce::Component
{
public:
    PerformanceMeterItem(
        PerformanceCanvasImpl& owner,
        talas::project::ProjectPerformanceMeterPresentation meter,
        std::span<std::atomic<float>> peaks,
        bool compact)
        : owner_(owner)
        , meter_(std::move(meter))
        , peaks_(peaks)
        , compact_(compact)
    {
        const std::array focusFields{
            std::string_view{meter_.meterId}};
        setComponentID(juceString(workbenchStableFocusItemId(
            "performance.meter", focusFields)));
        setWantsKeyboardFocus(true);
        setTitle("Output " + juce::String(meter_.outputChannel) + " meter");
        setDescription(
            "Live peak meter for prepared master output channel "
            + juce::String(meter_.outputChannel));
    }

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress& key) override;

    [[nodiscard]] std::string_view meterId() const noexcept
    {
        return meter_.meterId;
    }

    [[nodiscard]] talas::project::CanvasPosition canvasPosition() const noexcept
    {
        return meter_.position;
    }

    void setSelected(bool selected)
    {
        selected_ = selected;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto scaled = [](int pixels) {
            return workbenchScaledUiPixels(pixels);
        };
        auto bounds = getLocalBounds().toFloat();
        paintPerformanceTile(graphics, bounds, workbenchTheme.output, selected_);
        const auto index = meter_.outputChannel == 0
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(meter_.outputChannel - 1);
        const auto peak = index < peaks_.size()
            ? peaks_[index].load(std::memory_order_relaxed)
            : -1.0F;
        const auto normalized = peak < 0.0F
            ? 0.0F
            : std::clamp(peak, 0.0F, 1.0F);
        auto body = getLocalBounds().reduced(scaled(10))
                        .withTrimmedTop(scaled(8));
        graphics.setColour(workbenchTheme.primaryText);
        graphics.setFont(WorkbenchType::bodyStrong());
        graphics.drawText(
            "Output " + juce::String(meter_.outputChannel),
            body.removeFromTop(scaled(20)),
            juce::Justification::centredLeft);
        auto readout = body.removeFromBottom(scaled(18));
        auto meter = body.withSizeKeepingCentre(body.getWidth(),
            std::min(body.getHeight(), scaled(compact_ ? 12 : 16)));
        paintLevelMeter(graphics, meter.toFloat(), normalized);
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::data());
        const auto text = peak < 0.0F
            ? juce::String{"Unavailable"}
            : peak <= 1.0e-6F
                ? juce::String{"-inf dB"}
                : juce::String(
                    20.0 * std::log10(static_cast<double>(peak)), 1)
                    + " dB";
        graphics.drawText(text, readout, juce::Justification::centredRight);
    }

private:
    PerformanceCanvasImpl& owner_;
    talas::project::ProjectPerformanceMeterPresentation meter_;
    std::span<std::atomic<float>> peaks_;
    juce::Point<int> dragOrigin_;
    talas::project::CanvasPosition originalPosition_;
    bool compact_ = false;
    bool selected_ = false;
    bool moving_ = false;
};

class PerformanceCanvasImpl final : public WorkbenchPerformanceCanvas,
                                    private juce::Timer,
                                    private juce::AsyncUpdater
{
public:
    PerformanceCanvasImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        talas::app::PerformanceRuntimeDispatcher& dispatcher,
        talas::project::WorkspacePerformanceSessionState& session,
        std::span<std::atomic<float>> outputPeaks,
        ProjectAuthoringCallback onProjectAuthoring,
        SessionAuthoringCallback onSessionAuthoring,
        StatusCallback onStatus,
        SelectionCallback onSelection,
        AcceptedValueCallback onAcceptedValue)
        : bridge_(bridge)
        , dispatcher_(dispatcher)
        , session_(session)
        , outputPeaks_(outputPeaks)
        , onProjectAuthoring_(std::move(onProjectAuthoring))
        , onSessionAuthoring_(std::move(onSessionAuthoring))
        , onStatus_(std::move(onStatus))
        , onSelection_(std::move(onSelection))
        , onAcceptedValue_(std::move(onAcceptedValue))
        , editButton_("Edit")
        , compactButton_("Compact")
        , addMeterButton_("Meter")
        , zoomOutButton_("-")
        , zoomInButton_("+")
        , fitButton_("Fit")
    {
        describeControl(
            *this,
            "Performance canvas",
            "Operate and arrange stable Performance controls without changing Structure layout.");
        setFocusContainerType(
            juce::Component::FocusContainerType::none);
        addAndMakeVisible(editButton_);
        addAndMakeVisible(compactButton_);
        addAndMakeVisible(addMeterButton_);
        addAndMakeVisible(zoomOutButton_);
        addAndMakeVisible(zoomInButton_);
        addAndMakeVisible(fitButton_);
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, true);
        content_.addAndMakeVisible(emptyState_);
        emptyState_.setJustificationType(juce::Justification::centred);
        emptyState_.setFont(WorkbenchType::body());
        emptyState_.setColour(
            juce::Label::textColourId, workbenchTheme.secondaryText);
        emptyState_.setInterceptsMouseClicks(false, false);
        editButton_.setClickingTogglesState(true);
        compactButton_.setClickingTogglesState(true);
        describeControl(
            editButton_,
            "Edit Performance layout",
            "Enables portable control and meter placement on the 32-pixel grid.");
        describeControl(
            compactButton_,
            "Compact Performance controls",
            "Reduces secondary detail while retaining explicit state labels.");
        describeControl(
            addMeterButton_,
            "Add output meter",
            "Adds one portable meter for an available bounded master-output peak.");
        describeControl(
            zoomOutButton_,
            "Zoom out Performance canvas",
            "Reduces the local canvas view without changing portable positions.");
        describeControl(
            zoomInButton_,
            "Zoom in Performance canvas",
            "Enlarges the local canvas view without changing portable positions.");
        describeControl(
            fitButton_,
            "Fit Performance canvas",
            "Resets local pan and zoom without changing the project.");
        editButton_.setToggleState(session_.editMode, juce::dontSendNotification);
        compactButton_.setToggleState(session_.compact, juce::dontSendNotification);
        editButton_.onClick = [this] {
            session_.editMode = editButton_.getToggleState();
            onSessionAuthoring_();
            refresh();
        };
        compactButton_.onClick = [this] {
            session_.compact = compactButton_.getToggleState();
            onSessionAuthoring_();
            refresh();
        };
        addMeterButton_.onClick = [this] {
            (void)addOutputMeter();
        };
        zoomOutButton_.onClick = [this] {
            setZoom(session_.zoom - canvasZoomStep);
        };
        zoomInButton_.onClick = [this] {
            setZoom(session_.zoom + canvasZoomStep);
        };
        fitButton_.onClick = [this] {
            session_.panX = 0.0;
            session_.panY = 0.0;
            session_.zoom = 1.0;
            onSessionAuthoring_();
            refresh();
        };
        viewport_.onVisibleAreaChanged = [this](const auto& area) {
            if (restoringViewport_)
                return;
            const auto nextX = static_cast<double>(area.getX());
            const auto nextY = static_cast<double>(area.getY());
            if (session_.panX == nextX && session_.panY == nextY)
                return;
            session_.panX = nextX;
            session_.panY = nextY;
            onSessionAuthoring_();
        };
        startTimerHz(30);
        refresh();
    }

    ~PerformanceCanvasImpl() override
    {
        stopTimer();
        cancelPendingUpdate();
        for (const auto& token : {valueGestureToken_, layoutGestureToken_})
        {
            if (token.has_value())
                bridge_.editor().endOwnedHistoryTransaction(*token);
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
        graphics.setColour(juce::Colours::white.withAlpha(0.06F));
        graphics.fillRect(juce::Rectangle<int>{1, toolbarHeight - 1, std::max(0, getWidth() - 2), 1});
        WorkbenchStyle::paintPanelTitle(graphics, {14, 0, 120, toolbarHeight}, "Performance");
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::label());
        graphics.drawText(
            session_.editMode
                ? "Arrange layout"
                : "Perform controls",
            136,
            0,
            std::max(0, getWidth() - 410),
            toolbarHeight,
            juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto toolbar = bounds.removeFromTop(toolbarHeight).reduced(8, 6);
        fitButton_.setBounds(toolbar.removeFromRight(48));
        toolbar.removeFromRight(6);
        zoomInButton_.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(4);
        zoomOutButton_.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(8);
        compactButton_.setBounds(toolbar.removeFromRight(72));
        toolbar.removeFromRight(6);
        addMeterButton_.setBounds(toolbar.removeFromRight(58));
        toolbar.removeFromRight(6);
        editButton_.setBounds(toolbar.removeFromRight(54));
        viewport_.setBounds(bounds);
        if (autoFlowColumns() != autoColumns_)
        {
            const auto restoring = restoringViewport_;
            restoringViewport_ = true;
            layoutItems();
            restoringViewport_ = restoring;
        }
    }

    // Each item at its portable position, or flowed in rows when it has
    // none, and the content sized to hold them.
    void layoutItems()
    {
        const auto visualScale = canvasZoom()
            * static_cast<double>(WorkbenchType::interfaceScale());
        const auto scaled = [visualScale](int value) {
            return juce::roundToInt(
                static_cast<double>(value) * visualScale);
        };
        autoColumns_ = autoFlowColumns();
        const auto minimumWidth = std::max(viewport_.getWidth(), 520);
        const auto minimumHeight = std::max(viewport_.getHeight(), 320);
        auto width = minimumWidth;
        auto height = minimumHeight;
        const auto place = [&](juce::Component& item, talas::project::CanvasPosition position, int index) {
            const auto itemPosition = effectivePosition(position, index);
            item.setBounds(
                scaled(itemPosition.x),
                scaled(itemPosition.y),
                scaled(itemWidth),
                scaled(session_.compact ? compactItemHeight : itemHeight));
            width = std::max(width, item.getRight() + canvasPadding);
            height = std::max(height, item.getBottom() + canvasPadding);
        };
        for (auto index = 0; index < controls_.size(); ++index)
            place(*controls_[index], controls_[index]->canvasPosition(), index);
        for (auto index = 0; index < meters_.size(); ++index)
            place(*meters_[index], meters_[index]->canvasPosition(), controls_.size() + index);
        content_.setSize(width, height);
        emptyState_.setBounds(
            32,
            32,
            std::max(0, minimumWidth - 64),
            std::max(0, minimumHeight - 64));
    }

    // As many columns as the canvas shows whole at its zoom, at least one.
    [[nodiscard]] int autoFlowColumns() const
    {
        const auto visualScale = canvasZoom()
            * static_cast<double>(WorkbenchType::interfaceScale());
        const auto visible = static_cast<double>(std::max(0,
            viewport_.getWidth() - viewport_.getScrollBarThickness()));
        const auto available = visible / visualScale - 2.0 * canvasPadding;
        return std::max(1, static_cast<int>(
            (available + autoFlowGap) / static_cast<double>(itemWidth + autoFlowGap)));
    }

    void refresh() override
    {
        juce::String focusedIdentity;
        for (const auto* item : controls_)
        {
            if (item != nullptr && item->hasKeyboardFocus(true))
            {
                focusedIdentity = item->focusedComponentIdentity();
                break;
            }
        }
        if (focusedIdentity.isEmpty())
        {
            for (const auto* item : meters_)
            {
                if (item != nullptr && item->hasKeyboardFocus(true))
                {
                    focusedIdentity = item->getComponentID();
                    break;
                }
            }
        }
        // Rebuilding deletes an item mid-gesture, so its drag end never comes.
        finishValueGesture();
        if (layoutGestureToken_.has_value())
        {
            bridge_.editor().endOwnedHistoryTransaction(*layoutGestureToken_);
            layoutGestureToken_.reset();
            juce::Component::SafePointer<PerformanceCanvasImpl> safeThis(this);
            juce::MessageManager::callAsync([safeThis] {
                if (safeThis != nullptr)
                    safeThis->adoptLayoutEdit();
            });
        }
        controls_.clear(true);
        meters_.clear(true);
        projection_ = buildProjection();
        session_.zoom = std::clamp(
            session_.zoom, minimumCanvasZoom, maximumCanvasZoom);
        const auto& document = bridge_.editor().document();
        const auto& runtime = bridge_.performanceState();
        for (auto model : projection_.controls)
        {
            if (!model.visible)
                continue;
            const auto range = targetRange(document, runtime, model.target);
            model.directEligible = model.directEligible
                && range.has_value()
                && std::isfinite(range->minimum)
                && std::isfinite(range->maximum)
                && range->minimum < range->maximum;
            const auto value = currentValue(runtime, model.target);
            const auto minimum = range.has_value() ? range->minimum : 0.0;
            const auto maximum = range.has_value() ? range->maximum : 1.0;
            auto* item = controls_.add(new PerformanceControlItem(
                *this,
                model,
                value.value_or(minimum),
                minimum,
                maximum,
                session_.compact));
            content_.addAndMakeVisible(item);
            item->setSelected(
                session_.selectedControlId
                    == std::optional<std::string>{model.controlId});
        }
        for (const auto& meter : projection_.meters)
        {
            if (!meter.visible)
                continue;
            auto* item = meters_.add(new PerformanceMeterItem(
                *this, meter, outputPeaks_, session_.compact));
            content_.addAndMakeVisible(item);
            item->setSelected(
                session_.selectedMeterId
                    == std::optional<std::string>{meter.meterId});
        }
        const auto hasVisibleItems = controls_.size() + meters_.size() > 0;
        emptyState_.setVisible(!hasVisibleItems || !projection_.ok());
        if (!projection_.ok())
        {
            surfaceState_ = talas::app::workbenchSurfaceState({
                talas::app::WorkbenchSurfaceStateKind::Unavailable,
                "Performance layout unavailable",
                projection_.diagnostics.front(),
                {},
                false,
            });
            emptyState_.setText(
                "Performance layout unavailable\n"
                    + juce::String::fromUTF8(
                        projection_.diagnostics.front().c_str()),
                juce::dontSendNotification);
            emptyState_.setTitle("Performance layout unavailable");
            emptyState_.setDescription(juce::String::fromUTF8(
                projection_.diagnostics.front().c_str()));
        }
        else if (!hasVisibleItems)
        {
            surfaceState_ = talas::app::workbenchSurfaceState({
                talas::app::WorkbenchSurfaceStateKind::Empty,
                "No Performance controls",
                "Add or reveal stable controls from the Performance inspector.",
                talas::app::WorkbenchSurfaceActionEvidence{
                    "performance.add-control", "Add control", true},
                false,
            });
            emptyState_.setText(
                "No Performance controls\n"
                "Add or reveal controls from the Performance inspector.",
                juce::dontSendNotification);
            emptyState_.setTitle("No Performance controls");
            emptyState_.setDescription(
                "Add or reveal stable controls from the Performance inspector.");
        }
        else
        {
            surfaceState_ = talas::app::workbenchSurfaceState({
                talas::app::WorkbenchSurfaceStateKind::Content,
                "Performance controls",
                "Stable exposed controls are available for the active Sound instance.",
                {},
                true,
            });
        }
        restoringViewport_ = true;
        layoutItems();
        viewport_.setViewPosition(
            juce::roundToInt(session_.panX),
            juce::roundToInt(session_.panY));
        restoringViewport_ = false;
        content_.repaint();
        repaint();
        reconcileObservedValues();
        reconcileSelection();
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::structureChanged);
        }
        if (focusedIdentity.isNotEmpty() && isShowing())
        {
            for (auto* item : controls_)
            {
                if (item->restoreFocusedComponent(focusedIdentity))
                {
                    return;
                }
            }
            for (auto* item : meters_)
            {
                if (item->getComponentID() == focusedIdentity)
                {
                    item->grabKeyboardFocus();
                    return;
                }
            }
        }
    }

    void refreshRuntimeState() override
    {
        const auto& state = bridge_.performanceState();
        for (auto* item : controls_)
        {
            if (const auto value = currentValue(state, item->target()))
            {
                const auto identity =
                    talas::project::snapshotTargetIdentityString(
                        item->target());
                const auto observed = observedValues_.find(identity);
                if (observed != observedValues_.end()
                    && observed->second != *value)
                {
                    item->setActivity(
                        talas::app::PerformanceActivitySource::Snapshot);
                }
                observedValues_[identity] = *value;
                item->setExternalValue(*value);
            }
        }
        for (auto* meter : meters_)
            meter->repaint();
    }

    const std::optional<talas::app::WorkbenchSurfaceState>&
    workbenchSurfaceStateForContextHelp() const noexcept override
    {
        return surfaceState_;
    }

    void noteActivity(
        const talas::project::SnapshotTargetIdentity& target,
        talas::app::PerformanceActivitySource source) override
    {
        for (auto* item : controls_)
        {
            if (item->target() == target)
                item->setActivity(source);
        }
        if (const auto value = currentValue(
                bridge_.performanceState(), target))
        {
            observedValues_[
                talas::project::snapshotTargetIdentityString(target)] = *value;
        }
    }

    std::size_t visibleItemCountForSmoke() const noexcept override
    {
        return static_cast<std::size_t>(controls_.size() + meters_.size());
    }

    bool controlsAccessibleForSmoke() const override
    {
        return std::ranges::all_of(
                   controls_, [](const auto* item) {
                       return item != nullptr
                           && item->getTitle().isNotEmpty()
                           && item->getDescription().isNotEmpty();
                   })
            && std::ranges::all_of(
                meters_, [](const auto* item) {
                    return item != nullptr
                        && item->getTitle().isNotEmpty()
                        && item->getDescription().isNotEmpty();
                });
    }

    talas::app::PerformanceActivitySource firstControlActivityForSmoke()
        const noexcept override
    {
        return controls_.isEmpty()
            ? talas::app::PerformanceActivitySource::None
            : controls_[0]->activity();
    }

    bool setFirstControlValueForSmoke(double value) override
    {
        return controls_.size() > 0 && controls_[0]->setValueForSmoke(value);
    }

    bool addOutputMeterForSmoke() override
    {
        return addOutputMeter();
    }

    bool moveFirstControlForSmoke(
        talas::project::CanvasPosition position) override
    {
        if (controls_.size() == 0)
            return false;
        const auto controlId = std::string{controls_[0]->controlId()};
        beginMove(controlId);
        const auto moved = commitMove(controlId, position);
        endMove();
        return moved;
    }

    bool setCompactForSmoke(bool compact) override
    {
        session_.compact = compact;
        compactButton_.setToggleState(compact, juce::dontSendNotification);
        onSessionAuthoring_();
        refresh();
        return session_.compact == compact;
    }

    bool scaledLayoutCoherentForSmoke() const override
    {
        const auto expectedPosition = [&](
                                          talas::project::CanvasPosition position,
                                          int index) {
            return visualPosition(autoFlowPosition(position, index, autoColumns_));
        };
        std::vector<juce::Rectangle<int>> bounds;
        bounds.reserve(static_cast<std::size_t>(
            controls_.size() + meters_.size()));
        for (auto index = 0; index < controls_.size(); ++index)
        {
            const auto* item = controls_[index];
            if (item == nullptr
                || item->getPosition()
                    != expectedPosition(item->canvasPosition(), index))
            {
                return false;
            }
            bounds.push_back(item->getBounds());
        }
        for (auto index = 0; index < meters_.size(); ++index)
        {
            const auto* item = meters_[index];
            if (item == nullptr
                || item->getPosition()
                    != expectedPosition(
                        item->canvasPosition(),
                        controls_.size() + index))
            {
                return false;
            }
            bounds.push_back(item->getBounds());
        }
        for (std::size_t left = 0; left < bounds.size(); ++left)
        {
            for (auto right = left + 1; right < bounds.size(); ++right)
            {
                if (bounds[left].intersects(bounds[right]))
                    return false;
            }
        }
        return true;
    }

    void selectControl(std::string_view controlId)
    {
        session_.selectedControlId = std::string{controlId};
        session_.selectedMeterId.reset();
        onSessionAuthoring_();
        onSelection_(session_.selectedControlId, std::nullopt);
        for (auto* item : controls_)
            item->setSelected(item->controlId() == controlId);
        for (auto* item : meters_)
            item->setSelected(false);
    }

    void selectMeter(std::string_view meterId)
    {
        session_.selectedControlId.reset();
        session_.selectedMeterId = std::string{meterId};
        onSessionAuthoring_();
        onSelection_(std::nullopt, session_.selectedMeterId);
        for (auto* item : controls_)
            item->setSelected(false);
        for (auto* item : meters_)
            item->setSelected(item->meterId() == meterId);
    }

    bool dispatch(
        PerformanceControlItem& item,
        double value,
        talas::app::PerformanceActivitySource source)
    {
        const auto result = dispatcher_.dispatch(
            item.controlId(),
            value,
            bridge_.latestStageGeneration());
        if (!result.ok())
        {
            onStatus_(result.message.empty()
                    ? juce::String{"Performance update was rejected"}
                    : juce::String::fromUTF8(result.message.c_str()));
            return false;
        }
        item.setActivity(source);
        for (const auto& applied : result.appliedValues)
        {
            noteActivity(
                applied.target,
                applied.target == item.target()
                    ? source
                    : talas::app::PerformanceActivitySource::Macro);
            if (source
                    != talas::app::PerformanceActivitySource::UserInterface
                || applied.target.kind
                    != talas::project::SnapshotTargetKind::EventValue
                || applied.value.kind
                    != talas::project::SnapshotValueKind::Number)
            {
                continue;
            }
            const auto eventId =
                bridge_.realtimeEventValueIdForLatestStage(
                    applied.target.eventName,
                    applied.value.domain);
            const auto milliseconds =
                juce::Time::getMillisecondCounterHiRes();
            const auto nanoseconds = std::isfinite(milliseconds)
                    && milliseconds > 0.0
                    && milliseconds
                        < static_cast<double>(
                            std::numeric_limits<std::int64_t>::max())
                            / 1'000'000.0
                ? static_cast<std::int64_t>(
                    std::llround(milliseconds * 1'000'000.0))
                : std::int64_t{0};
            const auto stage = bridge_.latestStageGeneration();
            const auto admitted = eventId.has_value()
                && bridge_.enqueueControllerRecordingObservationAtTimeForStage(
                    *eventId,
                    applied.value.number,
                    applied.value.domain,
                    nanoseconds,
                    stage,
                    {
                        talas::project::ControllerRecordingOrigin::Ui,
                        nanoseconds,
                        stage,
                        0,
                    });
            if (!admitted && bridge_.controllerRecordingActive())
            {
                bridge_.stopControllerRecording(
                    talas::project::ControllerRecordingTerminal::CapacityExceeded);
            }
        }
        if (item.target().kind
            == talas::project::SnapshotTargetKind::EventValue)
        {
            onAcceptedValue_(item.target().eventName, value);
        }
        return true;
    }

    void beginMove(std::string_view controlId)
    {
        selectControl(controlId);
        beginLayoutGesture();
    }

    void beginLayoutGesture()
    {
        layoutGestureToken_ =
            bridge_.editor().beginOwnedHistoryTransaction();
    }

    void beginValueGesture()
    {
        valueGestureToken_ = bridge_.editor().beginValueHistoryTransaction();
    }

    void finishValueGesture()
    {
        if (!valueGestureToken_.has_value())
            return;
        bridge_.editor().endOwnedHistoryTransaction(*valueGestureToken_);
        valueGestureToken_.reset();
        // The authoring callback refreshes the canvas, which deletes the item
        // while juce::Slider is still inside its mouse-up handling; defer it.
        juce::Component::SafePointer<PerformanceCanvasImpl> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->onProjectAuthoring_(true);
        });
    }

    bool commitMove(
        std::string_view controlId,
        talas::project::CanvasPosition position)
    {
        const auto result = bridge_.editor().movePerformanceControl(
            controlId, position, layoutGestureToken_.value_or(0));
        if (!result.ok())
        {
            onStatus_(juce::String::fromUTF8(result.message.c_str()));
            return false;
        }
        return true;
    }

    bool commitMeterMove(
        std::string_view meterId,
        talas::project::CanvasPosition position)
    {
        const auto result = bridge_.editor().movePerformanceMeter(
            meterId, position, layoutGestureToken_.value_or(0));
        if (!result.ok())
        {
            onStatus_(juce::String::fromUTF8(result.message.c_str()));
            return false;
        }
        return true;
    }

    void endMove()
    {
        if (layoutGestureToken_.has_value())
            bridge_.editor().endOwnedHistoryTransaction(*layoutGestureToken_);
        layoutGestureToken_.reset();
        adoptLayoutEdit();
    }

    void adoptLayoutEdit()
    {
        const auto adopted = bridge_.adoptSnapshotAuthoringEdit();
        if (!adopted.ok())
        {
            onStatus_(adopted.message.empty()
                    ? juce::String{"Performance layout requires a fresh stage"}
                    : juce::String::fromUTF8(adopted.message.c_str()));
        }
        onProjectAuthoring_(adopted.ok());
    }

    bool hideSelected()
    {
        if (!session_.selectedControlId.has_value()
            && !session_.selectedMeterId.has_value())
            return false;
        const auto result = session_.selectedControlId.has_value()
            ? bridge_.editor().setPerformanceControlVisible(
                *session_.selectedControlId, false)
            : bridge_.editor().setPerformanceMeterVisible(
                *session_.selectedMeterId, false);
        if (!result.ok())
        {
            onStatus_(juce::String::fromUTF8(result.message.c_str()));
            return false;
        }
        session_.selectedControlId.reset();
        session_.selectedMeterId.reset();
        onSelection_(std::nullopt, std::nullopt);
        const auto adopted = bridge_.adoptSnapshotAuthoringEdit();
        if (!adopted.ok())
        {
            onStatus_(adopted.message.empty()
                    ? juce::String{"Performance layout requires a fresh stage"}
                    : juce::String::fromUTF8(adopted.message.c_str()));
        }
        onProjectAuthoring_(adopted.ok());
        triggerAsyncUpdate();
        return true;
    }

    [[nodiscard]] bool editMode() const noexcept
    {
        return session_.editMode;
    }

    [[nodiscard]] double canvasZoom() const noexcept
    {
        return std::clamp(
            session_.zoom, minimumCanvasZoom, maximumCanvasZoom);
    }

    [[nodiscard]] juce::Point<int> visualPosition(
        talas::project::CanvasPosition position) const noexcept
    {
        const auto zoom = canvasZoom();
        const auto visualScale = zoom
            * static_cast<double>(WorkbenchType::interfaceScale());
        return {
            juce::roundToInt(
                (canvasPadding + position.x) * visualScale),
            juce::roundToInt(
                (canvasPadding + position.y) * visualScale)};
    }

private:
    void setZoom(double zoom)
    {
        const auto next = std::clamp(
            zoom, minimumCanvasZoom, maximumCanvasZoom);
        if (next == session_.zoom)
            return;
        session_.zoom = next;
        onSessionAuthoring_();
        refresh();
    }

    bool addOutputMeter()
    {
        const auto& existing = bridge_.editor().document()
                                   .presentation.performanceMeters;
        std::uint32_t outputChannel = 0;
        for (std::size_t channel = 1; channel <= outputPeaks_.size(); ++channel)
        {
            if (!std::ranges::any_of(existing, [&](const auto& meter) {
                    return meter.outputChannel == channel;
                }))
            {
                outputChannel = static_cast<std::uint32_t>(channel);
                break;
            }
        }
        if (outputChannel == 0)
        {
            onStatus_("Every available master output already has a Performance meter");
            return false;
        }
        const auto meterId = "master-output-"
            + std::to_string(outputChannel);
        talas::project::ProjectPerformanceMeterPresentation meter;
        meter.meterId = meterId;
        meter.outputChannel = outputChannel;
        meter.position = {
            static_cast<int>((existing.size() % 4) * (itemWidth + 14)),
            static_cast<int>((existing.size() / 4 + 1)
                * (itemHeight + 14)),
        };
        const auto result = bridge_.editor().addPerformanceMeter(
            std::move(meter));
        if (!result.ok())
        {
            onStatus_(juce::String::fromUTF8(result.message.c_str()));
            return false;
        }
        const auto adopted = bridge_.adoptSnapshotAuthoringEdit();
        if (!adopted.ok() && !adopted.message.empty())
            onStatus_(juce::String::fromUTF8(adopted.message.c_str()));
        session_.selectedControlId.reset();
        session_.selectedMeterId = meterId;
        onSessionAuthoring_();
        onSelection_(std::nullopt, session_.selectedMeterId);
        onProjectAuthoring_(adopted.ok());
        if (adopted.ok())
            onStatus_("Added portable master-output meter");
        refresh();
        return true;
    }

    void handleAsyncUpdate() override
    {
        refresh();
    }

    void reconcileSelection()
    {
        if (session_.selectedControlId.has_value())
        {
            const auto found = std::ranges::any_of(
                projection_.controls,
                [&](const auto& control) {
                    return control.controlId
                            == *session_.selectedControlId
                        && control.visible;
                });
            if (found)
            {
                onSelection_(session_.selectedControlId, std::nullopt);
                return;
            }
            session_.selectedControlId.reset();
            onSessionAuthoring_();
        }
        if (session_.selectedMeterId.has_value())
        {
            const auto found = std::ranges::any_of(
                projection_.meters,
                [&](const auto& meter) {
                    return meter.meterId == *session_.selectedMeterId
                        && meter.visible;
                });
            if (found)
            {
                onSelection_(std::nullopt, session_.selectedMeterId);
                return;
            }
            session_.selectedMeterId.reset();
            onSessionAuthoring_();
        }
        onSelection_(std::nullopt, std::nullopt);
    }

    void reconcileObservedValues()
    {
        std::unordered_map<std::string, double> next;
        next.reserve(controls_.size());
        const auto& state = bridge_.performanceState();
        for (auto* item : controls_)
        {
            const auto value = currentValue(state, item->target());
            if (!value.has_value())
                continue;
            const auto identity =
                talas::project::snapshotTargetIdentityString(item->target());
            const auto previous = observedValues_.find(identity);
            next.emplace(
                identity,
                previous == observedValues_.end()
                    ? *value
                    : previous->second);
        }
        observedValues_ = std::move(next);
    }

    void timerCallback() override
    {
        refreshRuntimeState();
        for (auto* item : controls_)
            item->repaint();
    }

    talas::app::PerformancePanelProjection buildProjection() const
    {
        talas::app::PerformanceProjectionContext context;
        context.activeSnapshotBankId =
            bridge_.performanceState().activeBankId.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{
                bridge_.performanceState().activeBankId};
        for (const auto& control :
             bridge_.editor().document().presentation.performanceControls)
        {
            if (bridge_.snapshotDirectTargetPrepared(control.target))
            {
                context.directTargetIdentities.push_back(
                    talas::project::snapshotTargetIdentityString(control.target));
            }
        }
        return talas::app::buildPerformancePanelProjection(
            bridge_.editor().document(), context);
    }

    // A position of its own, or the item's place in the flow: the first at
    // the origin, the rest after it, row by row.
    [[nodiscard]] static talas::project::CanvasPosition autoFlowPosition(
        talas::project::CanvasPosition position,
        int index,
        int columns) noexcept
    {
        if (position.x == 0 && position.y == 0 && index != 0)
        {
            const auto across = std::max(1, columns);
            position.x = (index % across) * (itemWidth + autoFlowGap);
            position.y = (index / across) * (itemHeight + autoFlowGap);
        }
        return position;
    }

    [[nodiscard]] juce::Point<int> effectivePosition(
        talas::project::CanvasPosition position,
        int index) const noexcept
    {
        position = autoFlowPosition(position, index, autoColumns_);
        return {
            canvasPadding + position.x,
            canvasPadding + position.y};
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    talas::app::PerformanceRuntimeDispatcher& dispatcher_;
    talas::project::WorkspacePerformanceSessionState& session_;
    std::span<std::atomic<float>> outputPeaks_;
    ProjectAuthoringCallback onProjectAuthoring_;
    SessionAuthoringCallback onSessionAuthoring_;
    StatusCallback onStatus_;
    SelectionCallback onSelection_;
    AcceptedValueCallback onAcceptedValue_;
    juce::TextButton editButton_;
    juce::TextButton compactButton_;
    juce::TextButton addMeterButton_;
    juce::TextButton zoomOutButton_;
    juce::TextButton zoomInButton_;
    juce::TextButton fitButton_;
    PerformanceViewport viewport_;
    juce::Component content_;
    juce::Label emptyState_;
    std::optional<talas::app::WorkbenchSurfaceState> surfaceState_;
    juce::OwnedArray<PerformanceControlItem> controls_;
    juce::OwnedArray<PerformanceMeterItem> meters_;
    talas::app::PerformancePanelProjection projection_;
    std::unordered_map<std::string, double> observedValues_;
    bool restoringViewport_ = false;
    int autoColumns_ = 4;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        valueGestureToken_;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        layoutGestureToken_;

    friend class PerformanceControlItem;
    friend class PerformanceMeterItem;
};

PerformanceControlItem::PerformanceControlItem(
    PerformanceCanvasImpl& owner,
    talas::app::PerformanceControlViewModel model,
    double value,
    double minimum,
    double maximum,
    bool compact)
    : owner_(owner)
    , model_(std::move(model))
    , compact_(compact)
{
    const std::array focusFields{
        std::string_view{model_.controlId}};
    setComponentID(juceString(workbenchStableFocusItemId(
        "performance.control", focusFields)));
    const std::array valueFocusFields{
        std::string_view{model_.controlId}, std::string_view{"value"}};
    slider_.setComponentID(juceString(workbenchStableFocusItemId(
        "performance.control", valueFocusFields)));
    setWantsKeyboardFocus(true);
    setTitle(juce::String::fromUTF8(model_.displayName.c_str()));
    setDescription(
        kindName(model_.kind) + " Performance control. "
        + mappingSummary()
        + (model_.locked ? " Locked for morph and randomization." : "")
        + (model_.directEligible
                ? " Ready for the current prepared stage."
                : " Unavailable in the current prepared stage."));
    addAndMakeVisible(slider_);
    addAndMakeVisible(valueLabel_);
    slider_.setRange(minimum, maximum, 0.0);
    slider_.setValue(value, juce::dontSendNotification);
    slider_.addListener(this);
    configureSlider();
    // The value as a readout beside the knob: large, bright, tabular.
    valueLabel_.setFont(WorkbenchType::data().boldened().withHeight(
        WorkbenchType::data().getHeight() + 5.0F));
    valueLabel_.setColour(juce::Label::textColourId, workbenchTheme.primaryText);
    valueLabel_.setJustificationType(juce::Justification::bottomLeft);
    valueLabel_.setBorderSize({});
    valueLabel_.setMinimumHorizontalScale(0.7F);
    valueLabel_.setInterceptsMouseClicks(false, false);
    valueLabel_.setVisible(slider_.mode() == PerformanceControlSlider::Mode::Knob);
    // A pad's press and release are a value gesture of their own, as a
    // knob's drag is.
    slider_.onPadPress = [this] {
        sliderDragStarted(&slider_);
        const auto opened = slider_.mode() == PerformanceControlSlider::Mode::Gate || !slider_.on();
        slider_.setValue(opened ? slider_.getMaximum() : slider_.getMinimum(), juce::sendNotificationSync);
        slider_.repaint();
    };
    slider_.onPadRelease = [this] {
        if (slider_.mode() == PerformanceControlSlider::Mode::Gate)
            slider_.setValue(slider_.getMinimum(), juce::sendNotificationSync);
        slider_.repaint();
        sliderDragEnded(&slider_);
    };
    setExternalValue(value);
}

void PerformanceControlItem::configureSlider()
{
    switch (model_.kind)
    {
    // Every continuous control turns as a knob; a gate or a toggle is a pad.
    case talas::project::ProjectPerformanceControlKind::Gate:
        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setMode(PerformanceControlSlider::Mode::Gate);
        break;
    case talas::project::ProjectPerformanceControlKind::Toggle:
        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setMode(PerformanceControlSlider::Mode::Toggle);
        break;
    case talas::project::ProjectPerformanceControlKind::Knob:
    case talas::project::ProjectPerformanceControlKind::Macro:
    case talas::project::ProjectPerformanceControlKind::Fader:
    default:
        slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider_.setMode(PerformanceControlSlider::Mode::Knob);
        slider_.setMouseDragSensitivity(180);
        break;
    }
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setEnabled(model_.directEligible);
}

void PerformanceControlItem::paint(juce::Graphics& graphics)
{
    const auto scaled = [](int pixels) {
        return workbenchScaledUiPixels(pixels);
    };
    const auto accent = controlAccent(model_.kind);
    auto bounds = getLocalBounds().toFloat();
    paintPerformanceTile(graphics, bounds, accent, selected_);

    // The name, and the kind in a pill of the control's colour.
    auto body = getLocalBounds().reduced(scaled(10))
                    .withTrimmedTop(scaled(2));
    auto title = body.removeFromTop(scaled(20));
    const auto kind = kindName(model_.kind).toUpperCase();
    const auto kindFont = WorkbenchType::microStrong();
    const auto kindWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(kindFont, kind)) + scaled(12);
    const auto kindArea = title.removeFromRight(kindWidth).withSizeKeepingCentre(kindWidth, scaled(15));
    WorkbenchStyle::paintPill(graphics, kindArea.toFloat(), accent, workbenchTheme.controlTile, kind, kindFont);
    title.removeFromRight(scaled(6));
    graphics.setColour(workbenchTheme.primaryText);
    graphics.setFont(WorkbenchType::bodyStrong());
    graphics.drawFittedText(
        juce::String::fromUTF8(model_.displayName.c_str()),
        title,
        juce::Justification::centredLeft,
        1,
        0.8F);
    if (slider_.mode() == PerformanceControlSlider::Mode::Knob && !unitArea_.isEmpty())
    {
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::label());
        graphics.drawFittedText(unitText(), unitArea_, juce::Justification::topLeft, 1);
    }
    if (!compact_)
    {
        auto status = body.removeFromBottom(scaled(16));
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::micro());
        if (model_.locked)
        {
            graphics.setColour(workbenchTheme.control);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText(
                "LOCK",
                status.removeFromRight(scaled(34)),
                juce::Justification::centredRight);
            graphics.setColour(workbenchTheme.secondaryText);
            graphics.setFont(WorkbenchType::micro());
        }
        graphics.drawFittedText(
            mappingSummary(),
            status,
            juce::Justification::centredLeft,
            1);
    }
    const auto now = juce::Time::getMillisecondCounter();
    if (activity_ != talas::app::PerformanceActivitySource::None
        && now - activityAtMilliseconds_ <= activityLifetimeMilliseconds)
    {
        graphics.setColour(accent);
        graphics.drawRoundedRectangle(
            bounds.reduced(3.0F), 5.0F, 2.0F);
        graphics.setColour(workbenchTheme.primaryText);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(
            activityName(activity_),
            getLocalBounds().reduced(scaled(10))
                .removeFromBottom(scaled(16)),
            juce::Justification::centredRight);
    }
}

// A knob as tall as the card allows beside its readout, the value over its
// unit; a pad across the card.
void PerformanceControlItem::resized()
{
    const auto scaled = [](int pixels) {
        return workbenchScaledUiPixels(pixels);
    };
    auto body = getLocalBounds().reduced(scaled(10))
                    .withTrimmedTop(scaled(26));
    if (!compact_)
        body.removeFromBottom(scaled(18));
    unitArea_ = {};
    if (slider_.mode() != PerformanceControlSlider::Mode::Knob)
    {
        valueLabel_.setBounds({});
        slider_.setBounds(body.withSizeKeepingCentre(body.getWidth(),
            std::min(body.getHeight(), scaled(compact_ ? 34 : 46))));
        return;
    }
    const auto knobSize = std::min(body.getHeight() + scaled(4), body.getWidth() / 2);
    slider_.setBounds(body.removeFromLeft(knobSize)
                          .withSizeKeepingCentre(knobSize, knobSize));
    body.removeFromLeft(scaled(10));
    auto readout = body.withSizeKeepingCentre(body.getWidth(),
        std::min(body.getHeight(), scaled(compact_ ? 34 : 40)));
    valueLabel_.setBounds(readout.removeFromTop(readout.getHeight() * 11 / 20));
    unitArea_ = readout.withTrimmedTop(scaled(1));
}

void PerformanceControlItem::mouseDown(const juce::MouseEvent& event)
{
    owner_.selectControl(model_.controlId);
    if (!owner_.editMode()
        || slider_.getBounds().contains(event.getPosition()))
    {
        return;
    }
    moving_ = true;
    dragOrigin_ = event.getScreenPosition();
    originalPosition_ = model_.position;
    owner_.beginMove(model_.controlId);
}

void PerformanceControlItem::mouseDrag(const juce::MouseEvent& event)
{
    if (!moving_)
        return;
    const auto delta = event.getScreenPosition() - dragOrigin_;
    const auto visualScale = owner_.canvasZoom()
        * static_cast<double>(WorkbenchType::interfaceScale());
    const auto snapped = [](int value) {
        return std::max(0, ((value + canvasGrid / 2) / canvasGrid) * canvasGrid);
    };
    const talas::project::CanvasPosition position{
        snapped(originalPosition_.x
            + juce::roundToInt(delta.x / visualScale)),
        snapped(originalPosition_.y
            + juce::roundToInt(delta.y / visualScale)),
    };
    if (owner_.commitMove(model_.controlId, position))
    {
        model_.position = position;
        setTopLeftPosition(owner_.visualPosition(position));
    }
}

void PerformanceControlItem::mouseUp(const juce::MouseEvent&)
{
    if (!moving_)
        return;
    moving_ = false;
    owner_.endMove();
}

bool PerformanceControlItem::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        return owner_.editMode() && owner_.hideSelected();
    }
    if (!owner_.editMode())
        return false;
    auto position = model_.position;
    if (key == juce::KeyPress::leftKey)
        position.x = std::max(0, position.x - canvasGrid);
    else if (key == juce::KeyPress::rightKey)
        position.x += canvasGrid;
    else if (key == juce::KeyPress::upKey)
        position.y = std::max(0, position.y - canvasGrid);
    else if (key == juce::KeyPress::downKey)
        position.y += canvasGrid;
    else
        return false;
    owner_.beginMove(model_.controlId);
    const auto moved = owner_.commitMove(model_.controlId, position);
    if (moved)
    {
        model_.position = position;
        setTopLeftPosition(owner_.visualPosition(position));
    }
    owner_.endMove();
    return moved;
}

std::unique_ptr<juce::AccessibilityHandler>
PerformanceControlItem::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this,
        juce::AccessibilityRole::group);
}

void PerformanceControlItem::setSelected(bool selected)
{
    selected_ = selected;
    repaint();
}

void PerformanceControlItem::setExternalValue(double value)
{
    externalUpdate_ = true;
    slider_.setValue(value, juce::dontSendNotification);
    valueLabel_.setText(valueText(value), juce::dontSendNotification);
    slider_.repaint();
    externalUpdate_ = false;
}

void PerformanceControlItem::setActivity(
    talas::app::PerformanceActivitySource source)
{
    activity_ = source;
    activityAtMilliseconds_ = juce::Time::getMillisecondCounter();
    repaint();
}

bool PerformanceControlItem::setValueForSmoke(double value)
{
    return owner_.dispatch(
        *this, value, talas::app::PerformanceActivitySource::UserInterface);
}

void PerformanceMeterItem::mouseDown(const juce::MouseEvent& event)
{
    owner_.selectMeter(meter_.meterId);
    if (!owner_.editMode())
        return;
    moving_ = true;
    dragOrigin_ = event.getScreenPosition();
    originalPosition_ = meter_.position;
    owner_.beginLayoutGesture();
}

void PerformanceMeterItem::mouseDrag(const juce::MouseEvent& event)
{
    if (!moving_)
        return;
    const auto delta = event.getScreenPosition() - dragOrigin_;
    const auto visualScale = owner_.canvasZoom()
        * static_cast<double>(WorkbenchType::interfaceScale());
    const auto snapped = [](int value) {
        return std::max(
            0, ((value + canvasGrid / 2) / canvasGrid) * canvasGrid);
    };
    const talas::project::CanvasPosition position{
        snapped(originalPosition_.x
            + juce::roundToInt(delta.x / visualScale)),
        snapped(originalPosition_.y
            + juce::roundToInt(delta.y / visualScale)),
    };
    if (owner_.commitMeterMove(meter_.meterId, position))
    {
        meter_.position = position;
        setTopLeftPosition(owner_.visualPosition(position));
    }
}

void PerformanceMeterItem::mouseUp(const juce::MouseEvent&)
{
    if (!moving_)
        return;
    moving_ = false;
    owner_.endMove();
}

bool PerformanceMeterItem::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        return owner_.editMode() && owner_.hideSelected();
    }
    if (!owner_.editMode())
        return false;
    auto position = meter_.position;
    if (key == juce::KeyPress::leftKey)
        position.x = std::max(0, position.x - canvasGrid);
    else if (key == juce::KeyPress::rightKey)
        position.x += canvasGrid;
    else if (key == juce::KeyPress::upKey)
        position.y = std::max(0, position.y - canvasGrid);
    else if (key == juce::KeyPress::downKey)
        position.y += canvasGrid;
    else
        return false;
    owner_.beginLayoutGesture();
    const auto moved = owner_.commitMeterMove(meter_.meterId, position);
    if (moved)
    {
        meter_.position = position;
        setTopLeftPosition(owner_.visualPosition(position));
    }
    owner_.endMove();
    return moved;
}

void PerformanceControlItem::sliderValueChanged(juce::Slider*)
{
    if (externalUpdate_)
        return;
    const auto value = slider_.getValue();
    if (owner_.dispatch(
            *this,
            value,
            talas::app::PerformanceActivitySource::UserInterface))
    {
        valueLabel_.setText(
            valueText(value), juce::dontSendNotification);
    }
}

void PerformanceControlItem::sliderDragStarted(juce::Slider*)
{
    owner_.selectControl(model_.controlId);
    owner_.beginValueGesture();
}

void PerformanceControlItem::sliderDragEnded(juce::Slider*)
{
    owner_.finishValueGesture();
}

// As many decimals as the range needs to show a step of about a
// thousandth of it.
juce::String PerformanceControlItem::valueText(double value) const
{
    const auto span = std::abs(slider_.getMaximum() - slider_.getMinimum());
    const auto decimals = span >= 100.0 ? 1 : span >= 10.0 ? 2 : 3;
    return juce::String(value, decimals);
}

juce::String PerformanceControlItem::unitText() const
{
    switch (model_.target.domain)
    {
    case talas::expr::ValueDomain::Hertz: return "Hz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::NoteNumber: return "note";
    case talas::expr::ValueDomain::Samples: return "samples";
    case talas::expr::ValueDomain::Scalar: break;
    }
    return "scalar";
}

juce::String PerformanceControlItem::mappingSummary() const
{
    juce::StringArray states;
    if (model_.midiMapped)
    {
        states.add(model_.midiSourceUnavailable
                ? "MIDI unavailable"
                : model_.midiLegacy ? "MIDI legacy" : "MIDI");
    }
    if (model_.oscInputMapped)
        states.add("OSC in");
    if (model_.oscFeedbackMapped)
        states.add("OSC out");
    if (!model_.directEligible)
        states.add("Stage unavailable");
    if (model_.modulationSlots.size() > 0)
    {
        juce::StringArray depths;
        for (const auto& slot : model_.modulationSlots)
        {
            depths.add(
                (slot.depth >= 0.0 ? "+" : "")
                + juce::String(slot.depth, 2));
        }
        states.add("Mod " + depths.joinIntoString("/"));
    }
    return states.isEmpty() ? juce::String{"Unmapped"}
                            : states.joinIntoString(" / ");
}
}

std::unique_ptr<WorkbenchPerformanceCanvas>
makeWorkbenchPerformanceCanvas(
    talas::runtime::ProjectRuntimeBridge& bridge,
    talas::app::PerformanceRuntimeDispatcher& dispatcher,
    talas::project::WorkspacePerformanceSessionState& session,
    std::span<std::atomic<float>> outputPeaks,
    WorkbenchPerformanceCanvas::ProjectAuthoringCallback onProjectAuthoring,
    WorkbenchPerformanceCanvas::SessionAuthoringCallback onSessionAuthoring,
    WorkbenchPerformanceCanvas::StatusCallback onStatus,
    WorkbenchPerformanceCanvas::SelectionCallback onSelection,
    WorkbenchPerformanceCanvas::AcceptedValueCallback onAcceptedValue)
{
    return std::make_unique<PerformanceCanvasImpl>(
        bridge,
        dispatcher,
        session,
        outputPeaks,
        std::move(onProjectAuthoring),
        std::move(onSessionAuthoring),
        std::move(onStatus),
        std::move(onSelection),
        std::move(onAcceptedValue));
}
}
